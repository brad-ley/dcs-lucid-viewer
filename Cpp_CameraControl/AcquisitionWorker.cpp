// =============================================================================
// AcquisitionWorker.cpp
// =============================================================================
//
// This is the implementation of the AcquisitionWorker background thread.
// The run() method here is the heart of the acquisition loop:
//   1. Configure the camera stream for best performance
//   2. Start the stream
//   3. Grab frames in a tight loop until stop is requested
//   4. Save each frame as a .raw file
//   5. Stop the stream and clean up
//
// ABOUT .raw FILES:
//   A .raw file is simply the raw pixel bytes from the camera, no header,
//   no compression. The data layout depends on the pixel format (e.g., Mono8
//   = 1 byte per pixel; BayerRG8 = 1 byte per pixel; RGB8Packed = 3 bytes/pixel).
//   To load .raw files later, you need to know width, height, and pixel format.
//   We save a metadata.txt file in the output folder at acquisition start.
// =============================================================================

#include "AcquisitionWorker.h"

// Arena SDK — needed here (not just forward-declared) because we call methods
#include "ArenaApi.h"

// Arena Save API — for writing TIFF files
#include "SaveApi.h"

// TiffWriter — minimal multi-page TIFF writer (used for TiffStack and field captures)
#include "TiffWriter.h"

// C++ standard library headers
#include <algorithm> // std::min
#include <chrono>    // std::chrono::steady_clock — for debug log timestamps
#include <bitset>    // std::bitset — for binary bitmask formatting in frame_data.csv
#include <cinttypes> // PRIu64 — printf format macro for uint64_t
#include <cmath>     // std::sqrt, std::abs, std::round
#include <fstream>   // std::ofstream — for writing files
#include <iomanip>   // std::setw, std::setfill — for zero-padded frame numbers
#include <sstream>   // std::ostringstream — for building strings
#include <string>
#include <thread>    // std::thread — for the writer thread

// Qt headers
#include <QDir>          // QDir — for creating directories and handling paths
#include <QDateTime>     // QDateTime — for timestamping the metadata file
#include <QFile>         // QFile — for writing camera_settings.json
#include <QJsonDocument> // QJsonDocument — for serializing camera settings
#include <QJsonObject>   // QJsonObject — for building the settings JSON tree
#include <QDebug>        // qDebug — thread-safe debug output to VS Output window

#include <functional>    // std::function — for recursive node-walk lambda


// =============================================================================
// snapshotNodeMap — walk a GenApi node map and return all readable values as JSON
// =============================================================================
//
// Mirrors the logic in AdvancedParamsDialog::collectAllNodeValues() but starts
// from the "Root" category so it needs no UI state. Called once per session to
// write camera_settings.json alongside the acquired data.
namespace {

void collectNodeValues(GenApi::CCategoryPtr pCat, QJsonObject& result)
{
    if (!pCat || !GenApi::IsReadable(pCat))
        return;

    std::function<void(GenApi::CCategoryPtr)> collect =
        [&](GenApi::CCategoryPtr pParent)
    {
        GenApi::FeatureList_t children;
        try { pParent->GetFeatures(children); } catch (...) { return; }

        for (GenApi::IValue* pVal : children)
        {
            try
            {
                GenApi::CCategoryPtr pSub(pVal);
                if (pSub) { collect(pSub); continue; }

                if (!GenApi::IsReadable(pVal))
                    continue;

                GenApi::INode* pNode = pVal->GetNode();
                QString name = QString::fromLatin1(pNode->GetName().c_str());

                // Skip Chunk* nodes — they are only valid while actively processing
                // a frame buffer; reading them from a static node map snapshot
                // causes ACCESS_VIOLATION inside GenApi (confirmed on ChunkLineStatusAll).
                if (name.startsWith("Chunk"))
                    continue;

                if (pNode->GetPrincipalInterfaceType() == GenApi::intfICommand)
                    continue;
                if (result.contains(name))
                    continue;

                switch (pNode->GetPrincipalInterfaceType())
                {
                    case GenApi::intfIFloat: {
                        GenApi::CFloatPtr p(pNode);
                        if (p) result[name] = p->GetValue();
                        break;
                    }
                    case GenApi::intfIInteger: {
                        GenApi::CIntegerPtr p(pNode);
                        if (p) result[name] = static_cast<double>(p->GetValue());
                        break;
                    }
                    case GenApi::intfIEnumeration: {
                        GenApi::CEnumerationPtr p(pNode);
                        if (p) result[name] = QString::fromLatin1(p->ToString().c_str());
                        break;
                    }
                    case GenApi::intfIBoolean: {
                        GenApi::CBooleanPtr p(pNode);
                        if (p) result[name] = static_cast<bool>(p->GetValue());
                        break;
                    }
                    case GenApi::intfIString: {
                        GenApi::CStringPtr p(pNode);
                        if (p) result[name] = QString::fromLatin1(p->GetValue().c_str());
                        break;
                    }
                    default: break;
                }
            } catch (...) {}
        }
    };

    collect(pCat);
}

QJsonObject snapshotNodeMap(GenApi::INodeMap* pMap)
{
    QJsonObject result;
    if (!pMap)
        return result;

    GenApi::CCategoryPtr pRoot(pMap->GetNode("Root"));
    collectNodeValues(pRoot, result);
    return result;
}

} // anonymous namespace


// =============================================================================
// Constructor
// =============================================================================
AcquisitionWorker::AcquisitionWorker(QObject* parent)
    : QThread(parent)
    , m_pDevice(nullptr)
    , m_stopRequested(false)
    , m_frameCount(0)
    , m_saveFormat(SaveFormat::RawSequence)
    , m_fieldType(FieldType::None)
    , m_rawFrameLimit(0)
    , m_acquisitionDone(false)
    , m_customSessionName()      // Empty = auto-generate timestamp name
    , m_customFieldName()        // Set only for FieldType::Custom
    , m_notes()                  // Empty = no notes
    , m_nodeMapSnapshotJson()    // Set by main thread via setNodeMapSnapshotJson() before start()
    , m_firstCameraFrameId(0)
    , m_lastCameraFrameId(0)
    , m_firstFrameSeen(false)
    , m_networkDropCount(0)
    , m_savedCount(0)
{
}


// =============================================================================
// Destructor
// =============================================================================
AcquisitionWorker::~AcquisitionWorker()
{
    // If the thread is still running when we're destroyed, ask it to stop
    // and wait for it to finish. Deleting a running thread is undefined behavior.
    requestStop();
    wait();  // wait() blocks until the thread's run() method returns
}


// =============================================================================
// setDevice
// =============================================================================
void AcquisitionWorker::setDevice(Arena::IDevice* device)
{
    m_pDevice = device;
}


// =============================================================================
// setOutputPath
// =============================================================================
void AcquisitionWorker::setOutputPath(const QString& path)
{
    m_outputPath = path;
}


// =============================================================================
// setSaveFormat
// =============================================================================
void AcquisitionWorker::setSaveFormat(SaveFormat format)
{
    // Store the desired save format (raw sequence, TIFF stack, or raw video).
    // This is called before start() to configure which output format to use.
    m_saveFormat = format;
}


// =============================================================================
// setFieldType
// =============================================================================
void AcquisitionWorker::setFieldType(FieldType fieldType)
{
    // FieldType::None  = normal acquisition; save frames per m_saveFormat.
    // WhiteField / DarkField = streaming Welford mean, saved as a single TIFF.
    m_fieldType = fieldType;
}


// =============================================================================
// setRawFrameLimit
// =============================================================================
void AcquisitionWorker::setRawFrameLimit(int n)
{
    m_rawFrameLimit = n;
}


// =============================================================================
// setCustomSessionName
// =============================================================================
void AcquisitionWorker::setCustomSessionName(const QString& name)
{
    m_customSessionName = name.trimmed();
}


// =============================================================================
// setCustomFieldName
// =============================================================================
void AcquisitionWorker::setCustomFieldName(const QString& name)
{
    m_customFieldName = name.trimmed();
}


// =============================================================================
// setNotes
// =============================================================================
void AcquisitionWorker::setNotes(const QString& notes)
{
    m_notes = notes;
}


// =============================================================================
// setCameraParamsJson
// =============================================================================
void AcquisitionWorker::setCameraParamsJson(const QString& paramsJson)
{
    // Store the JSON string of visible camera settings passed from MainWindow.
    // This JSON object will be embedded in metadata.json under "camera_settings"
    // and represents the camera configuration at the time acquisition started.
    m_cameraParamsJson = paramsJson;
}


// =============================================================================
// setNodeMapSnapshotJson / buildCameraSettingsJson
// =============================================================================
void AcquisitionWorker::setNodeMapSnapshotJson(const QString& json)
{
    m_nodeMapSnapshotJson = json;
}

QString AcquisitionWorker::buildCameraSettingsJson(Arena::IDevice* pDevice)
{
    if (!pDevice)
        return {};
    try
    {
        QJsonObject root;
        root["format_version"] = 1;
        root["saved_at"]       = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        root["nodes"]          = snapshotNodeMap(pDevice->GetNodeMap());
        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }
    catch (...) {}
    return {};
}


// =============================================================================
// sessionPath — returns the folder path used in the most recent acquisition run
// =============================================================================
QString AcquisitionWorker::sessionPath() const
{
    return m_sessionPath;
}


// =============================================================================
// requestStop
// =============================================================================
void AcquisitionWorker::requestStop()
{
    // Writing to an atomic<bool> is thread-safe. The run() loop checks this flag
    // each iteration and exits when it becomes true.
    m_stopRequested.store(true);
}


// Forward declaration — defined later in this file.
// The writer-thread lambda captures this by name, so it must be declared before run().
static std::string jsonEscape(const std::string& s);


