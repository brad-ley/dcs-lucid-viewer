// =============================================================================
// MainWindow.cpp
// =============================================================================
//
// Implementation of the MainWindow class.
//
// KEY PATTERNS USED:
//   - Qt layouts (QVBoxLayout, QHBoxLayout, QFormLayout, QGroupBox)
//   - Signal/slot connections between widgets and our handler methods
//   - Enabling/disabling widgets based on application state
//   - Cross-thread communication via signals from AcquisitionWorker
//   - Dynamic parameter widgets built from a JSON configuration file
//   - GenApi node access for reading/writing camera parameters
// =============================================================================

#include "MainWindow.h"
#include "AdvancedParamsDialog.h"
#include "AutoStopDialog.h"
#include "ConfigDialog.h"
#include "PinnedParamsPanel.h"
#include "PreviewDialog.h"

// Qt file and system utilities
#include <QDateTime>        // Date/time for log timestamps
#include <QDesktopServices> // Open URLs in default browser
#include <QFileDialog>      // File browser dialog
#include <QMessageBox>      // Modal error/warning/info popups
#include <QPointer> // Weak pointer to QObject — guards against use-after-delete
#include <QProcess> // Launch external processes
#include <QProgressDialog> // Indeterminate busy dialog (range 0,0) for scope control startup
#include <QScrollBar>     // Scroll bar manipulation for auto-scrolling
#include <QSet>           // Hash set — used for process-tree PID lookup
#include <QStandardPaths> // Standard system paths (Documents, etc.)
#include <QUrl>           // URL handling
#include <tlhelp32.h> // CreateToolhelp32Snapshot — enumerate process tree for scope control
#include <windows.h> // OpenProcess, EnumWindows, SetForegroundWindow (viewer focus)

// Qt widgets and UI
#include <QAction>          // Menu actions
#include <QCloseEvent>      // QCloseEvent — for closeEvent override
#include <QDialog>          // Modal dialog base class
#include <QDialogButtonBox> // Standard Ok/Cancel buttons
#include <QIcon>            // Window/app icon
#include <QMenu>            // Menu items
#include <QMenuBar>         // Application menu bar
#include <QSizePolicy>      // Widget resize behavior
#include <QWidget>          // Base class for visual components

// Qt configuration and settings
#include <QCoreApplication> // Application-wide utilities
#include <QSettings>        // Persistent user settings (registry on Windows)
#include <QStorageInfo> // Disk free-space query — used for pre-acquisition size check

// Qt JSON support
#include <QFile>         // File I/O
#include <QJsonArray>    // JSON array
#include <QJsonDocument> // Parse/generate JSON
#include <QJsonObject>   // JSON object

// C++ standard library
#include <algorithm> // std::stable_sort
#include <climits>   // INT_MIN, INT_MAX constants
#include <cmath>     // std::ceil — used for auto-stop seconds rounding
#include <iomanip>   // std::setprecision for formatting
#include <memory> // std::make_shared — shared state across timer + dialog lambdas

// Debug output — visible in VS Output window even during hard crashes
#include <QDebug>

// =============================================================================
// Constructor
// =============================================================================
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_cameraManager(nullptr), m_worker(nullptr),
      m_fpsTimer(nullptr), m_lastFrameCount(0), m_currentFrameCount(0),
      m_paramOuterLayout(nullptr), m_pinnedPanel(nullptr),
      m_reorderButton(nullptr), m_previewDialog(nullptr),
      m_autoStopTriggerTimer(nullptr), m_autoStopDisplayTimer(nullptr),
      m_autoStopSecondsRemaining(0), m_autoStopPendingDurationMs(0),
      m_isFieldCapture(false) {
  setWindowTitle("Lucid Camera Acquisition Tool");
  setMinimumSize(600, 750); // Minimum window size in pixels

  // Set the app icon shown in the title bar and Windows taskbar.
  // ":/assets/app.ico" is a Qt resource path — the leading ":/" means
  // "look in the compiled-in resources", not the filesystem.
  // We use the .ico (multi-resolution) rather than .png because Windows
  // picks the best size automatically (16px for title bar, 32px for taskbar).
  setWindowIcon(QIcon(":/assets/app.ico"));

  // Create the camera manager and acquisition worker first,
  // because buildUI() may reference them.
  m_cameraManager = new CameraManager(); // 'new' allocates on the heap

  // Create the acquisition worker. 'this' as parent means Qt will
  // delete it automatically when MainWindow is deleted.
  m_worker = new AcquisitionWorker(this);

  // Build and display all widgets
  buildUI();

  // Restore output folder and save format from the registry.
  // Must come AFTER buildUI() (widgets must exist) and BEFORE wireConnections()
  // so the restored combo index doesn't fire any slots unexpectedly.
  loadSettings();

  // Wire signals to slots
  wireConnections();

  // Initialize the Arena SDK system
  std::string errorMsg;
  if (!m_cameraManager->initializeSystem(errorMsg)) {
    log("ERROR: Could not initialize Arena SDK: " +
        QString::fromStdString(errorMsg));
  } else {
    log("Arena SDK initialized. Click 'Refresh' to find cameras.");
  }

  // Disable controls that require a connected camera
  setParameterGroupEnabled(false);
  setAcquisitionGroupEnabled(false);
}

// =============================================================================
// Destructor
// =============================================================================
MainWindow::~MainWindow() {
  // Persist user preferences before the window closes
  saveSettings();

  // Stop acquisition if running
  if (m_worker && m_worker->isRunning()) {
    m_worker->requestStop();
    m_worker->wait(3000); // Wait up to 3 seconds
  }

  // Stop preview if running
  if (m_previewDialog) {
    m_previewDialog->stopPreviewIfRunning();
    delete m_previewDialog;
    m_previewDialog = nullptr;
  }

  // Clean up the camera manager (disconnects camera and closes Arena system)
  // C++ CONCEPT — 'delete':
  //   'new' allocates memory on the heap and returns a pointer.
  //   'delete' releases that memory. If you forget to delete, you have a
  //   "memory leak." Qt's parent system auto-deletes child QObjects, but
  //   CameraManager is NOT a QObject (it has no parent), so we delete it
  //   manually.
  delete m_cameraManager;
  m_cameraManager = nullptr;
  // m_worker is a child QObject of MainWindow, so Qt deletes it automatically.
}

// =============================================================================
// closeEvent — close the preview window at the same time as the main window
// =============================================================================
//
// Without this, the main window hides first and the destructor runs later,
// leaving the preview dialog visible as a stray window until deletion.
// Calling close() here ensures both windows disappear together.
void MainWindow::closeEvent(QCloseEvent *event) {
  if (m_previewDialog)
    m_previewDialog->close();

  QMainWindow::closeEvent(event);
}

