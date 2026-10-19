// =============================================================================
// ObjectiveDialog.cpp
// =============================================================================
//
// Implementation of the ObjectiveDialog preferences dialog — see
// ObjectiveDialog.h. Modeled directly on AutoStopDialog's
// read-QSettings-on-open / write-QSettings-on-OK pattern.
// =============================================================================

#include "ObjectiveDialog.h"

#include <QDoubleSpinBox>
#include <QLabel>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QSettings>
#include <QFont>


// =============================================================================
// Internal constants
// =============================================================================
namespace
{
    constexpr const char* kKeyMagnification = "objective/magnification";
    constexpr double kDefaultMagnification  = 5.0;
}


// =============================================================================
// Constructor
// =============================================================================
ObjectiveDialog::ObjectiveDialog(QWidget* parent)
    : QDialog(parent)
    , m_magnificationSpin(nullptr)
{
    setWindowTitle(QStringLiteral("Microscope Objective"));
    setMinimumWidth(340);

    QSettings settings;
    double savedMagnification = settings.value(
        QLatin1String(kKeyMagnification), kDefaultMagnification).toDouble();

    QVBoxLayout* outerLayout = new QVBoxLayout(this);
    outerLayout->setSpacing(12);
    outerLayout->setContentsMargins(14, 14, 14, 14);

    QLabel* headingLabel = new QLabel(QStringLiteral("Microscope Objective"), this);
    QFont boldFont = headingLabel->font();
    boldFont.setBold(true);
    headingLabel->setFont(boldFont);
    outerLayout->addWidget(headingLabel);

    QLabel* noteLabel = new QLabel(QStringLiteral(
        "Enter the magnification of the objective currently in front of the "
        "camera (e.g. 5 for a 5x objective). This is saved into every "
        "acquisition's metadata.json so downstream analysis can convert pixel "
        "measurements into real units at the sample plane."), this);
    noteLabel->setWordWrap(true);
    noteLabel->setStyleSheet(QStringLiteral("color: #666; font-size: 11px;"));
    outerLayout->addWidget(noteLabel);

    QFormLayout* formLayout = new QFormLayout();
    formLayout->setRowWrapPolicy(QFormLayout::DontWrapRows);
    formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    formLayout->setSpacing(8);

    m_magnificationSpin = new QDoubleSpinBox(this);
    m_magnificationSpin->setDecimals(2);
    m_magnificationSpin->setRange(0.1, 1000.0);
    m_magnificationSpin->setSingleStep(1.0);
    m_magnificationSpin->setSuffix(QStringLiteral("x"));
    m_magnificationSpin->setValue(savedMagnification > 0.0 ? savedMagnification : kDefaultMagnification);
    m_magnificationSpin->setToolTip(QStringLiteral(
        "Objective magnification, e.g. 5 for a 5x objective"));

    formLayout->addRow(QStringLiteral("Magnification:"), m_magnificationSpin);
    outerLayout->addLayout(formLayout);

    QDialogButtonBox* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &ObjectiveDialog::onAccepted);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outerLayout->addWidget(buttonBox);
}


// =============================================================================
// onAccepted — save when the user clicks OK
// =============================================================================
void ObjectiveDialog::onAccepted()
{
    QSettings settings;
    settings.setValue(QLatin1String(kKeyMagnification), m_magnificationSpin->value());
    accept();
}


// =============================================================================
// magnification — static accessor
// =============================================================================
double ObjectiveDialog::magnification()
{
    return QSettings().value(QLatin1String(kKeyMagnification), 0.0).toDouble();
}
