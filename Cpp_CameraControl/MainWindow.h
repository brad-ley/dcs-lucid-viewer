// =============================================================================
// MainWindow.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares the MainWindow class — the main (and only) window of the application.
//
// UI LAYOUT:
//   The window is divided into four vertical sections:
//
//   ┌──────────────────────────────────────────────────┐
//   │ [1] Camera Selection                             │
//   │     [Refresh]  Camera: [dropdown]  [Connect]    │
//   │     Status: Not connected                        │
//   ├──────────────────────────────────────────────────┤
//   │ [2] Parameters  (disabled until connected)       │
//   │     <pinned params from registry>                │
//   │     e.g. "Pixel Format:", "Exposure Auto:", etc. │
//   │     [Apply Pinned Parameters]  [Advanced...]     │
//   ├──────────────────────────────────────────────────┤
//   │ [3] Acquisition  (disabled until connected)      │
//   │     Output:  [text field]        [Browse]        │
//   │     [Start Acquisition]   [Stop Acquisition]     │
//   │     Frames saved: 0       FPS: --               │
//   ├──────────────────────────────────────────────────┤
//   │ [4] Log                                          │
//   │     > System initialized                         │
//   │     > Camera connected                           │
//   └──────────────────────────────────────────────────┘
//
// PINNED PARAMETERS:
//   The Parameters panel shows a user-curated list stored in the Windows registry
//   (HKCU\Software\Brad Simplified\Lucid Camera Acquisition Tool\pinnedParams).
//   Users add/remove parameters using the ★ button in the Advanced Parameters dialog.
//   For each pinned node, the app queries the live camera to discover its type and
//   creates the right widget:
//     - Enumeration → QComboBox populated with the camera's valid options
//     - Float       → QDoubleSpinBox with camera-reported min/max
//     - Integer     → QSpinBox with camera-reported min/max/step
//     - Boolean     → QComboBox with "false" / "true"
//
// C++ CONCEPT — QMainWindow:
//   QMainWindow is Qt's standard top-level window class. It provides:
//     - A menu bar (optional)
//     - A status bar at the bottom
//     - A "central widget" area where you put your content
//   We subclass it to add our own widgets and logic.
// =============================================================================

#pragma once

// Qt main window base class
#include <QMainWindow>

// Qt layout classes — used to arrange widgets in rows/columns
#include <QVBoxLayout>   // Vertical box layout: stacks widgets top-to-bottom
#include <QHBoxLayout>   // Horizontal box layout: places widgets side-by-side
#include <QFormLayout>   // Form layout: label + widget pairs (like a form)
#include <QGridLayout>   // Grid layout: rows and columns

// Qt container widget
#include <QGroupBox>     // A labeled box that groups related widgets visually

// Qt interactive widgets
#include <QPushButton>   // Clickable button
#include <QComboBox>     // Dropdown list (select one of several options)
#include <QSpinBox>      // Integer spinner (click up/down or type a whole number)
#include <QDoubleSpinBox> // Float spinner (click up/down or type a decimal number)
#include <QLineEdit>     // Single-line text input field
#include <QTextEdit>     // Multi-line text display/input (used for the log)
#include <QLabel>        // Static text label
#include <QTimer>        // Fires events at regular intervals (for FPS calculation)

// Standard C++
#include <vector>
#include <string>
#include <chrono>        // std::chrono — high-resolution clock for FPS measurement

// Our own classes
#include "CameraManager.h"
#include "AcquisitionWorker.h"

// Forward declarations — avoids pulling in their headers (and all Arena SDK headers)
// into every file that includes MainWindow.h. We only store pointers here.
class PreviewDialog;
class PinnedParamsPanel;


// =============================================================================
// MainWindow class
// =============================================================================
class MainWindow : public QMainWindow
{
    Q_OBJECT  // Required for signals/slots

public:
    // Constructor. Parent is nullptr for a top-level window.
    explicit MainWindow(QWidget* parent = nullptr);