// =============================================================================
// buildUI — construct all widgets and lay them out
// =============================================================================
void MainWindow::buildUI() {
  // In Qt, every window has one "central widget" that fills its content area.
  // We create a plain QWidget and set up a vertical layout inside it.
  QWidget *centralWidget = new QWidget(this);
  setCentralWidget(centralWidget);

  // QVBoxLayout stacks its children vertically (top to bottom).
  QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
  mainLayout->setSpacing(8); // Pixels between child widgets
  mainLayout->setContentsMargins(10, 10, 10, 10); // Margins around the edges

  // =========================================================================
  // SECTION 1: Camera Selection
  // =========================================================================
  //
  // QGroupBox draws a titled border around its children — good for grouping.
  m_cameraGroup = new QGroupBox("1. Camera Selection", centralWidget);
  QVBoxLayout *cameraLayout = new QVBoxLayout(m_cameraGroup);

  // Row 1: Refresh button + combo box + Connect/Disconnect
  QHBoxLayout *cameraRow = new QHBoxLayout();

  m_refreshButton = new QPushButton("Refresh", m_cameraGroup);
  m_refreshButton->setToolTip("Scan the network for cameras");

  m_cameraCombo = new QComboBox(m_cameraGroup);
  m_cameraCombo->setMinimumWidth(300);
  m_cameraCombo->setToolTip("Select a camera to connect to");
  // setSizePolicy controls how the widget expands when the window resizes.
  // Expanding + Preferred = stretch horizontally, fixed vertically.
  m_cameraCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  m_connectButton = new QPushButton("Connect", m_cameraGroup);
  m_disconnectButton = new QPushButton("Disconnect", m_cameraGroup);
  m_disconnectButton->setEnabled(false); // Greyed out until connected

  cameraRow->addWidget(m_refreshButton);
  cameraRow->addWidget(m_cameraCombo,
                       1); // '1' = stretch factor (takes remaining space)
  cameraRow->addWidget(m_connectButton);
  cameraRow->addWidget(m_disconnectButton);

  // Row 2: Connection status label
  m_connectionLabel = new QLabel("Status: Not connected", m_cameraGroup);
  m_connectionLabel->setStyleSheet("color: gray; font-style: italic;");

  cameraLayout->addLayout(cameraRow);
  cameraLayout->addWidget(m_connectionLabel);

  mainLayout->addWidget(m_cameraGroup);

  // =========================================================================
  // SECTION 2: Pinned Parameters
  // =========================================================================
  //
  // PinnedParamsPanel shows the registry-driven list of parameters the user
  // has pinned in the Advanced Parameters Dialog.
  // It is rebuilt (refreshFromCamera) each time a camera connects/disconnects.
  //
  // Stretch factor 3: gives this section much more vertical space than the log,
  // so many parameters can be visible without scrolling.
  m_paramGroup = new QGroupBox("2. Camera Parameters", centralWidget);
  m_paramOuterLayout = new QVBoxLayout(m_paramGroup);

  // The panel widget — owns its own scroll area and Apply button
  m_pinnedPanel = new PinnedParamsPanel(m_cameraManager, m_paramGroup);
  m_paramOuterLayout->addWidget(m_pinnedPanel);

  // Button row: "Advanced..." and "Edit list..." on the left; "Apply Pinned
  // Parameters" on the right
  m_advancedButton = new QPushButton("Advanced", m_paramGroup);
  m_advancedButton->setToolTip(
      "Browse and edit any camera parameter (full GenICam node tree).\n"
      "Use ★ in the Advanced dialog to pin parameters to this panel.");

  m_reorderButton = new QPushButton("Edit pinned", m_paramGroup);
  m_reorderButton->setToolTip(
      "Add, remove, or reorder pinned parameters in this panel.");

  m_applyParamsButton =
      new QPushButton("Apply Pinned Parameters", m_paramGroup);
  m_applyParamsButton->setToolTip(
      "Write all pinned parameter values to the camera");
  // Make Apply visually larger than the other buttons by increasing padding and
  // minimum width
  m_applyParamsButton->setStyleSheet("QPushButton { padding: 6px 18px; "
                                     "font-weight: bold; min-width: 180px; }");

  QHBoxLayout *paramButtonRow = new QHBoxLayout();
  paramButtonRow->addWidget(m_advancedButton);
  paramButtonRow->addWidget(m_reorderButton);
  paramButtonRow->addStretch();
  paramButtonRow->addWidget(m_applyParamsButton);
  m_paramOuterLayout->addLayout(paramButtonRow);

  // Stretch factor 3 → pinned panel takes 3× more vertical space than the log
  mainLayout->addWidget(m_paramGroup, 3);

  // =========================================================================
  // SECTION 3: Acquisition
  // =========================================================================
  m_acquisitionGroup = new QGroupBox("3. Acquisition", centralWidget);
  QVBoxLayout *acqLayout = new QVBoxLayout(m_acquisitionGroup);

  // QFormLayout is perfect for "Label: Widget" rows (like a settings panel)
  QFormLayout *acqForm = new QFormLayout();
  acqForm->setRowWrapPolicy(QFormLayout::DontWrapRows);
  acqForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

  // --- Save format ---
  m_saveFormatCombo = new QComboBox(m_acquisitionGroup);
  m_saveFormatCombo->addItem("Raw Video  (single .raw file, all frames)");
  m_saveFormatCombo->addItem(
      "TIFF Stack  (stack.tiff, all frames in one file)");
  m_saveFormatCombo->addItem("Raw Sequence  (.raw files, one per frame)");
  m_saveFormatCombo->setToolTip(
      "Choose the output format:\n"
      "  Raw Video: Single file with all frames concatenated (seekable, lowest "
      "disk usage)\n"
      "  TIFF Stack: All frames in a single multi-page stack.tiff\n"
      "  Raw Sequence: One .raw file per frame (fastest, uncompressed)");
  acqForm->addRow("Save format:", m_saveFormatCombo);

  // --- Acquisition name + Notes button ---
  m_sessionNameEdit = new QLineEdit(m_acquisitionGroup);
  // Placeholder is set by updateSessionNamePlaceholder() (called at end of
  // buildUI). It shows the auto-generated name that would be used if this field
  // is left blank.
  m_sessionNameEdit->setToolTip(
      "Custom folder name for this acquisition.\n"
      "Leave blank to auto-generate acq_YYYYMMDD_HHmmss.\n"
      "If the folder already exists, _2/_3/... is appended.");
  m_notesButton = new QPushButton("Notes...", m_acquisitionGroup);
  m_notesButton->setToolTip("Add free-form notes (sample ID, conditions, etc.) "
                            "saved to metadata.json");
  m_notesButton->setFixedWidth(80);
  QHBoxLayout *nameFieldRow = new QHBoxLayout();
  nameFieldRow->setContentsMargins(0, 0, 0, 0);
  nameFieldRow->addWidget(m_sessionNameEdit, 1);
  nameFieldRow->addWidget(m_notesButton);
  acqForm->addRow("Acq. name:", nameFieldRow);

  // --- Output folder + Browse button ---
  m_outputPathEdit = new QLineEdit(m_acquisitionGroup);
  m_outputPathEdit->setPlaceholderText("Select a folder to save frames...");
  m_outputPathEdit->setToolTip(
      "Frames will be saved here according to the selected format");
  // Default to the user's Documents/LucidCaptures folder
  m_outputPathEdit->setText(
      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
      "/LucidCaptures");
  m_browseButton = new QPushButton("Browse...", m_acquisitionGroup);
  QHBoxLayout *pathFieldRow = new QHBoxLayout();
  pathFieldRow->setContentsMargins(0, 0, 0, 0);
  pathFieldRow->addWidget(m_outputPathEdit, 1);
  pathFieldRow->addWidget(m_browseButton);
  acqForm->addRow("Output folder:", pathFieldRow);

  // --- Start / Stop buttons ---
  QHBoxLayout *controlRow = new QHBoxLayout();
  m_startButton = new QPushButton("Start Acquisition", m_acquisitionGroup);
  m_startButton->setStyleSheet(
      "QPushButton { background-color: #2E7D32; color: white; font-weight: "
      "bold; padding: 6px; }"
      "QPushButton:disabled { background-color: #888; color: #ccc; }");
  m_stopButton = new QPushButton("Stop Acquisition", m_acquisitionGroup);
  m_stopButton->setStyleSheet(
      "QPushButton { background-color: #B71C1C; color: white; font-weight: "
      "bold; padding: 6px; }"
      "QPushButton:disabled { background-color: #888; color: #ccc; }");
  m_stopButton->setEnabled(false);

  controlRow->addWidget(m_startButton);
  controlRow->addWidget(m_stopButton);

  // --- Preview button ---
  m_previewButton = new QPushButton("Preview", m_acquisitionGroup);
  m_previewButton->setStyleSheet(
      "QPushButton { background-color: #1565C0; color: white; font-weight: "
      "bold; padding: 6px; }"
      "QPushButton:disabled { background-color: #888; color: #ccc; }");
  m_previewButton->setToolTip(
      "Open a live preview window (no frames are saved)");
  controlRow->addWidget(m_previewButton);

  // --- Status labels ---
  QHBoxLayout *statusRow = new QHBoxLayout();
  m_frameCountLabel = new QLabel("Frames saved: 0", m_acquisitionGroup);
  m_fpsLabel = new QLabel("FPS: --", m_acquisitionGroup);
  statusRow->addWidget(m_frameCountLabel);
  statusRow->addStretch(); // Pushes FPS label to the right
  statusRow->addWidget(m_fpsLabel);

  acqLayout->addLayout(acqForm);
  acqLayout->addLayout(controlRow);
  acqLayout->addLayout(statusRow);

  mainLayout->addWidget(m_acquisitionGroup);

  // =========================================================================
  // SECTION 4: Log
  // =========================================================================
  QGroupBox *logGroup = new QGroupBox("Log", centralWidget);
  QVBoxLayout *logLayout = new QVBoxLayout(logGroup);

  m_logWidget = new QTextEdit(logGroup);
  m_logWidget->setReadOnly(true); // User can scroll/select but not type
  m_logWidget->setMinimumHeight(120);
  m_logWidget->setFont(
      QFont("Courier New", 9)); // Monospace font looks better for logs
  m_logWidget->setStyleSheet(
      "background-color: #1e1e1e; color: #d4d4d4;"); // Dark theme

  logLayout->addWidget(m_logWidget);
  mainLayout->addWidget(logGroup,
                        1); // '1' = stretch factor, log gets extra space

  // =========================================================================
  // FPS timer
  // =========================================================================
  m_fpsTimer = new QTimer(this);
  m_fpsTimer->setInterval(1000); // 1 second intervals

  // =========================================================================
  // Auto-Stop timers
  // =========================================================================
  //
  // Two timers cooperate to implement timed auto-stop:
  //
  //   m_autoStopTriggerTimer  — single-shot, fires at the exact millisecond
  //                             configured in AutoStopDialog.  On fire it calls
  //                             onAutoStopTriggered() which stops the
  //                             acquisition.
  //
  //   m_autoStopDisplayTimer  — 1-second repeating, runs while acquisition is
  //                             active.  Each tick updates the Stop button text
  //                             with the remaining seconds countdown.
  m_autoStopTriggerTimer = new QTimer(this);
  m_autoStopTriggerTimer->setSingleShot(true);

  m_autoStopDisplayTimer = new QTimer(this);
  m_autoStopDisplayTimer->setInterval(1000);

  // =========================================================================
  // MENU BAR
  // =========================================================================
  //
  // Create a menu bar with Help and Tools menus

  QMenuBar *menuBar = new QMenuBar(this);
  setMenuBar(menuBar);

  // --- Config menu ---
  QMenu *configMenu = menuBar->addMenu("Config");

  m_autoStopMenuAction = configMenu->addAction("Auto-Stop Acquisition");
  m_autoStopMenuAction->setCheckable(true);
  m_autoStopMenuAction->setChecked(AutoStopDialog::autoStopEnabled());
  m_autoStopMenuAction->setToolTip(
      "Enable or disable the timed auto-stop feature.\n"
      "Use \"Auto-Stop Settings...\" to configure the duration.");
  connect(m_autoStopMenuAction, &QAction::toggled, [this](bool checked) {
    AutoStopDialog::setAutoStopEnabled(checked);
    refreshStopButton();
  });

  QAction *autoStopSettingsAction =
      configMenu->addAction("Auto-Stop Settings...");
  connect(autoStopSettingsAction, &QAction::triggered, [this]() {
    AutoStopDialog dlg(this);
    dlg.exec();
    // Sync the checkable action and Stop button with whatever the user saved.
    m_autoStopMenuAction->setChecked(AutoStopDialog::autoStopEnabled());
    refreshStopButton();
  });

  // --- Tools menu ---
  QMenu *toolsMenu = menuBar->addMenu("Tools");

  QAction *viewerAction = toolsMenu->addAction("Image/Video Viewer");
  connect(viewerAction, &QAction::triggered, [this]() {
    // Refocus the existing window if we already know its HWND.
    HWND viewerHwnd = reinterpret_cast<HWND>(m_viewerHwnd);
    if (viewerHwnd && IsWindow(viewerHwnd) && IsWindowVisible(viewerHwnd)) {
      DWORD myTid = GetCurrentThreadId();
      DWORD targetTid = GetWindowThreadProcessId(viewerHwnd, nullptr);
      if (myTid != targetTid)
        AttachThreadInput(myTid, targetTid, TRUE);
      ShowWindow(viewerHwnd, SW_RESTORE);
      BringWindowToTop(viewerHwnd);
      SetForegroundWindow(viewerHwnd);
      if (myTid != targetTid)
        AttachThreadInput(myTid, targetTid, FALSE);
      return;
    }
    m_viewerHwnd = 0;

    // Guard against double-clicking while the window is still starting up.
    if (m_viewerPid != 0) {
      HANDLE h =
          OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(m_viewerPid));
      const bool alive = h && (WaitForSingleObject(h, 0) == WAIT_TIMEOUT);
      if (h)
        CloseHandle(h);
      if (alive)
        return;
      m_viewerPid = 0;
    }

    if (!QProcess::startDetached(
            "cmd.exe",
            QStringList() << "/c"
                          << "Z:\\6. Software\\prod_code\\LucidVisionCamera"
                             "\\lucid_viewer.bat",
            QString(), &m_viewerPid))
      return;

    // Poll until the Python window appears, then cache its HWND so subsequent
    // clicks can refocus it without re-launching.
    auto *pollTimer = new QTimer(this);
    auto elapsedMs = std::make_shared<int>(0);
    const DWORD rootPid = static_cast<DWORD>(m_viewerPid);

    connect(pollTimer, &QTimer::timeout, this,
            [this, pollTimer, elapsedMs, rootPid]() mutable {
              *elapsedMs += 500;

              // Walk the process tree rooted at cmd.exe (3 levels covers
              // conda → python).
              QSet<DWORD> pids;
              pids.insert(rootPid);
              HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
              if (snap != INVALID_HANDLE_VALUE) {
                for (int pass = 0; pass < 3; ++pass) {
                  PROCESSENTRY32W pe{};
                  pe.dwSize = sizeof(pe);
                  if (Process32FirstW(snap, &pe)) {
                    do {
                      if (pids.contains(pe.th32ParentProcessID))
                        pids.insert(pe.th32ProcessID);
                    } while (Process32NextW(snap, &pe));
                  }
                }
                CloseHandle(snap);
              }

              struct FindData {
                const QSet<DWORD> *pids;
                DWORD excludePid; // cmd.exe console — not the Python GUI
                HWND hwnd;
              };
              FindData fd{&pids, rootPid, nullptr};
              EnumWindows(
                  [](HWND hwnd, LPARAM lp) -> BOOL {
                    auto *fd = reinterpret_cast<FindData *>(lp);
                    DWORD winPid = 0;
                    GetWindowThreadProcessId(hwnd, &winPid);
                    if (winPid != fd->excludePid &&
                        fd->pids->contains(winPid) && IsWindowVisible(hwnd)) {
                      fd->hwnd = hwnd;
                      return FALSE;
                    }
                    return TRUE;
                  },
                  reinterpret_cast<LPARAM>(&fd));

              if (fd.hwnd) {
                m_viewerHwnd = reinterpret_cast<qintptr>(fd.hwnd);
                pollTimer->stop();
                pollTimer->deleteLater();
                return;
              }

              if (*elapsedMs >= 60000) {
                pollTimer->stop();
                pollTimer->deleteLater();
              }
            });
    pollTimer->start(500);
  });

  QAction *scopeControlAction = toolsMenu->addAction("Scope Control Software");
  connect(scopeControlAction, &QAction::triggered, [this]() {
    // Refocus if the window is already open.
    HWND scopeHwnd = reinterpret_cast<HWND>(m_scopeHwnd);
    if (scopeHwnd && IsWindow(scopeHwnd) && IsWindowVisible(scopeHwnd)) {
      DWORD myTid = GetCurrentThreadId();
      DWORD targetTid = GetWindowThreadProcessId(scopeHwnd, nullptr);
      if (myTid != targetTid)
        AttachThreadInput(myTid, targetTid, TRUE);
      ShowWindow(scopeHwnd, SW_RESTORE);
      BringWindowToTop(scopeHwnd);
      SetForegroundWindow(scopeHwnd);
      if (myTid != targetTid)
        AttachThreadInput(myTid, targetTid, FALSE);
      return;
    }
    m_scopeHwnd = 0;

    // Ignore if a launch is already in progress.
    if (m_scopeCmdPid != 0) {
      HANDLE h =
          OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(m_scopeCmdPid));
      const bool alive = h && (WaitForSingleObject(h, 0) == WAIT_TIMEOUT);
      if (h)
        CloseHandle(h);
      if (alive)
        return;
      m_scopeCmdPid = 0;
    }

    const QString batPath =
        R"(Z:\6. Software\prod_code\ScopeControl\run-scope-control.bat)";
    if (!QFile::exists(batPath)) {
      QMessageBox::warning(
          this, "Scope Control",
          "Batch file not found:\n" + batPath +
              "\n\nCheck that the Z: drive is mapped and the file exists.");
      return;
    }

    if (!QProcess::startDetached("cmd.exe", {"/c", batPath}, QString(),
                                 &m_scopeCmdPid)) {
      QMessageBox::warning(this, "Scope Control",
                           "Failed to launch:\n" + batPath);
      return;
    }

    // Indeterminate progress dialog — conda env activation can take 10-30 s.
    auto *dlg = new QProgressDialog(
        "Scope Control is starting\n(activating conda environment)...", "Hide",
        0, 0, this);
    dlg->setWindowTitle("Opening Scope Control");
    dlg->setWindowModality(Qt::NonModal);
    dlg->setMinimumDuration(0);
    dlg->setValue(0);
    dlg->show();

    QPointer<QProgressDialog> dlgPtr(dlg);
    auto *pollTimer = new QTimer(this);
    auto elapsedMs = std::make_shared<int>(0);
    const DWORD rootPid = static_cast<DWORD>(m_scopeCmdPid);

    // cleanup() is safe to call multiple times (QPointer + deleteLater
    // idempotency).
    auto cleanup = [dlgPtr, pollTimer]() {
      pollTimer->stop();
      pollTimer->deleteLater();
      if (dlgPtr) {
        dlgPtr->close();
        dlgPtr->deleteLater();
      }
    };

    // "Hide" button closes the dialog but leaves the launch running.
    connect(dlg, &QProgressDialog::canceled, this, cleanup);

    connect(pollTimer, &QTimer::timeout, this,
            [this, dlgPtr, pollTimer, elapsedMs, rootPid, cleanup]() mutable {
              *elapsedMs += 500;

              // Walk the process tree rooted at cmd.exe (3 levels covers conda
              // → python).
              QSet<DWORD> pids;
              pids.insert(rootPid);
              HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
              if (snap != INVALID_HANDLE_VALUE) {
                for (int pass = 0; pass < 3; ++pass) {
                  PROCESSENTRY32W pe{};
                  pe.dwSize = sizeof(pe);
                  if (Process32FirstW(snap, &pe)) {
                    do {
                      if (pids.contains(pe.th32ParentProcessID))
                        pids.insert(pe.th32ProcessID);
                    } while (Process32NextW(snap, &pe));
                  }
                }
                CloseHandle(snap);
              }

              // Find a visible top-level window owned by any process in the
              // tree.
              struct FindData {
                const QSet<DWORD> *pids;
                HWND hwnd;
              };
              FindData fd{&pids, nullptr};
              EnumWindows(
                  [](HWND hwnd, LPARAM lp) -> BOOL {
                    auto *fd = reinterpret_cast<FindData *>(lp);
                    DWORD winPid = 0;
                    GetWindowThreadProcessId(hwnd, &winPid);
                    if (fd->pids->contains(winPid) && IsWindowVisible(hwnd)) {
                      fd->hwnd = hwnd;
                      return FALSE;
                    }
                    return TRUE;
                  },
                  reinterpret_cast<LPARAM>(&fd));

              if (fd.hwnd) {
                m_scopeHwnd = reinterpret_cast<qintptr>(fd.hwnd);
                HWND hwnd = fd.hwnd;
                cleanup();
                DWORD myTid = GetCurrentThreadId();
                DWORD targetTid = GetWindowThreadProcessId(hwnd, nullptr);
                if (myTid != targetTid)
                  AttachThreadInput(myTid, targetTid, TRUE);
                ShowWindow(hwnd, SW_RESTORE);
                BringWindowToTop(hwnd);
                SetForegroundWindow(hwnd);
                if (myTid != targetTid)
                  AttachThreadInput(myTid, targetTid, FALSE);
                return;
              }

              if (*elapsedMs >= 60000) // 60-second timeout
                cleanup();
            });
    pollTimer->start(500);
  });

  toolsMenu->addSeparator();

  // Field capture actions — forced 5-second acquisition, streaming Welford
  // mean, saved as a single-page TIFF with outlier rejection.
  QAction *whiteFieldAction = toolsMenu->addAction("Capture White Field");
  whiteFieldAction->setToolTip(
      "Acquire 5 seconds of frames with the sample removed (or illuminated "
      "uniformly).\n"
      "Saves a per-pixel outlier-rejected mean as white_field_mean.tiff.");
  connect(whiteFieldAction, &QAction::triggered, this,
          &MainWindow::onCaptureWhiteField);

  QAction *darkFieldAction = toolsMenu->addAction("Capture Dark Field");
  darkFieldAction->setToolTip(
      "Acquire 5 seconds of frames with the lens capped / illumination off.\n"
      "Saves a per-pixel outlier-rejected mean as dark_field_mean.tiff.");
  connect(darkFieldAction, &QAction::triggered, this,
          &MainWindow::onCaptureDarkField);

  // toolsMenu->addSeparator();

  // --- Help menu ---
  QMenu *helpMenu = menuBar->addMenu("Help");

  QAction *opsGuideAction = helpMenu->addAction("User Operations Guide");
  connect(opsGuideAction, &QAction::triggered, [this]() {
    QString appDir = QCoreApplication::applicationDirPath();
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(appDir + "/assets/standard_operations.html"));
  });

  QAction *manualAction = helpMenu->addAction("Full Software Manual");
  connect(manualAction, &QAction::triggered, [this]() {
    QString appDir = QCoreApplication::applicationDirPath();
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(appDir + "/assets/manual.html"));
  });

  QAction *cameraManualAction = helpMenu->addAction("Camera Manual (ATX245S)");
  connect(cameraManualAction, &QAction::triggered, [this]() {
    QString appDir = QCoreApplication::applicationDirPath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        appDir + "/assets/ATX245S_1.21.0.0_Documentation_English/html/"
                 "welcome_page.html"));
  });

  QAction *aboutAction = helpMenu->addAction("About");
  connect(aboutAction, &QAction::triggered, [this]() {
    QMessageBox::information(
        this, "About",
        "Lucid Camera Acquisition Tool\n\n"
        "A Qt 6 application for controlling Lucid Vision Labs cameras\n"
        "and acquiring high-speed image streams.\n\n"
        "Uses Arena SDK for camera control and GenICam for parameter access.");
  });

  // Set the initial placeholder on the session name field
  updateSessionNamePlaceholder();

  // Set Stop button idle text to reflect the current auto-stop config.
  // If auto-stop is already enabled from a prior session, the button will
  // show "Stop (auto 10s)" immediately on launch rather than the generic label.
  refreshStopButton();
}

