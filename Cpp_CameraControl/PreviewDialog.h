// =============================================================================
// PreviewDialog.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares the PreviewDialog class — a modeless (non-modal) window for
//   live camera preview.  The user opens it via the "Preview" button on
//   the main window and can interact with other windows while it's open.
//
// LAYOUT (left to right):
//   +------------------------------------------+------+
//   |  ZoomableImageWidget                      | Hist |
//   |  (fills available space;                  | ogram|
//   |   mouse-wheel zoom, drag to pan,          | Widget
//   |   double-click to fit)                    |      |
//   +------------------------------------------+------+
//   | [Start Preview]  [Stop Preview]   FPS: --  Status|
//   +--------------------------------------------------+
//
// DISPLAY PIPELINE (applied each frame inside onNewFrame):
//   1. Pass raw QImage to HistogramWidget::updateHistogram()
//   2. Apply contrast remap: pixels below lowLevel → 0, above highLevel → 255
//   3. Paint semi-transparent overlays:
//        - RED  (value == 255 in raw)  → saturated pixels
//        - YELLOW (value == 0 in raw)  → dead / always-zero pixels
//   4. Set the result as a QPixmap on ZoomableImageWidget
//
// MODELESS VS MODAL:
//   A modal dialog blocks the main window until closed (exec()).
//   This dialog is modeless — shown with show() so the user can adjust camera
//   parameters on the main window while watching the live preview.
//
// INTERACTION WITH MainWindow:
//   - When real acquisition starts, MainWindow calls setAcquisitionRunning(true),
//     which disables preview to prevent two threads competing for the camera device.
//   - When acquisition finishes, MainWindow calls setAcquisitionRunning(false).
// =============================================================================

#pragma once

// Qt dialog and widget headers
#include <QMainWindow>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QImage>
#include <QRect>

// Forward declarations — avoids pulling in full headers here.
class CameraManager;
class PreviewWorker;
class ZoomableImageWidget;
class HistogramWidget;
class FocusDiagnosticDialog;


// =============================================================================
// PreviewDialog
// =============================================================================
class PreviewDialog : public QMainWindow
{
    Q_OBJECT

public:
    explicit PreviewDialog(CameraManager* mgr, QWidget* parent = nullptr);
    ~PreviewDialog() override;

    // Called by MainWindow when real acquisition starts or stops.
    // Prevents the preview and acquisition threads from competing for the camera.
    void setAcquisitionRunning(bool running);

    // Stop the preview thread if it is running.
    // Called by MainWindow just before it starts its own acquisition.
    void stopPreviewIfRunning();

    // Remove the persistent green ROI overlay from the image view.
    // Called by MainWindow::onResetRoiClicked() so the overlay disappears after a reset.
    void clearRoiOverlay();

    // Transforms the Draw ROI button into a blue "Apply ROI" button.
    // Called by MainWindow::onResetRoiClicked() when a full-frame reset is pending
    // while the preview is running so the button reflects the new pending state.
    void enterRoiApplyState();

    // Restores the Draw ROI button to its default white checkable-toggle appearance.
    // Called by MainWindow::onResetRoiClicked() to clear any in-progress draw or
    // pending apply before installing a new pending ROI.
    void exitRoiApplyState();

signals:
    // Emitted when the preview thread successfully starts.
    void previewStarted();

    // Emitted when the preview thread stops (user clicked Stop or error occurred).
    void previewStopped();

    // Emitted when the user finishes drawing an ROI rectangle.
    // Coordinates are in absolute sensor pixels (OffsetX/Y already added in).
    // Connect this to MainWindow::onRoiApplied to pin + populate the ROI spinboxes.
    void roiApplied(int offsetX, int offsetY, int width, int height);

    // Emitted when the user chooses "Reset ROI to Full Frame" from the right-click
    // context menu on the Draw ROI button.
    // Connect this to MainWindow::onResetRoiClicked.
    void resetRoiRequested();

    // Emitted when the preview worker detects a camera disconnect (cable pulled, device lost).
    // Connect this to MainWindow::onDisconnectClicked so the UI reflects the disconnected state.
    void cameraDisconnected();

    // Emitted when the user clicks the "Apply ROI" button (after drawing).
    // MainWindow writes the pending ROI to the camera with correct ordering and
    // resets the Apply Pinned Parameters button style.
    void roiApplyRequested();

    // Emitted when the user cancels a pending ROI via right-click "Cancel ROI".
    // MainWindow clears m_hasPendingRoi and resets the Apply button style.
    void roiCancelled();

public slots:
    // Called by AcquisitionWorker::fieldPreviewReady during a white/dark field capture
    // to display the current running mean in the preview window.
    // Works even when m_acquisitionRunning is true (preview worker is stopped).
    void feedFrame(const QImage& image);

private slots:
    void onStartPreviewClicked();
    void onStopPreviewClicked();

    // Called by the PreviewWorker thread (via Qt signal) with each new camera frame.
    void onNewFrame(const QImage& image);

    // Called when the PreviewWorker reports an error.
    void onWorkerError(const QString& message);