    // Destructor — declared virtual to match QMainWindow's virtual destructor
    ~MainWindow() override;

protected:
    // Intercept the window-close event so the preview dialog closes at the same
    // time as the main window instead of lingering on screen until the destructor runs.
    void closeEvent(QCloseEvent* event) override;

private slots:
    // C++ CONCEPT — slots:
    //   "private slots:" is a Qt section for "slot" methods.
    //   Slots are regular C++ functions that can be connected to signals.
    //   When a signal is emitted, its connected slots are called automatically.
    //   They're declared as 'private' because external code shouldn't call them directly.

    // --- Camera panel slots ---
    void onRefreshClicked();       // "Refresh" button pressed
    void onConnectClicked();       // "Connect" button pressed
    void onDisconnectClicked();    // "Disconnect" button pressed

    // --- Parameter panel slots ---
    void onAdvancedClicked();  // "Advanced..." button — opens AdvancedParamsDialog

    // --- Acquisition panel slots ---
    void onBrowseClicked();        // "Browse" button pressed (pick output folder)
    void onNotesClicked();         // "Notes..." button pressed (opens notes dialog)
    void onStartClicked();         // "Start Acquisition" button pressed
    void onStopClicked();          // "Stop Acquisition" button pressed
    void onPreviewClicked();       // "Preview" button pressed — opens PreviewDialog

    // --- Worker signal handlers ---
    // These slots are called when the AcquisitionWorker emits its signals.
    // Even though they run on the GUI thread, they're triggered by signals
    // from the worker thread — Qt routes them safely.
    void onFramesSaved(int count);
    void onAcquisitionError(const QString& message);
    void onWorkerStatus(const QString& message);
    void onWorkerFinished();       // Called when the worker thread ends (finished signal)

    // --- FPS timer slot ---
    void onFpsTimerTick();         // Called every second to update the FPS display

    // --- Field capture slots ---
    // Triggered from the Tools > Field Capture submenu.
    void onCaptureWhiteField();
    void onCaptureWhiteFieldPCA();
    void onCaptureDarkField();
    void onCaptureDotGrid();
    void onCaptureAmbient();
    void onCaptureCustomField();

    // --- ROI slots ---
    // Called when the user finishes drawing an ROI in the Preview window.
    // Pins the four ROI nodes if missing, refreshes the panel, and populates
    // the spinboxes with the drawn rectangle's coordinates.
    void onRoiApplied(int offsetX, int offsetY, int width, int height);

    // Called when the user chooses "Reset ROI to Full Frame" from the right-click
    // context menu on the Draw ROI button in the preview window.
    // Sets OffsetX=0, OffsetY=0 first, then Width=WidthMax, Height=HeightMax,
    // then refreshes the pinned params panel.
    void onResetRoiClicked();

    // Called when the user clicks the "Apply ROI" button in PreviewDialog.
    // Writes the pending ROI nodes to the camera in the correct order
    // (zero offsets → write size → write final offsets) and clears the pending state.
    void onRoiApplyRequested();

    // Called when the user cancels a pending ROI via right-click "Cancel ROI" in PreviewDialog.
    // Clears m_hasPendingRoi and resets the Apply Pinned Parameters button to white.
    void onRoiCancelled();

    // --- Auto-Stop slots ---
    // Called every second while an auto-stop acquisition is running.
    // Decrements the remaining-seconds counter and refreshes the Stop button label.
    void onAutoStopDisplayTick();

    // Called by the single-shot trigger timer when the full acquisition duration
    // has elapsed.  Requests a normal stop so the worker finishes cleanly.
    void onAutoStopTriggered();

private:
    // ----- Private helpers -----

    // Common implementation for white/dark field captures.
    // Validates camera state, builds the session folder name (white_field_... or
    // dark_field_...), configures the worker with a forced 5-second auto-stop,
    // wires the live-preview signal to the preview dialog if it is open, and starts
    // the worker thread.
    void startFieldCapture(AcquisitionWorker::FieldType ft);

    // ----- Private setup methods -----

    // Build and arrange all widgets in the window
    void buildUI();