// =============================================================================
// wireConnections — connect all signals to slots
// =============================================================================
//
// C++ CONCEPT — Qt signal/slot connection syntax:
//   connect(sender, &SenderClass::signalName, receiver,
//   &ReceiverClass::slotName);
//
//   This is the "new-style" connection syntax (Qt 5+).
//   The compiler verifies that the signal and slot signatures match.
void MainWindow::wireConnections() {
  // --- Camera panel ---
  connect(m_refreshButton, &QPushButton::clicked, this,
          &MainWindow::onRefreshClicked);
  connect(m_connectButton, &QPushButton::clicked, this,
          &MainWindow::onConnectClicked);
  connect(m_disconnectButton, &QPushButton::clicked, this,
          &MainWindow::onDisconnectClicked);

  // --- Parameter panel ---

  // Apply button: reset to default style first (clears the blue "dirty"
  // highlight), then write the values.  refreshFromCamera() inside
  // applyPinnedParameters() rebuilds all widgets with fresh camera values, so
  // the button stays gray until the user edits again.
  connect(m_applyParamsButton, &QPushButton::clicked, this, [this]() {
    // If there is a pending ROI and the camera is still streaming, we cannot
    // write to the node map — tell the user and keep everything pending.
    if (m_hasPendingRoi && isCameraStreaming()) {
      QMessageBox::information(
          this, "ROI Pending",
          "Stop preview or acquisition before applying ROI changes.\n"
          "The drawn ROI is preserved and will be ready to apply when "
          "stopped.");
      return;
    }

    m_applyParamsButton->setStyleSheet(
        "QPushButton { padding: 6px 18px; font-weight: bold; min-width: 180px; "
        "}");
    m_pinnedPanel->applyPinnedParameters();
    m_hasPendingRoi = false;
    updateStartButtonText();
    // The camera's ROI has changed, so any drawn ROI overlay is now stale —
    // the new frame data starts at (0,0) regardless of the sensor offset.
    if (m_previewDialog)
      m_previewDialog->clearRoiOverlay();
  });

  // When any pinned widget value is changed by the user, highlight the Apply
  // button blue so they remember to apply before starting acquisition.
  connect(m_pinnedPanel, &PinnedParamsPanel::parameterChanged, this, [this]() {
    m_applyParamsButton->setStyleSheet(
        "QPushButton { padding: 6px 18px; font-weight: bold; min-width: 180px; "
        "              background-color: #2196F3; color: white; }");
  });

  connect(m_advancedButton, &QPushButton::clicked, this,
          &MainWindow::onAdvancedClicked);
  connect(m_reorderButton, &QPushButton::clicked, this,
          [this]() { m_pinnedPanel->showReorderDialog(this); });
  // Log any informational messages from the panel (e.g. "Skipped ExposureTime:
  // disabled")
  connect(m_pinnedPanel, &PinnedParamsPanel::statusMessage, this,
          &MainWindow::log);

  // When a parameter is live-applied (preview running, no Apply button needed),
  // refresh the Start button label — TriggerMode changes affect what the button
  // says.
  connect(m_pinnedPanel, &PinnedParamsPanel::liveNodeApplied, this,
          [this](const QString &) { updateStartButtonText(); });

  // --- Acquisition panel ---
  connect(m_notesButton, &QPushButton::clicked, this,
          &MainWindow::onNotesClicked);
  connect(m_browseButton, &QPushButton::clicked, this,
          &MainWindow::onBrowseClicked);
  connect(m_startButton, &QPushButton::clicked, this,
          &MainWindow::onStartClicked);
  connect(m_stopButton, &QPushButton::clicked, this,
          &MainWindow::onStopClicked);
  connect(m_previewButton, &QPushButton::clicked, this,
          &MainWindow::onPreviewClicked);

  // --- Worker signals → GUI slots ---
  // These connections cross thread boundaries. Qt detects this automatically
  // and uses "queued connection" mode: the slot runs on the GUI thread's event
  // loop (not the worker thread), so it's safe to touch GUI widgets here.
  connect(m_worker, &AcquisitionWorker::framesSaved, this,
          &MainWindow::onFramesSaved);
  connect(m_worker, &AcquisitionWorker::errorOccurred, this,
          &MainWindow::onAcquisitionError);
  connect(m_worker, &AcquisitionWorker::statusMessage, this,
          &MainWindow::onWorkerStatus);
  // QThread::finished is emitted when the thread's run() method returns
  connect(m_worker, &AcquisitionWorker::finished, this,
          &MainWindow::onWorkerFinished);
  // Start auto-stop timers only when the first frame actually arrives from the
  // camera, not when the Start button is pressed. This ensures the configured
  // duration is the real data duration — the ~0.5s stream setup overhead
  // doesn't count against it. Regular (non-single-shot) connection: the lambda
  // guards against double-firing by checking m_autoStopPendingDurationMs > 0
  // and zeroing it immediately, so repeated acquisitions all work correctly
  // without re-wiring.
  connect(m_worker, &AcquisitionWorker::firstFrameAcquired, this, [this]() {
    if (m_autoStopPendingDurationMs > 0) {
      m_autoStopTriggerTimer->start(m_autoStopPendingDurationMs);
      m_autoStopDisplayTimer->start();
      m_autoStopPendingDurationMs = 0;
    }
  });

  // --- FPS timer ---
  connect(m_fpsTimer, &QTimer::timeout, this, &MainWindow::onFpsTimerTick);

  // --- Auto-Stop timers ---
  connect(m_autoStopDisplayTimer, &QTimer::timeout, this,
          &MainWindow::onAutoStopDisplayTick);
  connect(m_autoStopTriggerTimer, &QTimer::timeout, this,
          &MainWindow::onAutoStopTriggered);
}

