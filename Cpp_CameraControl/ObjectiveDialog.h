// =============================================================================
// ObjectiveDialog.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares ObjectiveDialog — a preferences dialog reachable via the
//   "Config → Microscope Objective..." menu item. Records the magnification
//   of the microscope objective currently in front of the camera (e.g. 5 for
//   a 5x objective), which is written into every subsequent acquisition's
//   metadata.json as "objective_magnification" (see
//   AcquisitionWorker::setObjectiveMagnification() /
//   AcquisitionWorker::writeMetadataJson()). Downstream tools (e.g. the
//   Lucid viewer's FFT dialog) use it to convert pixel-based spatial
//   frequency into real units at the sample plane.
//
// SETTINGS PERSISTED (HKCU\Software\Brad Simplified\Lucid Camera Acquisition Tool\):
//   Key                      Default   Meaning
//   objective/magnification  5.0       Microscope objective power (e.g. 5 = 5x)
//
// STATIC ACCESSOR:
//   ObjectiveDialog::magnification() — current saved value (0.0 if never set)
//
//   Call this from MainWindow wherever the acquisition config is assembled,
//   to fill in AcquisitionWorker's objective magnification without needing a
//   dialog instance.
// =============================================================================

#pragma once

#include <QDialog>

class QDoubleSpinBox;


// =============================================================================
// ObjectiveDialog
// =============================================================================
class ObjectiveDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ObjectiveDialog(QWidget* parent = nullptr);

    // -------------------------------------------------------------------------
    // Static accessor — read the saved value without instantiating the dialog
    // -------------------------------------------------------------------------

    // Returns the configured objective magnification (e.g. 5.0 for a 5x
    // objective), or 0.0 if the user has never set one.
    static double magnification();

private slots:
    // Called when OK is clicked — saves to QSettings.
    void onAccepted();

private:
    QDoubleSpinBox* m_magnificationSpin;
};
