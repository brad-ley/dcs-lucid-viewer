// =============================================================================
// PreviewWorker.cpp
// =============================================================================
//
// Implementation of the PreviewWorker background thread.
// This thread grabs frames from the camera and emits QImage signals so
// the PreviewDialog can display them live without blocking the GUI.
//
// FRAME RATE LIMITING:
//   The camera might run at 30+ fps, but we don't emit every frame.
//   We track elapsed time since the last emit and only emit if at least
//   ~33ms has passed (target: ~30 fps display rate).
//   Dropped frames are silently ignored — we just grab the next one.
//
// IMAGE CONVERSION:
//   Arena SDK gives us raw pixel bytes. We need to convert to QImage::Format_*
//   so Qt can display them. The conversion depends on bits-per-pixel and format:
//     8-bit:  Direct QImage (Mono8, Bayer, etc.) → grayscale
//     16-bit: Convert to 8-bit by taking high byte → grayscale
//     24-bit: Direct QImage if RGB8Packed
// =============================================================================

#include "PreviewWorker.h"

// Arena SDK — needed to call GetImage, StartStream, StopStream, etc.
#include "ArenaApi.h"

// C++ standard library
#include <chrono>      // std::chrono — for timing frames
#include <thread>      // std::this_thread::sleep_for — for pauses

// Qt
#include <QDateTime>    // QDateTime — for status messages with timestamps
#include <QString>      // Qt string type
#include <QMutexLocker> // RAII lock guard for QMutex


// =============================================================================
// Constructor
// =============================================================================
PreviewWorker::PreviewWorker(QObject* parent)
    : QThread(parent)
    , m_pDevice(nullptr)
    , m_stopRequested(false)
    , m_displayHint(0, 0)   // Disabled until PreviewDialog sets it before start()
{
    // All members initialized above. Nothing else needed in the body.
}


// =============================================================================
// Destructor
// =============================================================================
PreviewWorker::~PreviewWorker()
{
    // Ensure the thread is stopped before destroying the object.
    requestStop();
    if (!wait(5000))
    {
        // Thread is stuck (most likely blocked inside GetImage() on a dead camera).
        // Forcibly terminate rather than hanging the destructor forever.
        terminate();
        wait(1000);
    }
}


// =============================================================================
// setDevice
// =============================================================================
void PreviewWorker::setDisplayHint(QSize maxSize)
{
    m_displayHint = maxSize;
}


void PreviewWorker::setDevice(Arena::IDevice* device)
{
    // Store the pointer to the camera device.
    // The device is owned and managed by CameraManager, not by this worker.
    // We must NOT delete this pointer in our destructor.
    m_pDevice = device;
}


// =============================================================================
// requestStop
// =============================================================================
void PreviewWorker::requestStop()
{
    // Set the atomic stop flag to true.
    // The run() loop checks this flag and exits when it becomes true.
    // Using std::atomic<bool> ensures this is thread-safe — multiple threads
    // can safely call this without explicit locks.
    m_stopRequested.store(true);
}