// =============================================================================
// SLOT: onRefreshClicked
// =============================================================================
void MainWindow::onRefreshClicked() {
  log("Scanning for cameras...");

  std::string errorMsg;
  m_discoveredCameras = m_cameraManager->discoverCameras(errorMsg, 1000);

  // Clear the combo box and repopulate it
  m_cameraCombo->clear();

  if (m_discoveredCameras.empty()) {
    log("No cameras found on the network.");
    m_cameraCombo->addItem("(No cameras available)");
  } else {
    log(QString("Found %1 camera(s).")
            .arg(static_cast<int>(m_discoveredCameras.size())));

    for (const auto &cam : m_discoveredCameras) {
      m_cameraCombo->addItem(QString::fromStdString(cam.displayName));
    }

    // Auto-select the most recently connected camera if it appears in the list.
    // This saves the user from re-selecting the same camera after every
    // refresh. The serial is stored in the registry by onConnectClicked().
    QSettings settings;
    QString preferredSerial = settings.value("lastConnectedSerial").toString();
    if (!preferredSerial.isEmpty()) {
      for (int i = 0; i < static_cast<int>(m_discoveredCameras.size()); ++i) {
        if (QString::fromStdString(m_discoveredCameras[i].serialNumber) ==
            preferredSerial) {
          m_cameraCombo->setCurrentIndex(i);
          break;
        }
      }
    }
  }
}

// =============================================================================
// SLOT: onConnectClicked
// =============================================================================
void MainWindow::onConnectClicked() {
  // Get the index of the selected camera in the combo box
  int idx = m_cameraCombo->currentIndex();
  if (idx < 0 || idx >= static_cast<int>(m_discoveredCameras.size())) {
    log("Please select a camera first.");
    return;
  }

  const std::string &serial = m_discoveredCameras[idx].serialNumber;
  log("Connecting to camera: " +
      QString::fromStdString(m_discoveredCameras[idx].displayName));

  std::string errorMsg;
  if (!m_cameraManager->connectCamera(serial, errorMsg)) {
    // Collapse newlines for the single-line log, keep them for the message box.
    QString errQ = QString::fromStdString(errorMsg);
    log("ERROR: " + errQ.replace('\n', ' ').simplified());
    QMessageBox::critical(this, "Connection Error",
                          QString::fromStdString(errorMsg));
    return;
  }

  // If ForceIP reassigned the camera's IP address, m_deviceInfoList was
  // refreshed inside tryForceIpConnect.  Sync m_discoveredCameras[idx] and the
  // combo item so the dropdown shows the real IP rather than the pre-ForceIP
  // address.
  CameraInfo updated;
  if (m_cameraManager->getUpdatedCameraInfo(serial, updated)) {
    m_discoveredCameras[idx] = updated;
    m_cameraCombo->setItemText(idx,
                               QString::fromStdString(updated.displayName));
  }

  log("Connected successfully.");
  m_connectionLabel->setText(
      "Connected: " +
      QString::fromStdString(m_discoveredCameras[idx].modelName));
  m_connectionLabel->setStyleSheet("color: green; font-weight: bold;");

  // Persist the serial so Refresh can pre-select this camera next time.
  QSettings settings;
  settings.setValue("lastConnectedSerial", QString::fromStdString(serial));

  m_connectButton->setEnabled(false);
  m_disconnectButton->setEnabled(true);
  m_cameraCombo->setEnabled(false);
  m_refreshButton->setEnabled(false);

  // Rebuild the pinned params panel with values from the newly connected camera
  m_pinnedPanel->refreshFromCamera();
  setParameterGroupEnabled(true);
  setAcquisitionGroupEnabled(true);
  updateStartButtonText();
  log("Camera parameters loaded.");

  // Enable live-apply: widget changes immediately write to the camera.
  // Disabled during acquisition (onStartClicked) and re-enabled when it
  // finishes.
  m_pinnedPanel->setLiveMode(true);
}

// =============================================================================
// SLOT: onDisconnectClicked
// =============================================================================
void MainWindow::onDisconnectClicked() {
  // Stop acquisition if it's running
  if (m_worker->isRunning()) {
    m_worker->requestStop();
    m_worker->wait(3000);
  }

  // Stop preview if it's running
  if (m_previewDialog) {
    m_previewDialog->stopPreviewIfRunning();
  }

  // Turn off live-apply before disconnecting — no camera to write to.
  m_pinnedPanel->setLiveMode(false);

  // Refresh pinned panel — shows "(not available)" for each row when
  // disconnected
  m_pinnedPanel->refreshFromCamera();

  m_cameraManager->disconnectCamera();
  log("Camera disconnected.");

  m_connectionLabel->setText("Status: Not connected");
  m_connectionLabel->setStyleSheet("color: gray; font-style: italic;");

  m_connectButton->setEnabled(true);
  m_disconnectButton->setEnabled(false);
  m_cameraCombo->setEnabled(true);
  m_refreshButton->setEnabled(true);

  setParameterGroupEnabled(false);
  setAcquisitionGroupEnabled(false);

  m_frameCountLabel->setText("Frames saved: 0");
  m_fpsLabel->setText("FPS: --");
  m_currentFrameCount = 0;
  m_lastFrameCount = 0;
}

// =============================================================================
// SLOT: onAdvancedClicked
// =============================================================================
void MainWindow::onAdvancedClicked() {
  // Open the Advanced Parameters dialog for full node browser access
  AdvancedParamsDialog dlg(m_cameraManager, this);

  // Wire the dialog's pinnedParamsChanged signal so the pinned panel refreshes
  // live while the dialog is open (e.g., when user clicks the pin button).
  connect(&dlg, &AdvancedParamsDialog::pinnedParamsChanged, m_pinnedPanel,
          &PinnedParamsPanel::refreshFromCamera);

  dlg.exec();

  // Refresh once more after the dialog closes to pick up any final changes
  if (m_cameraManager->isConnected()) {
    m_pinnedPanel->refreshFromCamera();
    updateStartButtonText();
  }
}

