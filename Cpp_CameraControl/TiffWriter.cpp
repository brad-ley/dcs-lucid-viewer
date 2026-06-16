// =============================================================================
// TiffWriter.cpp
// =============================================================================
//
// Implementation of TiffWriter — writes a multi-page little-endian TIFF file
// without any external TIFF library.
//
// TIFF FORMAT QUICK REFERENCE:
//
//   Header (8 bytes):
//     [0-1]  "II"     — byte-order: "II" = little-endian, "MM" = big-endian
//     [2-3]  42       — TIFF magic number (always 42)
//     [4-7]  offset   — file offset of the first IFD (we put it at byte 8)
//
//   IFD (Image File Directory) — one per page:
//     [0-1]  count    — number of 12-byte entries that follow
//     [2..N] entries  — 12 bytes each (see writeIfdEntry below)
//     [N+2..N+5] next — file offset of next IFD, or 0 if this is the last page
//
//   IFD entry (12 bytes):
//     [0-1]  tag    — what this field describes (e.g. 256 = width)
//     [2-3]  type   — data type (3=SHORT/2-byte, 4=LONG/4-byte, 2=ASCII)
//     [4-7]  count  — number of values
//     [8-11] value  — the value itself, or a file offset if the data is > 4 bytes
//
//   Tags MUST be written in ascending numerical order.
//
//   Tags used here (in order):
//     254  NewSubfileType      LONG   1  value=0 (full-resolution image)
//     256  ImageWidth          LONG   1  image width in pixels
//     257  ImageLength         LONG   1  image height in pixels
//     258  BitsPerSample       SHORT  1  8 or 16
//     259  Compression         SHORT  1  1 = no compression
//     262  PhotometricInterp   SHORT  1  1 = BlackIsZero (grayscale)
//     270  ImageDescription    ASCII  N  JSON metadata string (page 0 only)
//     273  StripOffsets        LONG   1  file offset of pixel data
//     278  RowsPerStrip        LONG   1  = image height (one strip per image)
//     279  StripByteCounts     LONG   1  total pixel bytes
//     284  PlanarConfiguration SHORT  1  1 = chunky (interleaved channels)
//
// MULTI-PAGE STRATEGY:
//   Each addPage() call:
//     1. Notes the current file position as imageDataOffset
//     2. Writes the pixel bytes
//     3. Notes the current file position as ifdOffset
//     4. Calls writeIfd() which writes the IFD at ifdOffset
//     5. writeIfd() seeks back to m_prevNextIfdPtrOffset and patches it
//        so the previous IFD (or the header) points to the new IFD
//     6. m_prevNextIfdPtrOffset is updated to the "next IFD" field in the
//        new IFD, so the next page can patch it in turn
// =============================================================================

#include "TiffWriter.h"

#include <cstring>   // std::memcpy (not actually used, but conventional include)
#include <cstddef>   // size_t


// =============================================================================
// Constructor / Destructor
// =============================================================================

TiffWriter::TiffWriter()
    : m_width(0)
    , m_height(0)
    , m_bitsPerSample(8)
    , m_pageCount(0)
    , m_prevNextIfdPtrOffset(4)   // header's IFD pointer lives at byte 4
{
}

TiffWriter::~TiffWriter()
{
    close();
}


// =============================================================================
// open() — write the 8-byte TIFF header and prepare for page writes
// =============================================================================
bool TiffWriter::open(const std::string& path,
                      uint32_t           width,
                      uint32_t           height,
                      uint16_t           bitsPerSample,
                      const std::string& imageDescription)
{
    // Open for read+write in binary mode, truncating any existing file.
    // std::fstream requires ios::in|ios::out|ios::trunc|ios::binary.
    // Without ios::in, seekp() to already-written bytes would fail.
    m_file.open(path, std::ios::in | std::ios::out |
                      std::ios::trunc | std::ios::binary);
    if (!m_file.is_open())
        return false;

    m_width            = width;
    m_height           = height;
    m_bitsPerSample    = bitsPerSample;
    m_pageCount        = 0;
    m_imageDescription = imageDescription;

    // The first IFD will be placed right after the 8-byte header (at offset 8).
    // But we don't know the IFD's exact location yet because we write pixel data
    // first.  So we write the header with a placeholder at bytes 4-7, then patch
    // it in writeIfd() via m_prevNextIfdPtrOffset = 4.
    m_prevNextIfdPtrOffset = 4;

    // -------------------------------------------------------------------------
    // Write the 8-byte TIFF header
    // -------------------------------------------------------------------------
    // Byte-order marker: "II" = little-endian
    m_file.put('I');
    m_file.put('I');
    // Magic number 42, little-endian
    writeLe16(42);
    // Placeholder for the first IFD offset (will be patched by writeIfd)
    writeLe32(0);

    return m_file.good();
}