// =============================================================================
// run — THE MAIN PREVIEW THREAD BODY
// =============================================================================
void PreviewWorker::run()
{
    // Reset the stop flag at the start of each run (in case the thread is restarted)
    m_stopRequested.store(false);

    if (m_pDevice == nullptr)
    {
        emit errorOccurred("No device set. Cannot start preview.");
        return;
    }

    try
    {
        // =========================================================
        // STEP 1: Configure the Transport Layer (stream) settings
        // =========================================================
        //
        // These are the same settings used in AcquisitionWorker for best performance.

        GenApi::INodeMap* pStreamNodeMap = m_pDevice->GetTLStreamNodeMap();

        // Negotiate packet size with the NIC for best performance.
        Arena::SetNodeValue<bool>(pStreamNodeMap, "StreamAutoNegotiatePacketSize", true);

        // Enable packet resend in case UDP packets are lost.
        Arena::SetNodeValue<bool>(pStreamNodeMap, "StreamPacketResendEnable", true);

        // If we fall behind, drop old frames rather than queuing them.
        // This keeps the display responsive and prevents memory buildup.
        Arena::SetNodeValue<GenICam::gcstring>(
            pStreamNodeMap, "StreamBufferHandlingMode", "NewestOnly");

        // =========================================================
        // STEP 2: Configure the camera
        // =========================================================
        //
        // Set the camera to continuous acquisition mode.

        GenApi::INodeMap* pNodeMap = m_pDevice->GetNodeMap();
        Arena::SetNodeValue<GenICam::gcstring>(pNodeMap, "AcquisitionMode", "Continuous");

        // Attempt to use jumbo frames (8192 byte packets) for better performance
        // on 10GigE cameras. On failure, just log a warning and continue with default MTU.
        try
        {
            Arena::SetNodeValue<int64_t>(pNodeMap, "GevSCPSPacketSize", 8192);
            emit statusMessage("Jumbo frames enabled (8192 bytes)");
        }
        catch (const GenICam::GenericException&)
        {
            emit statusMessage("Warning: Could not set jumbo frame size. Using default MTU.");
        }

        // =========================================================
        // STEP 3: Start the stream
        // =========================================================

        m_pDevice->StartStream();
        emit statusMessage("Previewing");

        // =========================================================
        // STEP 4: Frame capture and display loop
        // =========================================================
        //
        // Loop continuously: GetImage → convert to QImage → if 33ms elapsed, emit → RequeueBuffer
        //
        // We track the last emit time to limit display rate to ~30 fps.
        // The camera may run faster (e.g., 47 fps on a Triton), so we silently drop frames.

        // Read pixel format string once before the loop.
        // pImage->GetPixelFormat() returns a uint64_t enum value, NOT a string.
        // The human-readable name (e.g., "Mono8", "RGB8Packed") must be read from
        // the device node map. The pixel format cannot change while streaming,
        // so reading it here (once) is both correct and efficient.
        GenICam::gcstring pfGcStr = Arena::GetNodeValue<GenICam::gcstring>(pNodeMap, "PixelFormat");
        std::string pixelFormat(pfGcStr.c_str());

        // Emit the raw format string so we can verify exactly what the SDK returns.
        // Visible in the preview status bar — helps diagnose dispatch issues.
        emit statusMessage(QString("PixelFormat string: \"%1\"")
            .arg(QString::fromStdString(pixelFormat)));

        // ------------------------------------------------------------------
        // Detect external trigger mode ONCE before entering the frame loop.
        //
        // In external trigger mode the camera only captures when it receives
        // a hardware pulse, which may arrive seconds or minutes apart.  A
        // 2-second GetImage() timeout would fire as an error immediately.
        // Instead we use a short (500 ms) polling timeout and treat any
        // timeout exception inside the loop as "no trigger yet — keep waiting."
        // The stop flag is checked between every poll, so clicking Stop
        // always responds within ~500 ms even in external trigger mode.
        // ------------------------------------------------------------------
        bool isExternalTrigger = false;
        try
        {
            GenICam::gcstring trigMode =
                Arena::GetNodeValue<GenICam::gcstring>(pNodeMap, "TriggerMode");
            isExternalTrigger = (std::string(trigMode.c_str()) == "On");
        }
        catch (...) {}   // If the node doesn't exist, treat as normal mode

        if (isExternalTrigger)
            emit statusMessage("External trigger mode");

        // In external trigger mode we poll every 500 ms so Stop responds quickly.
        // In normal (free-run) mode we use 2000 ms, which is fine since frames arrive constantly.
        const int timeoutMs = isExternalTrigger ? 500 : 2000;

        auto lastEmitTime   = std::chrono::high_resolution_clock::now();
        auto lastFpsReport  = lastEmitTime;   // When we last emitted cameraFps()
        const int EMIT_INTERVAL_MS = 33;      // Target: ~30 fps (1000 ms / 30 ≈ 33 ms)
        const int FPS_REPORT_MS    = 1000;    // Report camera rate once per second
        int displayFrameCount = 0;            // Frames actually sent to the GUI (≤ 30/s)
        int cameraFrameCount  = 0;            // All frames received from the camera

        while (!m_stopRequested.load())
        {
            // Wait for a frame from the camera.
            // In external trigger mode this frequently times out (no trigger yet) — that is
            // normal.  We catch the exception here and just retry.
            Arena::IImage* pImage = nullptr;
            try
            {
                pImage = m_pDevice->GetImage(timeoutMs);
            }
            catch (const GenICam::GenericException&)
            {
                if (isExternalTrigger)
                    continue;   // Timeout waiting for trigger pulse — not an error
                throw;          // In free-run mode a GetImage exception is a real error
            }

            if (pImage == nullptr)
            {
                // Some SDK versions return nullptr on timeout instead of throwing.
                continue;
            }

            // Check if the frame is incomplete (e.g., lost packets, etc.)
            if (pImage->IsIncomplete())
            {
                // Return the buffer to the camera and skip this frame
                m_pDevice->RequeueBuffer(pImage);
                emit statusMessage("Warning: received incomplete frame, skipping");
                continue;
            }

            // Count every successfully received, complete frame toward the camera rate.
            cameraFrameCount++;

            // Get frame metadata
            const uint8_t* pData = static_cast<const uint8_t*>(pImage->GetData());
            size_t         dataSize = pImage->GetSizeFilled();
            int            width    = static_cast<int>(pImage->GetWidth());
            int            height   = static_cast<int>(pImage->GetHeight());
            int            bitsPerPixel = static_cast<int>(pImage->GetBitsPerPixel());

            // =========================================================
            // Convert raw pixel data to QImage based on format
            // =========================================================
            //
            // C++ CONCEPT — switch statement:
            //   A switch evaluates an expression once and jumps to the matching case.
            //   More efficient than chained if/else when checking many conditions.
            //   'break' exits the switch; 'default' handles unmatched cases.

            QImage displayImage;

            // Determine the image format based on pixel format name and bits per pixel.
            //
            // IMPORTANT: Mono10Packed has PFNC ID 0x010C0004 — the 0x0C field encodes
            // 12 allocated bits per pixel (3 bytes / 2 pixels), so GetBitsPerPixel()
            // returns 12 for it, not 10. We must dispatch on the format name string for
            // packed formats BEFORE the bitsPerPixel == 12 check, otherwise Mono10Packed
            // is misidentified as Mono12Packed.
            const bool fmtIs10Packed = pixelFormat.find("10")     != std::string::npos
                                    && pixelFormat.find("Packed") != std::string::npos;
            const bool fmtIs12Packed = pixelFormat.find("12")     != std::string::npos
                                    && pixelFormat.find("Packed") != std::string::npos;

            if (bitsPerPixel == 8)
            {
                // 8-bit formats: Mono8, Bayer*, etc. — direct mapping to Grayscale8
                // Create a QImage from the raw data and immediately copy it.
                // We must .copy() because the pointer is only valid until RequeueBuffer().
                displayImage = QImage(pData, width, height, width, QImage::Format_Grayscale8).copy();
            }
            else if (bitsPerPixel == 16)
            {
                // 16-bit format (Mono16, BayerRG16, etc.)
                // For preview, we convert to 8-bit grayscale by taking the high byte.
                // This is a simple downsampling strategy that preserves contrast.
                // For a more sophisticated approach, you could use the low byte or average them.

                // Allocate an 8-bit buffer for the downsampled image
                std::vector<uint8_t> grayscaleData(width * height);

                // Convert each 16-bit pixel to 8-bit by taking the high byte
                const uint16_t* pData16 = reinterpret_cast<const uint16_t*>(pData);
                for (size_t i = 0; i < width * height; ++i)
                {
                    // Extract the high byte of the 16-bit value
                    // >> 8 shifts right by 8 bits, effectively dividing by 256
                    grayscaleData[i] = static_cast<uint8_t>(pData16[i] >> 8);
                }

                // Create QImage from the converted 8-bit data and copy it
                displayImage = QImage(grayscaleData.data(), width, height, width,
                                     QImage::Format_Grayscale8).copy();
            }
            else if (bitsPerPixel == 24 && pixelFormat.find("RGB") != std::string::npos)
            {
                // 24-bit RGB format (RGB8Packed or similar)
                // Qt's Format_RGB888 is exactly 3 bytes per pixel in R,G,B order
                displayImage = QImage(pData, width, height, 3 * width,
                                     QImage::Format_RGB888).copy();
            }
            else if (fmtIs10Packed)
            {
                // ---------------------------------------------------------------
                // Mono10Packed  —  Lucid Vision native layout (0x010C0004)
                //
                // IMPORTANT: GetBitsPerPixel() returns 12 for this format because the
                // PFNC ID encodes 12 allocated bits (3 bytes / 2 pixels). We must
                // dispatch on fmtIs10Packed BEFORE the bitsPerPixel == 12 branches to
                // avoid misidentifying this as Mono12Packed.
                //
                // Lucid Vision packs the LOW bits of both pixels into byte[1], with
                // p1's high bits in the HIGH field and p0's low bits in the LOW field
                // (mirroring their Mono12Packed layout, confirmed by PolarizedCamera example):
                //
                //   byte[n+0]            = pixel0[9:2]   (upper 8 bits of pixel 0)
                //   byte[n+1] bits [1:0] = pixel0[1:0]   (lower 2 bits of pixel 0)
                //   byte[n+1] bits [7:6] = pixel1[1:0]   (lower 2 bits of pixel 1)
                //   byte[n+2]            = pixel1[9:2]   (upper 8 bits of pixel 1)
                //
                //   p0 = (byte[0] << 2) | (byte[1] & 0x03)
                //   p1 = (byte[2] << 2) | (byte[1] >> 6)
                //
                // >> 2 maps 10-bit range [0, 1023] → 8-bit range [0, 255].
                // ---------------------------------------------------------------

                const int numPixels = width * height;
                std::vector<uint8_t> grayscaleData(numPixels, 0);

                int byteIdx = 0;
                for (int i = 0; i + 1 < numPixels; i += 2, byteIdx += 3)
                {
                    if (byteIdx + 2 >= static_cast<int>(dataSize))
                        break;

                    const uint16_t p0 = (static_cast<uint16_t>(pData[byteIdx])     << 2)
                                       |  static_cast<uint16_t>(pData[byteIdx + 1] & 0x03);
                    const uint16_t p1 = (static_cast<uint16_t>(pData[byteIdx + 2]) << 2)
                                       |  static_cast<uint16_t>(pData[byteIdx + 1] >> 6);

                    grayscaleData[i]     = static_cast<uint8_t>(p0 >> 2);
                    grayscaleData[i + 1] = static_cast<uint8_t>(p1 >> 2);
                }

                displayImage = QImage(grayscaleData.data(), width, height, width,
                                     QImage::Format_Grayscale8).copy();
            }
            else if (bitsPerPixel == 12 && !fmtIs12Packed)
            {
                // ---------------------------------------------------------------
                // Mono12p  —  PFNC standard (0x010C0047), LSB-first
                //
                // 2 pixels packed into 3 bytes, least-significant bits first:
                //
                //   byte[n+0]            = pixel0[7:0]    (low 8 bits of pixel 0)
                //   byte[n+1] bits [3:0] = pixel0[11:8]   (high 4 bits of pixel 0)
                //   byte[n+1] bits [7:4] = pixel1[3:0]    (low 4 bits of pixel 1)
                //   byte[n+2]            = pixel1[11:4]   (high 8 bits of pixel 1)
                //
                //   p0 = byte[0]  |  ((byte[1] & 0x0F) << 8)
                //   p1 = (byte[1] >> 4)  |  (byte[2] << 4)
                //
                // >> 4 maps 12-bit [0, 4095] → 8-bit [0, 255].
                // ---------------------------------------------------------------

                const int numPixels = width * height;
                std::vector<uint8_t> grayscaleData(numPixels, 0);

                int byteIdx = 0;
                for (int i = 0; i + 1 < numPixels; i += 2, byteIdx += 3)
                {
                    if (byteIdx + 2 >= static_cast<int>(dataSize))
                        break;

                    const uint16_t p0 = static_cast<uint16_t>(pData[byteIdx])
                                      | (static_cast<uint16_t>(pData[byteIdx + 1] & 0x0F) << 8);
                    const uint16_t p1 = static_cast<uint16_t>(pData[byteIdx + 1] >> 4)
                                      | (static_cast<uint16_t>(pData[byteIdx + 2]) << 4);

                    grayscaleData[i]     = static_cast<uint8_t>(p0 >> 4);
                    grayscaleData[i + 1] = static_cast<uint8_t>(p1 >> 4);
                }

                displayImage = QImage(grayscaleData.data(), width, height, width,
                                     QImage::Format_Grayscale8).copy();
            }
            else if (fmtIs12Packed)
            {
                // ---------------------------------------------------------------
                // Mono12Packed  —  Lucid Vision native layout (from Cpp_PolarizedCamera.cpp)
                //
                //   byte[n+0]            = pixel0[11:4]   (high 8 bits of pixel 0)
                //   byte[n+1] bits [3:0] = pixel0[3:0]    (low 4 bits of pixel 0)
                //   byte[n+1] bits [7:4] = pixel1[3:0]    (low 4 bits of pixel 1)
                //   byte[n+2]            = pixel1[11:4]   (high 8 bits of pixel 1)
                //
                //   p0 = (byte[0] << 4) | (byte[1] & 0x0F)
                //   p1 = (byte[2] << 4) | (byte[1] >> 4)
                //
                // >> 4 maps 12-bit [0, 4095] → 8-bit [0, 255].
                // ---------------------------------------------------------------

                const int numPixels = width * height;
                std::vector<uint8_t> grayscaleData(numPixels, 0);

                int byteIdx = 0;
                for (int i = 0; i + 1 < numPixels; i += 2, byteIdx += 3)
                {
                    if (byteIdx + 2 >= static_cast<int>(dataSize))
                        break;

                    const uint16_t p0 = (static_cast<uint16_t>(pData[byteIdx])     << 4)
                                      |  static_cast<uint16_t>(pData[byteIdx + 1] & 0x0F);
                    const uint16_t p1 = (static_cast<uint16_t>(pData[byteIdx + 2]) << 4)
                                      |  static_cast<uint16_t>(pData[byteIdx + 1] >> 4);

                    grayscaleData[i]     = static_cast<uint8_t>(p0 >> 4);
                    grayscaleData[i + 1] = static_cast<uint8_t>(p1 >> 4);
                }

                displayImage = QImage(grayscaleData.data(), width, height, width,
                                     QImage::Format_Grayscale8).copy();
            }
            else if (bitsPerPixel == 10)
            {
                // ---------------------------------------------------------------
                // Mono10p  —  10-bit pixels, 5 bytes per 4 pixels (tight packing)
                //
                // Little-endian bit packing across a 40-bit (5-byte) window:
                //
                //   p0 = bits  [9:0]  → byte[0]       | (byte[1] & 0x03) << 8
                //   p1 = bits [19:10] → byte[1] >> 2   | (byte[2] & 0x0F) << 6
                //   p2 = bits [29:20] → byte[2] >> 4   | (byte[3] & 0x3F) << 4
                //   p3 = bits [39:30] → byte[3] >> 6   |  byte[4]         << 2
                //
                // >> 2 maps 10-bit range [0, 1023] → 8-bit range [0, 255].
                // ---------------------------------------------------------------

                const int numPixels = width * height;
                std::vector<uint8_t> grayscaleData(numPixels, 0);

                // Process 4 pixels at a time (each group consumes 5 bytes)
                int byteIdx = 0;
                for (int i = 0; i + 3 < numPixels; i += 4, byteIdx += 5)
                {
                    if (byteIdx + 4 >= static_cast<int>(dataSize))
                        break;

                    const uint16_t p0 =  pData[byteIdx]
                                      | (static_cast<uint16_t>(pData[byteIdx + 1] & 0x03) << 8);
                    const uint16_t p1 =  (pData[byteIdx + 1] >> 2)
                                      | (static_cast<uint16_t>(pData[byteIdx + 2] & 0x0F) << 6);
                    const uint16_t p2 =  (pData[byteIdx + 2] >> 4)
                                      | (static_cast<uint16_t>(pData[byteIdx + 3] & 0x3F) << 4);
                    const uint16_t p3 =  (pData[byteIdx + 3] >> 6)
                                      | (static_cast<uint16_t>(pData[byteIdx + 4]) << 2);

                    // >> 2 maps 10-bit range [0, 1023] → 8-bit range [0, 255]
                    grayscaleData[i]     = static_cast<uint8_t>(p0 >> 2);
                    grayscaleData[i + 1] = static_cast<uint8_t>(p1 >> 2);
                    grayscaleData[i + 2] = static_cast<uint8_t>(p2 >> 2);
                    grayscaleData[i + 3] = static_cast<uint8_t>(p3 >> 2);
                }

                displayImage = QImage(grayscaleData.data(), width, height, width,
                                     QImage::Format_Grayscale8).copy();
            }
            else
            {
                // Fallback for any other format: treat as 8-bit grayscale.
                // If you see garbled output with a new format, add a branch above.
                displayImage = QImage(pData, width, height, width,
                                     QImage::Format_Grayscale8).copy();
            }

            // =========================================================
            // Downscale to display hint before cross-thread emit
            // =========================================================
            //
            // For high-resolution cameras (e.g. 24 MP) the raw image can be
            // 6000×4000 = 24 M pixels.  Emitting that across the thread boundary
            // copies ~24 MB, and the GUI thread then has to:
            //   1. Walk every pixel in applyDisplayPipeline (builds a 96 MB ARGB32 image)
            //   2. Upload 96 MB to the GPU via QPixmap::fromImage
            // Both steps block the GUI thread and cause a visible hang.
            //
            // Scaling here (on the worker thread) to ~2× the viewport size reduces
            // the emitted image to roughly the display resolution.  The cross-thread
            // copy shrinks proportionally, and the GUI thread work is 10–20× cheaper.
            // ZoomableImageWidget still zooms and pans the pixmap at paint time, so
            // the user gets smooth interaction — they just can't zoom in past 2× before
            // the image softens, which is a reasonable tradeoff for a live preview.
            if (m_displayHint.isValid() &&
                m_displayHint.width() > 0 &&
                m_displayHint.height() > 0 &&
                (displayImage.width()  > m_displayHint.width() ||
                 displayImage.height() > m_displayHint.height()))
            {
                // Qt::FastTransformation = nearest-neighbour — cheap, good enough for live preview.
                displayImage = displayImage.scaled(m_displayHint,
                                                   Qt::KeepAspectRatio,
                                                   Qt::FastTransformation);
            }

            // =========================================================
            // Frame rate limiting: only emit if 33ms+ has passed
            // =========================================================
            //
            // The camera might send frames faster than we want to display them.
            // We check elapsed time since the last emit. If enough time has passed,
            // we emit the frame and reset the timer. Otherwise, we skip it.
            // This keeps the GUI responsive and prevents the event queue from overflowing.

            auto now = std::chrono::high_resolution_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastEmitTime).count();

            if (elapsedMs >= EMIT_INTERVAL_MS)
            {
                // Cache the native-resolution raw bytes so the GUI thread can call
                // rawPixelAt() and see actual camera counts, not 8-bit display values.
                // This runs at most ~30 times/second (the display throttle rate), so
                // the copy cost is reasonable even for large sensors.
                // NOTE: pData is still valid here — RequeueBuffer hasn't been called yet.
                {
                    QMutexLocker lock(&m_rawMutex);
                    m_rawBytes = QByteArray(reinterpret_cast<const char*>(pData),
                                            static_cast<qsizetype>(dataSize));
                    m_rawWidth         = width;
                    m_rawHeight        = height;
                    m_rawBitsPerPixel  = bitsPerPixel;
                    m_rawPixelFormat   = pixelFormat;
                    m_rawDisplayWidth  = displayImage.width();
                    m_rawDisplayHeight = displayImage.height();
                }

                emit newFrame(displayImage);

                // Read ChunkLineStatusAll if the camera has chunk mode enabled.
                // The user configures chunk data via the Advanced dialog; we just attempt
                // the read and report available=true only when it actually succeeds.
                int64_t lineStatusAll = 0;
                bool lineStatusOk = false;
                try
                {
                    Arena::IChunkData* pChunkData = pImage->AsChunkData();
                    GenApi::CIntegerPtr pChunkLine(pChunkData->GetChunk("ChunkLineStatusAll"));
                    if (pChunkLine && GenApi::IsReadable(pChunkLine))
                    {
                        lineStatusAll = pChunkLine->GetValue();
                        lineStatusOk = true;
                    }
                }
                catch (...) {}
                emit lineStatusUpdated(lineStatusAll, lineStatusOk);

                lastEmitTime = now;
                displayFrameCount++;
            }

            // Once per second, report the true camera acquisition rate to the GUI.
            auto fpsDeltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastFpsReport).count();
            if (fpsDeltaMs >= FPS_REPORT_MS)
            {
                emit cameraFps(cameraFrameCount);
                cameraFrameCount = 0;
                lastFpsReport = now;
            }

            // Return the camera buffer immediately so it can be reused for the next frame.
            // This must happen for every GetImage call, whether or not we emit.
            m_pDevice->RequeueBuffer(pImage);
        }

        // =========================================================
        // STEP 5: Stop the stream
        // =========================================================

        m_pDevice->StopStream();
        emit statusMessage("Preview stopped");
    }
    catch (const GenICam::GenericException& e)
    {
        // GenICam exception — Arena SDK error (often a disconnect / timeout).
        // Do NOT call StopStream() here: if the device is disconnected, the Arena SDK
        // may throw an ACCESS_VIOLATION (Windows SEH) internally, which C++ catch(...)
        // cannot intercept and will crash the process.  The stream will be cleaned up
        // when CameraManager destroys the device.
        emit errorOccurred(QString("Arena SDK error: ") + QString::fromLatin1(e.what()));
    }
    catch (const std::exception& e)
    {
        // Standard C++ exception — same StopStream() risk as above.
        emit errorOccurred(QString("Error during preview: ") + QString::fromLatin1(e.what()));
    }
    catch (...)
    {
        // Catch any exception not derived from std::exception.
        emit errorOccurred("Unknown error occurred during preview");
    }
}


