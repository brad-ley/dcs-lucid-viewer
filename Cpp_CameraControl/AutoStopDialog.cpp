// =============================================================================
// AutoStopDialog.cpp
// =============================================================================
//
// Implementation of the AutoStopDialog preferences dialog.
//
// The dialog lets the user:
//   1. Toggle auto-stop on or off with a checkbox
//   2. Enter a duration value
//   3. Choose the unit — µs, ms, or s
//
// When the unit is changed, the displayed value is automatically converted so
// the physical duration stays the same.  For example, switching from "s" to "ms"
// changes "5 s" → "5000 ms" in the spinbox.
//
// All values are written to the Windows registry via QSettings on OK.
// The static accessors autoStopEnabled() / autoStopDurationMs() let MainWindow
// read the settings without constructing a dialog instance.
// =============================================================================

#include "AutoStopDialog.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QLabel>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QSettings>

#include <cmath>    // std::ceil, std::max


// =============================================================================
// Internal constants
// =============================================================================
namespace
{
    constexpr const char* kKeyEnabled  = "autoStop/enabled";
    constexpr const char* kKeyDuration = "autoStop/duration";
    constexpr const char* kKeyUnits    = "autoStop/units";    // 0=µs  1=ms  2=s

    constexpr bool   kDefaultEnabled  = false;
    constexpr double kDefaultDuration = 10.0;
    constexpr int    kDefaultUnits    = 2;    // seconds

    // Milliseconds per unit — index matches the combo order [µs, ms, s]
    //
    // C++ CONCEPT — constexpr array:
    //   Declared constexpr so the compiler can evaluate these at compile time.
    //   We use them to convert between units without any run-time overhead.
    constexpr double kMsPerUnit[3] = { 0.001, 1.0, 1000.0 };
}


// =============================================================================
// Constructor
// =============================================================================
AutoStopDialog::AutoStopDialog(QWidget* parent)
    : QDialog(parent)
    , m_enableCheck(nullptr)
    , m_durationSpin(nullptr)
    , m_unitsCombo(nullptr)
    , m_previousUnitsIndex(kDefaultUnits)
{
    setWindowTitle("Auto-Stop Acquisition");
    setMinimumWidth(360);

    // Read current settings (or defaults if not yet saved)
    QSettings settings;
    bool   savedEnabled  = settings.value(kKeyEnabled,  kDefaultEnabled).toBool();
    double savedDuration = settings.value(kKeyDuration, kDefaultDuration).toDouble();
    int    savedUnits    = settings.value(kKeyUnits,    kDefaultUnits).toInt();

    // Clamp units index so a corrupt registry entry can't crash us
    if (savedUnits < 0 || savedUnits > 2) savedUnits = kDefaultUnits;
    m_previousUnitsIndex = savedUnits;

    // =========================================================================
    // Outer layout
    // =========================================================================
    QVBoxLayout* outerLayout = new QVBoxLayout(this);
    outerLayout->setSpacing(12);
    outerLayout->setContentsMargins(14, 14, 14, 14);

    // Section heading
    QLabel* headingLabel = new QLabel("Automatic Acquisition Stop", this);
    QFont boldFont = headingLabel->font();
    boldFont.setBold(true);
    headingLabel->setFont(boldFont);
    outerLayout->addWidget(headingLabel);

    // =========================================================================
    // Enable checkbox
    // =========================================================================
    m_enableCheck = new QCheckBox("Enable auto-stop", this);
    m_enableCheck->setChecked(savedEnabled);
    m_enableCheck->setToolTip(
        "When checked, acquisition stops automatically after the specified duration.\n"
        "The Stop button shows a live countdown during acquisition.");
    outerLayout->addWidget(m_enableCheck);

    // =========================================================================
    // Duration row: [spinbox] [unit combo]
    // =========================================================================
    QFormLayout* formLayout = new QFormLayout();
    formLayout->setRowWrapPolicy(QFormLayout::DontWrapRows);
    formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    formLayout->setSpacing(8);

    // Duration spinbox
    m_durationSpin = new QDoubleSpinBox(this);
    m_durationSpin->setDecimals(3);
    m_durationSpin->setRange(0.001, 86400.0);   // 1 µs minimum (in any unit), 24 h maximum
    m_durationSpin->setSingleStep(1.0);
    m_durationSpin->setValue(savedDuration);
    m_durationSpin->setToolTip("Duration before acquisition automatically stops");

    // Unit selector
    m_unitsCombo = new QComboBox(this);
    m_unitsCombo->addItem("µs");   // index 0
    m_unitsCombo->addItem("ms");   // index 1
    m_unitsCombo->addItem("s");    // index 2
    m_unitsCombo->setCurrentIndex(savedUnits);
    m_unitsCombo->setToolTip("Unit for the duration value");

    // Pack the spinbox and unit combo side by side in one form row
    QHBoxLayout* durationRow = new QHBoxLayout();
    durationRow->setContentsMargins(0, 0, 0, 0);
    durationRow->addWidget(m_durationSpin, 1);
    durationRow->addWidget(m_unitsCombo,   0);

    formLayout->addRow("Duration:", durationRow);
    outerLayout->addLayout(formLayout);

    // Short note explaining the countdown display
    QLabel* noteLabel = new QLabel(
        "While acquiring, the Stop button displays the remaining time\n"
        "as a seconds countdown: \"Stop (auto 5s)\".", this);
    noteLabel->setStyleSheet("color: #666; font-size: 11px;");
    outerLayout->addWidget(noteLabel);

    // =========================================================================
    // OK / Cancel
    // =========================================================================
    QDialogButtonBox* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &AutoStopDialog::onAccepted);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outerLayout->addWidget(buttonBox);

    // =========================================================================
    // Wire the unit-change conversion
    // =========================================================================
    //
    // C++ CONCEPT — QOverload:
    //   QComboBox::currentIndexChanged has two overloads — one that passes an
    //   int index and one that passes a QString text.  QOverload<int>::of(...)
    //   tells Qt's connect() which overload we want.  Without it, the compiler
    //   would complain about an ambiguous signal.
    connect(m_unitsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AutoStopDialog::onUnitsChanged);
}