// =============================================================================
// addPage() — append one image to the TIFF file
// =============================================================================
bool TiffWriter::addPage(const void* data, size_t dataBytes,
                         const std::string& pageDescription)
{
    if (!m_file.is_open()) return false;

    // Decide which description string to embed in tag 270 for this page.
    // Caller-supplied pageDescription takes precedence; on page 0 fall back
    // to the description passed to open(); on later pages omit if not supplied.
    const std::string& desc = !pageDescription.empty()
        ? pageDescription
        : (m_pageCount == 0 ? m_imageDescription : std::string{});

    // Step 1: record where the pixel data starts
    uint32_t imageDataOffset = static_cast<uint32_t>(m_file.tellp());

    // Step 2: write the raw pixel bytes
    m_file.write(static_cast<const char*>(data), static_cast<std::streamsize>(dataBytes));
    if (!m_file.good()) return false;

    // Step 3: record where the IFD starts (IFD immediately follows pixel data)
    uint32_t ifdOffset = static_cast<uint32_t>(m_file.tellp());
    (void)ifdOffset;  // only used in the comment above; writeIfd reads tellp() itself

    // Step 4: write the IFD (also patches the previous next-IFD pointer)
    writeIfd(imageDataOffset, static_cast<uint32_t>(dataBytes), desc);

    ++m_pageCount;
    return m_file.good();
}


// =============================================================================
// close() — flush and close
// =============================================================================
void TiffWriter::close()
{
    if (m_file.is_open())
    {
        m_file.flush();
        m_file.close();
    }
}


// =============================================================================
// isOpen() / pageCount() — accessors
// =============================================================================
bool TiffWriter::isOpen()    const { return m_file.is_open(); }
int  TiffWriter::pageCount() const { return m_pageCount; }


// =============================================================================
// writeIfd() — write one IFD and patch the previous page's next-IFD pointer
// =============================================================================
void TiffWriter::writeIfd(uint32_t imageDataOffset, uint32_t imageDataBytes,
                          const std::string& desc)
{
    // -------------------------------------------------------------------------
    // Step A: record this IFD's file offset
    // -------------------------------------------------------------------------
    uint32_t thisIfdOffset = static_cast<uint32_t>(m_file.tellp());

    // -------------------------------------------------------------------------
    // Step B: patch the previous "next IFD" field so it points here.
    //   On the very first page, m_prevNextIfdPtrOffset == 4, which is the
    //   4-byte IFD-pointer slot in the file header.
    //   On subsequent pages it's the "next IFD" field of the previous IFD.
    // -------------------------------------------------------------------------
    uint32_t savedPos = static_cast<uint32_t>(m_file.tellp());
    m_file.seekp(m_prevNextIfdPtrOffset);
    writeLe32(thisIfdOffset);
    m_file.seekp(savedPos);

    // -------------------------------------------------------------------------
    // Step C: build the list of IFD entries we will write.
    //   Tags MUST be in ascending numerical order per the TIFF spec.
    //   We decide now whether to include tag 270 (ImageDescription),
    //   because it affects the entry count and we might need to write the
    //   description string after the IFD.
    // -------------------------------------------------------------------------

    // Write tag 270 (ImageDescription) for this page if a non-empty string was given.
    bool writeDesc = !desc.empty();

    // Count of IFD entries:
    //   254, 256, 257, 258, 259, 262, [270,] 273, 278, 279, 284
    //   That's 10 fixed + 1 optional = 10 or 11
    uint16_t entryCount = writeDesc ? 11 : 10;

    // -------------------------------------------------------------------------
    // Step D: if ImageDescription is needed, we will write the string AFTER
    //   the IFD block (not inline, because the string is typically > 4 bytes).
    //   We need to know the string's file offset before writing the IFD entry.
    //
    //   IFD block size = 2 (entry count) + 12*entryCount + 4 (next IFD ptr)
    //   String starts immediately after the IFD block.
    // -------------------------------------------------------------------------
    uint32_t descOffset = 0;
    uint32_t descLength = 0;
    if (writeDesc)
    {
        uint32_t ifdBlockSize = 2 + 12 * static_cast<uint32_t>(entryCount) + 4;
        descOffset = thisIfdOffset + ifdBlockSize;
        // TIFF ASCII count includes the null terminator
        descLength = static_cast<uint32_t>(desc.size()) + 1;
    }

    // -------------------------------------------------------------------------
    // Step E: write the IFD entry count
    // -------------------------------------------------------------------------
    writeLe16(entryCount);

    // -------------------------------------------------------------------------
    // Step F: write each IFD entry in ascending tag order
    //
    // TIFF types used:
    //   2  = ASCII  (bytes, null-terminated; count includes null)
    //   3  = SHORT  (uint16_t, 2 bytes)
    //   4  = LONG   (uint32_t, 4 bytes)
    //
    // Value packing rules:
    //   If count * typeSize <= 4 bytes, the value is stored directly in the
    //   4-byte value field (left-justified, zero-padded, little-endian).
    //   Otherwise the value field holds a file offset to the actual data.
    //
    //   SHORT (2 bytes), count=1: value field = [lo, hi, 0, 0]
    //   LONG  (4 bytes), count=1: value field = all 4 bytes of the uint32
    // -------------------------------------------------------------------------

    // Tag 254 — NewSubfileType (LONG, 1 value, value = 0 = full image)
    writeIfdEntry(254, 4, 1, 0);

    // Tag 256 — ImageWidth (LONG, 1 value)
    writeIfdEntry(256, 4, 1, m_width);

    // Tag 257 — ImageLength / height (LONG, 1 value)
    writeIfdEntry(257, 4, 1, m_height);

    // Tag 258 — BitsPerSample (SHORT, 1 value)
    writeIfdEntry(258, 3, 1, m_bitsPerSample);

    // Tag 259 — Compression (SHORT, 1 value, 1 = no compression)
    writeIfdEntry(259, 3, 1, 1);

    // Tag 262 — PhotometricInterpretation (SHORT, 1 value, 1 = BlackIsZero)
    writeIfdEntry(262, 3, 1, 1);

    // Tag 270 — ImageDescription (ASCII, count = string length + 1 for null)
    //   The string is too long to fit in 4 bytes, so the value field holds
    //   the file offset where the string will be written after the IFD.
    if (writeDesc)
        writeIfdEntry(270, 2, descLength, descOffset);

    // Tag 273 — StripOffsets (LONG, 1 value = file offset of pixel data)
    writeIfdEntry(273, 4, 1, imageDataOffset);

    // Tag 278 — RowsPerStrip (LONG, 1 value = total height = one strip)
    writeIfdEntry(278, 4, 1, m_height);

    // Tag 279 — StripByteCounts (LONG, 1 value = total pixel byte count)
    writeIfdEntry(279, 4, 1, imageDataBytes);

    // Tag 284 — PlanarConfiguration (SHORT, 1 value, 1 = chunky/interleaved)
    writeIfdEntry(284, 3, 1, 1);

    // -------------------------------------------------------------------------
    // Step G: write the "next IFD" pointer field (4 bytes).
    //   We set it to 0 now (meaning "end of file / last page").
    //   The NEXT addPage() call will seek back here and patch in the real address.
    //   m_prevNextIfdPtrOffset records where this field lives in the file.
    // -------------------------------------------------------------------------
    m_prevNextIfdPtrOffset = static_cast<uint32_t>(m_file.tellp());
    writeLe32(0);   // placeholder — patched by the next call to writeIfd()

    // -------------------------------------------------------------------------
    // Step H: write the ImageDescription string (if needed)
    //   Write every byte of the string plus a null terminator.
    // -------------------------------------------------------------------------
    if (writeDesc)
    {
        m_file.write(desc.c_str(), static_cast<std::streamsize>(desc.size()));
        m_file.put('\0');   // TIFF ASCII strings must be null-terminated
    }
}