// =============================================================================
// run  —  THE MAIN THREAD BODY
// =============================================================================
//
// Uses a producer-consumer pattern to decouple acquisition from disk I/O:
//
//   ACQUISITION LOOP (this thread):
//     GetImage → memcpy into FrameData → push to queue → RequeueBuffer → repeat
//     Never touches the disk. Camera buffers are returned in microseconds.
//
//   WRITER THREAD (std::thread launched below):
//     Sleeps until queue is non-empty → pops FrameData → writes to disk → repeat
//     Runs behind the camera; the queue absorbs bursts.
//
// WHY THIS MATTERS:
//   Without decoupling, each disk write blocks acquisition. Once the OS page cache
//   fills (~1-4 GB), write() stalls until the OS can flush to the SSD — causing
//   frame rate to drop from ~47 fps down to ~1 fps.
//   With this pattern, the acquisition loop is never blocked by disk I/O.
void AcquisitionWorker::run()
{

    m_stopRequested.store(false);
    m_acquisitionDone.store(false);
    m_frameCount        = 0;
    m_firstCameraFrameId = 0;
    m_lastCameraFrameId  = 0;
    m_firstFrameSeen     = false;
    m_networkDropCount   = 0;
    m_savedCount         = 0;

    // Discard any stale FrameData left over from a previous run that ended early
    // (e.g. writer exited on error without draining the queue).
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        while (!m_writeQueue.empty())
            m_writeQueue.pop();
    }

    if (m_pDevice == nullptr)
    {
        emit errorOccurred("No device set. Cannot start acquisition.");
        return;
    }

    // Ensure the base output directory exists
    QDir dir(m_outputPath);
    if (!dir.exists())
    {
        if (!dir.mkpath("."))
        {
            emit errorOccurred("Could not create output directory: " + m_outputPath);
            return;
        }
    }

    // Build the session folder path.
    //
    // Custom name: the MainWindow pre-resolved any conflict (sanitization,
    //   deduplication, user choice) before calling start(), so we use it as-is.
    // No custom name: auto-generate acq_YYYYMMDD_HHmmss (always unique).
    if (!m_customSessionName.isEmpty())
    {
        m_sessionPath = m_outputPath + QDir::separator() + m_customSessionName;
    }
    else
    {
        QString baseName = "acq_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
        m_sessionPath = m_outputPath + QDir::separator() + baseName;
    }

    if (!QDir().mkpath(m_sessionPath))
    {
        emit errorOccurred("Could not create session directory: " + m_sessionPath);
        return;
    }

    emit statusMessage("Session folder: " + m_sessionPath);

    // Write camera_settings.json — the JSON was built on the main thread before start()
    // via buildCameraSettingsJson() to avoid concurrent GenApi node-map access, which
    // is not thread-safe and caused an ACCESS_VIOLATION crash on the worker thread.
    if (!m_nodeMapSnapshotJson.isEmpty())
    {
        QString settingsPath = m_sessionPath + QDir::separator() + "camera_settings.json";
        QFile f(settingsPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(m_nodeMapSnapshotJson.toUtf8());
        else
            emit statusMessage("Warning: could not write camera_settings.json");
    }

    // These are declared OUTSIDE the try block so:
    //   (a) the catch blocks can reference them for cleanup, and
    //   (b) the writer thread lambda can capture them by reference safely
    //       (they outlive the thread because we join before run() returns).
    std::ofstream videoFile;
    int64_t     width        = 0;
    int64_t     height       = 0;
    int         bitsPerPixel = 8;
    std::string pixelFormat;

    // writerThread is default-constructed (not yet running).
    // We assign it inside the try block once setup succeeds.
    // In catch blocks we check joinable() before joining.
    std::thread writerThread;

    // Lambda that signals and joins the writer — called on both success and error paths.
    // C++ CONCEPT — auto + lambda:
    //   'auto' lets the compiler infer the type of the lambda (it has no written type).
    //   [&] captures all local variables by reference.
    auto joinWriter = [&]()
    {
        m_acquisitionDone.store(true);
        m_queueCV.notify_all();
        if (writerThread.joinable())
            writerThread.join();
    };

    try
    {

        // =========================================================
        // STEP 1: Configure the Transport Layer (stream) settings
        // =========================================================

        GenApi::INodeMap* pStreamNodeMap = m_pDevice->GetTLStreamNodeMap();

        // StreamAutoNegotiatePacketSize: finds the largest MTU the NIC supports.
        // Even with jumbo frames, we still set this so it falls back gracefully if needed.
        Arena::SetNodeValue<bool>(pStreamNodeMap, "StreamAutoNegotiatePacketSize", true);

        // StreamPacketResendEnable: request retransmission of lost UDP packets.
        Arena::SetNodeValue<bool>(pStreamNodeMap, "StreamPacketResendEnable", true);

        // OldestFirst: deliver frames in chronological order and queue them when the
        // host falls behind.  NewestOnly (the preview default) would silently discard
        // older frames during acquisition — exactly what we don't want.  Our
        // producer-consumer write queue handles the backpressure; the stream layer
        // should just keep all frames and hand them over in order.
        Arena::SetNodeValue<GenICam::gcstring>(
            pStreamNodeMap, "StreamBufferHandlingMode", "OldestFirst");

        // =========================================================
        // STEP 2: Configure the camera
        // =========================================================

        GenApi::INodeMap* pNodeMap = m_pDevice->GetNodeMap();

        // AcquisitionMode is intentionally NOT forced here.
        // The user sets it via the Advanced Parameters dialog (Continuous, SingleFrame,
        // MultiFrame, etc.).  Overriding it here would silently break multi-frame
        // and single-frame acquisitions every time Start is pressed.

        // GevSCPSPacketSize = 8192: use jumbo-frame-sized UDP packets.
        // Requires NIC MTU >= 9000 (set in Windows Device Manager → NIC → Advanced → Jumbo Packet).
        // 8192 bytes/packet vs default 1500 → ~5x fewer packets/frame → less CPU overhead.
        try
        {
            Arena::SetNodeValue<int64_t>(pNodeMap, "GevSCPSPacketSize", 8192);
            emit statusMessage("Packet size set to 8192 bytes (jumbo frames).");
        }
        catch (const GenICam::GenericException&)
        {
            emit statusMessage("Warning: could not set GevSCPSPacketSize to 8192. "
                               "Enable jumbo frames on your NIC for best 10GigE performance.");
        }

        // =========================================================
        // STEP 2b: Enable chunk data for per-frame embedded metadata
        // =========================================================
        //
        // Chunk mode tells the camera to append metadata to each image payload.
        // This guarantees the values (gain, exposure, GPIO states) describe the
        // exact frame they accompany — unlike reading from the node map after
        // GetImage(), which returns the camera's current live setting and can lag
        // by one frame when AutoGain or AutoExposure is running.
        //
        // Each chunk must be selected via ChunkSelector before enabling individually.
        // If a camera does not support a particular chunk, the inner try skips it silently.
        bool gainChunkAvailable       = false;
        bool exposureChunkAvailable   = false;
        bool lineStatusChunkAvailable = false;
        try
        {
            Arena::SetNodeValue<bool>(pNodeMap, "ChunkModeActive", true);

            auto tryEnableChunk = [&](const char* chunkName) -> bool
            {
                try
                {
                    Arena::SetNodeValue<GenICam::gcstring>(
                        pNodeMap, "ChunkSelector", chunkName);
                    Arena::SetNodeValue<bool>(pNodeMap, "ChunkEnable", true);
                    return true;
                }
                catch (...) { return false; }
            };

            gainChunkAvailable       = tryEnableChunk("Gain");
            exposureChunkAvailable   = tryEnableChunk("ExposureTime");
            lineStatusChunkAvailable = tryEnableChunk("LineStatusAll");

            emit statusMessage(
                QString("Chunk data enabled — Gain:%1  ExposureTime:%2  LineStatusAll:%3")
                    .arg(gainChunkAvailable       ? "yes" : "no")
                    .arg(exposureChunkAvailable   ? "yes" : "no")
                    .arg(lineStatusChunkAvailable ? "yes" : "no"));
        }
        catch (...)
        {
            emit statusMessage("Note: chunk mode not available on this camera — "
                               "gain/exposure will be read from the live node map.");
        }

        // =========================================================
        // STEP 3: Read camera metadata
        // =========================================================

        width  = Arena::GetNodeValue<int64_t>(pNodeMap, "Width");
        height = Arena::GetNodeValue<int64_t>(pNodeMap, "Height");
        GenICam::gcstring pfGcStr = Arena::GetNodeValue<GenICam::gcstring>(pNodeMap, "PixelFormat");
        pixelFormat = std::string(pfGcStr.c_str());

        // Heuristic bpp from format string — updated to exact value from first image in writer
        if      (pixelFormat.find("16")   != std::string::npos) bitsPerPixel = 16;
        else if (pixelFormat.find("12")   != std::string::npos) bitsPerPixel = 12;
        else if (pixelFormat.find("RGB8") != std::string::npos ||
                 pixelFormat.find("BGR8") != std::string::npos) bitsPerPixel = 24;

        emit statusMessage(
            QString("Stream config: %1 x %2  %3")
                .arg(width).arg(height).arg(QString::fromStdString(pixelFormat)));

        // =========================================================
        // STEP 4: Write initial metadata.json (complete=false)
        // =========================================================

        writeMetadataJson(width, height, pixelFormat, bitsPerPixel, 0, false);

        // =========================================================
        // STEP 5: Log save format
        // =========================================================

        QString formatString;
        if (m_fieldType == FieldType::WhiteField)
            formatString = "White Field capture (streaming Welford mean → white_field_mean.tiff)";
        else if (m_fieldType == FieldType::WhiteFieldPCA)
            formatString = "White Field multi-frame PCA capture (max(5s,100f) Welford + recording.raw → white_field_mean.tiff)";
        else if (m_fieldType == FieldType::WhiteFieldMaster)
            formatString = "White Field master capture (max(5s,100f) Welford + recording.raw → white_field_master_mean.tiff)";
        else if (m_fieldType == FieldType::DarkField)
            formatString = "Dark Field capture (streaming Welford mean → dark_field_mean.tiff)";
        else if (m_fieldType == FieldType::DotGrid)
            formatString = "Dot Grid capture (streaming Welford mean → dot_grid_mean.tiff)";
        else if (m_fieldType == FieldType::Ambient)
            formatString = "Ambient capture (streaming Welford mean → ambient_mean.tiff)";
        else if (m_fieldType == FieldType::Custom)
            formatString = QString("Custom capture (streaming Welford mean → %1_mean.tiff)")
                               .arg(m_customFieldName.isEmpty() ? "custom" : m_customFieldName);
        else
        {
            switch (m_saveFormat)
            {
                case SaveFormat::RawSequence: formatString = "Raw Sequence (.raw files)";       break;
                case SaveFormat::TiffStack:   formatString = "TIFF Stack (stack.tiff)";         break;
                case SaveFormat::RawVideo:    formatString = "Raw Video (single .raw file)";    break;
            }
        }
        emit statusMessage("Save format: " + formatString);

        // =========================================================
        // STEP 6: Launch the writer thread
        // =========================================================
        //
        // The lambda [&] captures everything in run()'s scope by reference.
        // It is safe because writerThread.join() is always called before run() returns,
        // guaranteeing those stack variables still exist while the thread runs.

        writerThread = std::thread([&]()
        {
            // C++ CONCEPT — std::thread and exceptions:
            //   If an exception escapes from a std::thread, C++ calls std::terminate()
            //   (instant crash, no cleanup). Unlike QThread::run(), there is no Qt
            //   machinery to catch it. We wrap the entire body in try/catch so that
            //   any unexpected error signals the GUI and stops acquisition cleanly.
            try
            {

            // Open frame_data.csv — one row per frame with the camera's hardware timestamp and gain reading.
            // The camera timestamp (nanoseconds from an internal counter) lets you compute
            // exact inter-frame intervals and detect dropped frames.
            // The gain reading (in dB) is a snapshot of the camera's gain setting when the frame was captured.
            std::ofstream timestampFile;
            {
                QString tsPath = m_sessionPath + QDir::separator() + "frame_data.csv";
                timestampFile.open(tsPath.toStdString());
                if (timestampFile.is_open())
                {
                    timestampFile << "frame_index,timestamp_ns,gain_db,exposure_us";
                    if (lineStatusChunkAvailable)
                        timestampFile << ",line_status_all";
                    timestampFile << "\n";
                }
                else
                {
                    emit statusMessage("Warning: could not open frame_data.csv for writing.");
                }
            }

            // Open the raw video file for normal RawVideo acquisitions, or for
            // PCA/Master field captures that also write raw frames alongside the mean.
            const bool isPCACapture = (m_fieldType == FieldType::WhiteFieldPCA ||
                                       m_fieldType == FieldType::WhiteFieldMaster);
            if ((m_saveFormat == SaveFormat::RawVideo && m_fieldType == FieldType::None)
                || (isPCACapture && m_rawFrameLimit > 0))
            {
                QString videoPath = m_sessionPath + QDir::separator() + "recording.raw";
                videoFile.open(videoPath.toStdString(), std::ios::binary);
                if (!videoFile.is_open())
                {
                    emit errorOccurred("Could not open recording.raw for writing.");
                    m_stopRequested.store(true);
                    return;
                }
            }

            // TiffStack writer — only used when m_saveFormat == TiffStack and
            // this is not a field capture.  Opened lazily on the first frame.
            TiffWriter tiffStackWriter;
            bool tiffStackReady = false;

            // ---------------------------------------------------------------
            // Welford streaming mean arrays — only allocated for field captures.
            //
            // WHY WELFORD'S ALGORITHM?
            //   Welford's online algorithm computes a running mean and variance
            //   in a single pass with no frame storage.  At each step n it updates:
            //     delta  = x - mean
            //     mean  += delta / n
            //     delta2 = x - mean   (post-update)
            //     M2    += delta * delta2
            //   From which: variance = M2 / n,  std = sqrt(M2 / n).
            //
            //   We run this over ALL pixels from ALL frames to get a robust
            //   variance estimate, then separately accumulate only inlier pixels
            //   (those whose Z-score |x - mean| / std is below a dynamic threshold).
            //
            // MEMORY:
            //   For a 5 MP sensor at 16-bit:
            //     wf_mean + wf_M2 + clean_sum  = 3 × 5M × 8 bytes = 120 MB
            //     clean_count                  = 1 × 5M × 4 bytes =  20 MB
            //   Total ≈ 140 MB — far less than accumulating frames.
            // ---------------------------------------------------------------
            const bool isFieldCapture = (m_fieldType != FieldType::None);
            const size_t nPixels = static_cast<size_t>(width * height);
            // Packed formats store fewer than 2 bytes per pixel and cannot be read
            // as uint16_t[] without a buffer overrun. Each needs its own unpack path.
            //   Mono12Packed / Mono12p : 3 bytes per 2 pixels (1.5 bytes/px)
            //   Mono10Packed / Mono10p : 5 bytes per 4 pixels (1.25 bytes/px)
            const bool isPacked12 = (pixelFormat == "Mono12Packed" || pixelFormat == "Mono12p");
            const bool isPacked10 = (pixelFormat == "Mono10Packed" || pixelFormat == "Mono10p");

            // uint32_t accumulator: the inner loop becomes pure integer addition with no
            // float conversion at all — the fastest possible accumulation.
            // Max sum per pixel: 65535 (16-bit max) × 10000 frames = 655,350,000
            // uint32_t max is ~4,294,967,295 so there is no overflow risk.
            //
            // Welford outlier-rejection is bypassed — direct sum averaging is used instead.
            // At 800+ frames the statistical benefit is negligible and the Welford loop is
            // ~8-10x more work per pixel. To re-enable, uncomment wf_mean/wf_M2/clean_count
            // below and swap back to the Welford inner loop further down.

            // -- Welford accumulators (bypassed, kept for easy re-enable) ----------------
            // std::vector<float>    wf_mean;     // Running Welford mean
            // std::vector<float>    wf_M2;       // Welford M2 (sum of squared deviations)
            // std::vector<uint32_t> clean_count; // Count of inlier contributions per pixel
            // ----------------------------------------------------------------------------

            std::vector<uint32_t> clean_sum;   // Integer sum of all pixel values across frames
            uint32_t wf_n = 0;                 // Frame count (same for every pixel)
            int wf_bitsPerPixel = 0;           // Set from the first frame
            double wf_gainMean     = 0.0;      // Running mean of per-frame gain (dB)
            double wf_exposureMean = 0.0;      // Running mean of per-frame exposure (µs)

            if (isFieldCapture)
            {
                // wf_mean.assign(nPixels, 0.0f);
                // wf_M2.assign(nPixels, 0.0f);
                // clean_count.assign(nPixels, 0u);
                clean_sum.assign(nPixels, 0u);
            }

            int framesWritten = 0;

            // Drain the queue until acquisition is done AND the queue is empty
            while (true)
            {
                FrameData frame;
                {
                    // C++ CONCEPT — unique_lock + condition_variable::wait():
                    //   unique_lock is like lock_guard but can be released mid-scope.
                    //   wait() atomically: releases the lock → sleeps → re-acquires on wake.
                    //   The predicate (lambda returning bool) prevents spurious wakeups:
                    //   the thread only proceeds when there is actually work to do.
                    std::unique_lock<std::mutex> lock(m_queueMutex);
                    m_queueCV.wait(lock, [this]
                    {
                        return !m_writeQueue.empty() || m_acquisitionDone.load();
                    });

                    if (m_writeQueue.empty())
                        break;  // Acquisition finished and queue is fully drained

                    // C++ CONCEPT — std::move:
                    //   Transfers the vector's heap memory to 'frame' without copying bytes.
                    //   After this, m_writeQueue.front() is left in a valid-but-empty state.
                    frame = std::move(m_writeQueue.front());
                    m_writeQueue.pop();
                }
                // Lock is released here — disk I/O happens without holding the queue lock,
                // so the acquisition loop can keep pushing new frames while we write.


                // Write per-frame metadata to frame_data.csv.
                // line_status_all column is only present when ChunkLineStatusAll was enabled.
                if (timestampFile.is_open())
                {
                    timestampFile << frame.index << ","
                                  << frame.timestampNs << ","
                                  << std::fixed << std::setprecision(4) << frame.gainDb << ","
                                  << std::fixed << std::setprecision(2) << frame.exposureUs;
                    if (lineStatusChunkAvailable)
                        timestampFile << "," << std::bitset<8>(frame.lineStatusAll);
                    timestampFile << "\n";
                }

                if (isFieldCapture)
                {
                    // -----------------------------------------------------------
                    // Welford streaming outlier-rejected mean
                    // -----------------------------------------------------------
                    //
                    // Per-pixel, per-frame update:
                    //   1. Update running mean and M2 for ALL pixels unconditionally
                    //      (so variance reflects the true spread including outliers).
                    //   2. Compute the dynamic Z-score threshold for this frame:
                    //        threshold = min(5.0,  0.95 * sqrt(n - 1))
                    //      At n=1: threshold = 0, and |x - mean_after_update| = 0 exactly,
                    //      so the first frame always passes the inlier check.
                    //   3. Pixel passes inlier check if |x - mean| <= threshold * std.
                    //      If so, add x to clean_sum and increment clean_count.
                    //
                    // Final result (computed after the drain loop):
                    //   pixel = clean_sum[i] / clean_count[i]
                    //   Fallback to wf_mean[i] if clean_count[i] == 0 (no inliers).
                    // -----------------------------------------------------------

                    wf_n++;
                    if (wf_bitsPerPixel == 0) wf_bitsPerPixel = frame.bitsPerPixel;

                    // For PCA/Master captures, also write raw frames to recording.raw
                    // until we've written m_rawFrameLimit frames.
                    if (isPCACapture && videoFile.is_open() && (int)wf_n <= m_rawFrameLimit)
                    {
                        videoFile.write(reinterpret_cast<const char*>(frame.bytes.data()),
                                        static_cast<std::streamsize>(frame.bytes.size()));
                    }

                    // Running mean of gain and exposure — incremental update avoids
                    // accumulating a large sum that could lose precision.
                    wf_gainMean    += (frame.gainDb    - wf_gainMean)    / wf_n;
                    wf_exposureMean += (frame.exposureUs - wf_exposureMean) / wf_n;

                    const bool is16bit = (wf_bitsPerPixel > 8);

                    // Accumulate this frame into clean_sum.
                    //
                    // The branch on is16bit lives OUTSIDE the loop so the compiler sees a
                    // simple pointer += loop in each branch — trivially vectorizable to AVX2
                    // (8× uint32 per instruction).  The uint16_t* reinterpret is safe on
                    // x86/x64 (little-endian, supports unaligned loads), and avoids the
                    // 2*i indexing that prevented auto-vectorization before.
                    const int nPix = static_cast<int>(nPixels);
                    if (isPacked12)
                    {
                        // Mono12Packed / Mono12p: 3 bytes per 2 pixels (1.5 bytes/px).
                        // Reading as uint16_t would exceed the buffer (nPix*2 bytes vs nPix*1.5).
                        //
                        // Two different bit layouts — must branch before the OMP loop:
                        //
                        // Mono12Packed (Lucid Vision, MSB-first):
                        //   b0       = pix0[11:4]  (high 8 bits)
                        //   b1[3:0]  = pix0[3:0]   (low  4 bits)
                        //   b1[7:4]  = pix1[3:0]
                        //   b2       = pix1[11:4]
                        //   p0 = (b0 << 4) | (b1 & 0x0F)
                        //   p1 = (b2 << 4) | (b1 >> 4)
                        //
                        // Mono12p (PFNC standard, LSB-first):
                        //   b0       = pix0[7:0]   (low  8 bits)
                        //   b1[3:0]  = pix0[11:8]  (high 4 bits)
                        //   b1[7:4]  = pix1[3:0]
                        //   b2       = pix1[11:4]
                        //   p0 = b0 | ((b1 & 0x0F) << 8)
                        //   p1 = (b1 >> 4) | (b2 << 4)
                        const bool isMono12Packed = (pixelFormat == "Mono12Packed");
                        const uint8_t* src = frame.bytes.data();
                        const int nPairs = nPix / 2;
                        if (isMono12Packed)
                        {
                            #pragma omp parallel for schedule(static)
                            for (int i = 0; i < nPairs; i++)
                            {
                                const uint8_t b0 = src[3 * i];
                                const uint8_t b1 = src[3 * i + 1];
                                const uint8_t b2 = src[3 * i + 2];
                                clean_sum[2 * i]     += (static_cast<uint32_t>(b0) << 4) | (b1 & 0x0Fu);
                                clean_sum[2 * i + 1] += (static_cast<uint32_t>(b2) << 4) | (b1 >> 4);
                            }
                            if (nPix & 1)  // odd pixel count (unusual but safe)
                            {
                                const uint8_t b0 = src[3 * (nPix / 2)];
                                const uint8_t b1 = src[3 * (nPix / 2) + 1];
                                clean_sum[nPix - 1] += (static_cast<uint32_t>(b0) << 4) | (b1 & 0x0Fu);
                            }
                        }
                        else  // Mono12p
                        {
                            #pragma omp parallel for schedule(static)
                            for (int i = 0; i < nPairs; i++)
                            {
                                const uint8_t b0 = src[3 * i];
                                const uint8_t b1 = src[3 * i + 1];
                                const uint8_t b2 = src[3 * i + 2];
                                clean_sum[2 * i]     += static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1 & 0x0Fu) << 8);
                                clean_sum[2 * i + 1] += static_cast<uint32_t>(b1 >> 4) | (static_cast<uint32_t>(b2) << 4);
                            }
                            if (nPix & 1)  // odd pixel count (unusual but safe)
                            {
                                const uint8_t b0 = src[3 * (nPix / 2)];
                                const uint8_t b1 = src[3 * (nPix / 2) + 1];
                                clean_sum[nPix - 1] += static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1 & 0x0Fu) << 8);
                            }
                        }
                    }
                    else if (isPacked10)
                    {
                        // Two different bit layouts — must branch before the OMP loop:
                        //
                        // Mono10Packed (Lucid Vision, MSB-first, 3 bytes per 2 pixels):
                        //   b0       = pix0[9:2]   (upper 8 bits)
                        //   b1[1:0]  = pix0[1:0]   (lower 2 bits of pix0)
                        //   b1[7:6]  = pix1[1:0]   (lower 2 bits of pix1)
                        //   b2       = pix1[9:2]   (upper 8 bits)
                        //   p0 = (b0 << 2) | (b1 & 0x03)
                        //   p1 = (b2 << 2) | (b1 >> 6)
                        //
                        // Mono10p (PFNC, LSB-first, 5 bytes per 4 pixels):
                        //   pix0 = b0 | ((b1 & 0x03) << 8)
                        //   pix1 = (b1 >> 2) | ((b2 & 0x0F) << 6)
                        //   pix2 = (b2 >> 4) | ((b3 & 0x3F) << 4)
                        //   pix3 = (b3 >> 6) | (b4 << 2)
                        const bool isMono10Packed = (pixelFormat == "Mono10Packed");
                        const uint8_t* src = frame.bytes.data();
                        if (isMono10Packed)
                        {
                            const int nPairs = nPix / 2;
                            #pragma omp parallel for schedule(static)
                            for (int i = 0; i < nPairs; i++)
                            {
                                const uint8_t b0 = src[3 * i];
                                const uint8_t b1 = src[3 * i + 1];
                                const uint8_t b2 = src[3 * i + 2];
                                clean_sum[2 * i]     += (static_cast<uint32_t>(b0) << 2) | (b1 & 0x03u);
                                clean_sum[2 * i + 1] += (static_cast<uint32_t>(b2) << 2) | (b1 >> 6);
                            }
                            if (nPix & 1)  // odd pixel count (unusual but safe)
                            {
                                const uint8_t b0 = src[3 * (nPix / 2)];
                                const uint8_t b1 = src[3 * (nPix / 2) + 1];
                                clean_sum[nPix - 1] += (static_cast<uint32_t>(b0) << 2) | (b1 & 0x03u);
                            }
                        }
                        else  // Mono10p: 5 bytes per 4 pixels, LSB-first
                        {
                            const int nGroups = nPix / 4;
                            #pragma omp parallel for schedule(static)
                            for (int i = 0; i < nGroups; i++)
                            {
                                const uint8_t b0 = src[5*i],   b1 = src[5*i+1], b2 = src[5*i+2],
                                              b3 = src[5*i+3], b4 = src[5*i+4];
                                clean_sum[4*i]   += static_cast<uint32_t>(b0)        | (static_cast<uint32_t>(b1 & 0x03u) << 8);
                                clean_sum[4*i+1] += static_cast<uint32_t>(b1 >> 2)   | (static_cast<uint32_t>(b2 & 0x0Fu) << 6);
                                clean_sum[4*i+2] += static_cast<uint32_t>(b2 >> 4)   | (static_cast<uint32_t>(b3 & 0x3Fu) << 4);
                                clean_sum[4*i+3] += static_cast<uint32_t>(b3 >> 6)   | (static_cast<uint32_t>(b4) << 2);
                            }
                            // Handle remaining 1-3 pixels (width*height rarely not a multiple of 4)
                            const int rem = nPix % 4;
                            if (rem > 0)
                            {
                                const int base = nGroups * 5;
                                const uint8_t b0 = src[base], b1 = src[base+1];
                                clean_sum[4*nGroups] += static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1 & 0x03u) << 8);
                                if (rem > 1)
                                {
                                    const uint8_t b2 = src[base+2];
                                    clean_sum[4*nGroups+1] += static_cast<uint32_t>(b1 >> 2) | (static_cast<uint32_t>(b2 & 0x0Fu) << 6);
                                }
                                if (rem > 2)
                                {
                                    const uint8_t b2 = src[base+2], b3 = src[base+3];
                                    clean_sum[4*nGroups+2] += static_cast<uint32_t>(b2 >> 4) | (static_cast<uint32_t>(b3 & 0x3Fu) << 4);
                                }
                            }
                        }
                    }
                    else if (is16bit)
                    {
                        const uint16_t* src = reinterpret_cast<const uint16_t*>(frame.bytes.data());
                        #pragma omp parallel for schedule(static)
                        for (int i = 0; i < nPix; i++)
                            clean_sum[i] += src[i];
                    }
                    else
                    {
                        const uint8_t* src = frame.bytes.data();
                        #pragma omp parallel for schedule(static)
                        for (int i = 0; i < nPix; i++)
                            clean_sum[i] += src[i];
                    }

                    // -- Welford inner loop (bypassed — uncomment to re-enable) -----------
                    // const float thresh    = static_cast<float>(std::min(5.0,
                    //     0.95 * std::sqrt(static_cast<double>(wf_n - 1))));
                    // const float thresh_sq = thresh * thresh;
                    // const float inv_n     = 1.0f / static_cast<float>(wf_n);
                    // #pragma omp parallel for schedule(static)
                    // for (int i = 0; i < nPix; i++)
                    // {
                    //     float x;
                    //     if (!is16bit)
                    //         x = static_cast<float>(frame.bytes[i]);
                    //     else
                    //         x = static_cast<float>(
                    //                 static_cast<uint16_t>(frame.bytes[2 * i]) |
                    //                (static_cast<uint16_t>(frame.bytes[2 * i + 1]) << 8));
                    //     float delta  = x - wf_mean[i];
                    //     wf_mean[i]  += delta * inv_n;
                    //     float delta2 = x - wf_mean[i];
                    //     wf_M2[i]    += delta * delta2;
                    //     const float diff  = x - wf_mean[i];
                    //     const bool inlier = (wf_n < 2)
                    //         || (diff * diff <= thresh_sq * (wf_M2[i] * inv_n));
                    //     if (inlier) { clean_sum[i] += x; clean_count[i] += 1; }
                    // }
                    // ---------------------------------------------------------------------

                    // Emit a live preview every 50 frames (frames 1, 51, 101, ...).
                    // Reduced from every 10: the preview build is 5M pixels single-threaded
                    // and was adding ~20-50ms per render, which dominated the per-frame cost.
                    // At 800 frames, every-50 means 16 renders instead of 80.
                    if (wf_n % 50 == 1)
                    {
                        // Single multiply replaces per-pixel divide: factor = 1/n * scale.
                        // Scale maps the pixel bit depth to 0..255: 8-bit→÷1, 12-bit→÷16, 16-bit→÷256.
                        const float depthScale = (wf_bitsPerPixel > 8)
                            ? (1.0f / static_cast<float>(1 << (wf_bitsPerPixel - 8)))
                            : 1.0f;
                        const float factor = (1.0f / static_cast<float>(wf_n)) * depthScale;

                        QImage previewImg(static_cast<int>(width),
                                          static_cast<int>(height),
                                          QImage::Format_Grayscale8);

                        // Flat loop + OpenMP: avoids scanLine call overhead per row and
                        // parallelizes the pixel conversion across cores.
                        // bytesPerLine() may be padded; for Grayscale8 Qt pads to 4-byte
                        // alignment so we must use scanLine for correct row addressing.
                        const int iHeight = static_cast<int>(height);
                        const int iWidth  = static_cast<int>(width);
                        #pragma omp parallel for schedule(static)
                        for (int row = 0; row < iHeight; row++)
                        {
                            uchar* line = previewImg.scanLine(row);
                            const int base = row * iWidth;
                            for (int col = 0; col < iWidth; col++)
                            {
                                float v = static_cast<float>(clean_sum[base + col]) * factor;
                                line[col] = static_cast<uchar>(
                                    v <= 0.0f ? 0 : v >= 255.0f ? 255 : static_cast<uchar>(v));
                            }
                        }
                        emit fieldPreviewReady(previewImg);
                    }

                    // Emit after each frame so the main window FPS counter updates.
                    // The "Frames saved" label is relabelled to "Frames averaged" by
                    // MainWindow during field captures, so this is not misleading.
                    emit framesSaved(wf_n);
                }
                else
                {
                    // Normal per-frame saving
                    std::string saveError;
                    bool saveSuccess = true;

                    switch (m_saveFormat)
                    {
                        case SaveFormat::RawSequence:
                        {
                            if (!saveRawFrame(frame.bytes.data(), frame.bytes.size(),
                                              frame.index, saveError))
                                saveSuccess = false;
                            break;
                        }

                        case SaveFormat::TiffStack:
                        {
                            if (!tiffStackReady)
                            {
                                QString tiffPath = m_sessionPath + QDir::separator() + "stack.tiff";
                                if (!tiffStackWriter.open(
                                        tiffPath.toStdString(),
                                        static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height),
                                        static_cast<uint16_t>(frame.bitsPerPixel)))
                                {
                                    saveError = "Could not open stack.tiff for writing: "
                                                + tiffPath.toStdString();
                                    saveSuccess = false;
                                    break;
                                }
                                tiffStackReady = true;
                                bitsPerPixel   = frame.bitsPerPixel;
                            }
                            char pageMeta[256];
                            std::snprintf(pageMeta, sizeof(pageMeta),
                                "{\"frame\":%u,"
                                "\"timestamp_ns\":%" PRIu64 ","
                                "\"gain_db\":%.4f,"
                                "\"exposure_us\":%.2f}",
                                frame.index,
                                frame.timestampNs,
                                frame.gainDb,
                                frame.exposureUs);

                            if (!tiffStackWriter.addPage(frame.bytes.data(),
                                                          frame.bytes.size(),
                                                          pageMeta))
                            {
                                saveError = "Failed to write TIFF page for frame "
                                          + std::to_string(frame.index);
                                saveSuccess = false;
                            }
                            break;
                        }

                        case SaveFormat::RawVideo:
                        {
                            videoFile.write(
                                reinterpret_cast<const char*>(frame.bytes.data()),
                                static_cast<std::streamsize>(frame.bytes.size()));
                            if (!videoFile.good())
                            {
                                saveError = "Failed to write to recording.raw";
                                saveSuccess = false;
                            }
                            break;
                        }
                    }

                    if (!saveSuccess)
                    {
                        emit errorOccurred(QString::fromStdString(saveError));
                        m_stopRequested.store(true);  // Tell acquisition loop to stop too
                        break;
                    }
                }

                // For field captures, frames are folded into the Welford mean —
                // nothing is written to disk yet.  The counter increments only for
                // normal per-frame saves so the UI and diagnostic stay accurate.
                if (!isFieldCapture)
                {
                    framesWritten++;
                    m_savedCount = framesWritten;
                    emit framesSaved(framesWritten);
                }
            }

            // -----------------------------------------------------------------------
            // Post-drain: close multi-page TIFF stack (if it was opened)
            // -----------------------------------------------------------------------
            if (tiffStackReady && tiffStackWriter.isOpen())
                tiffStackWriter.close();

            // -----------------------------------------------------------------------
            // Post-drain: finalize and save the field capture mean TIFF
            // -----------------------------------------------------------------------
            //
            // Now that all frames have been processed by Welford, compute:
            //   pixel[i] = clean_sum[i] / clean_count[i]   (inlier-only average)
            //   Fallback:  wf_mean[i]                       (if no inliers — rare)
            //
            // The result is written as a single-page TIFF.  JSON metadata is
            // embedded in TIFF tag 270 (ImageDescription) so the file is
            // self-documenting when opened in ImageJ, Python (tifffile), etc.
            if (isFieldCapture && wf_n > 0)
            {
                std::string fieldTypeStr;
                switch (m_fieldType) {
                    case FieldType::WhiteField:
                    case FieldType::WhiteFieldPCA:    fieldTypeStr = "white_field";        break;
                    case FieldType::WhiteFieldMaster: fieldTypeStr = "white_field_master"; break;
                    case FieldType::DarkField:        fieldTypeStr = "dark_field";         break;
                    case FieldType::DotGrid:          fieldTypeStr = "dot_grid";           break;
                    case FieldType::Ambient:          fieldTypeStr = "ambient";            break;
                    case FieldType::Custom:
                        fieldTypeStr = m_customFieldName.isEmpty()
                                           ? "custom"
                                           : m_customFieldName.toStdString();
                        break;
                    default: fieldTypeStr = "field"; break;
                }

                // Welford mode counted outlier pixels here — direct averaging has none.
                // Kept commented so it's easy to wire back up when re-enabling Welford.
                // int outlierPixels = 0;
                // for (size_t i = 0; i < nPixels; i++)
                //     if (clean_count[i] < wf_n) outlierPixels++;
                const int outlierPixels = 0;

                // Build the mean image byte array at the correct bit depth.
                // Any bit depth > 8 is stored as 16-bit little-endian (TIFF standard).
                const uint16_t outBps = (wf_bitsPerPixel > 8) ? 16 : 8;
                std::vector<uint8_t> meanImage;

                const double inv_wf_n = 1.0 / static_cast<double>(wf_n);
                if (outBps == 8)
                {
                    meanImage.resize(nPixels);
                    for (size_t i = 0; i < nPixels; i++)
                    {
                        double v = static_cast<double>(clean_sum[i]) * inv_wf_n;
                        // Welford fallback was: (clean_count[i] > 0) ? clean_sum[i]/clean_count[i] : wf_mean[i]
                        meanImage[i] = static_cast<uint8_t>(
                            std::min(std::max(std::round(v), 0.0), 255.0));
                    }
                }
                else
                {
                    // 16-bit output: two bytes per pixel, little-endian
                    meanImage.resize(nPixels * 2);
                    const double maxVal = static_cast<double>((1u << wf_bitsPerPixel) - 1u);
                    for (size_t i = 0; i < nPixels; i++)
                    {
                        double v = static_cast<double>(clean_sum[i]) * inv_wf_n;
                        // Welford fallback was: (clean_count[i] > 0) ? clean_sum[i]/clean_count[i] : wf_mean[i]
                        uint16_t pv = static_cast<uint16_t>(
                            std::min(std::max(std::round(v), 0.0), maxVal));
                        meanImage[2 * i]     = static_cast<uint8_t>( pv       & 0xFF);
                        meanImage[2 * i + 1] = static_cast<uint8_t>((pv >> 8) & 0xFF);
                    }
                }

                // Build JSON string for ImageDescription (TIFF tag 270).
                // This embeds capture metadata directly inside the TIFF so it is
                // self-documenting when opened in ImageJ, Python (tifffile), etc.
                const std::string notesEsc   = jsonEscape(m_notes.toStdString());
                const std::string paramsJson = m_cameraParamsJson.isEmpty()
                    ? "null"
                    : m_cameraParamsJson.toStdString();

                std::ostringstream descSS;
                descSS << "{"
                       << "\"field_type\":\""   << fieldTypeStr    << "\","
                       << "\"n_frames\":"        << wf_n            << ","
                       << "\"width\":"           << width           << ","
                       << "\"height\":"               << height          << ","
                       << "\"pixel_format\":\""       << pixelFormat                        << "\","
                       << "\"bits_per_pixel\":"       << wf_bitsPerPixel << ","
                       << "\"tiff_pixel_format\":\""  << (outBps == 8 ? "uint8" : "uint16") << "\","
                       << "\"tiff_bits_per_pixel\":" << outBps                               << ","
                       << "\"outlier_pixels\":"  << outlierPixels   << ","
                       << "\"gain_db_mean\":"     << std::fixed << std::setprecision(4)
                                                  << wf_gainMean     << ","
                       << "\"exposure_us_mean\":" << std::fixed << std::setprecision(2)
                                                  << wf_exposureMean << ","
                       << "\"notes\":\""         << notesEsc        << "\","
                       << "\"camera_settings\":" << paramsJson
                       << "}";

                // Write the single-page mean TIFF
                const std::string tiffName = fieldTypeStr + "_mean.tiff";
                const QString tiffPath = m_sessionPath + QDir::separator()
                                       + QString::fromStdString(tiffName);
                TiffWriter fieldTiff;
                if (fieldTiff.open(tiffPath.toStdString(),
                                   static_cast<uint32_t>(width),
                                   static_cast<uint32_t>(height),
                                   outBps, descSS.str()))
                {
                    fieldTiff.addPage(meanImage.data(), meanImage.size());
                    fieldTiff.close();
                    m_savedCount = 1;
                    emit framesSaved(1);  // 1 file saved (the mean TIFF)
                    emit statusMessage("Field capture mean saved: " + tiffPath);
                }
                else
                {
                    emit errorOccurred("Could not write field TIFF: " + tiffPath);
                }

                // Also save as .raw if the selected format is not TIFF.
                // Re-pack meanImage (decoded uint16 or uint8) back to the camera's
                // native byte layout so the .raw matches what the camera actually sends.
                if (m_saveFormat != SaveFormat::TiffStack)
                {
                    // Build a packed byte buffer that mirrors the camera's native format.
                    std::vector<uint8_t> meanRaw;

                    if (isPacked12 && pixelFormat == "Mono12Packed")
                    {
                        // Mono12Packed: 3 bytes per 2 pixels
                        // b0 = p0 >> 4
                        // b1 = (p0 & 0x0F) | ((p1 & 0x0F) << 4)
                        // b2 = p1 >> 4
                        meanRaw.reserve((nPixels / 2) * 3);
                        for (size_t i = 0; i + 1 < nPixels; i += 2)
                        {
                            const uint16_t p0 = static_cast<uint16_t>(meanImage[2*i]     | (meanImage[2*i+1]     << 8));
                            const uint16_t p1 = static_cast<uint16_t>(meanImage[2*i+2]   | (meanImage[2*i+3]     << 8));
                            meanRaw.push_back(static_cast<uint8_t>(p0 >> 4));
                            meanRaw.push_back(static_cast<uint8_t>((p0 & 0x0F) | ((p1 & 0x0F) << 4)));
                            meanRaw.push_back(static_cast<uint8_t>(p1 >> 4));
                        }
                    }
                    else if (isPacked12)  // Mono12p
                    {
                        // Mono12p: 3 bytes per 2 pixels
                        // b0 = p0 & 0xFF
                        // b1 = (p0 >> 8) | ((p1 & 0x0F) << 4)
                        // b2 = p1 >> 4
                        meanRaw.reserve((nPixels / 2) * 3);
                        for (size_t i = 0; i + 1 < nPixels; i += 2)
                        {
                            const uint16_t p0 = static_cast<uint16_t>(meanImage[2*i]     | (meanImage[2*i+1]     << 8));
                            const uint16_t p1 = static_cast<uint16_t>(meanImage[2*i+2]   | (meanImage[2*i+3]     << 8));
                            meanRaw.push_back(static_cast<uint8_t>(p0 & 0xFF));
                            meanRaw.push_back(static_cast<uint8_t>((p0 >> 8) | ((p1 & 0x0F) << 4)));
                            meanRaw.push_back(static_cast<uint8_t>(p1 >> 4));
                        }
                    }
                    else if (isPacked10 && pixelFormat == "Mono10Packed")
                    {
                        // Mono10Packed: 3 bytes per 2 pixels
                        // b0 = p0 >> 2
                        // b1 = (p0 & 0x03) | ((p1 & 0x03) << 6)
                        // b2 = p1 >> 2
                        meanRaw.reserve((nPixels / 2) * 3);
                        for (size_t i = 0; i + 1 < nPixels; i += 2)
                        {
                            const uint16_t p0 = static_cast<uint16_t>(meanImage[2*i]     | (meanImage[2*i+1]     << 8));
                            const uint16_t p1 = static_cast<uint16_t>(meanImage[2*i+2]   | (meanImage[2*i+3]     << 8));
                            meanRaw.push_back(static_cast<uint8_t>(p0 >> 2));
                            meanRaw.push_back(static_cast<uint8_t>((p0 & 0x03) | ((p1 & 0x03) << 6)));
                            meanRaw.push_back(static_cast<uint8_t>(p1 >> 2));
                        }
                    }
                    else if (isPacked10)  // Mono10p
                    {
                        // Mono10p: 5 bytes per 4 pixels
                        // b0 = p0 & 0xFF
                        // b1 = (p0 >> 8) | ((p1 & 0x3F) << 2)
                        // b2 = (p1 >> 6) | ((p2 & 0x0F) << 4)
                        // b3 = (p2 >> 4) | ((p3 & 0x03) << 6)
                        // b4 = p3 >> 2
                        meanRaw.reserve((nPixels / 4) * 5);
                        for (size_t i = 0; i + 3 < nPixels; i += 4)
                        {
                            const uint16_t p0 = static_cast<uint16_t>(meanImage[2*i]     | (meanImage[2*i+1]     << 8));
                            const uint16_t p1 = static_cast<uint16_t>(meanImage[2*i+2]   | (meanImage[2*i+3]     << 8));
                            const uint16_t p2 = static_cast<uint16_t>(meanImage[2*i+4]   | (meanImage[2*i+5]     << 8));
                            const uint16_t p3 = static_cast<uint16_t>(meanImage[2*i+6]   | (meanImage[2*i+7]     << 8));
                            meanRaw.push_back(static_cast<uint8_t>(p0 & 0xFF));
                            meanRaw.push_back(static_cast<uint8_t>((p0 >> 8) | ((p1 & 0x3F) << 2)));
                            meanRaw.push_back(static_cast<uint8_t>((p1 >> 6) | ((p2 & 0x0F) << 4)));
                            meanRaw.push_back(static_cast<uint8_t>((p2 >> 4) | ((p3 & 0x03) << 6)));
                            meanRaw.push_back(static_cast<uint8_t>(p3 >> 2));
                        }
                    }
                    else
                    {
                        // Non-packed (Mono8, Mono16): meanImage is already in native format.
                        meanRaw = meanImage;
                    }

                    const QString rawPath = m_sessionPath + QDir::separator()
                                          + QString::fromStdString(fieldTypeStr + "_mean.raw");
                    std::ofstream rawOut(rawPath.toStdString(), std::ios::binary);
                    if (rawOut.is_open())
                    {
                        rawOut.write(reinterpret_cast<const char*>(meanRaw.data()),
                                     static_cast<std::streamsize>(meanRaw.size()));
                        rawOut.close();
                        emit statusMessage("Field capture raw saved: " + rawPath);
                    }
                    else
                    {
                        emit errorOccurred("Could not write field raw: " + rawPath);
                    }
                }
            }

            if (timestampFile.is_open())
                timestampFile.close();
            if (videoFile.is_open())
                videoFile.close();

            } // end try
            catch (const std::exception& e)
            {
                // Catch any exception from disk I/O, TIFF writer, etc.
                // Without this catch, an uncaught exception in a std::thread
                // would call std::terminate() — an immediate crash with no log output.
                emit errorOccurred(QString("Writer thread error: ") + QString::fromLatin1(e.what()));
                m_stopRequested.store(true);
                if (videoFile.is_open()) videoFile.close();
            }
            catch (...)
            {
                // Catch anything not derived from std::exception (e.g., Arena SDK internals)
                emit errorOccurred("Writer thread: unknown error.");
                m_stopRequested.store(true);
                if (videoFile.is_open()) videoFile.close();
            }
        });


        // =========================================================
        // STEP 7: Start the stream
        // =========================================================

        // Pre-allocate 100 DMA buffers in the driver.  The Arena SDK default is 10.
        // With high-resolution sensors (e.g. 24 MP @ 20 fps = ~1 GB/s), 10 buffers
        // gives only ~500 ms of headroom before the driver must drop a frame.
        // 100 buffers gives ~5 seconds of burst headroom, which covers typical disk-
        // flush stalls without a frame being lost at the network/RDMA layer.
        // Stop any stream left open from a previous session that crashed without cleanup.
        // If no stream is active this throws (harmlessly caught); if one is active it clears
        // it so the StartStream call below doesn't fail with GC_ERR_RESOURCE_IN_USE.
        try { m_pDevice->StopStream(); } catch (...) {}
        m_pDevice->StartStream(100);
        emit statusMessage("Stream started (100 buffers pre-allocated). Acquiring frames...");

        // =========================================================
        // STEP 8: Acquisition loop — grab, copy, enqueue, return buffer
        // =========================================================
        //
        // This loop does NO disk I/O. Its only job is to copy pixels out of the
        // camera's DMA buffer as fast as possible and hand them to the writer.

        // ------------------------------------------------------------------
        // In external trigger mode the camera only fires when a hardware
        // pulse arrives, so GetImage() will time out on every poll until
        // the next trigger.  Use a short polling interval (500 ms) and
        // swallow timeout exceptions inside the loop so they are never
        // reported as errors.  The stop flag is checked each iteration,
        // so clicking Stop responds within ~500 ms.
        // ------------------------------------------------------------------
        bool isExternalTrigger = false;
        try
        {
            GenICam::gcstring trigMode =
                Arena::GetNodeValue<GenICam::gcstring>(pNodeMap, "TriggerMode");
            isExternalTrigger = (std::string(trigMode.c_str()) == "On");
        }
        catch (...) {}

        if (isExternalTrigger)
            emit statusMessage("External trigger mode");

        // Read whether we are in a finite-frame mode (SingleFrame or MultiFrame).
        // In these modes the camera stops transmitting after delivering its frame count,
        // so a GetImage() timeout at the end is normal end-of-acquisition — not an error.
        bool isContinuousMode = true;
        try
        {
            GenICam::gcstring acqMode =
                Arena::GetNodeValue<GenICam::gcstring>(pNodeMap, "AcquisitionMode");
            isContinuousMode = (std::string(acqMode.c_str()) == "Continuous");
        }
        catch (...) {}

        // For finite-frame modes, read how many frames the camera will deliver.
        //   SingleFrame  → always 1
        //   MultiFrame   → read AcquisitionFrameCount node
        // We compare against m_frameCount in the timeout handler: if we have received
        // at least as many frames as expected the timeout is a normal end-of-acquisition.
        // If we timed out before that, it is a genuine error and we rethrow.
        int64_t expectedFrameCount = 0;  // 0 = not applicable (continuous mode)
        if (!isContinuousMode)
        {
            try
            {
                GenICam::gcstring acqMode =
                    Arena::GetNodeValue<GenICam::gcstring>(pNodeMap, "AcquisitionMode");
                if (std::string(acqMode.c_str()) == "SingleFrame")
                {
                    expectedFrameCount = 1;
                }
                else  // MultiFrame
                {
                    expectedFrameCount =
                        Arena::GetNodeValue<int64_t>(pNodeMap, "AcquisitionFrameCount");
                }
            }
            catch (...) {}
        }

        const int timeoutMs = isExternalTrigger ? 500 : 2000;

        while (!m_stopRequested.load())
        {
            Arena::IImage* pImage = nullptr;
            try
            {
                pImage = m_pDevice->GetImage(timeoutMs);
            }
            catch (const GenICam::GenericException& ex)
            {
                const std::string what = ex.what();
                const bool isTimeout = (what.find("GC_ERR_TIMEOUT") != std::string::npos
                                     || what.find("TimeoutException")  != std::string::npos);

                if (isExternalTrigger && isTimeout)
                    continue;  // Timeout waiting for trigger pulse — not an error

                // In a finite-frame mode: if we received all expected frames, the camera
                // has simply stopped transmitting — treat the timeout as a normal exit.
                // If the timeout arrived before all frames were received, it is a real
                // problem (cable loss, camera reset, etc.) and we rethrow as an error.
                if (!isContinuousMode && isTimeout && m_frameCount >= expectedFrameCount)
                    break;

                throw;  // Genuine error — mid-acquisition timeout or continuous mode timeout
            }

            // GetImage() can return nullptr on timeout with some SDK versions.
            // Dereferencing a null pointer is undefined behavior (instant crash).
            if (pImage == nullptr)
            {
                continue;
            }

            if (pImage->IsIncomplete())
            {
                m_pDevice->RequeueBuffer(pImage);
                emit statusMessage("Warning: incomplete frame, skipping.");
                continue;
            }

            // ---- Frame-drop diagnostic: check hardware block counter ----
            //
            // pImage->GetFrameId() is the GigE Vision block ID — a hardware counter the
            // camera increments for every frame it transmits, regardless of whether the
            // host received it.  Gaps in consecutive IDs mean frames were dropped on the
            // network or RDMA path before they ever reached host RAM.
            uint64_t cameraId = pImage->GetFrameId();

            if (!m_firstFrameSeen)
            {
                m_firstCameraFrameId = cameraId;
                m_lastCameraFrameId  = cameraId;
                m_firstFrameSeen     = true;
                emit firstFrameAcquired();
            }
            else if (cameraId > m_lastCameraFrameId + 1)
            {
                // Gap detected — frames were lost between the camera and host RAM.
                uint64_t dropped = cameraId - (m_lastCameraFrameId + 1);
                m_networkDropCount += static_cast<int>(dropped);
                emit statusMessage(
                    QString("Warning: network drop — expected frame ID %1, got %2 (%3 frame(s) lost)")
                        .arg(m_lastCameraFrameId + 1)
                        .arg(cameraId)
                        .arg(dropped));
                m_lastCameraFrameId = cameraId;
            }
            else
            {
                m_lastCameraFrameId = cameraId;
            }

            // Copy pixel data and metadata into a FrameData for the writer thread.
            // vector::assign(begin, end) does a memcpy — about 1-2ms for 23 MB.
            const uint8_t* src = static_cast<const uint8_t*>(pImage->GetData());

            // Compute the pure image payload size from dimensions and bit depth.
            // GetSizeFilled() includes chunk data appended after the image, which
            // causes raw files to be 44 bytes too large — a viewer stacking frames
            // by exact frame size sees each subsequent frame shifted by 44/bytesPerPixel pixels.
            // For packed formats (Mono12Packed = 12 bpp, Mono10p = 10 bpp) the
            // formula (W × H × bpp + 7) / 8 gives the correct packed byte count.
            const size_t dataSize = (static_cast<size_t>(pImage->GetWidth())
                                   * static_cast<size_t>(pImage->GetHeight())
                                   * static_cast<size_t>(pImage->GetBitsPerPixel())
                                   + 7u) / 8u;

            FrameData frame;
            frame.index          = m_frameCount;
            frame.bitsPerPixel   = static_cast<int>(pImage->GetBitsPerPixel());
            frame.timestampNs    = pImage->GetTimestamp();  // Camera hardware clock (nanoseconds)
            frame.cameraFrameId  = cameraId;

            // Read per-frame metadata.
            // Chunk data (embedded in the image payload) is preferred because the values
            // are guaranteed to match this exact frame.  Falling back to a live node-map
            // read is only done when chunk mode is unavailable; those readings may lag by
            // one frame when AutoGain or AutoExposure is active.
            double gainDb = 0.0;
            if (gainChunkAvailable)
            {
                // Arena SDK chunk API: cast the image to IChunkData, then call GetChunk()
                // which returns a GenApi::INode* that we cast to a typed smart pointer.
                // There is no Arena::GetChunkValue<T>() free function in this SDK version.
                try
                {
                    Arena::IChunkData* pChunkData = pImage->AsChunkData();
                    GenApi::CFloatPtr pChunkGain(pChunkData->GetChunk("ChunkGain"));
                    if (pChunkGain && GenApi::IsReadable(pChunkGain))
                        gainDb = pChunkGain->GetValue();
                }
                catch (...) {}
            }
            else
            {
                try
                {
                    GenApi::INodeMap* pLiveMap = m_pDevice->GetNodeMap();
                    GenApi::CFloatPtr pGain(pLiveMap->GetNode("Gain"));
                    if (pGain && GenApi::IsReadable(pGain))
                        gainDb = pGain->GetValue();
                }
                catch (...) {}
            }
            frame.gainDb = gainDb;

            double exposureUs = 0.0;
            if (exposureChunkAvailable)
            {
                try
                {
                    Arena::IChunkData* pChunkData = pImage->AsChunkData();
                    GenApi::CFloatPtr pChunkExp(pChunkData->GetChunk("ChunkExposureTime"));
                    if (pChunkExp && GenApi::IsReadable(pChunkExp))
                        exposureUs = pChunkExp->GetValue();
                }
                catch (...) {}
            }
            else
            {
                try
                {
                    GenApi::INodeMap* pLiveMap = m_pDevice->GetNodeMap();
                    GenApi::CFloatPtr pExposure(pLiveMap->GetNode("ExposureTime"));
                    if (pExposure && GenApi::IsReadable(pExposure))
                        exposureUs = pExposure->GetValue();
                }
                catch (...) {}
            }
            frame.exposureUs = exposureUs;

            // GPIO line state bitmask — bit N corresponds to Line N.
            // Only populated when ChunkLineStatusAll was successfully enabled.
            int64_t lineStatusAll = 0;
            if (lineStatusChunkAvailable)
            {
                try
                {
                    Arena::IChunkData* pChunkData = pImage->AsChunkData();
                    GenApi::CIntegerPtr pChunkLine(pChunkData->GetChunk("ChunkLineStatusAll"));
                    if (pChunkLine && GenApi::IsReadable(pChunkLine))
                        lineStatusAll = pChunkLine->GetValue();
                }
                catch (...) {}
            }
            frame.lineStatusAll = lineStatusAll;

            frame.bytes.assign(src, src + dataSize);


            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_writeQueue.push(std::move(frame));
            }
            m_queueCV.notify_one();

            // Return the camera buffer IMMEDIATELY — before any disk I/O.
            // The camera can reuse this buffer for the very next frame.
            m_pDevice->RequeueBuffer(pImage);
            m_frameCount++;

            // In finite-frame modes (SingleFrame / MultiFrame): exit the loop as soon
            // as we have received the expected number of frames, without waiting for the
            // next GetImage() call to time out.  expectedFrameCount == 0 means continuous.
            if (expectedFrameCount > 0 && m_frameCount >= expectedFrameCount)
                break;
        }

        // =========================================================
        // STEP 9: Signal writer and wait for it to drain the queue
        // =========================================================

        emit statusMessage(
            QString("Acquisition stopped. %1 frames grabbed. Flushing writer...")
                .arg(m_frameCount));
        joinWriter();

        // If the user stopped immediately (zero frames captured), skip file output
        // and clean up the empty session folder to avoid clutter.
        if (m_frameCount == 0 && m_fieldType == FieldType::None)
        {
            emit statusMessage("Acquisition stopped: no frames captured.");
            QDir(m_sessionPath).removeRecursively();
            m_pDevice->StopStream();
            return;
        }

        // ---- Frame-drop diagnostic summary ----
        //
        // After joinWriter() the writer thread has fully exited, so m_savedCount
        // is stable and there is no data race reading it here.
        //
        // Three numbers together pinpoint where frames were lost:
        //   cameraProduced  = last frame ID - first frame ID + 1
        //     (every frame the sensor transmitted, including ones never received)
        //   m_frameCount    = frames received into RAM successfully
        //   m_savedCount    = frames the writer thread wrote to disk
        uint64_t cameraProduced = m_firstFrameSeen
            ? (m_lastCameraFrameId - m_firstCameraFrameId + 1)
            : 0;

        QString summary = QString(
            "--- Frame Diagnostic ---\n"
            "  Camera produced (hardware IDs %1 to %2): %3\n"
            "  Received into RAM:                       %4\n"
            "  Saved to storage:                        %5\n"
            "  Network-level drops:                     %6")
            .arg(m_firstCameraFrameId)
            .arg(m_lastCameraFrameId)
            .arg(cameraProduced)
            .arg(m_frameCount)
            .arg(m_savedCount)
            .arg(m_networkDropCount);

        emit statusMessage(summary);

        // =========================================================
        // STEP 10: Write final metadata.json and stop stream
        // =========================================================

        writeMetadataJson(width, height, pixelFormat, bitsPerPixel, m_frameCount, true);
        emit statusMessage("Stopping stream...");
        m_pDevice->StopStream();
        emit statusMessage("Stream stopped.");
    }
    catch (const GenICam::GenericException& e)
    {
        joinWriter();  // Always drain and join before cleanup
        const std::string what = e.what();
        const bool isDisconnect = (what.find("GC_ERR_TIMEOUT")      != std::string::npos
                                || what.find("TimeoutException")     != std::string::npos
                                || what.find("GC_ERR_NOT_CONNECTED") != std::string::npos
                                || what.find("disconnected")         != std::string::npos
                                || what.find("DeviceLost")           != std::string::npos);
        if (isDisconnect)
            emit errorOccurred("Camera disconnected or timed out — acquisition stopped.");
        else
            emit errorOccurred(QString("Arena SDK error: ") + QString::fromLatin1(e.what()));
        if (videoFile.is_open()) videoFile.close();
        // Do NOT call StopStream() here: on a disconnected device the Arena SDK may
        // raise an ACCESS_VIOLATION (Windows SEH) internally.  C++ catch(...) cannot
        // intercept SEH exceptions, so the call would crash the process.
        // The stream is cleaned up when CameraManager destroys the device.
        if (!isDisconnect)
        {
            try { m_pDevice->StopStream(); } catch (...) {}
        }
    }
    catch (const std::exception& e)
    {
        joinWriter();
        emit errorOccurred(QString("Error during acquisition: ") + QString::fromLatin1(e.what()));
        if (videoFile.is_open()) videoFile.close();
        // Same StopStream() risk as above — skip to avoid a potential crash.
    }
    catch (...)
    {
        joinWriter();
        emit errorOccurred("Acquisition failed: unknown exception (non-standard type).");
        if (videoFile.is_open()) videoFile.close();
        // Same StopStream() risk as above — skip to avoid a potential crash.
    }
}