// =============================================================================
// onUnitsChanged — convert the displayed value when the user switches units
// =============================================================================
//
// Example: user had "5 s".  They switch to "ms".
//   oldMs = 5.0 * kMsPerUnit[2]  = 5000.0 ms
//   newValue = 5000.0 / kMsPerUnit[1] = 5000.0
//   Spinbox now shows "5000 ms" — same physical duration.
//
// C++ CONCEPT — function parameter shadowing:
//   The parameter 'newIndex' shadows the member m_unitsCombo->currentIndex() but
//   they hold the same value.  We use the parameter for clarity.
void AutoStopDialog::onUnitsChanged(int newIndex)
{
    if (newIndex < 0 || newIndex > 2) return;
    if (newIndex == m_previousUnitsIndex) return;

    // Convert current displayed value to milliseconds, then to the new unit
    double currentValue = m_durationSpin->value();
    double valueInMs    = currentValue * kMsPerUnit[m_previousUnitsIndex];
    double newValue     = valueInMs    / kMsPerUnit[newIndex];

    m_previousUnitsIndex = newIndex;

    // Update spinbox without triggering another conversion.
    // blockSignals(true) suppresses all signals from m_durationSpin while we set
    // the new value — otherwise valueChanged would fire, potentially confusing
    // any connected logic.  We restore signals with blockSignals(false) after.
    m_durationSpin->blockSignals(true);
    m_durationSpin->setValue(newValue);
    m_durationSpin->blockSignals(false);
}


// =============================================================================
// onAccepted — validate and save when the user clicks OK
// =============================================================================
void AutoStopDialog::onAccepted()
{
    double duration = m_durationSpin->value();
    int    units    = m_unitsCombo->currentIndex();

    // Convert to ms for validation — must be at least 1 ms to be meaningful
    double durationMs = duration * kMsPerUnit[units];
    if (durationMs < 1.0)
    {
        QMessageBox::warning(this, "Invalid Duration",
            "The acquisition duration must be at least 1 ms.\n"
            "Please enter a larger value.");
        return;
    }

    QSettings settings;
    settings.setValue(kKeyEnabled,  m_enableCheck->isChecked());
    settings.setValue(kKeyDuration, duration);
    settings.setValue(kKeyUnits,    units);

    accept();
}


// =============================================================================
// autoStopEnabled — static accessor
// =============================================================================
bool AutoStopDialog::autoStopEnabled()
{
    return QSettings().value(kKeyEnabled, kDefaultEnabled).toBool();
}


// =============================================================================
// setAutoStopEnabled — static setter
// =============================================================================
void AutoStopDialog::setAutoStopEnabled(bool enabled)
{
    QSettings().setValue(kKeyEnabled, enabled);
}


// =============================================================================
// autoStopDurationMs — static accessor
// =============================================================================
//
// Reads the saved duration and unit, converts to milliseconds, and returns the
// result as a double.  The caller (MainWindow::onStartClicked) passes this to
// QTimer::start() (cast to int) for the exact stop trigger.
double AutoStopDialog::autoStopDurationMs()
{
    QSettings settings;
    double duration = settings.value(kKeyDuration, kDefaultDuration).toDouble();
    int    units    = settings.value(kKeyUnits,    kDefaultUnits).toInt();
    if (units < 0 || units > 2) units = kDefaultUnits;
    return duration * kMsPerUnit[units];
}