// =============================================================================
// writeLe16 — write a uint16_t as two bytes, least-significant byte first
// =============================================================================
void TiffWriter::writeLe16(uint16_t v)
{
    // C++ CONCEPT — bit masking and shifting:
    //   v & 0xFF  extracts the low 8 bits (least significant byte)
    //   v >> 8    shifts right by 8 bits to get the high byte
    m_file.put(static_cast<char>(v & 0xFF));
    m_file.put(static_cast<char>((v >> 8) & 0xFF));
}


// =============================================================================
// writeLe32 — write a uint32_t as four bytes, least-significant byte first
// =============================================================================
void TiffWriter::writeLe32(uint32_t v)
{
    m_file.put(static_cast<char>( v        & 0xFF));
    m_file.put(static_cast<char>((v >>  8) & 0xFF));
    m_file.put(static_cast<char>((v >> 16) & 0xFF));
    m_file.put(static_cast<char>((v >> 24) & 0xFF));
}


// =============================================================================
// writeIfdEntry — write one 12-byte IFD entry
// =============================================================================
//
// Layout of a TIFF IFD entry (all little-endian):
//   Bytes 0-1:  tag    (uint16)
//   Bytes 2-3:  type   (uint16)
//   Bytes 4-7:  count  (uint32)
//   Bytes 8-11: value  (uint32) — either the value itself or a file offset
//
// For SHORT (type=3, 2 bytes each), count=1:
//   The spec says "values that fit in 4 bytes are stored in the value field."
//   Little-endian means byte 8 = low byte of the uint16, byte 9 = high byte,
//   bytes 10-11 = 0 padding.  We achieve this by passing the uint16 value cast
//   to uint32 — writeLe32 writes it correctly.
//
void TiffWriter::writeIfdEntry(uint16_t tag, uint16_t type,
                                uint32_t count, uint32_t value)
{
    writeLe16(tag);
    writeLe16(type);
    writeLe32(count);
    writeLe32(value);
}