    // Called when the PreviewWorker thread finishes.
    void onWorkerFinished();

    // Called by m_fpsTimer every 1 second.
    void onFpsTimerTick();

    // Called by PreviewWorker once per second with the actual camera acquisition rate.
    void onCameraFps(int fps);

    // Called when the user moves a histogram level handle.
    // Re-renders the last received frame with the new contrast settings.
    void onLevelsChanged(double low, double high);

    // Called when the Auto Contrast button is toggled.
    // When newly checked, immediately applies auto-levels to the current frame.
    void onAutoContrastToggled(bool checked);

    // Called when the Draw ROI button is toggled on or off.
    // Forwards the checked state to ZoomableImageWidget::setRoiDrawMode().
    void onDrawRoiClicked(bool checked);

    // Called when the Open Focus Diagnostic menu action is triggered.
    void onOpenFocusDiagnostic();

    // Called when the user clicks "Redraw ROI" inside the focus diagnostic dialog.
    void onFocusRedrawRoiRequested();

    // Called by ZoomableImageWidget::roiDrawn() when the user releases the mouse.
    // Converts image-space coords to absolute sensor coords and emits roiApplied().
    void onRoiDrawn(QRect imageRect);

    // Called by ZoomableImageWidget::pixelClicked() when the user right-clicks the image.
    // Looks up the actual camera count at (imageX, imageY) and shows a readback overlay.
    void onPixelClicked(int imageX, int imageY);

    // Called by PreviewWorker::lineStatusUpdated() each display frame.
    // Updates m_lineStatusLabel with individual line states from the bitmask.
    void onLineStatusUpdated(int64_t lineStatusAll, bool available);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    // Apply the full display pipeline to a raw frame:
    //   contrast remap + saturation (red) / dead-pixel (yellow) overlay.
    // Returns an ARGB32 QImage ready to be converted to QPixmap for display.
    QImage applyDisplayPipeline(const QImage& raw) const;

    // Compute percentile-based levels from the current histogram and apply them
    // to the histogram handles.  The percentile thresholds are tunable:
    //   lowPct  = 3.0  → black point at the 3rd  percentile (ignores dark noise)
    //   highPct = 97.0 → white point at the 97th percentile (ignores hot pixels)
    // Call after updateHistogram() and before applyDisplayPipeline().
    void applyAutoLevels(double lowPct = 3.0, double highPct = 97.0);

    // ---- Camera / threading ----
    CameraManager* m_mgr;
    PreviewWorker* m_worker;

    // ---- Image display ----
    ZoomableImageWidget* m_imageView;   // Zoom/pan image canvas (replaces QLabel in QScrollArea)
    HistogramWidget*     m_histogram;   // Rotated histogram with draggable level handles

    // Last raw frame received from the worker.
    // Stored so we can re-render instantly when the user adjusts histogram handles
    // (without waiting for the next frame from the camera).
    QImage m_lastRawFrame;

    // ---- Buttons ----
    QPushButton* m_startButton;
    QPushButton* m_stopButton;
    QPushButton* m_autoContrastButton;   // Bottom-bar toggle; mirrors m_autoContrastAction
    QPushButton* m_drawRoiButton;        // Bottom-bar toggle; mirrors m_drawRoiAction

    // ---- Toolbar menu actions ----
    QAction*     m_autoContrastAction;   // Auto-contrast menu → Enable (checkable)
    QAction*     m_drawRoiAction;        // ROI → Draw ROI (checkable)

    // ---- Focus diagnostic dialog (lazily created) ----
    FocusDiagnosticDialog* m_focusDialog;

    // Camera ROI rectangle in display-image pixel coordinates.
    QRect m_roiDisplayRect;

    // Focus diagnostic ROI in display-image pixel coordinates.
    QRect m_focusRoiDisplayRect;

    // True while the user is drawing an ROI specifically for the focus diagnostic.
    bool m_drawingForFocus = false;

    // True after the user finishes drawing (or resetting) an ROI and the result is
    // pending apply.  In this state the Draw ROI button becomes the "Apply ROI" button.
    bool m_roiPending = false;


    // ---- Status bar ----
    QLabel* m_statusLabel;
    QLabel* m_fpsLabel;          // Display FPS (throttled to ~30 fps)
    QLabel* m_cameraFpsLabel;    // Camera FPS (true acquisition rate from the worker)
    QLabel* m_lineStatusLabel;   // ChunkLineStatusAll bitmask shown as individual line states

    // ---- FPS tracking ----
    QTimer* m_fpsTimer;
    int     m_frameCount;      // Display frames received this second (from onNewFrame)
    int     m_feedFrameCount;  // feedFrame calls this second; used to estimate Camera FPS
                               // during field captures (each call covers ~10 camera frames)

    // True when main-window acquisition is running (preview disabled)
    bool m_acquisitionRunning;

    // Last pixel position clicked for the readback overlay.
    // -1 when no overlay is pinned.  Refreshed automatically on each new frame.
    int m_pinnedPixelX = -1;
    int m_pinnedPixelY = -1;
};