// =============================================================================
// SLOT: onPreviewClicked
// =============================================================================
void MainWindow::onPreviewClicked() {
  // Create the preview dialog on first use (lazy initialization)
  if (m_previewDialog == nullptr) {
    // Create the dialog without a parent ownership relationship
    // (modeless dialog, managed manually)
    // Pass nullptr as parent so Windows does not create an owner-owned
    // HWND relationship.  An owned window is always forced in front of its
    // owner by the OS, regardless of Qt window flags.  With nullptr, the
    // preview window is a fully independent top-level window and can be
    // moved behind the main window or minimized freely.
    // Lifetime is managed explicitly: the MainWindow destructor calls
    // stopPreviewIfRunning() and delete on m_previewDialog directly.
    m_previewDialog = new PreviewDialog(m_cameraManager, nullptr);

    // When the user finishes drawing an ROI in the preview window,
    // wire it back to onRoiApplied so the main params panel is updated.
    connect(m_previewDialog, &PreviewDialog::roiApplied, this,
            &MainWindow::onRoiApplied);

    // Right-click Reset ROI on the Draw ROI button in the preview window.
    connect(m_previewDialog, &PreviewDialog::resetRoiRequested, this,
            &MainWindow::onResetRoiClicked);

    // If the preview worker detects a camera disconnect, update the main window
    // connection state (disable UI, release device) exactly as if the user had
    // clicked Disconnect.
    connect(m_previewDialog, &PreviewDialog::cameraDisconnected, this,
            &MainWindow::onDisconnectClicked);

    // Track whether the preview thread is running so isCameraStreaming() is
    // accurate.
    connect(m_previewDialog, &PreviewDialog::previewStarted, this,
            [this]() { m_previewIsRunning = true; });
    connect(m_previewDialog, &PreviewDialog::previewStopped, this, [this]() {
      m_previewIsRunning = false;
      if (m_hasPendingRoi)
        applyPendingRoiToPanel();
    });

    // "Apply ROI" button in PreviewDialog: write only the four ROI nodes.
    connect(m_previewDialog, &PreviewDialog::roiApplyRequested, this,
            &MainWindow::onRoiApplyRequested);

    // "Cancel ROI" right-click: clear pending state and reset Apply button.
    connect(m_previewDialog, &PreviewDialog::roiCancelled, this,
            &MainWindow::onRoiCancelled);
  }

  // Show the dialog (non-blocking)
  m_previewDialog->show();
  m_previewDialog->raise();          // Bring to front
  m_previewDialog->activateWindow(); // Give it keyboard focus
}

// =============================================================================
// SLOT: onBrowseClicked
// =============================================================================
void MainWindow::onBrowseClicked() {
  // Open a folder browser dialog
  QString folder = QFileDialog::getExistingDirectory(
      this, "Select Output Folder", m_outputPathEdit->text(),
      QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

  if (!folder.isEmpty()) {
    m_outputPathEdit->setText(folder);
  }
}

// =============================================================================
// SLOT: onNotesClicked
// =============================================================================
void MainWindow::onNotesClicked() {
  // Open a modal dialog for editing acquisition notes
  QDialog notesDialog(this);
  notesDialog.setWindowTitle("Acquisition Notes");
  notesDialog.setMinimumSize(500, 250);

  QVBoxLayout *layout = new QVBoxLayout(&notesDialog);

  // Label explaining what notes are for
  QLabel *label =
      new QLabel("Enter free-form notes about this acquisition session:\n"
                 "(sample ID, lighting conditions, temperature, etc.)\n"
                 "These will be saved in metadata.json.");
  layout->addWidget(label);

  // Text editor for notes
  QTextEdit *notesEdit = new QTextEdit(&notesDialog);
  notesEdit->setPlainText(m_notesText);
  layout->addWidget(notesEdit);

  // OK / Cancel buttons
  QDialogButtonBox *buttonBox = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &notesDialog);
  connect(buttonBox, &QDialogButtonBox::accepted, &notesDialog,
          &QDialog::accept);
  connect(buttonBox, &QDialogButtonBox::rejected, &notesDialog,
          &QDialog::reject);
  layout->addWidget(buttonBox);

  if (notesDialog.exec() == QDialog::Accepted) {
    m_notesText = notesEdit->toPlainText();
    log("Notes updated.");
  }
}

// =============================================================================
// SLOT: onStartClicked
// =============================================================================
void MainWindow::onStartClicked() {
  if (!m_cameraManager->isConnected()) {
    log("No camera connected.");
    return;
  }

  if (m_worker->isRunning()) {
    log("Acquisition is already running.");
    return;
  }

  QString outputPath = m_outputPathEdit->text().trimmed();
  if (outputPath.isEmpty()) {
    log("Please select an output folder first.");
    return;
  }

  // =========================================================================
  // Pre-acquisition disk space check
  // =========================================================================
  //
  // Estimate how many bytes this acquisition will produce and warn the user
  // if that exceeds the free space on the output volume.
  //
  // Estimation inputs:
  //   frame size  — PayloadSize node (exact bytes per frame from firmware);
  //                 falls back to Width × Height × bytes-per-pixel if
  //                 unavailable
  //   frame rate  — AcquisitionFrameRate node when software-timed; or 1.25 GB/s
  //                 divided by frame size when externally triggered (worst-case
  //                 10 GigE saturation)
  //   duration    — auto-stop duration when enabled; 60 s assumed otherwise
  {
    // --- 1. Frame size in bytes ---
    // PayloadSize is the authoritative source — it includes any chunk data the
    // firmware appends (e.g., LineStatusAll timestamp).  Fall back to a
    // geometric estimate if the node is unavailable on this camera model.
    int64_t frameSizeBytes = m_cameraManager->getInt64Value("PayloadSize", 0);
    if (frameSizeBytes <= 0) {
      int64_t w = m_cameraManager->getInt64Value("Width", 2048);
      int64_t h = m_cameraManager->getInt64Value("Height", 2048);
      QString fmt =
          QString::fromStdString(m_cameraManager->getEnumValue("PixelFormat"));

      // Derive bits-per-pixel from the format name string.
      // GenICam format names embed the bit depth: Mono8, Mono16, BayerRG12,
      // etc.
      int bpp = 8;
      if (fmt.contains("16"))
        bpp = 16;
      else if (fmt.contains("12"))
        bpp = 12;
      else if (fmt.contains("10"))
        bpp = 10;

      frameSizeBytes = w * h * bpp / 8;
    }

    // Guard against a zero frame size so we don't divide by zero below
    if (frameSizeBytes <= 0)
      frameSizeBytes = 1;

    // --- 2. Effective frame rate ---
    // If the camera is in externally-triggered mode the user controls when
    // frames arrive — assume the worst case: the link is saturated at 10 GigE
    // speed (1.25 GB/s). Otherwise, read the camera's configured frame rate
    // directly.
    bool externallyTriggered =
        m_cameraManager->getEnumValue("TriggerMode") == "On" &&
        m_cameraManager->getEnumValue("TriggerSource") != "Software";

    double fps = 0.0;
    if (externallyTriggered) {
      // 10 GigE max throughput = 1.25 GB/s
      fps = 1'250'000'000.0 / static_cast<double>(frameSizeBytes);
    } else {
      fps = m_cameraManager->getDoubleValue("AcquisitionFrameRate", 30.0);
      if (fps <= 0.0)
        fps = 30.0;
    }

    // --- 3. Acquisition duration ---
    // Use auto-stop duration when it is configured; otherwise assume 60 s so
    // the user at least gets a heads-up for very large ROI / high frame rate
    // setups.
    bool isContinuous =
        (m_cameraManager->getNodeEnumString("AcquisitionMode") == "Continuous");
    bool autoStopOn = AutoStopDialog::autoStopEnabled() && isContinuous;
    double durationSec =
        autoStopOn ? AutoStopDialog::autoStopDurationMs() / 1000.0 : 60.0;

    // --- 4. Estimate and compare ---
    int64_t estimatedBytes = static_cast<int64_t>(
        fps * durationSec * static_cast<double>(frameSizeBytes));
    int64_t availableBytes = QStorageInfo(outputPath).bytesAvailable();

    if (availableBytes > 0 && estimatedBytes > availableBytes) {
      double estGB = estimatedBytes / 1.0e9;
      double freeGB = availableBytes / 1.0e9;

      // Build a detail line explaining each assumption so the user knows
      // which inputs to adjust if the estimate looks wrong.
      QString rateNote = externallyTriggered
                             ? " (assumed — external trigger, 10 GigE max)"
                             : "";
      QString durationNote = autoStopOn ? "" : " (assumed — auto-stop is off)";

      QString detail = QString("Frame size:  %1 MB\n"
                               "Frame rate:  %2 fps%3\n"
                               "Duration:    %4 s%5\n"
                               "Estimated:   %6 GB\n"
                               "Free space:  %7 GB")
                           .arg(frameSizeBytes / 1.0e6, 0, 'f', 2)
                           .arg(fps, 0, 'f', 1)
                           .arg(rateNote)
                           .arg(durationSec, 0, 'f', 0)
                           .arg(durationNote)
                           .arg(estGB, 0, 'f', 1)
                           .arg(freeGB, 0, 'f', 1);

      auto reply = QMessageBox::warning(
          this, "Disk Space Warning",
          QString("Estimated acquisition size (%1 GB) may exceed the free "
                  "space on the "
                  "output volume (%2 GB).\n\n%3\n\nStart acquisition anyway?")
              .arg(estGB, 0, 'f', 1)
              .arg(freeGB, 0, 'f', 1)
              .arg(detail),
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

      if (reply != QMessageBox::Yes)
        return;
    }
  }
  // =========================================================================

  // --- Resolve the session folder name ---
  QString resolvedName = m_sessionNameEdit->text().trimmed();
  if (!resolvedName.isEmpty()) {
    // Replace characters Windows doesn't allow in folder names.
    for (QChar ch : {'\\', '/', ':', '*', '?', '"', '<', '>', '|'})
      resolvedName.replace(ch, '_');

    // Always append a timestamp so each run produces a unique folder,
    // matching the behaviour of auto-generated names.
    resolvedName +=
        "_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");

    QString proposedPath = outputPath + "/" + resolvedName;
    if (QDir(proposedPath).exists()) {
      // Find the next available indexed name
      int counter = 2;
      QString candidate;
      do {
        candidate =
            outputPath + "/" + resolvedName + "_" + QString::number(counter++);
      } while (QDir(candidate).exists());

      QString indexedName = QDir(candidate).dirName();

      QMessageBox msgBox(this);
      msgBox.setWindowTitle("Folder Already Exists");
      msgBox.setText(
          QString("The folder \"%1\" already exists in the output directory.")
              .arg(resolvedName));
      msgBox.setInformativeText(
          "Overwriting mixes new frames with any existing files in that "
          "folder.\n"
          "Appending an index creates a fresh folder alongside it.");

      QPushButton *appendBtn = msgBox.addButton(
          QString("Save to \"%1/\"").arg(indexedName), QMessageBox::AcceptRole);
      QPushButton *overwriteBtn = msgBox.addButton(
          "Overwrite existing folder", QMessageBox::DestructiveRole);
      QPushButton *cancelBtn =
          msgBox.addButton("Cancel", QMessageBox::RejectRole);

      msgBox.setDefaultButton(appendBtn);
      msgBox.exec();

      if (msgBox.clickedButton() == cancelBtn)
        return;
      if (msgBox.clickedButton() == appendBtn)
        resolvedName = indexedName;
    }
  }

  // --- Snapshot current pinned parameter values into the acquisition metadata
  // ---
  //
  // PinnedParamsPanel::currentValuesJson() reads each visible widget and
  // returns a JSON object (node name → current value). This gets embedded in
  // metadata.json so the acquisition record shows exactly what settings were
  // active at capture time.
  QString paramsJson = QJsonDocument(m_pinnedPanel->currentValuesJson())
                           .toJson(QJsonDocument::Compact);

  // --- Determine the selected save format ---
  AcquisitionWorker::SaveFormat fmt;
  switch (m_saveFormatCombo->currentIndex()) {
  case 1:
    fmt = AcquisitionWorker::SaveFormat::TiffStack;
    break;
  case 2:
    fmt = AcquisitionWorker::SaveFormat::RawSequence;
    break;
  default:
    fmt = AcquisitionWorker::SaveFormat::RawVideo;
    break;
  }

  // Disable live-apply during acquisition — the device stream owns the node
  // map.
  m_pinnedPanel->setLiveMode(false);

  // Stop preview BEFORE snapshotting the node map — both would access GenApi
  // concurrently (GenApi is not thread-safe), which can crash inside the SDK.
  if (m_previewDialog) {
    m_previewDialog->stopPreviewIfRunning();
    m_previewDialog->setAcquisitionRunning(true);
  }

  // Configure and start the worker thread
  m_worker->setDevice(m_cameraManager->getDevice());
  m_worker->setOutputPath(outputPath);
  m_worker->setSaveFormat(fmt);
  m_worker->setFieldType(
      AcquisitionWorker::FieldType::None); // Normal acquisition
  m_worker->setCustomSessionName(resolvedName);
  m_worker->setNotes(m_notesText);
  m_worker->setCameraParamsJson(paramsJson); // Pass params JSON to worker
  m_worker->setNodeMapSnapshotJson(
      AcquisitionWorker::buildCameraSettingsJson(m_cameraManager->getDevice()));

  // Disconnect any field-preview connection left over from a previous field
  // capture
  disconnect(m_worker, &AcquisitionWorker::fieldPreviewReady, nullptr, nullptr);

  // Reset frame count display
  m_currentFrameCount = 0;
  m_lastFrameCount = 0;
  m_frameCountLabel->setText("Frames saved: 0");
  m_fpsLabel->setText("FPS: 0.0");

  m_worker->start();

  // Update UI state
  m_startButton->setEnabled(false);
  m_stopButton->setEnabled(true);
  m_previewButton->setEnabled(false); // Disable preview during acquisition
  m_disconnectButton->setEnabled(false);
  m_saveFormatCombo->setEnabled(false);

  m_fpsTimer->start();

  // --- Arm auto-stop countdown if the feature is enabled AND mode is
  // Continuous ---
  //
  // Auto-Stop is a time-based mechanism that only makes sense for Continuous
  // mode. SingleFrame and MultiFrame acquisitions are controlled by frame
  // count, not time, so the countdown timer would be misleading and would cut
  // them off prematurely.
  //
  // We use two timers so the stop fires at exactly the right millisecond
  // (trigger timer) while the button text updates every second (display timer).
  const bool isContinuous =
      (m_cameraManager->getNodeEnumString("AcquisitionMode") == "Continuous");

  if (AutoStopDialog::autoStopEnabled() && isContinuous) {
    double durationMs = AutoStopDialog::autoStopDurationMs();

    // Compute how many whole seconds to show in the countdown.
    // std::ceil rounds 0.5 s → 1 s so very short durations still show "1s".
    m_autoStopSecondsRemaining =
        static_cast<int>(std::ceil(durationMs / 1000.0));

    // Set Stop button text and tooltip to show the countdown
    QString btnText =
        QString("Stop (auto %1s)").arg(m_autoStopSecondsRemaining);
    QString btnTooltip =
        QString("Automatically stopping in %1 seconds, as defined using\n"
                "Config → Auto-Stop Acquisition")
            .arg(m_autoStopSecondsRemaining);
    m_stopButton->setText(btnText);
    m_stopButton->setToolTip(btnTooltip);
    m_stopButton->setStyleSheet(
        "QPushButton { background-color: #E65100; color: white; font-weight: "
        "bold; "
        "              padding: 6px; min-width: 160px; }"
        "QPushButton:disabled { background-color: #888; color: #ccc; }");

    // Timers are NOT started here — they are armed on firstFrameAcquired so
    // the duration counts from the moment data arrives, not button press.
    m_autoStopPendingDurationMs = static_cast<int>(durationMs);
  }

  log("Acquisition started. Saving to: " + outputPath + "/" +
      (resolvedName.isEmpty() ? QString("acq_<timestamp>") : resolvedName));
}

