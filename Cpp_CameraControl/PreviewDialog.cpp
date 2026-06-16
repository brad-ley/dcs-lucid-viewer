// =============================================================================
// PreviewDialog.cpp
// =============================================================================
//
// Implementation of the live camera preview dialog.
// Features:
//   - ZoomableImageWidget: mouse-wheel zoom, drag-to-pan, double-click to fit
//   - HistogramWidget: rotated histogram on the right, with two draggable
//     handles (cyan = black point, yellow = white point)
//   - Per-frame display pipeline: contrast remap + saturation (red) / dead (yellow)
//     pixel overlay
//   - External trigger awareness: timeout errors are suppressed (handled in
//     PreviewWorker; errors that reach here are real errors worth showing)
// =============================================================================

#include "PreviewDialog.h"
#include "PreviewWorker.h"
#include "CameraManager.h"
#include "ZoomableImageWidget.h"
#include "HistogramWidget.h"
#include "ConfigDialog.h"
#include "FocusDiagnosticDialog.h"

// Qt headers
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QCloseEvent>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QAction>


// =============================================================================
// Constructor
// =============================================================================
PreviewDialog::PreviewDialog(CameraManager* mgr, QWidget* parent)
    : QMainWindow(parent)
    , m_mgr(mgr)
    , m_worker(nullptr)
    , m_imageView(nullptr)
    , m_histogram(nullptr)
    , m_startButton(nullptr)
    , m_stopButton(nullptr)
    , m_autoContrastButton(nullptr)
    , m_autoContrastAction(nullptr)
    , m_drawRoiAction(nullptr)
    , m_focusDialog(nullptr)
    , m_statusLabel(nullptr)
    , m_fpsLabel(nullptr)
    , m_cameraFpsLabel(nullptr)
    , m_lineStatusLabel(nullptr)
    , m_fpsTimer(nullptr)
    , m_frameCount(0)
    , m_feedFrameCount(0)
    , m_acquisitionRunning(false)
{
    setWindowTitle("Live Preview");
    setMinimumSize(860, 600);

    // QMainWindow requires a central widget to hold all content.
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout* mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(6);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    // =========================================================================
    // MENU BAR  (QMainWindow provides menuBar() natively)
    // =========================================================================
    QMenu* acMenu = menuBar()->addMenu("Auto-contrast");

    m_autoContrastAction = acMenu->addAction("Enable Auto Contrast");
    m_autoContrastAction->setCheckable(true);
    m_autoContrastAction->setToolTip(
        "When on: each frame sets black point = 3rd percentile, "
        "white point = 97th percentile of pixel values");

    QAction* autoContrastSettingsAction = acMenu->addAction("Auto Contrast Settings...");
    autoContrastSettingsAction->setToolTip(
        "Configure the percentile thresholds used by Auto Contrast.");

    QMenu* roiMenu = menuBar()->addMenu("ROI");
    m_drawRoiAction = roiMenu->addAction("Draw ROI Bounds");
    m_drawRoiAction->setCheckable(true);
    m_drawRoiAction->setToolTip(
        "Click to enter ROI draw mode, then drag a rectangle on the image.");

    QAction* resetRoiAction = roiMenu->addAction("Reset ROI to Full Frame");
    resetRoiAction->setToolTip("Set OffsetX=0, OffsetY=0, Width=WidthMax, Height=HeightMax.");

    roiMenu->addSeparator();

    QAction* focusDiagAction = roiMenu->addAction("Open Focus Diagnostic...");
    focusDiagAction->setToolTip("Open a window that plots a focus metric over the ROI each frame.");

    // =========================================================================
    // CONTENT ROW: image view (left, stretches) + histogram (right, fixed 70px)
    // =========================================================================

    QHBoxLayout* contentRow = new QHBoxLayout();
    contentRow->setSpacing(0);

    m_imageView = new ZoomableImageWidget(this);
    m_histogram = new HistogramWidget(this);

    contentRow->addWidget(m_imageView, 1);  // stretch=1 → takes all remaining space
    contentRow->addWidget(m_histogram, 0); // stretch=0 → fixed at HistogramWidget::fixedWidth()

    mainLayout->addLayout(contentRow, 1);  // stretch=1 → content row takes most height

    // =========================================================================
    // BUTTON ROW
    // =========================================================================

    QHBoxLayout* buttonRow = new QHBoxLayout();

    // Base style shared by the two bottom-bar buttons so they have uniform height
    // and font size regardless of which Qt style the OS applies.
    const QString btnBase =
        "QPushButton { min-height: 28px; font-size: 12px; padding: 4px 14px; }";

    m_startButton = new QPushButton("Start Preview", this);
    m_startButton->setStyleSheet(
        btnBase +
        "QPushButton         { background-color: #2E7D32; color: white; font-weight: bold; }"
        "QPushButton:disabled{ background-color: #888; color: #ccc; }");
    m_startButton->setToolTip("Begin live preview from the connected camera");

    m_stopButton = new QPushButton("Stop Preview", this);
    m_stopButton->setStyleSheet(
        btnBase +
        "QPushButton         { background-color: #B71C1C; color: white; font-weight: bold; }"
        "QPushButton:disabled{ background-color: #888; color: #ccc; }");
    m_stopButton->setToolTip("Stop live preview streaming");
    m_stopButton->setEnabled(false);

    m_autoContrastButton = new QPushButton("Auto Contrast", this);
    m_autoContrastButton->setCheckable(true);
    m_autoContrastButton->setFixedWidth(110);
    m_autoContrastButton->setStyleSheet(
        btnBase +
        "QPushButton         { background-color: white; color: #222; }"
        "QPushButton:checked { background-color: #1565C0; color: white; font-weight: bold; }"
        "QPushButton:disabled{ background-color: #888; color: #ccc; }");
    m_autoContrastButton->setToolTip(
        "Toggle Auto Contrast — also available in the Auto-contrast menu");

    m_drawRoiButton = new QPushButton("Draw ROI", this);
    m_drawRoiButton->setCheckable(true);
    m_drawRoiButton->setFixedWidth(110);
    m_drawRoiButton->setStyleSheet(
        btnBase +
        "QPushButton         { background-color: white; color: #222; }"
        "QPushButton:checked { background-color: #E65100; color: white; font-weight: bold; }"
        "QPushButton:disabled{ background-color: #888; color: #ccc; }");
    m_drawRoiButton->setToolTip(
        "Left-click to draw an ROI rectangle on the image.\n"
        "Right-click for Reset ROI to Full Frame.");
    m_drawRoiButton->setContextMenuPolicy(Qt::CustomContextMenu);

    buttonRow->addWidget(m_startButton);
    buttonRow->addWidget(m_stopButton);
    buttonRow->addStretch();
    buttonRow->addWidget(m_autoContrastButton);
    buttonRow->addSpacing(6);
    buttonRow->addWidget(m_drawRoiButton);

    // =========================================================================
    // STATUS ROW
    // =========================================================================

    QHBoxLayout* statusRow = new QHBoxLayout();

    m_statusLabel = new QLabel("Ready", this);
    m_statusLabel->setStyleSheet("color: #888; font-style: italic;");

    m_fpsLabel = new QLabel("Display FPS: --", this);
    m_fpsLabel->setStyleSheet("color: #888; font-style: italic;");

    // Camera FPS label — shows the true acquisition rate reported by the worker thread,
    // which may be higher than display FPS if the camera runs faster than 30 fps.
    m_cameraFpsLabel = new QLabel("Camera FPS: --", this);
    m_cameraFpsLabel->setStyleSheet("color: #888; font-style: italic;");

    // Small legend for the overlay colours
    QLabel* legendLabel = new QLabel(
        "<span style='color:#ff4040;'>■</span> Saturated&nbsp;&nbsp;"
        "<span style='color:#dddd00;'>■</span> Zero/dead", this);
    legendLabel->setTextFormat(Qt::RichText);
    legendLabel->setStyleSheet("font-size: 10px; color: #aaa;");

    m_lineStatusLabel = new QLabel("GPIO: --", this);
    m_lineStatusLabel->setStyleSheet(
        "color: #aaa; font-style: italic; font-family: monospace;");
    m_lineStatusLabel->setToolTip(
        "ChunkLineStatusAll — per-frame GPIO line states (bit N = Line N).\n"
        "Only populated when the camera supports chunk data.");

    statusRow->addWidget(m_statusLabel);
    statusRow->addStretch();
    statusRow->addWidget(m_lineStatusLabel);
    statusRow->addSpacing(12);
    statusRow->addWidget(legendLabel);
    statusRow->addSpacing(12);
    statusRow->addWidget(m_cameraFpsLabel);
    statusRow->addSpacing(8);
    statusRow->addWidget(m_fpsLabel);

    mainLayout->addLayout(buttonRow);
    mainLayout->addLayout(statusRow);

    // =========================================================================
    // FPS TIMER
    // =========================================================================

    m_fpsTimer = new QTimer(this);
    m_fpsTimer->setInterval(1000);
    connect(m_fpsTimer, &QTimer::timeout, this, &PreviewDialog::onFpsTimerTick);

    // =========================================================================
    // SIGNAL CONNECTIONS
    // =========================================================================

    connect(m_startButton, &QPushButton::clicked,
            this, &PreviewDialog::onStartPreviewClicked);
    connect(m_stopButton,  &QPushButton::clicked,
            this, &PreviewDialog::onStopPreviewClicked);

    connect(m_histogram, &HistogramWidget::levelsChanged,
            this, &PreviewDialog::onLevelsChanged);

    connect(m_autoContrastAction, &QAction::toggled,
            this, &PreviewDialog::onAutoContrastToggled);

    // Keep button and menu action in sync — only one drives the logic via toggled.
    connect(m_autoContrastAction, &QAction::toggled,
            m_autoContrastButton, &QPushButton::setChecked);
    connect(m_autoContrastButton, &QPushButton::toggled,
            m_autoContrastAction, &QAction::setChecked);

    connect(autoContrastSettingsAction, &QAction::triggered, [this]()
    {
        ConfigDialog dlg(this);
        dlg.exec();
    });

    connect(m_drawRoiAction, &QAction::toggled,
            this, &PreviewDialog::onDrawRoiClicked);

    // Keep button and menu action in sync.
    connect(m_drawRoiAction,  &QAction::toggled,
            m_drawRoiButton,  &QPushButton::setChecked);
    connect(m_drawRoiButton,  &QPushButton::toggled,
            m_drawRoiAction,  &QAction::setChecked);

    // Left-click dispatcher: when the ROI is pending the button is non-checkable
    // ("Apply ROI" mode) so toggled() doesn't fire — handle the apply action here.
    // When not pending the button is a normal checkable toggle and toggled() drives
    // onDrawRoiClicked; this handler is a no-op in that path.
    connect(m_drawRoiButton, &QPushButton::clicked, this, [this]()
    {
        if (!m_roiPending)
            return;

        bool streaming = m_acquisitionRunning || (m_worker && m_worker->isRunning());
        if (streaming)
        {
            QMessageBox::information(
                this,
                "Cannot Apply While Streaming",
                "ROI cannot be applied while the camera is streaming.\n"
                "Stop acquisition or preview first, then click Apply ROI.");
            return;  // Stay blue — pending ROI is preserved
        }

        clearRoiOverlay();
        exitRoiApplyState();
        emit roiApplyRequested();
    });

    // Right-click context menu on the Draw ROI button.
    connect(m_drawRoiButton, &QPushButton::customContextMenuRequested,
            [this](const QPoint& pos)
    {
        QMenu menu(m_drawRoiButton);
        QAction* redrawAction = menu.addAction("Re-draw ROI");
        QAction* cancelAction = menu.addAction("Cancel ROI");
        menu.addSeparator();
        QAction* resetAction = menu.addAction("Reset ROI to Full Frame");
        redrawAction->setEnabled(m_roiPending);
        cancelAction->setEnabled(m_roiPending);

        QAction* chosen = menu.exec(m_drawRoiButton->mapToGlobal(pos));
        if (chosen == resetAction)
        {
            // If a drawn ROI was pending apply, discard it silently — the full-frame
            // reset replaces it.  Don't emit roiCancelled; MainWindow will set up a
            // new pending ROI and call enterRoiApplyState() if still streaming.
            if (m_roiPending)
            {
                clearRoiOverlay();
                exitRoiApplyState();
            }
            emit resetRoiRequested();
        }
        else if (chosen == cancelAction)
        {
            clearRoiOverlay();
            exitRoiApplyState();
            emit roiCancelled();
        }
        else if (chosen == redrawAction)
        {
            exitRoiApplyState();
            emit roiCancelled();
            // Enter draw mode — action.setChecked drives onDrawRoiClicked + button sync
            m_drawRoiAction->setChecked(true);
        }
    });

    connect(resetRoiAction, &QAction::triggered,
            this, &PreviewDialog::resetRoiRequested);

    connect(focusDiagAction, &QAction::triggered,
            this, &PreviewDialog::onOpenFocusDiagnostic);

    connect(m_imageView, &ZoomableImageWidget::roiDrawn,
            this, &PreviewDialog::onRoiDrawn);

    connect(m_imageView, &ZoomableImageWidget::pixelClicked,
            this, &PreviewDialog::onPixelClicked);
}


