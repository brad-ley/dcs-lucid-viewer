// =============================================================================
// ConfigDialog.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares ConfigDialog — a small preferences dialog reachable via the
//   "Config → Open Configs..." menu item.  Currently it exposes the two
//   percentile thresholds used by the Preview window's Auto Contrast feature.
//
// PERSISTENCE:
//   Values are stored in the Windows registry under the same QSettings path
//   as the rest of the application:
//     HKCU\Software\Brad Simplified\Lucid Camera Acquisition Tool\
//     Keys: "config/autoContrastLowPct"   (default 3.0)
//           "config/autoContrastHighPct"  (default 97.0)
//
// STATIC ACCESSORS:
//   ConfigDialog::autoContrastLowPct()  and  ConfigDialog::autoContrastHighPct()
//   can be called from anywhere (e.g. PreviewDialog) without needing a dialog
//   instance.  They read the current saved value, or the default if not yet set.
//
// =============================================================================

#pragma once

#include <QDialog>

class QDoubleSpinBox;
class QCheckBox;
class QLabel;


// =============================================================================
// ConfigDialog
// =============================================================================
class ConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConfigDialog(QWidget* parent = nullptr);

    // -------------------------------------------------------------------------
    // Static accessors — read saved settings from the registry.
    // Call these from PreviewDialog (or anywhere) to get the live values without
    // instantiating the dialog.
    // -------------------------------------------------------------------------

    // Black-point percentile for auto contrast (default 3.0).
    // Pixels at or below this percentile in the frame are mapped to black.
    static double autoContrastLowPct();

    // White-point percentile for auto contrast (default 97.0).
    // Pixels at or above this percentile in the frame are mapped to white.
    static double autoContrastHighPct();

    // Whether the red "saturated pixel" overlay is shown in the preview (default true).
    static bool showSaturationMask();

    // Whether the yellow "zero / dead pixel" overlay is shown in the preview (default true).
    static bool showDeadPixelMask();

private slots:
    // Called when the user clicks OK.  Validates and saves to QSettings.
    void onAccepted();

private:
    QDoubleSpinBox* m_lowSpin;          // Spinner for the low (black-point) percentile
    QDoubleSpinBox* m_highSpin;         // Spinner for the high (white-point) percentile
    QCheckBox*      m_satMaskCheck;     // Enable/disable the red saturated-pixel overlay
    QCheckBox*      m_deadMaskCheck;    // Enable/disable the yellow zero-pixel overlay
};
