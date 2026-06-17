// =============================================================================
// AcquisitionWorker.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares the AcquisitionWorker class — the background thread that grabs
//   frames from the camera and saves them as .raw files to disk.
//
// WHY A SEPARATE THREAD?
//   Image acquisition involves waiting for frames from the camera (blocking I/O)
//   and writing large files to disk. If we did this on the main thread (the one
//   running the GUI), the entire interface would freeze while waiting.
//
//   By running acquisition on a separate thread:
//     - The GUI stays responsive (user can click Stop, see frame counts, etc.)
//     - Acquisition runs as fast as the camera allows
//     - File I/O doesn't block the camera from grabbing the next frame
//
// C++ CONCEPT — QThread:
//   Qt provides QThread as a way to run code concurrently.
//   To use it, you subclass QThread and override the run() method.
//   When you call start() on the object, Qt creates an OS thread and calls run()
//   on that thread. When run() returns, the thread ends.
//
// C++ CONCEPT — signals and slots:
//   Qt's signal/slot system is a safe way for objects on different threads
//   to communicate. The worker emits signals, the GUI connects slots to them.
//   Qt automatically routes the call across the thread boundary safely.
//   For this to work, the class MUST have the Q_OBJECT macro.
// =============================================================================

#pragma once

// Qt threading
#include <QThread>   // Base class — provides thread management
#include <QImage>    // QImage — used for the live field-capture preview signal
#include <QString>   // Qt's Unicode string type

// C++ standard library
#include <atomic>              // std::atomic<bool/int> — thread-safe primitives
#include <condition_variable>  // std::condition_variable — thread wake/sleep signaling
#include <cstdint>             // uint64_t
#include <mutex>               // std::mutex, std::lock_guard, std::unique_lock
#include <queue>               // std::queue — FIFO container
#include <string>
#include <vector>              // std::vector<uint8_t> — heap byte array

// Arena Save API — for saving images as TIFF files
#include "SaveApi.h"

// Forward declaration: tell the compiler "Arena::IDevice is a class that exists"
// without including the full ArenaApi.h header here. This speeds up compilation
// because files that only include AcquisitionWorker.h don't need all of Arena SDK.
// (We still include ArenaApi.h in the .cpp file where we actually USE the class.)
namespace Arena { class IDevice; }


// =============================================================================
// AcquisitionWorker class
// =============================================================================
class AcquisitionWorker : public QThread
{
    // Q_OBJECT is a macro that Qt requires for any class that uses signals or slots.
    // It tells Qt's "meta-object compiler" (moc) to generate extra code for
    // the signal/slot machinery, introspection, and more.
    // Rule: if your class has Q_OBJECT, the header MUST be processed by moc.
    // (CMake's AUTOMOC setting handles this automatically.)
    Q_OBJECT

public:
    // ==========================================================================
    // FrameData — one frame's worth of data passed from acquisition to writer
    // ==========================================================================
    //
    // C++ CONCEPT — struct:
    //   A struct is just a class where members are public by default.
    //   We use it here as a plain data bundle (no methods, no invariants).
    //   Declaring it inside AcquisitionWorker scopes its name, just like SaveFormat.
    struct FrameData
    {
        int      index;          // 0-based frame number
        int      bitsPerPixel;   // From pImage->GetBitsPerPixel() — needed for TIFF init
        uint64_t timestampNs;    // Camera hardware clock in nanoseconds (pImage->GetTimestamp())
        uint64_t cameraFrameId;  // Hardware block counter from pImage->GetFrameId() — used for
                                 // drop detection: gaps between consecutive IDs mean frames were
                                 // lost at the network/RDMA level before reaching host RAM.
        double   gainDb;         // Camera gain (dB) for this exact frame — from chunk data if available, else live node map
        double   exposureUs;     // Exposure time (µs) for this exact frame — from chunk data if available, else live node map
        int64_t  lineStatusAll;  // GPIO line state bitmask from ChunkLineStatusAll (bit N = Line N); 0 if chunk unavailable
        std::vector<uint8_t> bytes;  // Copy of the raw pixel data
    };

    // ==========================================================================
    // SaveFormat — nested enum class
    // ==========================================================================
    //
    // An 'enum class' (scoped enumeration) is a set of named constants.
    // Declaring it *inside* the class means you refer to it as:
    //   AcquisitionWorker::SaveFormat::TiffStack
    // This keeps the name out of the global namespace and makes it clear
    // which class owns this concept.
    enum class SaveFormat
    {
        RawSequence,  // One .raw file per frame (frame_000000.raw, ...)
        TiffStack,    // All frames in a single multi-page stack.tiff
        RawVideo      // One .raw file total (all frames concatenated sequentially)
    };

    // FieldType controls whether this run is a normal acquisition or a white/dark
    // field calibration capture.  Field captures use streaming Welford outlier-
    // rejected averaging and save a single-page mean TIFF instead of per-frame files.
    enum class FieldType
    {
        None,        // Normal acquisition — save frames per the SaveFormat
        WhiteField,  // Capture bright reference — saves white_field_mean.tiff
        DarkField,   // Capture dark reference  — saves dark_field_mean.tiff
        DotGrid,     // Capture dot-grid registration image — saves dot_grid_mean.tiff
        Ambient,     // Capture ambient reference — saves ambient_mean.tiff
        Custom       // Capture with user-defined base name — saves {name}_mean.tiff
    };