// =============================================================================
// Destructor
// =============================================================================
PreviewDialog::~PreviewDialog()
{
    stopPreviewIfRunning();
}


// =============================================================================
// setAcquisitionRunning
// =============================================================================
void PreviewDialog::setAcquisitionRunning(bool running)
{
    m_acquisitionRunning = running;

    m_imageView->setStatusOverlay(
        running ? QString::fromUtf8("Acquiring \xe2\x80\x94 no preview available") : QString());

    if (running)
    {
        m_startButton->setEnabled(false);
        m_stopButton->setEnabled(false);
        m_statusLabel->setText("Preview disabled (acquisition in progress)");
        m_statusLabel->setStyleSheet("color: #ff6b6b; font-style: italic;");
    }
    else
    {
        // If the FPS timer was running (started by feedFrame during a field capture),
        // stop it and reset the labels — the preview worker is not running.
        if (m_fpsTimer->isActive() && !(m_worker && m_worker->isRunning()))
        {
            m_fpsTimer->stop();
            m_fpsLabel->setText("Display FPS: --");
            m_cameraFpsLabel->setText("Camera FPS: --");
            m_lineStatusLabel->setText("GPIO: --");
        }

        m_startButton->setEnabled(true);
        if (m_worker && m_worker->isRunning())
        {
            m_stopButton->setEnabled(true);
        }
        else
        {
            m_stopButton->setEnabled(false);
            m_statusLabel->setText("Ready");
            m_statusLabel->setStyleSheet("color: #888; font-style: italic;");
        }
    }
}