// =============================================================================
// SLOT: onStopClicked
// =============================================================================
void MainWindow::onStopClicked() {
  if (!m_worker->isRunning()) {
    return;
  }

  // Cancel any armed auto-stop countdown so a manual stop also disables the
  // timers.
  m_autoStopTriggerTimer->stop();
  m_autoStopDisplayTimer->stop();

  // Reset Stop button appearance (it may have been showing a live countdown).
  // refreshStopButton() sets the idle text — "Stop (auto Xs)" if auto-stop is
  // still configured, or "Stop Acquisition" if it is disabled.
  refreshStopButton();
  m_stopButton->setStyleSheet(
      "QPushButton { background-color: #B71C1C; color: white; font-weight: "
      "bold; padding: 6px; }"
      "QPushButton:disabled { background-color: #888; color: #ccc; }");

  log("Stopping acquisition...");
  m_worker->requestStop();
  // Don't call wait() here — it would block the GUI until the thread stops.
  // Instead, we wait for the 'finished' signal (connected to onWorkerFinished).
  m_stopButton->setEnabled(false);
}

// =============================================================================
// SLOT: onFramesSaved — called each time a frame is saved by the worker
// =============================================================================
void MainWindow::onFramesSaved(int count) {
  m_currentFrameCount = count;
  const QString label =
      m_isFieldCapture ? "Frames averaged: %1" : "Frames saved: %1";
  m_frameCountLabel->setText(label.arg(count));
}

// =============================================================================
// SLOT: onAcquisitionError
// =============================================================================
void MainWindow::onAcquisitionError(const QString &message) {
  log("ERROR: " + message);
  QMessageBox::critical(this, "Acquisition Error", message);
}

// =============================================================================
// SLOT: onWorkerStatus
// =============================================================================
void MainWindow::onWorkerStatus(const QString &message) { log(message); }

// =============================================================================
// SLOT: onWorkerFinished
// =============================================================================
void MainWindow::onWorkerFinished() {
  m_fpsTimer->stop();

  // Stop auto-stop timers in case the worker ended due to an error or
  // the trigger timer fired and the display timer is still ticking.
  m_autoStopTriggerTimer->stop();
  m_autoStopDisplayTimer->stop();

  m_startButton->setEnabled(true);
  m_stopButton->setEnabled(false);
  m_previewButton->setEnabled(true); // Re-enable preview
  m_disconnectButton->setEnabled(true);
  m_saveFormatCombo->setEnabled(true);

  // Restore Stop button to its idle state (it may have shown a live countdown).
  // refreshStopButton() will show "Stop (auto Xs)" if auto-stop is configured,
  // or "Stop Acquisition" if it is disabled — same as the pre-acquisition
  // state.
  refreshStopButton();
  m_stopButton->setStyleSheet(
      "QPushButton { background-color: #B71C1C; color: white; font-weight: "
      "bold; padding: 6px; }"
      "QPushButton:disabled { background-color: #888; color: #ccc; }");

  m_fpsLabel->setText("FPS: --");
  m_frameCountLabel->setText("Frames saved: 0"); // Reset to default label text

  // Tell preview dialog that acquisition is no longer running
  if (m_previewDialog) {
    m_previewDialog->setAcquisitionRunning(false);
  }

  // Log the total frame count together with the actual folder path.
  QString savedPath = m_worker->sessionPath();
  savedPath.replace('\\', '/');
  if (m_isFieldCapture) {
    log(QString("Field capture finished — %1 frames averaged, 1 TIFF saved to: "
                "'%2'")
            .arg(m_currentFrameCount)
            .arg(savedPath));
  } else {
    log(QString("Acquisition finished — %1 frames saved to: '%2'")
            .arg(m_currentFrameCount)
            .arg(savedPath));
  }
  m_isFieldCapture = false;

  // Re-enable live-apply now that the stream has ended and the node map is
  // free.
  m_pinnedPanel->setLiveMode(true);

  // If an ROI was drawn while streaming, populate the spinboxes now that
  // GenICam has unlocked the ROI nodes.
  if (m_hasPendingRoi)
    applyPendingRoiToPanel();

  // Refresh the placeholder to show the next auto-generated name (new
  // timestamp)
  updateSessionNamePlaceholder();
}

// =============================================================================
// SLOT: onFpsTimerTick — called every second, updates the FPS display
// =============================================================================
void MainWindow::onFpsTimerTick() {
  // FPS = frames saved in the last second
  int framesThisSecond = m_currentFrameCount - m_lastFrameCount;
  m_lastFrameCount = m_currentFrameCount;

  m_fpsLabel->setText(QString("FPS: %1").arg(framesThisSecond));
}

// =============================================================================
// HELPER: setParameterGroupEnabled
// =============================================================================
void MainWindow::setParameterGroupEnabled(bool enabled) {
  // QGroupBox::setEnabled(false) disables the group AND all its child widgets.
  // This is the Qt way to grey out an entire section.
  m_paramGroup->setEnabled(enabled);
}

