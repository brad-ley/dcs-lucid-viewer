// =============================================================================
// ConfigDialog.cpp
// =============================================================================
//
// Implementation of the ConfigDialog preferences dialog.
//
// The dialog shows two QDoubleSpinBox controls — one for the auto-contrast
// low percentile (black point) and one for the high percentile (white point).
// Values are saved in the Windows registry via QSettings on OK, and read back
// by the static accessors so PreviewDialog can use them without a dialog instance.
//
// REGISTRY KEYS (under HKCU\Software\Brad Simplified\Lucid Camera Acquisition Tool\):
//   config/autoContrastLowPct   — default 3.0
//   config/autoContrastHighPct  — default 97.0
// =============================================================================

#include "ConfigDialog.h"

// Qt widgets
#include <QDoubleSpinBox>   // Floating-point spin box
#include <QCheckBox>        // Boolean checkbox
#include <QLabel>           // Text labels for the form rows
#include <QFormLayout>      // "Label: Widget" row layout
#include <QVBoxLayout>      // Stacks children vertically
#include <QHBoxLayout>      // Stacks children horizontally
#include <QDialogButtonBox> // Standard OK / Cancel buttons
#include <QMessageBox>      // Error dialog when validation fails

// Qt settings
#include <QSettings>        // Persistent registry storage


// =============================================================================
// Registry key names — defined once here so they stay in sync with the accessors
// =============================================================================

// C++ CONCEPT — namespace-scope constants:
//   These anonymous-namespace constants are only visible inside this .cpp file.
//   Using a named constant (instead of a raw string literal) prevents typos and
//   makes it easy to rename the key in one place.
namespace
{
    // Keys under the app's QSettings root:
    //   HKCU\Software\Brad Simplified\Lucid Camera Acquisition Tool\config\...
    constexpr const char* kKeyLow      = "config/autoContrastLowPct";
    constexpr const char* kKeyHigh     = "config/autoContrastHighPct";
    constexpr const char* kKeySatMask  = "config/showSaturationMask";
    constexpr const char* kKeyDeadMask = "config/showDeadPixelMask";

    // Factory defaults — used both in the dialog and in the static accessors
    constexpr double kDefaultLow      = 3.0;
    constexpr double kDefaultHigh     = 97.0;
    constexpr bool   kDefaultSatMask  = true;
    constexpr bool   kDefaultDeadMask = true;
}


