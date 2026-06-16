// =============================================================================
// PreviewWorker.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares the PreviewWorker class — a background thread that grabs camera
//   frames and converts them to QImage objects for live display in the UI.
//
// WHY A SEPARATE WORKER?
//   Just like AcquisitionWorker, preview needs a background thread because:
//     - GetImage() blocks waiting for the camera
//     - Image conversion (especially for 16-bit formats) takes CPU time
//     - If this ran on the main GUI thread, the UI would freeze
//   PreviewWorker emits QImage signals so the GUI can display frames without
//   being blocked by camera I/O.
//
// KEY DIFFERENCES FROM AcquisitionWorker:
//   - No file I/O or disk writing
//   - No producer-consumer queue (frames are dropped if display can't keep up)
//   - Frame rate limited to ~30 fps to avoid flooding the event queue
//   - Emits QImage objects directly (not raw bytes)
//
// C++ CONCEPT — std::atomic<bool>:
//   Like in AcquisitionWorker, we use std::atomic<bool> for the stop flag
//   so the main thread can set it safely from a different thread.
// =============================================================================

#pragma once

// Qt threading
#include <QThread>      // Base class — provides thread management
#include <QImage>       // Qt's image class for display
#include <QSize>        // QSize — used for the display resolution hint
#include <QMutex>       // QMutex — protects the raw pixel cache from concurrent access
#include <QByteArray>   // QByteArray — byte buffer for the cached raw frame

// C++ standard library
#include <atomic>    // std::atomic<bool> — thread-safe boolean
#include <cstdint>   // uint32_t
#include <string>    // std::string (pixel format name)

// Forward declaration: tell the compiler Arena::IDevice exists without including ArenaApi.h
namespace Arena { class IDevice; }


// =============================================================================
// PreviewWorker class — background thread for live preview
// =============================================================================
class PreviewWorker : public QThread
{
    // Q_OBJECT is required for any class that uses signals or slots
    Q_OBJECT

public:
    // Constructor — creates the worker thread object (doesn't start it yet).
    // 'parent' follows Qt's object ownership pattern.
    explicit PreviewWorker(QObject* parent = nullptr);

    // Destructor — ensures the thread is stopped before cleanup.
    ~PreviewWorker() override;

    // Set the camera device to use for preview. Must be called before start().
    // The worker does NOT take ownership — the CameraManager still owns the device.
    void setDevice(Arena::IDevice* device);

    // Set the maximum display size for emitted frames.
    // If the camera image exceeds this size, the worker scales it down (keeping
    // aspect ratio) before emitting.  This keeps the cross-thread copy small and
    // the GUI thread's QPixmap::fromImage fast.
    // Call before start() — the thread reads this value without a lock.
    // A sensible default is 2× the viewport size; (0,0) disables scaling.
    void setDisplayHint(QSize maxSize);

    // Request the thread to stop. Does not wait for it to finish.
    // Call wait() on the thread after this to block until it exits.
    void requestStop();

    // Return the actual camera count at display-image pixel (x, y).
    //
    // Coordinates are in the space of the last emitted QImage (which may be
    // smaller than the native sensor if the display hint caused downscaling).
    // The mapping back to native resolution is handled internally.
    //
    // Called by PreviewDialog::onPixelClicked() when the user clicks the image.
    // Thread-safe: safe to call from the GUI thread while run() is executing.
    // Returns 0 if no frame has been cached yet or if (x,y) is out of bounds.
    uint32_t rawPixelAt(int x, int y) const;

signals:
    // Emitted when a new frame is available for display.
    // The QImage is a copy, safe to use on the GUI thread.
    // Typically emitted at most ~30 times per second to avoid flooding the event queue.
    void newFrame(QImage image);

    // Emitted once per second with the number of frames the camera actually delivered
    // in that second (regardless of how many were dropped for display throttling).
    // This is the true camera acquisition rate.
    void cameraFps(int fps);

    // Emitted when an error occurs (e.g., camera disconnected, GetImage timeout).
    // The preview thread exits after emitting this signal.
    void errorOccurred(const QString& message);

    // Emitted for informational status messages (e.g., "Preview started", "Display FPS: 29.8").
    void statusMessage(const QString& message);

    // Emitted ~30 fps with the ChunkLineStatusAll bitmask (bit N = state of Line N).
    // available is false when the camera does not support chunk line status data;
    // in that case lineStatusAll is always 0 and the display should show "N/A".
    void lineStatusUpdated(int64_t lineStatusAll, bool available);

protected:
    // run() is the method that executes on the worker thread.
    // This is where we configure the stream, grab frames, and emit newFrame signals.
    void run() override;

private:
    // Pointer to the camera device — not owned by this class.
    // Nullptr means no device is set yet.
    Arena::IDevice* m_pDevice;

    // Thread-safe stop flag. Set to true by requestStop(), checked in run() loop.
    // std::atomic<bool> is safe to read/write from multiple threads without explicit locking.
    std::atomic<bool> m_stopRequested;

    // Maximum emitted frame size (set once before start(), read only by the worker thread).
    // If either dimension is 0, no scaling is applied.
    QSize m_displayHint;

    // ---- Raw pixel cache — protected by m_rawMutex ----
    //
    // Updated once per emitted display frame (≤30 fps) just before the emit.
    // Stores the native-resolution raw bytes from pData so the GUI thread can
    // call rawPixelAt() and get the actual camera count rather than a 0–255
    // display value.  m_rawDisplayWidth/Height record the emitted image size so
    // rawPixelAt() can scale display coords back to native coords when the worker
    // has downsampled for the display hint.
    mutable QMutex m_rawMutex;
    QByteArray     m_rawBytes;
    int            m_rawWidth         = 0;   // native camera pixel columns
    int            m_rawHeight        = 0;   // native camera pixel rows
    int            m_rawBitsPerPixel  = 0;
    std::string    m_rawPixelFormat;         // e.g. "Mono8", "Mono16", "Mono12p"
    int            m_rawDisplayWidth  = 0;   // columns of the emitted (possibly scaled) image
    int            m_rawDisplayHeight = 0;
};