// =============================================================================
// saveRawFrame — write one frame's pixel data to a .raw file
// =============================================================================
bool AcquisitionWorker::saveRawFrame(const void* pData, size_t dataSize,
                                     int frameIndex, std::string& errorMsg)
{
    // Build the output file path.
    // Format: frame_000001.raw, frame_000002.raw, etc.
    //
    // C++ CONCEPT — std::ostringstream:
    //   Acts like a string you can "print" into using << operators.
    //   std::setw(6) sets field width to 6 characters.
    //   std::setfill('0') fills empty space with '0'.
    //   Together they produce zero-padded numbers: 000001, 000002, ...
    std::ostringstream ss;
    ss << "frame_"
       << std::setw(6) << std::setfill('0') << frameIndex
       << ".raw";

    // Combine output path and filename.
    // QDir::separator() returns '/' on Unix, '\' on Windows.
    QString filePath = m_sessionPath + QDir::separator() + QString::fromStdString(ss.str());

    // Open file for binary writing.
    // std::ios::binary is important — without it, on Windows, '\n' bytes
    // would be translated to "\r\n", corrupting the raw pixel data.
    std::ofstream file(filePath.toStdString(), std::ios::binary);
    if (!file.is_open())
    {
        errorMsg = "Could not open file for writing: " + filePath.toStdString();
        return false;
    }

    // Write all pixel bytes in one call.
    // pData is a void*, which we cast to char* because write() expects char*.
    // dataSize is the number of bytes to write.
    file.write(static_cast<const char*>(pData), static_cast<std::streamsize>(dataSize));

    if (!file.good())
    {
        errorMsg = "Failed to write frame data to: " + filePath.toStdString();
        return false;
    }

    // file is automatically closed when it goes out of scope (RAII pattern).
    return true;
}