    // Constructor.
    // 'explicit' prevents C++ from silently converting other types to this class.
    // 'QObject* parent = nullptr' follows Qt's object tree pattern:
    //   every QObject can have a parent that owns it and deletes it automatically.
    explicit AcquisitionWorker(QObject* parent = nullptr);

    // Destructor. Declared virtual because QThread's destructor is virtual.
    // This ensures the right destructor is called when deleting through a base pointer.
    ~AcquisitionWorker() override;

    // --- Configuration (call BEFORE start()) ---

    // Set the camera device to use for acquisition.
    // The worker does NOT take ownership — the CameraManager still owns m_pDevice.
    void setDevice(Arena::IDevice* device);

    // Set the output directory where frames will be written.
    // e.g. "C:/Users/brad/captures/"
    void setOutputPath(const QString& path);

    // Set the save format (raw sequence, TIFF stack, or raw video).
    // Must be called BEFORE start().
    void setSaveFormat(SaveFormat format);

    // Set the field type (None = normal acquisition; field types activate
    // streaming Welford mean computation instead of per-frame file saving).
    // Must be called BEFORE start().
    void setFieldType(FieldType ft);

    // For FieldType::Custom — the base name used for the output TIFF and metadata.
    // E.g. "dot_grid" → dot_grid_mean.tiff.  Has no effect for other FieldTypes.
    // Must be called BEFORE start().
    void setCustomFieldName(const QString& name);

    // Optional custom name for the session subfolder (e.g. "calibration_run1").
    // If empty or not set, the folder name defaults to acq_YYYYMMDD_HHmmss.
    // If the chosen name already exists, a counter suffix is appended (_2, _3, ...).
    void setCustomSessionName(const QString& name);

    // Free-form notes to embed in metadata.json (sample ID, conditions, etc.).
    // May contain newlines; they are JSON-escaped before writing.
    void setNotes(const QString& notes);

    // Store a JSON string of the current camera settings (from the main window's visible params).
    // Written into metadata.json under "camera_settings" at acquisition start.
    // The JSON string is typically a compact object like:
    //   {"PixelFormat":"Mono8","ExposureTime":5000.0,"Gain":10.0,...}
    void setCameraParamsJson(const QString& paramsJson);

    // Pass a pre-built full-node-map JSON string (from buildCameraSettingsJson()) to
    // be written as camera_settings.json in the session folder.  The worker writes the
    // string to disk but never touches the GenApi node map itself — that avoids a
    // concurrent-access crash when the GUI thread also reads the node map.
    void setNodeMapSnapshotJson(const QString& json);

    // Snapshot ALL readable GenApi nodes from the device into a camera_settings JSON string.
    // MUST be called on the main/GUI thread before start() — GenApi node maps are not
    // thread-safe and walking them from the worker thread while the GUI reads the same
    // map causes an ACCESS_VIOLATION crash inside the SDK.
    static QString buildCameraSettingsJson(Arena::IDevice* pDevice);

    // --- Control ---

    // Request the thread to stop capturing.
    // This sets a flag that the run() loop checks. The thread will finish its
    // current frame and then exit cleanly.
    // IMPORTANT: Call wait() after requestStop() to block until the thread finishes.
    void requestStop();

    // Returns the full path of the session folder used in the most recent run().
    // Safe to call from the main thread after the 'finished' signal is received —
    // the worker thread has already exited by then, so there is no data race.
    QString sessionPath() const;

signals:
    // C++ CONCEPT — signals:
    //   "signals:" is a Qt keyword (like "public:" but for signals).
    //   Signals are declared like functions but never implemented by you —
    //   Qt generates the implementation automatically via moc.
    //   To send a signal, call emit signalName(arguments).

    // Emitted once when the very first frame is received from the camera.
    // Used to start the auto-stop timer from the moment data starts arriving
    // rather than from when the acquisition button is pressed.
    void firstFrameAcquired();

    // Emitted each time a frame is successfully saved to disk.
    // 'count' is the total number of frames saved so far.
    void framesSaved(int count);

    // Emitted when an error occurs during acquisition (e.g., camera disconnected,
    // disk full, timeout). Acquisition stops after emitting this.
    void errorOccurred(const QString& message);

    // Emitted for informational log messages (e.g., "Stream started", "Saving to C:\...")
    void statusMessage(const QString& message);

    // Emitted during field captures (white/dark) every ~10 frames with the current
    // running-mean image so the preview window can show it live.
    // The QImage is always 8-bit grayscale (16-bit means are scaled down by 256).
    void fieldPreviewReady(QImage image);

protected:
    // run() is the method that executes on the worker thread.
    // Override it from QThread to define what the thread does.
    // It's 'protected' so external code can't call it directly —
    // call start() instead (which sets up the OS thread, then calls run()).
    // The 'override' keyword makes the compiler verify we're actually
    // overriding a virtual function (catches typos like "Run" vs "run").
    void run() override;

private:
    // ----- Private members -----