// =============================================================================
// HELPER: setAcquisitionGroupEnabled
// =============================================================================
void MainWindow::setAcquisitionGroupEnabled(bool enabled) {
  m_acquisitionGroup->setEnabled(enabled);
}

// =============================================================================
// saveSettings — persist user preferences to the registry
// =============================================================================
void MainWindow::saveSettings() {
  // QSettings automatically maps to the Windows registry based on
  // QApplication::organizationName() and applicationName() set in main.cpp.
  // On Windows: HKCU\Software\<org>\<app>
  QSettings settings;

  // Save the output folder path
  settings.setValue("outputPath", m_outputPathEdit->text());

  // Save the selected save format (0 = Raw, 1 = TIFF, 2 = Raw Video)
  settings.setValue("saveFormat", m_saveFormatCombo->currentIndex());
}

// =============================================================================
// loadSettings — restore user preferences from the registry
// =============================================================================
void MainWindow::loadSettings() {
  QSettings settings;

  // Load output folder path.
  // Priority:
  //   1. Registry value (user previously chose a path) — use it as-is.
  //   2. D:\ drive present  — default to D:\LucidCaptures (fast data drive
  //   convention).
  //   3. Fallback           — Documents\LucidCaptures on whatever drive holds
  //   Documents.
  QString outputPath;
  if (settings.contains("outputPath")) {
    outputPath = settings.value("outputPath").toString();
  } else {
    // Check whether D:\ is present by asking Qt if the path exists.
    // QDir(path).exists() returns true only when the drive is mounted and
    // accessible.
    const QString dDrive = "D:/LucidCaptures";
    if (QDir("D:/").exists()) {
      outputPath = dDrive;
    } else {
      outputPath =
          QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
          "/LucidCaptures";
    }
  }
  m_outputPathEdit->setText(outputPath);

  // Load save format (default to index 0 = Raw Sequence)
  int saveFormat = settings.value("saveFormat", 0).toInt();
  if (saveFormat >= 0 && saveFormat < m_saveFormatCombo->count()) {
    m_saveFormatCombo->setCurrentIndex(saveFormat);
  }
}

// =============================================================================
// updateSessionNamePlaceholder — show the next auto-generated folder name
// =============================================================================
//
// The session name field is optional.  When left blank, the worker generates a
// folder name of the form acq_YYYYMMDD_HHmmss.  Showing that name in the
// placeholder text answers the user's "where will my data go?" question without
// them having to type anything.
//
// We recompute the placeholder after every acquisition so it always reflects
// the current wall-clock time rather than the time the app was launched.
void MainWindow::updateSessionNamePlaceholder() {
  // Build what the auto-generated name would look like if Start were pressed
  // now. The worker uses the same format string — keep them in sync.
  QString autoName =
      "acq_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");

  m_sessionNameEdit->setPlaceholderText("Type here or auto-generate: '" +
                                        autoName + "'");
}

// =============================================================================
// SLOT: onAutoStopDisplayTick — called every second during a timed acquisition
// =============================================================================
//
// Decrements the remaining-seconds counter and refreshes the Stop button label
// so the user sees a live countdown: "Stop (auto 5s)", "Stop (auto 4s)", etc.
// The display timer keeps running until onStopClicked() or onWorkerFinished()
// stops it, which handles both manual stops and the trigger-timer firing.
void MainWindow::onAutoStopDisplayTick() {
  if (m_autoStopSecondsRemaining > 0)
    --m_autoStopSecondsRemaining;

  QString btnText = QString("Stop (auto %1s)").arg(m_autoStopSecondsRemaining);
  QString btnTooltip =
      QString("Automatically stopping in %1 seconds, as defined using\n"
              "Config → Auto-Stop Acquisition")
          .arg(m_autoStopSecondsRemaining);

  m_stopButton->setText(btnText);
  m_stopButton->setToolTip(btnTooltip);
}

// =============================================================================
// SLOT: onAutoStopTriggered — called by the single-shot trigger timer
// =============================================================================
//
// The trigger timer fires at the exact millisecond the user configured.
// We log the event and delegate to onStopClicked() so the acquisition stops
// cleanly (same path as a manual stop — worker finishes its current frame).
void MainWindow::onAutoStopTriggered() {
  log("Auto-Stop triggered — stopping acquisition.");
  onStopClicked();
}

// =============================================================================
// refreshStopButton — sync the Stop button's idle text with the saved config
// =============================================================================
//
// Called at startup, after the auto-stop config dialog closes, and whenever
// acquisition ends so the button always reflects the current setting even when
// no acquisition is running.  During an active countdown this method is NOT
// called — the display timer owns the text then.
void MainWindow::refreshStopButton() {
  // Show the auto-stop countdown hint when auto-stop is enabled AND the camera
  // is in Continuous mode (or no camera is connected yet — assume Continuous as
  // the default so the hint is visible at startup and before the first camera
  // connect). SingleFrame / MultiFrame acquisitions end by frame count, so we
  // suppress the time-based hint for those once a camera is actually connected.
  const QString acqMode = m_cameraManager->getNodeEnumString("AcquisitionMode");
  const bool isContinuous = acqMode.isEmpty() || acqMode == "Continuous";

  if (AutoStopDialog::autoStopEnabled() && isContinuous) {
    double durationMs = AutoStopDialog::autoStopDurationMs();
    int seconds = static_cast<int>(std::ceil(durationMs / 1000.0));
    m_stopButton->setText(QString("Stop (auto %1s)").arg(seconds));
    m_stopButton->setToolTip(
        QString("Auto-Stop is configured for %1 s.\n"
                "Change or disable it via Config → Auto-Stop Acquisition")
            .arg(seconds));
  } else {
    m_stopButton->setText("Stop Acquisition");
    m_stopButton->setToolTip(QString());
  }
}

// =============================================================================
// updateStartButtonText — reflect TriggerMode in the Start button label
// =============================================================================
void MainWindow::updateStartButtonText() {
  if (!m_cameraManager->isConnected())
    return;

  const bool triggerOn = m_cameraManager->getEnumValue("TriggerMode") == "On";
  m_startButton->setText(triggerOn ? "Start Streaming" : "Start Acquisition");
}

// =============================================================================
// SLOT: onRoiApplied — called when the user finishes drawing an ROI in Preview
// =============================================================================
//
// PreviewDialog emits roiApplied(offsetX, offsetY, width, height) with absolute
// sensor coordinates (Preview's frame offset already added in).
//
// If the camera is idle we populate the spinboxes immediately and turn Apply
// blue. If the camera is streaming (GenICam locks ROI nodes as read-only), we
// store the ROI as pending and force Apply blue — the spinboxes are populated
// once streaming stops (onWorkerFinished / previewStopped).
void MainWindow::onRoiApplied(int offsetX, int offsetY, int width, int height) {
  // Pin the four ROI nodes if they are not already in the list.
  // addPinnedNode() is a no-op when the node is already present.
  PinnedParamsPanel::addPinnedNode("OffsetX");
  PinnedParamsPanel::addPinnedNode("OffsetY");
  PinnedParamsPanel::addPinnedNode("Width");
  PinnedParamsPanel::addPinnedNode("Height");

  m_hasPendingRoi = true;
  m_pendingRoiOffsetX = offsetX;
  m_pendingRoiOffsetY = offsetY;
  m_pendingRoiWidth = width;
  m_pendingRoiHeight = height;

  if (isCameraStreaming()) {
    // Force Apply blue now even though the spinboxes can't be set yet.
    m_applyParamsButton->setStyleSheet(
        "QPushButton { padding: 6px 18px; font-weight: bold; min-width: 180px; "
        "              background-color: #2196F3; color: white; }");
    log(QString("ROI drawn: OffsetX=%1, OffsetY=%2, Width=%3, Height=%4 — stop "
                "preview/acquisition to apply.")
            .arg(offsetX)
            .arg(offsetY)
            .arg(width)
            .arg(height));
  } else {
    applyPendingRoiToPanel();
  }
}

// =============================================================================
// SLOT: onResetRoiClicked — set ROI back to full sensor size
// =============================================================================
//
// Called from the right-click context menu on the Draw ROI button in
// PreviewDialog.
//
// If the camera is streaming we cannot write to the node map, so we store the
// full-frame dimensions as a pending ROI and apply them once streaming stops.
// When idle we apply immediately (same path as applyPendingRoiToPanel but we
// also do the live camera writes since ordering matters on idle cameras too —
// applyPinnedParameters() in the Apply button handler handles that correctly).
void MainWindow::onResetRoiClicked() {
  if (!m_cameraManager->isConnected()) {
    log("Reset ROI: no camera connected.");
    return;
  }

  // WidthMax/HeightMax are read-only sensor-dimension nodes — always accessible
  // and always valid values for Width/Height (no snapping needed).
  // Do NOT use snapInt64Value here: it clamps against Width.GetMax() which
  // equals SensorWidth - CurrentOffsetX, truncating the result when an offset
  // is active.
  int64_t widthMax = m_cameraManager->getInt64Value("WidthMax", 0);
  int64_t heightMax = m_cameraManager->getInt64Value("HeightMax", 0);

  PinnedParamsPanel::addPinnedNode("OffsetX");
  PinnedParamsPanel::addPinnedNode("OffsetY");
  PinnedParamsPanel::addPinnedNode("Width");
  PinnedParamsPanel::addPinnedNode("Height");

  m_hasPendingRoi = true;
  m_pendingRoiOffsetX = 0;
  m_pendingRoiOffsetY = 0;
  m_pendingRoiWidth = static_cast<int>(widthMax);
  m_pendingRoiHeight = static_cast<int>(heightMax);

  // Reset any in-progress draw or pending-apply state on the preview dialog.
  // This makes the menu-bar path behave identically to the right-click path
  // (which goes through PreviewDialog first and calls exitRoiApplyState there).
  if (m_previewDialog) {
    m_previewDialog->clearRoiOverlay();
    m_previewDialog->exitRoiApplyState();
  }

  if (isCameraStreaming()) {
    m_applyParamsButton->setStyleSheet(
        "QPushButton { padding: 6px 18px; font-weight: bold; min-width: 180px; "
        "              background-color: #2196F3; color: white; }");
    // Also flip the Draw ROI button to "Apply ROI" (blue) so the user can see
    // the pending full-frame reset directly in the Preview window.
    if (m_previewDialog)
      m_previewDialog->enterRoiApplyState();
    log(QString("Reset ROI to full frame (%1×%2) stored — stop "
                "preview/acquisition to apply.")
            .arg(widthMax)
            .arg(heightMax));
  } else {
    // Not streaming — write directly to camera with the three-step ordering,
    // then refresh the panel so spinboxes show the new camera state.
    onRoiApplyRequested();
    m_pinnedPanel->refreshFromCamera();
  }
}