// =============================================================================
// jsonEscape — escape a string for safe embedding in a JSON value
// =============================================================================
//
// JSON strings must escape backslash, double-quote, and the C0 control chars.
// We handle the ones most likely to appear in user-typed notes.
static std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // ASCII control characters below 0x20 must be \uXXXX-encoded
                if (c < 0x20)
                {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}


// =============================================================================
// writeMetadataJson — write a JSON metadata file
// =============================================================================
//
// Writes a JSON file describing the acquisition session.
// Called at the start (complete=false) and end (complete=true) of acquisition.
//
// Example JSON output:
// {
//   "acquisition_time": "2026-06-03T14:30:00",
//   "save_format": "tiff_sequence",
//   "width": 1920,
//   "height": 1080,
//   "pixel_format": "BayerRG8",
//   "bits_per_pixel": 8,
//   "tiff_pixel_format": "uint16",
//   "tiff_bits_per_pixel": 12,
//   "bytes_per_frame": 2073600,
//   "frame_count": 1234,
//   "complete": true
// }
//
// Python loading example:
//   import json
//   with open('metadata.json') as f:
//       meta = json.load(f)
//   print(f"Frames: {meta['frame_count']}, Format: {meta['pixel_format']}")
void AcquisitionWorker::writeMetadataJson(int64_t width, int64_t height,
                                          const std::string& pixelFormat,
                                          int bitsPerPixel, int frameCount, bool complete)
{
    // Build the output path for metadata.json inside the session folder
    QString metaPath = m_sessionPath + QDir::separator() + "metadata.json";
    std::ofstream meta(metaPath.toStdString());

    if (!meta.is_open())
    {
        emit statusMessage("Warning: could not write metadata.json");
        return;
    }

    // Get the current timestamp in ISO format (e.g., "2026-06-03T14:30:00")
    QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);

    // Determine the save format string.
    // Field captures override the normal save format.
    std::string formatString;
    if (m_fieldType == FieldType::WhiteField)
    {
        formatString = "white_field";
    }
    else if (m_fieldType == FieldType::DarkField)
    {
        formatString = "dark_field";
    }
    else if (m_fieldType == FieldType::DotGrid)
    {
        formatString = "dot_grid";
    }
    else if (m_fieldType == FieldType::Ambient)
    {
        formatString = "ambient";
    }
    else if (m_fieldType == FieldType::Custom)
    {
        formatString = m_customFieldName.isEmpty() ? "custom"
                                                   : m_customFieldName.toStdString();
    }
    else
    {
        switch (m_saveFormat)
        {
            case SaveFormat::RawSequence:
                formatString = "raw_sequence";
                break;
            case SaveFormat::TiffStack:
                formatString = "tiff_stack";
                break;
            case SaveFormat::RawVideo:
                formatString = "raw_video";
                break;
        }
    }

    // Calculate the actual number of bytes saved per frame.
    // Non-packed formats (Mono8, Mono12, Mono16) pad each pixel to a whole byte boundary,
    // so bytes/pixel = ceil(bpp/8).  Packed formats (Mono12p, Mono12Packed, Mono10p,
    // Mono10Packed) pack multiple pixels into each byte with no padding, so the frame size
    // is exactly (bpp * W * H) / 8 — always an integer because the camera guarantees
    // pixel counts are multiples of the packing group size.
    bool isPacked = (pixelFormat.find("Packed") != std::string::npos) ||
                    (!pixelFormat.empty() && pixelFormat.back() == 'p');
    int64_t bytesPerFrame;
    if (isPacked)
        bytesPerFrame = (static_cast<int64_t>(bitsPerPixel) * width * height) / 8;
    else
        bytesPerFrame = width * height * ((bitsPerPixel + 7) / 8);

    // TIFF-decoded depth: packed formats unpack to uint16; 8-bit stays uint8.
    const int  tiffBps    = (bitsPerPixel > 8) ? 16 : 8;
    const char* tiffFmt   = (tiffBps == 8) ? "uint8" : "uint16";

    // Write JSON manually (no external library needed).
    // C++ CONCEPT — std::ofstream:
    //   Using << to write to a file is like using std::cout to write to the terminal.
    // Extract just the folder name (e.g., "acq_20260603_143022") for the JSON field
    QString sessionName = QDir(m_sessionPath).dirName();

    // Notes: write even if empty so the field is always present and parseable
    std::string notesEscaped = jsonEscape(m_notes.toStdString());

    meta << "{\n";
    meta << "  \"session\": \""          << sessionName.toStdString() << "\",\n";
    meta << "  \"acquisition_time\": \"" << timestamp.toStdString()   << "\",\n";
    meta << "  \"notes\": \""            << notesEscaped               << "\",\n";
    meta << "  \"save_format\": \""      << formatString               << "\",\n";
    meta << "  \"width\": "              << width                      << ",\n";
    meta << "  \"height\": "             << height                     << ",\n";
    meta << "  \"pixel_format\": \""      << pixelFormat                << "\",\n";
    meta << "  \"bits_per_pixel\": "     << bitsPerPixel               << ",\n";
    meta << "  \"tiff_pixel_format\": \""<< tiffFmt                    << "\",\n";
    meta << "  \"tiff_bits_per_pixel\": "<< tiffBps                    << ",\n";
    meta << "  \"bytes_per_frame\": "    << bytesPerFrame              << ",\n";
    meta << "  \"frame_count\": "        << frameCount                 << ",\n";
    meta << "  \"complete\": "           << (complete ? "true" : "false");
    // If we have camera_settings JSON from MainWindow, write it after the complete field
    if (!m_cameraParamsJson.isEmpty())
        meta << ",\n  \"camera_settings\": " << m_cameraParamsJson.toStdString();
    meta << "\n}\n";

    meta.close();

    // Log a message indicating which write this was
    if (complete)
    {
        emit statusMessage("Final metadata saved to metadata.json");
    }
    else
    {
        emit statusMessage("Initial metadata saved to metadata.json");
    }
}