// =============================================================================
// rawPixelAt — return the actual camera count at a display-image pixel
// =============================================================================
//
// displayX / displayY are in the coordinate space of the QImage last emitted
// via newFrame().  If the worker downscaled for the display hint, we scale the
// coordinates back to native resolution before looking up the raw bytes.
//
// Pixel decoding mirrors the format branches in run():
//   8-bit          → 1 byte per pixel, value 0-255
//   16-bit         → 2 bytes per pixel (little-endian uint16), value 0-65535
//   10-bit Packed  → Mono10Packed: 3 bytes per 2 pixels, MSB-first, value 0-1023
//                    (checked before bitsPerPixel==12 because PFNC encodes 12 alloc bits)
//   12-bit         → Mono12p:      3 bytes per 2 pixels, LSB-first, value 0-4095
//   12-bit Packed  → Mono12Packed: 3 bytes per 2 pixels, LSB-first (Lucid Vision), value 0-4095
//   10-bit         → Mono10p:      5 bytes per 4 pixels, LSB-first, value 0-1023
// =============================================================================
uint32_t PreviewWorker::rawPixelAt(int displayX, int displayY) const
{
    QMutexLocker lock(&m_rawMutex);

    if (m_rawBytes.isEmpty())
        return 0;

    // Map from display-image coords to native-resolution coords.
    // If no downscaling occurred the ratio is exactly 1:1.
    const int nx = (m_rawDisplayWidth  > 0)
                   ? (displayX * m_rawWidth  / m_rawDisplayWidth)
                   : displayX;
    const int ny = (m_rawDisplayHeight > 0)
                   ? (displayY * m_rawHeight / m_rawDisplayHeight)
                   : displayY;

    if (nx < 0 || ny < 0 || nx >= m_rawWidth || ny >= m_rawHeight)
        return 0;

    const auto* data = reinterpret_cast<const uint8_t*>(m_rawBytes.constData());
    const int idx = ny * m_rawWidth + nx;

    // Mirror the run() dispatch logic: Mono10Packed has GetBitsPerPixel()==12 due to
    // PFNC ID encoding, so we must check the format name BEFORE checking bitsPerPixel==12.
    const bool rawIs10Packed = m_rawPixelFormat.find("10")     != std::string::npos
                            && m_rawPixelFormat.find("Packed") != std::string::npos;
    const bool rawIs12Packed = m_rawPixelFormat.find("12")     != std::string::npos
                            && m_rawPixelFormat.find("Packed") != std::string::npos;

    if (m_rawBitsPerPixel == 8)
    {
        return data[idx];
    }
    else if (m_rawBitsPerPixel == 16)
    {
        // Little-endian uint16 — same as what the camera sends on x86/x64
        const auto* p16 = reinterpret_cast<const uint16_t*>(data);
        return p16[idx];
    }
    else if (rawIs10Packed)
    {
        // Mono10Packed: Lucid Vision native layout
        //   p0 = (byte[0] << 2) | (byte[1] & 0x03)
        //   p1 = (byte[2] << 2) | (byte[1] >> 6)
        const int base = (idx / 2) * 3;
        if (idx % 2 == 0)
            return (static_cast<uint32_t>(data[base])     << 2) |  static_cast<uint32_t>(data[base + 1] & 0x03);
        else
            return (static_cast<uint32_t>(data[base + 2]) << 2) |  static_cast<uint32_t>(data[base + 1] >> 6);
    }
    else if (m_rawBitsPerPixel == 12 && !rawIs12Packed)
    {
        // Mono12p: LSB-first PFNC layout
        //   p0 = byte[0] | ((byte[1] & 0x0F) << 8)
        //   p1 = (byte[1] >> 4) | (byte[2] << 4)
        const int base = (idx / 2) * 3;
        if (idx % 2 == 0)
            return static_cast<uint32_t>(data[base])
                 | (static_cast<uint32_t>(data[base + 1] & 0x0F) << 8);
        else
            return static_cast<uint32_t>(data[base + 1] >> 4)
                 | (static_cast<uint32_t>(data[base + 2]) << 4);
    }
    else if (rawIs12Packed)
    {
        // Mono12Packed: Lucid Vision native layout
        //   p0 = (byte[0] << 4) | (byte[1] & 0x0F)
        //   p1 = (byte[2] << 4) | (byte[1] >> 4)
        const int base = (idx / 2) * 3;
        if (idx % 2 == 0)
            return (static_cast<uint32_t>(data[base])     << 4)
                 |  static_cast<uint32_t>(data[base + 1] & 0x0F);
        else
            return (static_cast<uint32_t>(data[base + 2]) << 4)
                 |  static_cast<uint32_t>(data[base + 1] >> 4);
    }
    else if (m_rawBitsPerPixel == 10)
    {
        // Mono10p: 4 pixels in 5 bytes (little-endian bit packing)
        const int base = (idx / 4) * 5;
        switch (idx % 4)
        {
            case 0: return  data[base]
                          | (static_cast<uint32_t>(data[base + 1] & 0x03) << 8);
            case 1: return (data[base + 1] >> 2)
                          | (static_cast<uint32_t>(data[base + 2] & 0x0F) << 6);
            case 2: return (data[base + 2] >> 4)
                          | (static_cast<uint32_t>(data[base + 3] & 0x3F) << 4);
            case 3: return (data[base + 3] >> 6)
                          | (static_cast<uint32_t>(data[base + 4]) << 2);
            default: return 0;
        }
    }

    // Unknown format — best effort: return the raw byte at the linear index
    return static_cast<uint32_t>(data[idx]);
}