// =============================================================================
// SLOT: onRoiApplyRequested — write pending ROI nodes directly to the camera
// =============================================================================
//
// Called when the user clicks "Apply ROI" (the transformed Draw ROI button) in
// PreviewDialog.  Writes OffsetX/Y/Width/Height in the safe three-step order,
// clears the pending state, and resets the Apply Pinned Parameters button.
// The panel spinboxes are left as-is (they already show the applied values).
void MainWindow::onRoiApplyRequested() {
  if (!m_hasPendingRoi || !m_cameraManager->isConnected())
    return;

  std::string err;
  // Step 1: zero offsets so Width/Height writes never violate the sum
  // constraint.
  m_cameraManager->setNodeInt64Value("OffsetX", 0, err);
  m_cameraManager->setNodeInt64Value("OffsetY", 0, err);
  // Step 2: write the target dimensions.
  if (!m_cameraManager->setNodeInt64Value("Width", m_pendingRoiWidth, err))
    log("Apply ROI: failed to set Width — " + QString::fromStdString(err));
  if (!m_cameraManager->setNodeInt64Value("Height", m_pendingRoiHeight, err))
    log("Apply ROI: failed to set Height — " + QString::fromStdString(err));
  // Step 3: write the final offsets.
  if (!m_cameraManager->setNodeInt64Value("OffsetX", m_pendingRoiOffsetX, err))
    log("Apply ROI: failed to set OffsetX — " + QString::fromStdString(err));
  if (!m_cameraManager->setNodeInt64Value("OffsetY", m_pendingRoiOffsetY, err))
    log("Apply ROI: failed to set OffsetY — " + QString::fromStdString(err));

  log(QString("ROI applied: OffsetX=%1, OffsetY=%2, Width=%3, Height=%4.")
          .arg(m_pendingRoiOffsetX)
          .arg(m_pendingRoiOffsetY)
          .arg(m_pendingRoiWidth)
          .arg(m_pendingRoiHeight));

  m_hasPendingRoi = false;

  // Reset the Apply Pinned Parameters button to white — the ROI is now in
  // camera, spinboxes match, and we assume no other params were changed while
  // idle.
  m_applyParamsButton->setStyleSheet("QPushButton { padding: 6px 18px; "
                                     "font-weight: bold; min-width: 180px; }");

  updateStartButtonText();
}

// =============================================================================
// SLOT: onRoiCancelled — clear pending ROI without writing to the camera
// =============================================================================
void MainWindow::onRoiCancelled() {
  m_hasPendingRoi = false;
  m_applyParamsButton->setStyleSheet("QPushButton { padding: 6px 18px; "
                                     "font-weight: bold; min-width: 180px; }");
  log("ROI cancelled — no changes written to camera.");
}

// =============================================================================
// isCameraStreaming — true while any camera stream is active
// =============================================================================
bool MainWindow::isCameraStreaming() const {
  return (m_worker && m_worker->isRunning()) || m_previewIsRunning;
}

// =============================================================================
// applyPendingRoiToPanel — populate spinboxes with stored ROI and force Apply
// blue
// =============================================================================
//
// Must only be called when isCameraStreaming() is false so that
// refreshFromCamera() creates editable spinboxes (not read-only labels) for the
// ROI nodes.
void MainWindow::applyPendingRoiToPanel() {
  if (!m_hasPendingRoi)
    return;

  // Rebuild panel widgets now that the node map is unlocked.
  m_pinnedPanel->refreshFromCamera();

  // Width/Height/OffsetX/OffsetY spinboxes all use WidthMax/HeightMax as their
  // upper bound (set in PinnedParamsPanel::buildValueWidget), so setValue never
  // clamps. applyPinnedParameters() writes in the correct three-step order.
  m_pinnedPanel->setIntNodeValue("OffsetX", m_pendingRoiOffsetX);
  m_pinnedPanel->setIntNodeValue("OffsetY", m_pendingRoiOffsetY);
  m_pinnedPanel->setIntNodeValue("Width", m_pendingRoiWidth);
  m_pinnedPanel->setIntNodeValue("Height", m_pendingRoiHeight);

  // setIntNodeValue fires parameterChanged which turns Apply blue via the
  // connected lambda, but force it here as a belt-and-suspenders measure
  // in case the nodes were already at these values and no signal fired.
  m_applyParamsButton->setStyleSheet(
      "QPushButton { padding: 6px 18px; font-weight: bold; min-width: 180px; "
      "              background-color: #2196F3; color: white; }");

  log(QString("ROI ready to apply: OffsetX=%1, OffsetY=%2, Width=%3, Height=%4 "
              "— click Apply.")
          .arg(m_pendingRoiOffsetX)
          .arg(m_pendingRoiOffsetY)
          .arg(m_pendingRoiWidth)
          .arg(m_pendingRoiHeight));
}

// =============================================================================
// onCaptureWhiteField / onCaptureDarkField — Tools menu handlers
// =============================================================================
void MainWindow::onCaptureWhiteField() {
  startFieldCapture(AcquisitionWorker::FieldType::WhiteField);
}

void MainWindow::onCaptureDarkField() {
  startFieldCapture(AcquisitionWorker::FieldType::DarkField);
}

// =============================================================================
// startFieldCapture — shared implementation for white/dark field acquisitions
// =============================================================================
//
// Differences from a normal acquisition:
//   - Session folder is named white_field_YYYYMMDD_HHmmss or
//   dark_field_YYYYMMDD_HHmmss
//   - A forced 5-second auto-stop is used regardless of the user's configured
//   auto-stop
//   - The worker runs in streaming Welford mode (FieldType != None) instead of
//   saving frames
//   - If the preview dialog is open, the running mean is routed to it live via
//   fieldPreviewReady
//
void MainWindow::startFieldCapture(AcquisitionWorker::FieldType ft) {
  if (!m_cameraManager->isConnected()) {
    log("Field capture: no camera connected.");
    return;
  }

  if (m_worker->isRunning()) {
    log("Field capture: acquisition already in progress.");
    return;
  }

  // Validate output path (reuse whatever the user has set in the main window)
  QString outputPath = m_outputPathEdit->text().trimmed();
  if (outputPath.isEmpty()) {
    QMessageBox::warning(
        this, "Field Capture",
        "Please set an output directory before capturing a field image.");
    return;
  }

  // Build the session folder name: white_field_YYYYMMDD_HHmmss or
  // dark_field_...
  const QString prefix = (ft == AcquisitionWorker::FieldType::WhiteField)
                             ? "white_field_"
                             : "dark_field_";
  const QString sessionName =
      prefix + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");

  // Collect the current camera settings JSON just like onStartClicked does
  QString paramsJson = QJsonDocument(m_pinnedPanel->currentValuesJson())
                           .toJson(QJsonDocument::Compact);

  // Configure the worker
  m_worker->setDevice(m_cameraManager->getDevice());
  m_worker->setOutputPath(outputPath);
  // Pass the current save format so the worker can also write a .raw alongside
  // the TIFF.
  AcquisitionWorker::SaveFormat fieldFmt;
  switch (m_saveFormatCombo->currentIndex()) {
  case 1:
    fieldFmt = AcquisitionWorker::SaveFormat::TiffStack;
    break;
  case 2:
    fieldFmt = AcquisitionWorker::SaveFormat::RawSequence;
    break;
  default:
    fieldFmt = AcquisitionWorker::SaveFormat::RawVideo;
    break;
  }
  m_worker->setSaveFormat(fieldFmt);
  m_worker->setFieldType(ft);
  m_worker->setCustomSessionName(sessionName);
  m_worker->setNotes(m_notesText);
  m_worker->setCameraParamsJson(paramsJson);
  m_worker->setNodeMapSnapshotJson(
      AcquisitionWorker::buildCameraSettingsJson(m_cameraManager->getDevice()));

  // Reset frame counter display.  During field captures the counter tracks
  // how many frames have been averaged, not how many files were saved.
  m_isFieldCapture = true;
  m_currentFrameCount = 0;
  m_lastFrameCount = 0;
  m_frameCountLabel->setText("Frames averaged: 0");
  m_fpsLabel->setText("FPS: 0.0");

  // Disable live-apply during field capture — stream owns the node map.
  m_pinnedPanel->setLiveMode(false);

  // Stop the preview worker (can't use the device simultaneously),
  // but keep the dialog open so it can display the running mean.
  if (m_previewDialog) {
    m_previewDialog->stopPreviewIfRunning();
    m_previewDialog->setAcquisitionRunning(true);
  }

  // Wire fieldPreviewReady → preview dialog if the dialog is currently visible.
  // Disconnect first to avoid duplicates if the user runs multiple field
  // captures.
  disconnect(m_worker, &AcquisitionWorker::fieldPreviewReady, nullptr, nullptr);
  if (m_previewDialog && m_previewDialog->isVisible()) {
    connect(m_worker, &AcquisitionWorker::fieldPreviewReady, m_previewDialog,
            &PreviewDialog::feedFrame);
  }

  m_worker->start();

  // Update UI
  m_startButton->setEnabled(false);
  m_stopButton->setEnabled(true);
  m_previewButton->setEnabled(false);
  m_disconnectButton->setEnabled(false);
  m_saveFormatCombo->setEnabled(false);

  m_fpsTimer->start();

  // Force a 5-second auto-stop regardless of the user's auto-stop configuration
  constexpr int fieldCaptureDurationMs = 5000;
  m_autoStopSecondsRemaining = 5;

  const QString fieldLabel = (ft == AcquisitionWorker::FieldType::WhiteField)
                                 ? "white field"
                                 : "dark field";

  m_stopButton->setText("Stop (auto 5s)");
  m_stopButton->setToolTip(
      QString("Field capture runs for 5 seconds and stops automatically.\n"
              "Click to stop earlier."));
  m_stopButton->setStyleSheet(
      "QPushButton { background-color: #E65100; color: white; font-weight: "
      "bold; "
      "              padding: 6px; min-width: 160px; }"
      "QPushButton:disabled { background-color: #888; color: #ccc; }");

  m_autoStopPendingDurationMs = fieldCaptureDurationMs;

  log(QString("Field capture (%1) started. Saving to: %2/%3")
          .arg(fieldLabel, outputPath, sessionName));
}

// =============================================================================
// log — append a message to the log widget with a timestamp
// =============================================================================
void MainWindow::log(const QString &message) {
  // Get the current time
  QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");

  // Append the timestamped message to the log
  m_logWidget->append("[" + timestamp + "] " + message);

  // Auto-scroll to the bottom so the user sees the latest messages
  QScrollBar *scrollBar = m_logWidget->verticalScrollBar();
  scrollBar->setValue(scrollBar->maximum());
}