    // Connect all signals to their slots (wires the UI together)
    void wireConnections();

    // Persist output folder and save format to the Windows registry via QSettings.
    // Called in the destructor so settings survive across app launches.
    void saveSettings();

    // Restore output folder and save format from the registry.
    // Called after buildUI() so it overrides the hardcoded defaults.
    void loadSettings();

    // ----- Private UI state helpers -----

    // Enable or disable the parameter control group
    void setParameterGroupEnabled(bool enabled);

    // Enable or disable the acquisition control group
    void setAcquisitionGroupEnabled(bool enabled);


    // Append a timestamped message to the log text area
    void log(const QString& message);

    // Update m_sessionNameEdit's placeholder text to show what the auto-generated
    // folder name would look like right now (e.g. "acq_20260604_143022").
    // Called at startup and after each acquisition completes so the example stays fresh.
    void updateSessionNamePlaceholder();

    // Set the Stop button's idle text and tooltip based on the current auto-stop config.
    // When auto-stop is enabled this shows "Stop (auto 10s)" even before acquisition starts
    // so the user can see at a glance that the feature is armed.
    // Call this whenever the button should reflect the saved config (startup, after the
    // config dialog closes, and when acquisition ends or is manually stopped).
    void refreshStopButton();

    // Set the Start button text based on whether Trigger Mode is on.
    //   TriggerMode Off → "Start Acquisition"  (software-initiated)
    //   TriggerMode On  → "Start Streaming"         (hardware trigger initiates acquisition)
    void updateStartButtonText();

    // ----- Camera section widgets -----
    QGroupBox*   m_cameraGroup;
    QComboBox*   m_cameraCombo;      // Lists discovered cameras
    QPushButton* m_refreshButton;    // Scan for cameras
    QPushButton* m_connectButton;    // Connect to selected camera
    QPushButton* m_disconnectButton; // Disconnect current camera
    QLabel*      m_connectionLabel;  // Shows "Connected: TRI028S-CC" etc.

    // ----- Parameter section widgets -----
    //
    // The parameter group box holds PinnedParamsPanel, which shows a
    // registry-driven list of parameters the user has starred (★) in the
    // Advanced Parameters Dialog. The Advanced button opens that dialog.
    QGroupBox*        m_paramGroup;
    QVBoxLayout*      m_paramOuterLayout;  // Permanent outer layout for m_paramGroup
    PinnedParamsPanel* m_pinnedPanel;      // Registry-driven pinned params widget
    QPushButton*      m_applyParamsButton; // Writes all pinned parameter values to the camera
    QPushButton*      m_advancedButton;    // Opens AdvancedParamsDialog for full node access
    QPushButton*      m_reorderButton;     // Opens drag-and-drop reorder dialog

    // ----- Acquisition section widgets -----
    QGroupBox*   m_acquisitionGroup;
    QComboBox*   m_saveFormatCombo;   // Selects save format (raw/tiff/video)
    QLineEdit*   m_outputPathEdit;    // Shows current output directory
    QLineEdit*   m_sessionNameEdit;   // Optional custom acquisition folder name
    QPushButton* m_notesButton;       // Opens the notes dialog
    QPushButton* m_browseButton;
    QPushButton* m_startButton;
    QPushButton* m_stopButton;
    QPushButton* m_previewButton;     // Opens the modeless PreviewDialog
    QLabel*      m_frameCountLabel;  // "Frames saved: 1234"
    QLabel*      m_fpsLabel;         // "FPS: 23.5"

    // Notes entered via the Notes dialog — saved to metadata.json each acquisition
    QString m_notesText;

    // ----- Log section -----
    QTextEdit*   m_logWidget;

    // ----- Backend objects -----
    CameraManager*     m_cameraManager;    // Manages camera SDK
    AcquisitionWorker* m_worker;           // Background acquisition thread
    PreviewDialog*     m_previewDialog;    // Lazily created modeless preview window; nullptr until first use

    // ----- FPS tracking -----
    QTimer* m_fpsTimer;                    // Fires every 1 second
    int     m_lastFrameCount;              // Frame count at last FPS measurement
    int     m_currentFrameCount;           // Total frames saved so far