// =============================================================================
// Constructor
// =============================================================================
ConfigDialog::ConfigDialog(QWidget* parent)
    : QDialog(parent)
    , m_lowSpin(nullptr)
    , m_highSpin(nullptr)
    , m_satMaskCheck(nullptr)
    , m_deadMaskCheck(nullptr)
{
    setWindowTitle("Configuration");
    setMinimumWidth(340);

    // =========================================================================
    // Read current saved values (or defaults) to pre-populate the spinboxes
    // =========================================================================
    //
    // C++ CONCEPT — QSettings with no arguments:
    //   When constructed with no arguments, QSettings uses the organization and
    //   application name set in main.cpp via QApplication::setOrganizationName()
    //   and setApplicationName().  On Windows this maps to the registry under:
    //     HKCU\Software\<org>\<appName>
    QSettings settings;
    double savedLow      = settings.value(kKeyLow,      kDefaultLow).toDouble();
    double savedHigh     = settings.value(kKeyHigh,     kDefaultHigh).toDouble();
    bool   savedSatMask  = settings.value(kKeySatMask,  kDefaultSatMask).toBool();
    bool   savedDeadMask = settings.value(kKeyDeadMask, kDefaultDeadMask).toBool();

    // =========================================================================
    // Layout
    // =========================================================================

    QVBoxLayout* outerLayout = new QVBoxLayout(this);
    outerLayout->setSpacing(12);
    outerLayout->setContentsMargins(14, 14, 14, 14);

    // Section heading
    QLabel* sectionLabel = new QLabel("Auto Contrast Percentile Thresholds", this);
    QFont boldFont = sectionLabel->font();
    boldFont.setBold(true);
    sectionLabel->setFont(boldFont);
    outerLayout->addWidget(sectionLabel);

    // Combined explanation covering both percentile thresholds and pixel overlays
    QLabel* explanationLabel = new QLabel(
        "Low/High define the black and white points for Auto Contrast.\n"
        "The checkboxes toggle pixel-clipping overlays (independent of contrast):\n"
        "  Yellow = pixel value is 0 (dead / fully unexposed)\n"
        "  Red    = pixel value is 255 (saturated / overexposed)\n\n"
        "Typical values: Low = 3.0, High = 97.0", this);
    explanationLabel->setStyleSheet("color: #666; font-size: 11px;");
    outerLayout->addWidget(explanationLabel);

    // Form rows: each row has a spinbox AND its corresponding overlay checkbox
    // side by side so the visual association is clear.
    QFormLayout* formLayout = new QFormLayout();
    formLayout->setRowWrapPolicy(QFormLayout::DontWrapRows);
    formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    formLayout->setSpacing(8);

    // ---- Low percentile row: spinbox + dead-pixel (yellow) checkbox ----
    // Range 0.0–49.9 so it can never meet or exceed the high value.
    // Dead pixels are always zero — so they relate to the low (black) end.
    m_lowSpin = new QDoubleSpinBox(this);
    m_lowSpin->setRange(0.0, 49.9);
    m_lowSpin->setDecimals(1);
    m_lowSpin->setSingleStep(0.5);
    m_lowSpin->setSuffix(" %");
    m_lowSpin->setValue(savedLow);
    m_lowSpin->setToolTip("Black point: pixels at or below this percentile are shown as black");

    // C++ CONCEPT — inline widget container:
    //   QFormLayout only accepts one widget per row.  To put two widgets side by
    //   side we wrap them in a plain QWidget with a horizontal box layout, then
    //   add that wrapper as the form row's field widget.
    QWidget* lowRowWidget = new QWidget(this);
    QHBoxLayout* lowRowLayout = new QHBoxLayout(lowRowWidget);
    lowRowLayout->setContentsMargins(0, 0, 0, 0);
    lowRowLayout->setSpacing(10);

    m_deadMaskCheck = new QCheckBox("Show yellow (dead / 0)", lowRowWidget);
    m_deadMaskCheck->setChecked(savedDeadMask);
    m_deadMaskCheck->setToolTip("Paint semi-transparent yellow over any pixel whose raw sensor value is 0");

    lowRowLayout->addWidget(m_lowSpin);
    lowRowLayout->addWidget(m_deadMaskCheck);
    lowRowLayout->addStretch();   // Push controls left; don't stretch the checkbox

    formLayout->addRow("Low percentile:", lowRowWidget);

    // ---- High percentile row: spinbox + saturation (red) checkbox ----
    // Range 50.1–100.0 so it can never meet or fall below the low value.
    // Saturated pixels are always 255 — so they relate to the high (white) end.
    m_highSpin = new QDoubleSpinBox(this);
    m_highSpin->setRange(50.1, 100.0);
    m_highSpin->setDecimals(1);
    m_highSpin->setSingleStep(0.5);
    m_highSpin->setSuffix(" %");
    m_highSpin->setValue(savedHigh);
    m_highSpin->setToolTip("White point: pixels at or above this percentile are shown as white");

    QWidget* highRowWidget = new QWidget(this);
    QHBoxLayout* highRowLayout = new QHBoxLayout(highRowWidget);
    highRowLayout->setContentsMargins(0, 0, 0, 0);
    highRowLayout->setSpacing(10);

    m_satMaskCheck = new QCheckBox("Show red (saturated / 255)", highRowWidget);
    m_satMaskCheck->setChecked(savedSatMask);
    m_satMaskCheck->setToolTip("Paint solid red over any pixel whose raw sensor value is 255");

    highRowLayout->addWidget(m_highSpin);
    highRowLayout->addWidget(m_satMaskCheck);
    highRowLayout->addStretch();

    formLayout->addRow("High percentile:", highRowWidget);

    outerLayout->addLayout(formLayout);

    // =========================================================================
    // OK / Cancel buttons
    // =========================================================================
    //
    // QDialogButtonBox creates a row of standard buttons and emits 'accepted'
    // (OK) or 'rejected' (Cancel) signals — the Qt-idiomatic way to confirm or
    // dismiss a dialog.
    QDialogButtonBox* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &ConfigDialog::onAccepted);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    outerLayout->addWidget(buttonBox);
}


// =============================================================================
// onAccepted — validate and save when the user clicks OK
// =============================================================================
void ConfigDialog::onAccepted()
{
    double low  = m_lowSpin->value();
    double high = m_highSpin->value();

    // Safety check: low must be strictly less than high.
    // The spinbox ranges already prevent overlap, but this guards against any
    // edge case where both values are 50.0 / 50.1 rounding exactly together.
    if (low >= high)
    {
        QMessageBox::warning(this, "Invalid Values",
            "The low percentile must be less than the high percentile.\n"
            "Please correct the values and try again.");
        return;   // Don't close the dialog — let the user fix the values
    }

    // Persist to registry
    QSettings settings;
    settings.setValue(kKeyLow,      low);
    settings.setValue(kKeyHigh,     high);
    settings.setValue(kKeySatMask,  m_satMaskCheck->isChecked());
    settings.setValue(kKeyDeadMask, m_deadMaskCheck->isChecked());

    accept();   // Close the dialog with QDialog::Accepted result
}


// =============================================================================
// autoContrastLowPct — static accessor
// =============================================================================
//
// Called from PreviewDialog (and anywhere else) to get the current saved value
// without constructing a ConfigDialog instance.
//
// C++ CONCEPT — no-argument QSettings constructor:
//   Each call constructs a temporary QSettings that reads from the same registry
//   path used everywhere else in the app.  The object is destroyed at the end of
//   the expression — lightweight, no persistent state needed.
// =============================================================================
double ConfigDialog::autoContrastLowPct()
{
    return QSettings().value(kKeyLow, kDefaultLow).toDouble();
}


// =============================================================================
// autoContrastHighPct — static accessor
// =============================================================================
double ConfigDialog::autoContrastHighPct()
{
    return QSettings().value(kKeyHigh, kDefaultHigh).toDouble();
}


// =============================================================================
// showSaturationMask — static accessor
// =============================================================================
bool ConfigDialog::showSaturationMask()
{
    return QSettings().value(kKeySatMask, kDefaultSatMask).toBool();
}


// =============================================================================
// showDeadPixelMask — static accessor
// =============================================================================
bool ConfigDialog::showDeadPixelMask()
{
    return QSettings().value(kKeyDeadMask, kDefaultDeadMask).toBool();
}