    // Pointer to the camera device. Not owned by this class.
    // Initialized to nullptr in constructor.
    Arena::IDevice* m_pDevice;

    // Base output directory chosen by the user (e.g., "C:/captures/")
    QString m_outputPath;

    // Session subfolder created at the start of each acquisition run.
    // Format: m_outputPath/acq_YYYYMMDD_HHmmss  (e.g., "C:/captures/acq_20260603_143022")
    // A new subfolder is created every time Start is pressed, so:
    //   - Changing settings and re-acquiring never overwrites previous files
    //   - All files from one session (frames, timestamps.csv, metadata.json) stay together
    QString m_sessionPath;

    // Optional custom name set by the user before start().
    // Empty string means "auto-generate from timestamp".
    QString m_customSessionName;

    // Base name for the TIFF when FieldType::Custom is active.
    // E.g. "dot_grid" → dot_grid_mean.tiff.  Empty when not a Custom capture.
    QString m_customFieldName;

    // Free-form notes entered in the Notes dialog — written verbatim into metadata.json.
    QString m_notes;

    // JSON snippet of visible camera settings, passed from MainWindow via setCameraParamsJson().
    // Contains settings like {"PixelFormat":"Mono8","ExposureTime":5000.0,...}
    // Written to metadata.json under "camera_settings" at acquisition start.
    QString m_cameraParamsJson;

    // Full GenApi node-map snapshot, pre-built on the main thread via buildCameraSettingsJson().
    // Written to camera_settings.json in the session folder at the start of run().
    QString m_nodeMapSnapshotJson;

    // Thread-safe stop flag.
    // std::atomic<bool> can be read/written from multiple threads safely.
    // A plain 'bool' would be a "data race" — undefined behavior in C++.
    // When the main thread writes true (via requestStop()), the worker thread
    // sees the change and exits its loop.
    std::atomic<bool> m_stopRequested;

    // Running count of frames grabbed from the camera in this session
    int m_frameCount;

    // -------------------------------------------------------------------------
    // Frame-drop diagnostic tracking
    // -------------------------------------------------------------------------
    //
    // Three numbers together tell you where frames were lost:
    //
    //   cameraProduced  = m_lastCameraFrameId - m_firstCameraFrameId + 1
    //     → every frame the sensor transmitted (whether the host received it or not)
    //
    //   m_frameCount  (received into RAM)
    //     → frames GetImage() returned that were complete and not skipped
    //
    //   m_savedCount  (saved to storage)
    //     → frames the writer thread successfully wrote to disk
    //
    // If cameraProduced > m_frameCount → network/RDMA drops (never reached RAM)
    // If m_frameCount   > m_savedCount → queue or disk write failures
    uint64_t m_firstCameraFrameId;  // Frame ID of the very first received frame
    uint64_t m_lastCameraFrameId;   // Frame ID of the most recently received frame
    bool     m_firstFrameSeen;      // False until the first frame ID is recorded
    int      m_networkDropCount;    // Total frames lost before reaching host RAM
    int      m_savedCount;          // Filled in by the writer thread at exit

    // Selected save format (RawSequence, TiffStack, or RawVideo)
    SaveFormat m_saveFormat;

    // Field capture type: None = normal acquisition
    FieldType  m_fieldType;

    // -------------------------------------------------------------------------
    // Producer-consumer queue — shared between acquisition loop and writer thread
    // -------------------------------------------------------------------------
    //
    // WHY THIS DESIGN:
    //   Without this, the acquisition loop writes to disk synchronously. Once the
    //   OS page cache fills (~1-4 GB), every write() call blocks until the OS can
    //   flush to the SSD — causing the frame rate to drop to ~1 fps.
    //
    //   With this queue, the acquisition loop only copies 23 MB of pixels (fast),
    //   pushes to the queue, and returns the camera buffer immediately. The writer
    //   thread drains the queue to disk at whatever pace the OS allows.
    //
    // C++ CONCEPT — mutex + condition_variable:
    //   A mutex (mutual exclusion) ensures only one thread touches the queue at a
    //   time. A condition_variable lets the writer thread sleep (no CPU burn) until
    //   the acquisition loop pushes something and wakes it with notify_one().
    std::queue<FrameData>   m_writeQueue;
    std::mutex              m_queueMutex;
    std::condition_variable m_queueCV;

    // Set to true when the acquisition loop is finished, so the writer thread
    // knows to drain the queue and then exit.
    std::atomic<bool> m_acquisitionDone;

    // --- Private helper ---

    // Save a single frame's raw pixel data to a .raw file.
    // Returns true on success, false on failure.
    bool saveRawFrame(const void* pData, size_t dataSize, int frameIndex,
                      std::string& errorMsg);

    // Write a metadata.json file describing the acquisition session.
    // Called at the start (complete=false) and end (complete=true) of acquisition.
    void writeMetadataJson(int64_t width, int64_t height, const std::string& pixelFormat,
                           int bitsPerPixel, int frameCount, bool complete);
};