// =============================================================================
// stopPreviewIfRunning
// =============================================================================
void PreviewDialog::stopPreviewIfRunning()
{
    if (m_worker && m_worker->isRunning())
    {
        m_worker->requestStop();
        // The worker polls every 500 ms (external trigger) or 2000 ms (normal mode),
        // so it should exit well within this 3-second wait.
        if (!m_worker->wait(3000))
        {
            // Worker is still blocked inside GetImage() (e.g., camera cable was pulled
            // and GenTL is waiting for its own internal timeout).  Force the stream off
            // so GetImage() returns an error and the worker can exit cleanly.
            // This is safe when the device is physically connected — StopStream() breaks
            // the blocking call.  On a physically disconnected device the call may throw
            // but cannot crash worse than leaving the worker running while the device is
            // destroyed.
            try { m_mgr->stopStream(); } catch (...) {}
            m_worker->wait(3000);  // Give it another 3 s after the forced stop
        }
    }
}


// =============================================================================
// clearRoiOverlay — remove the confirmed-ROI green rectangle from the image view
// =============================================================================
void PreviewDialog::clearRoiOverlay()
{
    m_imageView->clearRoiOverlay();
    m_roiDisplayRect = QRect();
}


// =============================================================================
// applyDisplayPipeline
// =============================================================================
//
// This is the heart of the display system.  It runs once per frame (up to 30 fps)
// and transforms the raw 8-bit camera image into an ARGB32 display image with:
//   1. Contrast remap  — linear stretch between the histogram handles
//   2. Saturation mark — red overlay for pixels at display maximum (255)
//   3. Dead-pixel mark — yellow overlay for pixels at zero (0)
//
// C++ CONCEPT — const method:
//   The 'const' at the end of the signature means this method promises not to
//   modify any member variables.  It can only read them.  This is a contract
//   that helps the compiler catch bugs.
// =============================================================================
QImage PreviewDialog::applyDisplayPipeline(const QImage& raw) const
{
    if (raw.isNull())
        return raw;

    // ---- Convert to Grayscale8 for processing ----
    // All our arithmetic operates on single-byte (0–255) values.
    // If the worker already sent a Grayscale8 image, this is a no-op (zero copy).
    QImage gray;
    if (raw.format() == QImage::Format_Grayscale8)
        gray = raw;
    else
        gray = raw.convertToFormat(QImage::Format_Grayscale8);

    const int w = gray.width();
    const int h = gray.height();

    // ---- Build a contrast remap lookup table (LUT) ----
    //
    // lowLevel / highLevel are 0–1 fractions from the histogram handles.
    // We convert them to 0–255 integer thresholds.
    //
    // LUT[i] = the display value for raw pixel value i:
    //   - Values at or below 'lo' map to 0 (black)
    //   - Values at or above 'hi' map to 255 (white)
    //   - Values in between are linearly interpolated
    //
    // C++ CONCEPT — lookup table:
    //   Instead of computing the linear remap for each of the millions of pixels,
    //   we pre-compute it for all 256 possible input values.  Then the per-pixel
    //   work is just a single array index — extremely fast.

    const int lo = static_cast<int>(m_histogram->lowLevel()  * 255.0);
    const int hi = static_cast<int>(m_histogram->highLevel() * 255.0);
    const int range = (hi > lo) ? (hi - lo) : 1;  // Guard against divide-by-zero

    uchar lut[256];
    for (int i = 0; i < 256; ++i)
    {
        int v = (i - lo) * 255 / range;
        // qBound(min, value, max) clamps value to [min, max]
        lut[i] = static_cast<uchar>(qBound(0, v, 255));
    }

    // ---- Build the output ARGB32 image ----
    //
    // We work in ARGB32 (4 bytes per pixel: alpha, red, green, blue) so we can
    // paint semi-transparent coloured overlays on top of the grayscale image.
    //
    // All three operations (remap, saturated overlay, dead-pixel overlay) happen
    // in a single pass over the pixels for efficiency.

    // Read mask flags once outside the pixel loop — QSettings reads registry each call,
    // so calling them per-pixel would be extremely slow at multi-megapixel resolutions.
    const bool showSat  = ConfigDialog::showSaturationMask();
    const bool showDead = ConfigDialog::showDeadPixelMask();

    QImage result(w, h, QImage::Format_ARGB32);

    for (int y = 0; y < h; ++y)
    {
        const uchar* srcLine = gray.constScanLine(y);
        QRgb*        dstLine = reinterpret_cast<QRgb*>(result.scanLine(y));

        for (int x = 0; x < w; ++x)
        {
            const uchar orig = srcLine[x];   // Raw pixel value BEFORE remap

            if (orig == 255 && showSat)
            {
                // Saturated pixel — paint solid-ish red.
                // We use full opacity (255 alpha) so these stand out clearly.
                // qRgba(r, g, b, a): r=red, g=green, b=blue, a=alpha (255=opaque)
                dstLine[x] = qRgba(220, 30, 30, 255);
            }
            else if (orig == 0 && showDead)
            {
                // Zero pixel — paint semi-transparent yellow.
                // Using some transparency (180/255) so you can still see the context.
                dstLine[x] = qRgba(210, 210, 0, 200);
            }
            else
            {
                // Normal pixel — apply contrast remap and show as grayscale.
                const uchar v = lut[orig];
                dstLine[x] = qRgb(v, v, v);   // qRgb: same value for R, G, B → gray
            }
        }
    }

    return result;
}


