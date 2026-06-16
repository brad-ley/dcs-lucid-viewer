// =============================================================================
// PinnedParamsPanel.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares PinnedParamsPanel — a widget that lives in the main window and shows
//   a user-curated list of camera parameters. The list is stored in the Windows
//   registry so it persists across sessions and can be edited without recompiling.
//
// HOW PARAMETERS GET INTO THIS LIST:
//   The user opens the Advanced Parameters Dialog, selects any parameter, and
//   clicks the star (★) button. That writes the parameter's GenICam node name
//   to the registry and immediately refreshes this panel.
//
// REGISTRY STORAGE:
//   Key:   HKEY_CURRENT_USER\Software\Brad Simplified\Lucid Camera Acquisition Tool
//   Value: "pinnedParams" = "PixelFormat,Gain,ExposureTime,..."
//
//   Qt's QSettings class handles the registry read/write automatically when
//   constructed with no arguments (it uses the app's organization/name set in main.cpp).
//
// C++ CONCEPT — static methods:
//   addPinnedNode(), removePinnedNode(), isPinnedNode(), and loadPinnedList() are
//   declared 'static'. A static method belongs to the class itself, not to any
//   instance. This lets AdvancedParamsDialog call PinnedParamsPanel::isPinnedNode()
//   without needing a pointer to a PinnedParamsPanel object.
// =============================================================================

#pragma once

#include <QWidget>
#include <QFormLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <vector>
#include <string>

// Forward declaration — avoids pulling in CameraManager.h and Arena SDK here.
// We only need the full definition in the .cpp where we actually call methods.
class CameraManager;


// =============================================================================
// PinnedParamsPanel
// =============================================================================
class PinnedParamsPanel : public QWidget
{
    Q_OBJECT

public:
    // mgr    — pointer to the camera manager (not owned here; owned by MainWindow)
    // parent — Qt parent widget
    explicit PinnedParamsPanel(CameraManager* mgr, QWidget* parent = nullptr);

    // Rebuild all widgets from the current registry list + live camera state.
    // Call this after connecting/disconnecting a camera, or after the pinned list changes.
    void refreshFromCamera();

    // Returns the current widget values as a JSON object (node name → value).
    // Used by MainWindow to embed parameter state in the acquisition metadata.
    QJsonObject currentValuesJson() const;

    // -------------------------------------------------------------------------
    // Static registry helpers
    // -------------------------------------------------------------------------
    // These operate directly on the registry — no instance needed.
    // They are used by both this class and AdvancedParamsDialog.

    // Append nodeName to the pinned list if it isn't already there.
    static void addPinnedNode(const QString& nodeName);

    // Remove nodeName from the pinned list. No-op if not present.
    static void removePinnedNode(const QString& nodeName);

    // Returns true if nodeName is currently in the pinned list.
    static bool isPinnedNode(const QString& nodeName);

    // Returns the full ordered list of pinned node names from the registry.
    // Writes the default list first if the registry key doesn't exist yet.
    static QStringList loadPinnedList();

    // Open a modal dialog that lets the user drag rows to reorder the pinned list.
    // On OK the new order is saved to the registry and the panel refreshes.
    // parent — used to centre the dialog over the main window.
    void showReorderDialog(QWidget* parent = nullptr);

    // Programmatically set the value of an integer spinbox row by node name.
    // Used by MainWindow after drawing an ROI to populate the spinbox without
    // having to tear down and rebuild the whole panel.
    // No-op if the node is not currently shown or is not an integer spinbox.
    void setIntNodeValue(const QString& nodeName, int value);

    // Temporarily expand the maximum of an integer spinbox beyond its default camera-reported
    // range.  Used by MainWindow before calling setIntNodeValue for Width/Height when setting
    // a full-frame ROI: the camera's Width.GetMax() equals SensorWidth-OffsetX, so it may be
    // less than SensorWidth while OffsetX is still non-zero.  applyPinnedParameters() writes
    // the values to the camera in the correct order (zero offsets first, then set dimensions).
    // No-op if the node is not shown or is not an integer spinbox.
    void setIntNodeMaximum(const QString& nodeName, int max);

    // Enable or disable live-apply mode.
    // When enabled, any widget change (editingFinished for spinboxes,
    // currentIndexChanged for combos) immediately writes that single parameter
    // to the camera without requiring the user to click Apply.
    // Called by MainWindow when preview starts/stops.
    void setLiveMode(bool enabled);

signals:
    // Emitted for informational messages during applyPinnedParameters() —
    // e.g. when a parameter is skipped because it is currently disabled by
    // another parameter (ExposureTime when ExposureAuto is active).
    // MainWindow connects this to its log() function.
    void statusMessage(const QString& message);

    // Emitted whenever the user edits any pinned parameter widget.
    // MainWindow uses this to highlight the Apply button blue until the user applies.
    void parameterChanged();

    // Emitted after a successful live-apply write (m_liveMode == true).
    // nodeName is the GenApi node name that was written (e.g. "TriggerMode").
    // MainWindow uses this to update UI state that depends on camera parameters.
    void liveNodeApplied(const QString& nodeName);

public slots:
    // Called when the user clicks "Apply Pinned Parameters".
    // Writes all widget values to the camera using CameraManager's setNode*Value() methods.
    // Declared public so MainWindow can connect its own button to this slot directly.
    void applyPinnedParameters();

private:
    // Build one widget row per pinned parameter. Called by refreshFromCamera().
    void buildRows();

    // Delete all existing rows and their widgets.
    void clearRows();

    // Write a single parameter to the camera immediately, based on the current
    // widget value.  Used by the live-apply connections wired in buildRows().
    // Silently skips nodes that are not writable during streaming.
    void applyLiveSingleNode(const std::string& nodeName, QWidget* w);

    // Read all current camera values back into the existing widgets in-place.
    // Lighter than refreshFromCamera() — does not rebuild widgets, so there is
    // no flicker and no focus/edit-state loss.  Called after a live-apply write
    // so side-effect changes (e.g. binning shrinks Width/Height) are reflected.
    void updateWidgetValuesFromCamera();

    // -------------------------------------------------------------------------
    // PinnedRow — one entry in the pinned list
    // -------------------------------------------------------------------------
    //
    // C++ CONCEPT — inner struct:
    //   A struct defined inside a class is scoped to that class. It's private
    //   to PinnedParamsPanel — nobody outside needs to know about PinnedRow.
    //   We use 'int' instead of GenApi::EInterfaceType to avoid requiring
    //   Arena SDK headers in this header file.
    struct PinnedRow
    {
        std::string nodeName;     // GenICam node name (e.g. "ExposureTime")
        int         nodeType;     // GenApi::EInterfaceType cast to int; 0 = unavailable
        QWidget*    valueWidget;  // Spinbox/combo/etc.; nullptr if node is not available
    };

    CameraManager* m_mgr;           // Camera manager — not owned
    QFormLayout*   m_formLayout;    // Rows of [label : widget] inside the scroll area
    bool           m_liveMode = false;  // When true, widget changes write to camera immediately

    // All currently shown rows — used by onApplyClicked() to find each widget
    std::vector<PinnedRow> m_rows;

    // -------------------------------------------------------------------------
    // Default pinned list
    // -------------------------------------------------------------------------
    // Written to the registry on first run (when the key doesn't exist yet).
    // These are standard GenICam node names present on most Lucid cameras.
    //
    // C++ CONCEPT — static const:
    //   'static' means there is one copy shared by all instances (and no instance needed).
    //   'const' means it cannot be changed after initialization.
    //   Defined in the .cpp file.
    static const QStringList s_defaults;
};
