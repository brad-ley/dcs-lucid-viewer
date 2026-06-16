// =============================================================================
// AutoStopDialog.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares AutoStopDialog — a preferences dialog reachable via the
//   "Config → Auto-Stop Acquisition" menu item.
//
//   When auto-stop is enabled, starting an acquisition automatically arms a
//   countdown timer.  The Stop button shows the remaining seconds
//   ("Stop (auto 5s)"), and when the timer expires the acquisition stops
//   without the user having to click anything.
//
// SETTINGS PERSISTED (HKCU\Software\Brad Simplified\Lucid Camera Acquisition Tool\):
//   Key                      Default   Meaning
//   autoStop/enabled         false     Whether auto-stop is active
//   autoStop/duration        10.0      Numeric duration in the chosen unit
//   autoStop/units           2         0 = µs,  1 = ms,  2 = s
//
// STATIC ACCESSORS:
//   AutoStopDialog::autoStopEnabled()    — true if auto-stop is turned on
//   AutoStopDialog::autoStopDurationMs() — duration converted to milliseconds
//
//   Call these from MainWindow::onStartClicked() to arm the countdown without
//   needing a dialog instance.
// =============================================================================

#pragma once

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QComboBox;
class QLabel;


// =============================================================================
// AutoStopDialog
// =============================================================================
class AutoStopDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AutoStopDialog(QWidget* parent = nullptr);

    // -------------------------------------------------------------------------
    // Static accessors — read saved settings without instantiating the dialog
    // -------------------------------------------------------------------------

    // Returns true when the user has enabled the auto-stop feature.
    static bool autoStopEnabled();

    // Returns the configured acquisition duration converted to milliseconds.
    // Returns 0.0 if auto-stop is disabled (caller should check autoStopEnabled()).
    static double autoStopDurationMs();

    // Write the enabled flag to QSettings without opening the dialog.
    // Used by the checkable menu action in MainWindow.
    static void setAutoStopEnabled(bool enabled);

private slots:
    // Called when OK is clicked — validates then saves to QSettings
    void onAccepted();

    // Called when the units combo changes — converts the displayed value so the
    // physical duration stays the same (e.g. switching from s to ms: 5 → 5000)
    void onUnitsChanged(int newIndex);

private:
    QCheckBox*      m_enableCheck;   // Enables / disables the whole feature
    QDoubleSpinBox* m_durationSpin;  // Numeric duration
    QComboBox*      m_unitsCombo;    // Unit selector: µs | ms | s

    // Tracks the previous units index so onUnitsChanged() can convert the value
    int m_previousUnitsIndex;
};