// =============================================================================
// onStartPreviewClicked
// =============================================================================
void PreviewDialog::onStartPreviewClicked()
{
    if (!m_mgr || !m_mgr->isConnected())
    {
        m_statusLabel->setText("ERROR: No camera connected");
        m_statusLabel->setStyleSheet("color: #ff6b6b; font-style: italic;");
        return;
    }

    if (!m_worker)
    {
        m_worker = new PreviewWorker(this);

        connect(m_worker, &PreviewWorker::newFrame,
                this,     &PreviewDialog::onNewFrame);
        connect(m_worker, &PreviewWorker::cameraFps,
                this,     &PreviewDialog::onCameraFps);
        connect(m_worker, &PreviewWorker::errorOccurred,
                this,     &PreviewDialog::onWorkerError);
        connect(m_worker, &PreviewWorker::finished,
                this,     &PreviewDialog::onWorkerFinished);
        connect(m_worker, &PreviewWorker::statusMessage,
                this, [this](const QString& msg) { m_statusLabel->setText(msg); });
        connect(m_worker, &PreviewWorker::lineStatusUpdated,
                this,     &PreviewDialog::onLineStatusUpdated);
    }

    m_lastRawFrame = QImage();   // Clear any stale frame from a previous session

    m_worker->setDevice(m_mgr->getDevice());

    // Tell the worker the maximum useful image size to emit.
    // We use 2× the current viewport dimensions so the user can zoom in 2× before
    // the image softens.  If the widget hasn't been laid out yet, fall back to
    // 2560×1440 — still a large reduction for a 24 MP camera (8.3 M vs 24 M pixels).
    QSize hint = m_imageView->size() * 2;
    if (!hint.isValid() || hint.width() < 640)
        hint = QSize(2560, 1440);
    m_worker->setDisplayHint(hint);

    m_worker->start();

    m_startButton->setEnabled(false);
    m_stopButton->setEnabled(true);
    m_statusLabel->setText("Streaming...");
    m_statusLabel->setStyleSheet("color: #4CAF50; font-style: italic;");

    m_frameCount = 0;
    m_fpsTimer->start();

    emit previewStarted();
}


// =============================================================================
// onStopPreviewClicked
// =============================================================================
void PreviewDialog::onStopPreviewClicked()
{
    if (m_worker && m_worker->isRunning())
    {
        m_worker->requestStop();
        m_worker->wait(3000);
    }

    m_fpsTimer->stop();
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    m_statusLabel->setText("Stopped");
    m_statusLabel->setStyleSheet("color: #888; font-style: italic;");
    m_fpsLabel->setText("Display FPS: --");
    m_cameraFpsLabel->setText("Camera FPS: --");
    m_lineStatusLabel->setText("GPIO: --");

    emit previewStopped();
}


