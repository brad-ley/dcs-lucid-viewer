// =============================================================================
// TiffWriter.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares TiffWriter — a minimal class for writing single- or multi-page
//   TIFF files directly, without any external TIFF library.
//
// WHY WRITE TIFF BY HAND?
//   The Arena SDK's Save API writes TIFF files but does not let us embed custom
//   metadata in the ImageDescription tag (tag 270), and it doesn't support the
//   multi-page (TiffStack) format we need.  Rolling our own writer gives us full
//   control over the IFD chain and tag values.
//
// TIFF FILE STRUCTURE (little-endian, as written here):
//
//   Byte 0-1:  "II"          — byte-order marker (little-endian)
//   Byte 2-3:  42            — TIFF magic number
//   Byte 4-7:  offset_of_ifd0 — file offset of the first IFD (usually 8)
//
//   For each page:
//     [pixel data bytes]
//     [IFD — a list of 12-byte tag entries, followed by next-IFD pointer]
//
//   Tags are written in ascending numerical order (TIFF specification requirement).
//   The next-IFD pointer of each IFD points to the next page's IFD.
//   The last IFD has next-IFD == 0 (marks end of file).
//
// C++ CONCEPT — std::fstream:
//   We use std::fstream (read+write) so we can seek back to patch the
//   next-IFD pointer of the previous page after we know the current IFD's offset.
//   std::ofstream (write-only) does not allow seekp() to an already-written area.
// =============================================================================

#pragma once

#include <cstdint>   // uint16_t, uint32_t
#include <fstream>   // std::fstream
#include <string>    // std::string

// =============================================================================
// TiffWriter class
// =============================================================================
class TiffWriter
{
public:

    // Constructor / destructor
    TiffWriter();
    ~TiffWriter();  // calls close() if still open

    // -------------------------------------------------------------------------
    // open() — create the TIFF file and write the 8-byte header.
    //
    //   path            — full file path (e.g. "C:/captures/acq_X/stack.tiff")
    //   width           — image width in pixels
    //   height          — image height in pixels (= rows)
    //   bitsPerSample   — bits per pixel (8 or 16)
    //   imageDescription — optional JSON string written into tag 270 (page 0 only).
    //                      Pass "" to omit the tag.
    //
    // Returns true on success, false if the file could not be created.
    // -------------------------------------------------------------------------
    bool open(const std::string& path,
              uint32_t           width,
              uint32_t           height,
              uint16_t           bitsPerSample,
              const std::string& imageDescription = "");

    // -------------------------------------------------------------------------
    // addPage() — append one image frame to the TIFF file.
    //
    //   data            — pointer to raw pixel bytes (row-major, no padding)
    //   dataBytes       — total byte count (must equal width * height * (bitsPerSample/8))
    //   pageDescription — optional JSON string written into tag 270 for this page.
    //                     On page 0, this overrides the imageDescription passed to open().
    //                     Pass "" (default) to omit the tag on that page (except page 0
    //                     which still uses the open()-time description if set).
    //
    // Returns true on success.
    // -------------------------------------------------------------------------
    bool addPage(const void* data, size_t dataBytes,
                 const std::string& pageDescription = "");

    // -------------------------------------------------------------------------
    // close() — flush and close the file.
    //   The last IFD already has next-IFD == 0 (written by addPage), so the
    //   file is valid as soon as addPage returns — close() just flushes buffers.
    // -------------------------------------------------------------------------
    void close();

    // Accessors
    bool isOpen()    const;
    int  pageCount() const;

private:

    std::fstream m_file;
    uint32_t     m_width;
    uint32_t     m_height;
    uint16_t     m_bitsPerSample;
    int          m_pageCount;

    // File offset of the "next IFD offset" field in the most recently written IFD.
    // When we write a new IFD we seek back here to patch in its address, then
    // seek forward again to write the actual data.
    // Before the first page this holds offset 4 (the initial IFD pointer in the header).
    uint32_t m_prevNextIfdPtrOffset;

    // JSON string embedded in tag 270 on the first page only.
    std::string m_imageDescription;

    // -------------------------------------------------------------------------
    // writeIfd() — internal helper.
    //   Called by addPage() after the pixel data has been written.
    //   Writes the IFD at the current file position and patches m_prevNextIfdPtrOffset
    //   so the previous IFD (or the header) points here.
    //
    //   imageDataOffset — file offset where the pixel bytes start
    //   imageDataBytes  — number of pixel bytes
    // -------------------------------------------------------------------------
    void writeIfd(uint32_t imageDataOffset, uint32_t imageDataBytes,
                  const std::string& desc);

    // -------------------------------------------------------------------------
    // writeLe16 / writeLe32 — write a little-endian integer to m_file.
    //   TIFF requires little-endian when the header says "II".
    //   On Windows (x86/x64) the native byte order is already little-endian,
    //   but we write byte-by-byte to be correct on any platform.
    // -------------------------------------------------------------------------
    void writeLe16(uint16_t v);
    void writeLe32(uint32_t v);

    // -------------------------------------------------------------------------
    // writeIfdEntry() — write one 12-byte IFD entry.
    //
    //   tag    — TIFF tag number (e.g. 256 = ImageWidth)
    //   type   — TIFF data type (3 = SHORT/uint16, 4 = LONG/uint32)
    //   count  — number of values
    //   value  — the value itself (or file offset if count*typeSize > 4)
    //
    // For SHORT (type 3, 2 bytes) the 4-byte value field holds the uint16 in
    // the low 2 bytes with the high 2 bytes zero (little-endian: low byte first).
    // -------------------------------------------------------------------------
    void writeIfdEntry(uint16_t tag, uint16_t type, uint32_t count, uint32_t value);
};