    // ----- Auto-Stop -----
    //
    // Two timers work together to implement the timed auto-stop feature:
    //
    //   m_autoStopTriggerTimer  — single-shot, fires at the exact millisecond
    //                             duration the user configured.  When it fires,
    //                             it calls onAutoStopTriggered() which stops the
    //                             acquisition cleanly.
    //
    //   m_autoStopDisplayTimer  — 1-second repeating, fires while the trigger
    //                             timer is counting down.  Each tick decrements
    //                             m_autoStopSecondsRemaining and updates the Stop
    //                             button text to show the remaining seconds.
    //
    // Both timers are stopped in onStopClicked() and onWorkerFinished() so that
    // a manual stop also cancels any pending auto-stop.
    QTimer* m_autoStopTriggerTimer;        // Single-shot: fires when acquisition should end
    QTimer* m_autoStopDisplayTimer;        // 1-second tick: refreshes the Stop button countdown
    int     m_autoStopSecondsRemaining;    // Seconds left to display in the Stop button label
    int     m_autoStopPendingDurationMs;   // Duration stored at start; timers armed on first frame

    // ----- State -----
    // Store the list of discovered cameras so we can look up serial numbers by index
    std::vector<CameraInfo> m_discoveredCameras;

    // True while a white/dark field capture is running.
    // Used to relabel the "Frames saved" counter to "Frames averaged" in the UI
    // and to adjust the "Acquisition finished" log message.
    bool m_isFieldCapture;

    // When > 0, stop acquisition automatically once this many frames have been averaged.
    // Used by the "White field multi-frame" PCA capture. Reset to 0 after each run.
    int m_pcaFrameTarget;

    // PID of the cmd.exe that launched lucid_viewer (via lucid_viewer.bat).
    // Guards against double-launching while the window is still starting up.
    qint64 m_viewerPid = 0;

    // Native HWND of the lucid_viewer window once it has appeared.
    // Stored as qintptr so MainWindow.h doesn't need <windows.h>.
    // Used to refocus on subsequent "Image/Video Viewer" clicks.
    qintptr m_viewerHwnd = 0;

    // PID of the cmd.exe that launched Scope Control (via run-scope-control.bat).
    // Used to detect whether a launch is still in progress (guards double-clicks).
    qint64 m_scopeCmdPid = 0;

    // Native HWND of the Scope Control window once it has appeared.
    // Stored as qintptr so MainWindow.h doesn't need <windows.h>.
    // Used to refocus on subsequent "Open Scope Control" clicks.
    qintptr m_scopeHwnd = 0;

    // Checkable menu action for auto-stop enable/disable.
    // Stored so we can sync its check state after the settings dialog closes.
    QAction* m_autoStopMenuAction = nullptr;

    // ----- Pending ROI -----
    //
    // When the user draws (or resets) an ROI while the camera is streaming,
    // we can't write to the spinboxes or camera immediately — GenICam locks
    // Width/Height/OffsetX/OffsetY as read-only during streaming, and
    // refreshFromCamera() creates labels instead of spinboxes for those nodes.
    //
    // Instead we store the desired ROI here and apply it once streaming stops
    // (onWorkerFinished or previewStopped).  The Apply button is forced blue
    // to signal the pending state.
    bool m_hasPendingRoi     = false;
    int  m_pendingRoiOffsetX = 0;
    int  m_pendingRoiOffsetY = 0;
    int  m_pendingRoiWidth   = 0;
    int  m_pendingRoiHeight  = 0;

    // True while the PreviewDialog's internal preview thread is running.
    // Combined with m_worker->isRunning() in isCameraStreaming().
    bool m_previewIsRunning = false;

    // Returns true while any camera stream is active (acquisition worker or preview).
    bool isCameraStreaming() const;

    // Populate spinboxes with the pending ROI and force Apply blue.
    // Must only be called when isCameraStreaming() is false.
    void applyPendingRoiToPanel();
};