// =============================================================================
// feedFrame — display an externally supplied frame (e.g. a field-capture mean)
// =============================================================================
//
// Called by MainWindow when AcquisitionWorker emits fieldPreviewReady during a
// white/dark field capture.  The preview worker is stopped at that point, so
// this is the only way new images reach the display.  Delegates directly to
// onNewFrame so the full display pipeline (histogram, auto-levels, overlays) runs.
void PreviewDialog::feedFrame(const QImage& image)
{
    // Start the FPS timer the first time a field-capture frame arrives.
    // The preview worker is stopped during field captures, so the timer is not
    // running.  setAcquisitionRunning(false) will stop it when the capture ends.
    if (!m_fpsTimer->isActive())
    {
        m_frameCount    = 0;
        m_feedFrameCount = 0;
        m_fpsTimer->start();
    }
    m_feedFrameCount++;

    // Downscale to ~2× viewport size, matching what PreviewWorker does for normal frames.
    // Field-capture images are full-resolution; keeping them that size wastes the
    // cross-thread copy and causes an apparent zoom jump against a scaled live preview.
    QSize hint = m_imageView->size() * 2;
    QImage display = image;
    if (hint.isValid() && hint.width() > 0 && hint.height() > 0 &&
        (image.width() > hint.width() || image.height() > hint.height()))
    {
        display = image.scaled(hint, Qt::KeepAspectRatio, Qt::FastTransformation);
    }
    onNewFrame(display);
}


// =============================================================================
// onNewFrame — the display pipeline runs here
// =============================================================================
void PreviewDialog::onNewFrame(const QImage& image)
{
    m_frameCount++;

    // Store the raw frame so the histogram handles can re-render it immediately
    // when the user drags them (even if no new frame arrives from the camera).
    m_lastRawFrame = image;

    // Update the histogram bar chart with the new frame's pixel distribution.
    // This must happen BEFORE applyAutoLevels() so the bins are current.
    m_histogram->updateHistogram(image);

    // If auto-contrast is enabled, set the histogram handles to the configured
    // percentile thresholds (default 3rd / 97th).  Edit these via Config → Open Configs...
    if (m_autoContrastAction->isChecked())
        applyAutoLevels(ConfigDialog::autoContrastLowPct(), ConfigDialog::autoContrastHighPct());

    // Apply contrast remap + saturation/dead-pixel overlay, then display.
    QImage processed = applyDisplayPipeline(image);
    m_imageView->setPixmap(QPixmap::fromImage(processed));

    // Feed focus diagnostic if it is open and a frame is available.
    if (m_focusDialog && m_focusDialog->isVisible() && !m_lastRawFrame.isNull())
    {
        QImage roiImg = m_focusRoiDisplayRect.isNull() || !m_focusRoiDisplayRect.isValid()
                        ? m_lastRawFrame
                        : m_lastRawFrame.copy(m_focusRoiDisplayRect);
        m_focusDialog->addFrame(roiImg);
    }

    // Refresh the pixel readback overlay with this frame's value.
    if (m_pinnedPixelX >= 0)
        onPixelClicked(m_pinnedPixelX, m_pinnedPixelY);
}


// =============================================================================
// onLevelsChanged — re-render current frame with new contrast settings
// =============================================================================
void PreviewDialog::onLevelsChanged(double /*low*/, double /*high*/)
{
    // The new levels are already stored in m_histogram — applyDisplayPipeline
    // reads them from there.  We just need to re-run the pipeline on the last frame.
    if (!m_lastRawFrame.isNull())
        m_imageView->setPixmap(QPixmap::fromImage(applyDisplayPipeline(m_lastRawFrame)));
}


// =============================================================================
// onWorkerError
// =============================================================================
void PreviewDialog::onWorkerError(const QString& message)
{
    m_statusLabel->setText("ERROR: " + message);
    m_statusLabel->setStyleSheet("color: #ff6b6b; font-style: italic;");

    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    m_fpsTimer->stop();

    emit previewStopped();

    // Detect camera disconnect so MainWindow can update its connection state.
    // Emit before showing the dialog so MainWindow processes the disconnect
    // (disables UI, releases device) before the modal blocks the event loop.
    const bool isDisconnect = message.contains("GC_ERR_TIMEOUT",      Qt::CaseInsensitive)
                           || message.contains("GC_ERR_NOT_CONNECTED", Qt::CaseInsensitive)
                           || message.contains("TimeoutException",     Qt::CaseInsensitive)
                           || message.contains("disconnected",         Qt::CaseInsensitive)
                           || message.contains("DeviceLost",           Qt::CaseInsensitive);
    if (isDisconnect)
        emit cameraDisconnected();

    // Show a warning dialog AFTER emitting previewStopped so the main window
    // has already updated its state before the modal dialog blocks the event loop.
    QMessageBox::warning(this, "Preview Error", message);
}


// =============================================================================
// onWorkerFinished
// =============================================================================
void PreviewDialog::onWorkerFinished()
{
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    m_fpsTimer->stop();
    m_fpsLabel->setText("Display FPS: --");
    m_cameraFpsLabel->setText("Camera FPS: --");
    m_lineStatusLabel->setText("GPIO: --");

    // Don't overwrite an error message that onWorkerError() already set.
    if (!m_statusLabel->text().startsWith("ERROR:"))
    {
        m_statusLabel->setText("Stopped");
        m_statusLabel->setStyleSheet("color: #888; font-style: italic;");
    }
}


// =============================================================================
// onFpsTimerTick
// =============================================================================
void PreviewDialog::onFpsTimerTick()
{
    m_fpsLabel->setText(QString("Display FPS: %1").arg(m_frameCount));
    m_frameCount = 0;

    // During field captures the preview worker is stopped, so Camera FPS is not
    // reported via the worker's cameraFps signal.  Estimate it from how many
    // feedFrame calls arrived this second: each call covers ~10 camera frames
    // (we emit fieldPreviewReady every 10 frames in AcquisitionWorker).
    if (m_feedFrameCount > 0)
    {
        m_cameraFpsLabel->setText(QString("Camera FPS: ~%1").arg(m_feedFrameCount * 10));
        m_feedFrameCount = 0;
    }
}


// =============================================================================
// onCameraFps — update the camera acquisition rate label
// =============================================================================
//
// Called via Qt signal from the worker thread once per second.
// 'fps' is the count of complete frames the camera delivered in the last second,
// regardless of how many were dropped by the display throttle.
void PreviewDialog::onCameraFps(int fps)
{
    m_cameraFpsLabel->setText(QString("Camera FPS: %1").arg(fps));
}


// =============================================================================
// onLineStatusUpdated — update the GPIO line state label each display frame
// =============================================================================
//
// lineStatusAll is a bitmask: bit N = 1 means Line N is HIGH.
// We show the first 4 lines individually (L0–L3) which covers all Lucid GPIO
// lines, plus a hex value for the full bitmask.
void PreviewDialog::onLineStatusUpdated(int64_t lineStatusAll, bool available)
{
    if (!available)
    {
        m_lineStatusLabel->setText("GPIO: N/A");
        return;
    }

    // Individual line states (bits 0-3)
    const int l0 = (lineStatusAll >> 0) & 1;
    const int l1 = (lineStatusAll >> 1) & 1;
    const int l2 = (lineStatusAll >> 2) & 1;
    const int l3 = (lineStatusAll >> 3) & 1;

    m_lineStatusLabel->setText(
        QString("GPIO  L0:%1 L1:%2 L2:%3 L3:%4  (0x%5)")
            .arg(l0).arg(l1).arg(l2).arg(l3)
            .arg(static_cast<uint64_t>(lineStatusAll), 4, 16, QChar('0')));
}


// =============================================================================
// applyAutoLevels
// =============================================================================
//
// Reads the current histogram bins (already filled by updateHistogram() this frame)
// and moves both level handles to the requested percentile positions.
//
// lowPct  = 3.0  → black point at the 3rd  percentile (clips bottom 3% of pixels to black)
// highPct = 97.0 → white point at the 97th percentile (clips top  3% of pixels to white)
//
// setLevels() does NOT emit levelsChanged, so this won't trigger a second render pass.
// The caller (onNewFrame) applies applyDisplayPipeline() immediately after.
//
// C++ CONCEPT — structured bindings (C++17):
//   auto [lo, hi] = someFunction();
//   This unpacks the QPair returned by computePercentileLevels() into two named variables
//   in one line.  Requires C++17 (enabled in CMakeLists.txt).
// =============================================================================
void PreviewDialog::applyAutoLevels(double lowPct, double highPct)
{
    auto [lo, hi] = m_histogram->computePercentileLevels(lowPct, highPct);
    m_histogram->setLevels(lo, hi);
}


// =============================================================================
// onAutoContrastToggled
// =============================================================================
//
// Called when the user checks or unchecks the "Auto Contrast" button.
//
// When newly CHECKED: immediately apply auto-levels to the current frame so
// the display updates right away — no need to wait for the next camera frame.
//
// When UNCHECKED: do nothing.  The handles stay where they are; the user can
// drag them manually from that position.
// =============================================================================
void PreviewDialog::onAutoContrastToggled(bool checked)
{
    if (checked && !m_lastRawFrame.isNull())
    {
        applyAutoLevels(ConfigDialog::autoContrastLowPct(), ConfigDialog::autoContrastHighPct());
        m_imageView->setPixmap(QPixmap::fromImage(applyDisplayPipeline(m_lastRawFrame)));
    }
}


// =============================================================================
// onDrawRoiClicked — enter or exit ROI rectangle-draw mode
// =============================================================================
//
// Called when the Draw ROI button is toggled.
// Forwards the checked state to ZoomableImageWidget, which changes the cursor
// and routes mouse events to draw a rectangle instead of panning.
void PreviewDialog::onDrawRoiClicked(bool checked)
{
    // If the ROI menu action is checked while a drawn ROI is pending (user selecting
    // "Draw ROI Bounds" from the menu to re-draw), clear the pending state first.
    // The right-click "Re-draw ROI" path already calls exitRoiApplyState() before
    // setting the action, so this guard handles the menu-bar action path only.
    if (checked && m_roiPending)
    {
        exitRoiApplyState();
        emit roiCancelled();
    }

    m_imageView->setRoiDrawMode(checked);
    m_drawRoiAction->setText(checked ? "Cancel ROI" : "Draw ROI Bounds");
    m_drawRoiButton->setText(checked ? "Cancel ROI" : "Draw ROI");

    // If the user cancelled while in focus-ROI draw mode, restore both overlays.
    if (!checked && m_drawingForFocus)
    {
        m_drawingForFocus = false;
        if (!m_roiDisplayRect.isNull())
            m_imageView->setRoiOverlay(m_roiDisplayRect);
        if (!m_focusRoiDisplayRect.isNull())
            m_imageView->setFocusRoiOverlay(m_focusRoiDisplayRect);
    }
}


// =============================================================================
// enterRoiApplyState / exitRoiApplyState — toggle the button's dual personality
// =============================================================================
//
// After drawing:  non-checkable "Apply ROI" (blue) — left-click emits roiApplyRequested().
// At rest:        checkable "Draw ROI" toggle — toggled() enters/exits draw mode.
void PreviewDialog::enterRoiApplyState()
{
    m_roiPending = true;

    // Make non-checkable so toggled() doesn't fire and the draw-mode path is skipped.
    m_drawRoiButton->setCheckable(false);
    m_drawRoiButton->setText("Apply ROI");
    m_drawRoiButton->setStyleSheet(
        "QPushButton { min-height: 28px; font-size: 12px; padding: 4px 14px; "
        "              background-color: #2196F3; color: white; font-weight: bold; }"
        "QPushButton:disabled { background-color: #888; color: #ccc; }");
    m_drawRoiButton->setToolTip(
        "Click to apply the drawn ROI to the camera.\n"
        "Right-click to re-draw, cancel, or reset to full frame.");
}

void PreviewDialog::exitRoiApplyState()
{
    m_roiPending = false;

    m_drawRoiButton->setCheckable(true);
    m_drawRoiButton->setChecked(false);
    m_drawRoiButton->setText("Draw ROI");
    m_drawRoiButton->setStyleSheet(
        "QPushButton { min-height: 28px; font-size: 12px; padding: 4px 14px; "
        "              background-color: white; color: #222; }"
        "QPushButton:checked { background-color: #E65100; color: white; font-weight: bold; }"
        "QPushButton:disabled { background-color: #888; color: #ccc; }");
    m_drawRoiButton->setToolTip(
        "Left-click to draw an ROI rectangle on the image.\n"
        "Right-click for Reset ROI to Full Frame.");
}


// =============================================================================
// onRoiDrawn — convert drawn rect to absolute sensor coords and forward upstream
// =============================================================================
//
// The imageRect coordinates are relative to the frame currently displayed in the
// preview window.  If the camera already has a non-zero OffsetX/OffsetY (i.e. the
// sensor is already cropped), the drawn rect is in that sub-window's coordinate
// space.  We add the current offset back so MainWindow receives absolute sensor
// coordinates that can be written directly to OffsetX/OffsetY/Width/Height.
void PreviewDialog::onRoiDrawn(QRect imageRect)
{
    // Reset the action to its un-checked state without re-entering this slot.
    // blockSignals temporarily prevents toggled() from firing so we don't call
    // setRoiDrawMode() again (it was already called inside mouseReleaseEvent).
    m_drawRoiAction->blockSignals(true);
    m_drawRoiAction->setChecked(false);
    m_drawRoiAction->setText("Draw ROI Bounds");
    m_drawRoiAction->blockSignals(false);
    m_drawRoiButton->blockSignals(true);
    m_drawRoiButton->setChecked(false);
    m_drawRoiButton->blockSignals(false);

    // If this draw was for the focus diagnostic, store the rect and show the overlay.
    // Do NOT touch camera nodes or emit roiApplied.
    if (m_drawingForFocus)
    {
        m_drawingForFocus = false;
        m_focusRoiDisplayRect = imageRect;
        m_imageView->setFocusRoiOverlay(imageRect);
        // setRoiDrawMode(true) cleared the green camera ROI overlay — restore it.
        if (!m_roiDisplayRect.isNull())
            m_imageView->setRoiOverlay(m_roiDisplayRect);
        return;
    }

    // Scale imageRect from display-pixmap coordinates to native sensor coordinates.
    //
    // PreviewWorker downscales frames to ~2× the viewport size before emitting
    // newFrame(), so the QImage stored in ZoomableImageWidget is smaller than the
    // actual camera output.  widgetToImage() correctly maps widget pixels to
    // display-image pixels, but those are NOT 1:1 with sensor pixels.
    //
    // We recover the true sensor dimensions from the camera's Width/Height nodes
    // (which always equal the current output frame size) and scale the drawn rect
    // up by the ratio (nativeDim / displayDim) before any further processing.
    //
    // IMPORTANT: hoist displayW/nativeW so we can invert the scale when building
    // the confirmed-ROI overlay at the bottom (setRoiOverlay expects display coords).
    int displayW = 0, displayH = 0, nativeW = 0, nativeH = 0;
    if (!m_lastRawFrame.isNull() && m_mgr)
    {
        displayW = m_lastRawFrame.width();
        displayH = m_lastRawFrame.height();
        nativeW  = static_cast<int>(m_mgr->getInt64Value("Width",  displayW));
        nativeH  = static_cast<int>(m_mgr->getInt64Value("Height", displayH));

        if (displayW > 0 && displayH > 0 && (nativeW != displayW || nativeH != displayH))
        {
            double scaleX = static_cast<double>(nativeW) / displayW;
            double scaleY = static_cast<double>(nativeH) / displayH;
            imageRect = QRect(
                qRound(imageRect.x()      * scaleX),
                qRound(imageRect.y()      * scaleY),
                qRound(imageRect.width()  * scaleX),
                qRound(imageRect.height() * scaleY)
            );
        }
    }

    // Read current camera OffsetX/OffsetY so we can convert from sub-window
    // coordinates to absolute sensor coordinates.
    //
    // If no camera is connected or the nodes aren't readable, these default to 0
    // (which is the correct assumption — the frame IS the full sensor).
    int64_t currentOffsetX = m_mgr ? m_mgr->getInt64Value("OffsetX", 0) : 0;
    int64_t currentOffsetY = m_mgr ? m_mgr->getInt64Value("OffsetY", 0) : 0;

    int rawAbsX = static_cast<int>(currentOffsetX) + imageRect.x();
    int rawAbsY = static_cast<int>(currentOffsetY) + imageRect.y();
    int rawW    = imageRect.width();
    int rawH    = imageRect.height();

    // Snap Width and Height first — their valid range is [min, SensorDim], independent
    // of the current offset, so the standard snapInt64Value is correct for them.
    int w = m_mgr ? static_cast<int>(m_mgr->snapInt64Value("Width",  rawW)) : rawW;
    int h = m_mgr ? static_cast<int>(m_mgr->snapInt64Value("Height", rawH)) : rawH;

    // For OffsetX/OffsetY, the live node max equals (SensorDim - CurrentWidth), NOT
    // (SensorDim - targetWidth).  When the camera is streaming at full frame,
    // OffsetX.Max = 0, which causes snapInt64Value to clamp every offset to 0.
    // Fix: read the true sensor dimensions (WidthMax / HeightMax) and pass
    // (SensorDim - snappedW) as the correct upper bound.
    int64_t sensorW = m_mgr ? m_mgr->getInt64Value("WidthMax",  rawAbsX + w) : (rawAbsX + w);
    int64_t sensorH = m_mgr ? m_mgr->getInt64Value("HeightMax", rawAbsY + h) : (rawAbsY + h);
    int absX = m_mgr ? static_cast<int>(m_mgr->snapInt64ValueWithMax("OffsetX", rawAbsX, sensorW - w)) : rawAbsX;
    int absY = m_mgr ? static_cast<int>(m_mgr->snapInt64ValueWithMax("OffsetY", rawAbsY, sensorH - h)) : rawAbsY;

    // Show the snapped rectangle as a persistent green overlay in the preview so
    // the user can see the adjusted region before clicking Apply.
    // Convert absolute sensor coords back to display-frame pixel coords:
    //   1. Subtract currentOffset → sub-window coords (what the display image shows)
    //   2. Divide by the scale factor used above → back to display-image pixel space
    //      (setRoiOverlay expects the same coordinate space that roiDrawn() emitted)
    const double invScaleX = (nativeW > 0 && displayW > 0)
                             ? static_cast<double>(displayW) / nativeW : 1.0;
    const double invScaleY = (nativeH > 0 && displayH > 0)
                             ? static_cast<double>(displayH) / nativeH : 1.0;
    QRect snappedDisplayRect(
        qRound((absX - static_cast<int>(currentOffsetX)) * invScaleX),
        qRound((absY - static_cast<int>(currentOffsetY)) * invScaleY),
        qRound(w * invScaleX),
        qRound(h * invScaleY)
    );
    m_imageView->setRoiOverlay(snappedDisplayRect);
    m_roiDisplayRect = snappedDisplayRect;

    emit roiApplied(absX, absY, w, h);

    // Switch the Draw ROI button to "Apply ROI" mode so the user can apply the
    // drawn rectangle with a single click (or right-click to re-draw / cancel).
    enterRoiApplyState();
}


// =============================================================================
// onPixelClicked — show actual camera counts for a right-clicked pixel
// =============================================================================
//
// (imageX, imageY) are in the coordinate space of whatever pixmap is currently
// displayed — the same 8-bit downscaled image that PreviewWorker emitted.
//
// If the PreviewWorker is running we ask it for the raw camera count at those
// coordinates; it maps them back to native resolution internally.
// If the worker is not running (e.g. during a field capture feedFrame) we fall
// back to reading the 8-bit display value from m_lastRawFrame.
// =============================================================================
void PreviewDialog::onPixelClicked(int imageX, int imageY)
{
    // Bounds-check against the last displayed frame
    if (m_lastRawFrame.isNull()
        || imageX < 0 || imageY < 0
        || imageX >= m_lastRawFrame.width()
        || imageY >= m_lastRawFrame.height())
    {
        m_imageView->clearPixelOverlay();
        m_pinnedPixelX = -1;
        m_pinnedPixelY = -1;
        return;
    }

    m_pinnedPixelX = imageX;
    m_pinnedPixelY = imageY;

    uint32_t counts = 0;
    bool gotRaw = false;

    // Try to get the actual camera count from the worker's raw pixel cache.
    if (m_worker && m_worker->isRunning())
    {
        counts = m_worker->rawPixelAt(imageX, imageY);
        gotRaw = true;
    }

    if (!gotRaw)
    {
        // Worker not running (field capture preview) — read the 8-bit display value.
        // m_lastRawFrame is already Grayscale8 (confirmed in onNewFrame / feedFrame).
        if (m_lastRawFrame.format() == QImage::Format_Grayscale8)
            counts = m_lastRawFrame.constScanLine(imageY)[imageX];
        else
            counts = qGray(m_lastRawFrame.pixel(imageX, imageY));
    }

    QString label = QString("[%1, %2,  %3 cts]").arg(imageX).arg(imageY).arg(counts);
    m_imageView->setPixelOverlay(imageX, imageY, label);
}


// =============================================================================
// onOpenFocusDiagnostic
// =============================================================================
void PreviewDialog::onOpenFocusDiagnostic()
{
    if (!m_focusDialog)
    {
        m_focusDialog = new FocusDiagnosticDialog(this);
        connect(m_focusDialog, &FocusDiagnosticDialog::redrawRoiRequested,
                this, &PreviewDialog::onFocusRedrawRoiRequested);
        connect(m_focusDialog, &FocusDiagnosticDialog::dialogClosed, this, [this]()
        {
            m_imageView->clearFocusRoiOverlay();
            m_drawingForFocus = false;
        });
    }
    m_focusDialog->show();
    m_focusDialog->raise();
    m_focusDialog->activateWindow();

    // Auto-enter draw mode if no focus ROI is set yet.
    if (m_focusRoiDisplayRect.isNull())
        onFocusRedrawRoiRequested();
}


// =============================================================================
// onFocusRedrawRoiRequested — enter draw mode for the focus diagnostic ROI
// =============================================================================
void PreviewDialog::onFocusRedrawRoiRequested()
{
    // If a camera ROI was pending apply, cancel it before entering focus-draw mode.
    if (m_roiPending)
    {
        exitRoiApplyState();
        emit roiCancelled();
    }

    m_drawingForFocus = true;
    m_imageView->clearFocusRoiOverlay();

    m_drawRoiAction->blockSignals(true);
    m_drawRoiAction->setChecked(true);
    m_drawRoiAction->setText("Cancel ROI");
    m_drawRoiAction->blockSignals(false);
    m_drawRoiButton->blockSignals(true);
    m_drawRoiButton->setChecked(true);
    m_drawRoiButton->setText("Cancel ROI");
    m_drawRoiButton->blockSignals(false);

    m_imageView->setRoiDrawMode(true);

    // setRoiDrawMode(true) clears the green camera ROI overlay — restore it.
    if (!m_roiDisplayRect.isNull())
        m_imageView->setRoiOverlay(m_roiDisplayRect);
}


// =============================================================================
// closeEvent
// =============================================================================
void PreviewDialog::closeEvent(QCloseEvent* event)
{
    stopPreviewIfRunning();
    m_fpsTimer->stop();
    QMainWindow::closeEvent(event);
}
