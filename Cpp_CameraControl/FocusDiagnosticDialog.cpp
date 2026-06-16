// =============================================================================
// FocusDiagnosticDialog.cpp
// =============================================================================

#include "FocusDiagnosticDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QPushButton>
#include <QSpinBox>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPaintEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QSettings>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <numeric>

static constexpr const char* kKeyWindowPoints = "FocusDiagnostic/WindowPoints";
static constexpr const char* kKeyMetric       = "FocusDiagnostic/Metric";


// =============================================================================
// FocusPlotWidget::FocusPlotWidget
// =============================================================================
FocusPlotWidget::FocusPlotWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(400, 200);
    setMouseTracking(false);

    QPalette p = palette();
    p.setColor(QPalette::Window, QColor(30, 30, 30));
    setPalette(p);
    setAutoFillBackground(true);

    // Accept wheel events
    setFocusPolicy(Qt::WheelFocus);
}


// =============================================================================
// FocusPlotWidget::plotArea
// =============================================================================
QRect FocusPlotWidget::plotArea() const
{
    return rect().adjusted(kLeftMargin, 10, -8, -18);
}


// =============================================================================
// FocusPlotWidget::setWindowPoints
// =============================================================================
void FocusPlotWidget::setWindowPoints(int n)
{
    m_windowPoints = std::max(10, n);

    // Trim std window to new size
    while (m_stdWindow.size() > m_windowPoints)
        m_stdWindow.removeFirst();

    // If in autoscroll mode, snap the view width to the new window size
    if (m_autoScroll)
        m_viewWidth = m_windowPoints;

    update();
}


// =============================================================================
// FocusPlotWidget::addValue
// =============================================================================
void FocusPlotWidget::addValue(double v)
{
    // ---- Outlier rejection ----
    // Compare the new value against the last received value.
    // If the jump is more than 10× the running std, reject it from the plot
    // (but always add it to the std-tracking window so a real step change is
    // detected quickly as the std grows to accommodate the new level).
    bool isOutlier = false;

    if (m_stdWindow.size() >= 3 && !m_plotValues.isEmpty())
    {
        // Compute std BEFORE adding the new value so one spike doesn't excuse itself.
        double sum = 0.0, sum2 = 0.0;
        for (double x : m_stdWindow) { sum += x; sum2 += x * x; }
        const double mean     = sum  / m_stdWindow.size();
        const double variance = sum2 / m_stdWindow.size() - mean * mean;
        const double stdDev   = (variance > 0.0) ? std::sqrt(variance) : 0.0;

        const double lastReceived = m_stdWindow.last();
        if (stdDev > 0.0 && std::abs(v - lastReceived) > 10.0 * stdDev)
            isOutlier = true;
    }

    // Always track in the std window (bounded to m_windowPoints)
    m_stdWindow.append(v);
    if (m_stdWindow.size() > m_windowPoints)
        m_stdWindow.removeFirst();

    if (!isOutlier)
    {
        m_plotValues.append(v);
        if (m_plotValues.size() > kMaxHistory)
            m_plotValues.removeFirst();

        if (m_autoScroll)
            m_viewStart = std::max(0.0, (double)m_plotValues.size() - m_viewWidth);
    }

    update();
}


// =============================================================================
// FocusPlotWidget::reset
// =============================================================================
void FocusPlotWidget::reset()
{
    m_plotValues.clear();
    m_stdWindow.clear();
    m_viewStart  = 0.0;
    m_viewWidth  = m_windowPoints;
    m_autoScroll = true;
    update();
}


// =============================================================================
// FocusPlotWidget::paintEvent
// =============================================================================
void FocusPlotWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 30));

    const QRect pa = plotArea();

    // ---- No data ----
    if (m_plotValues.isEmpty())
    {
        p.setPen(QColor(100, 100, 100));
        p.setFont(QFont("Arial", 10));
        p.drawText(rect(), Qt::AlignCenter, "No data");

        // Still draw the border
        p.setPen(QPen(QColor(60, 60, 60), 1));
        p.drawRect(pa);
        return;
    }

    // ---- Compute visible index range ----
    const int n = m_plotValues.size();
    const int iStart = static_cast<int>(std::max(0.0, std::floor(m_viewStart)));
    const int iEnd   = static_cast<int>(std::min((double)n,
                                                  std::ceil(m_viewStart + m_viewWidth)));
    if (iEnd <= iStart)
    {
        p.setPen(QColor(100, 100, 100));
        p.drawText(pa, Qt::AlignCenter, "No data in view");
        return;
    }

    // ---- Find min/max in visible range ----
    double minVal = m_plotValues[iStart];
    double maxVal = m_plotValues[iStart];
    for (int i = iStart + 1; i < iEnd && i < n; ++i)
    {
        minVal = std::min(minVal, m_plotValues[i]);
        maxVal = std::max(maxVal, m_plotValues[i]);
    }
    if (minVal == maxVal) maxVal = minVal + 1.0;
    const double range = maxVal - minVal;

    // Helper: map (data index, value) → widget point
    auto toWidget = [&](double idx, double v) -> QPointF {
        const double xFrac = (idx - m_viewStart) / m_viewWidth;
        const double yFrac = (v - minVal) / range;
        return QPointF(pa.left() + xFrac * pa.width(),
                       pa.bottom() - yFrac * pa.height());
    };

    // ---- Grid lines ----
    p.setPen(QPen(QColor(55, 55, 55), 1));
    for (int i = 1; i <= 4; ++i)
    {
        const int y = pa.bottom() - static_cast<int>(0.2 * i * pa.height());
        p.drawLine(pa.left(), y, pa.right(), y);
    }

    // ---- Plot line ----
    p.setPen(QPen(QColor(0, 220, 200), 2));
    QPointF prev;
    bool hasPrev = false;
    for (int i = iStart; i < iEnd && i < n; ++i)
    {
        const QPointF pt = toWidget(i, m_plotValues[i]);
        if (hasPrev)
            p.drawLine(prev, pt);
        prev   = pt;
        hasPrev = true;
    }

    // ---- Dot at the most recent visible point ----
    const int lastIdx = std::min(iEnd - 1, n - 1);
    if (lastIdx >= iStart)
    {
        const QPointF dot = toWidget(lastIdx, m_plotValues[lastIdx]);
        p.setBrush(QColor(0, 220, 200));
        p.setPen(Qt::NoPen);
        p.drawEllipse(dot, 4.0, 4.0);
    }

    // ---- Border ----
    p.setPen(QPen(QColor(80, 80, 80), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(pa);

    // ---- Y-axis labels ----
    p.setPen(QColor(150, 150, 150));
    p.setFont(QFont("Arial", 8));
    const QString minStr = QString::number(minVal, 'g', 4);
    const QString maxStr = QString::number(maxVal, 'g', 4);
    p.drawText(0, pa.bottom() - 8, kLeftMargin - 4, 16, Qt::AlignRight | Qt::AlignVCenter, minStr);
    p.drawText(0, pa.top()    - 8, kLeftMargin - 4, 16, Qt::AlignRight | Qt::AlignVCenter, maxStr);

    // ---- X-axis: point count / scroll indicator ----
    p.setPen(QColor(90, 90, 90));
    p.setFont(QFont("Arial", 8));
    const QString info = m_autoScroll
        ? QString("%1 pts  [live]").arg(n)
        : QString("%1/%2 pts  [scroll to end to resume]").arg(iEnd - iStart).arg(n);
    p.drawText(pa.left(), pa.bottom() + 2, pa.width(), 16,
               Qt::AlignLeft | Qt::AlignTop, info);
}


// =============================================================================
// FocusPlotWidget::wheelEvent — zoom X axis centered on cursor
// =============================================================================
void FocusPlotWidget::wheelEvent(QWheelEvent* event)
{
    const QRect pa = plotArea();
    if (pa.width() <= 0) { event->ignore(); return; }

    // Fraction of the plot area where the cursor is (0 = left edge, 1 = right edge)
    const double cursorFrac = qBound(0.0,
        (event->position().x() - pa.left()) / pa.width(), 1.0);

    // Data index currently under the cursor
    const double cursorIndex = m_viewStart + cursorFrac * m_viewWidth;

    // Scale the view width: scrolling up zooms in (narrows window), down zooms out
    const double factor   = std::pow(1.2, event->angleDelta().y() / 120.0);
    double newWidth = m_viewWidth / factor;
    newWidth = qBound(5.0, newWidth, static_cast<double>(kMaxHistory));

    // Adjust viewStart so the cursor stays over the same data index
    m_viewStart = cursorIndex - cursorFrac * newWidth;
    m_viewWidth = newWidth;

    // Clamp to valid range
    const int n = m_plotValues.size();
    if (n > 0)
        m_viewStart = qBound(0.0, m_viewStart, static_cast<double>(n) - 1.0);
    else
        m_viewStart = 0.0;

    // Re-enable autoscroll when the user has scrolled to the right end
    m_autoScroll = (m_viewStart + m_viewWidth >= static_cast<double>(n));

    update();
    event->accept();
}


// =============================================================================
// FocusPlotWidget::mousePressEvent — begin pan drag
// =============================================================================
void FocusPlotWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        m_dragging           = true;
        m_dragStartPos       = event->position();
        m_dragStartViewStart = m_viewStart;
        setCursor(Qt::ClosedHandCursor);
    }
}


// =============================================================================
// FocusPlotWidget::mouseMoveEvent — pan view
// =============================================================================
void FocusPlotWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging) return;

    const QRect pa = plotArea();
    if (pa.width() <= 0) return;

    // dx pixels → how many data indices to shift
    const double dx       = event->position().x() - m_dragStartPos.x();
    const double deltaIdx = dx / pa.width() * m_viewWidth;

    m_viewStart = m_dragStartViewStart - deltaIdx;

    // Clamp
    const int n = m_plotValues.size();
    if (n > 0)
        m_viewStart = qBound(0.0, m_viewStart, static_cast<double>(n) - 1.0);
    else
        m_viewStart = 0.0;

    m_autoScroll = (m_viewStart + m_viewWidth >= static_cast<double>(n));

    update();
}


// =============================================================================
// FocusPlotWidget::mouseReleaseEvent
// =============================================================================
void FocusPlotWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        m_dragging = false;
        setCursor(Qt::ArrowCursor);
    }
}


// =============================================================================
// FocusPlotWidget::mouseDoubleClickEvent — reset to autoscroll
// =============================================================================
void FocusPlotWidget::mouseDoubleClickEvent(QMouseEvent*)
{
    m_viewWidth  = m_windowPoints;
    m_autoScroll = true;
    if (!m_plotValues.isEmpty())
        m_viewStart = std::max(0.0, (double)m_plotValues.size() - m_viewWidth);
    update();
}


// =============================================================================
// FocusDiagnosticDialog::FocusDiagnosticDialog
// =============================================================================
FocusDiagnosticDialog::FocusDiagnosticDialog(QWidget* parent)
    : QDialog(parent)
    , m_metricCombo(nullptr)
    , m_resetButton(nullptr)
    , m_redrawRoiButton(nullptr)
    , m_windowSpinBox(nullptr)
    , m_plot(nullptr)
    , m_valueLabel(nullptr)
{
    setWindowTitle("Focus Diagnostic");
    setWindowFlags(Qt::Window);
    setMinimumSize(520, 360);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(6);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    // =========================================================================
    // TOP ROW: metric selector | value | window spinbox | reset | redraw ROI
    // =========================================================================
    QHBoxLayout* topRow = new QHBoxLayout();

    QLabel* metricLabel = new QLabel("Metric:");
    m_metricCombo = new QComboBox();
    m_metricCombo->addItem("Image Variance");
    m_metricCombo->addItem("Brenner Gradient");
    m_metricCombo->addItem("Laplacian Variance");
    m_metricCombo->addItem("Tenengrad (Sobel)");
    m_metricCombo->addItem("Entropy");

    m_valueLabel = new QLabel("Value: --");
    m_valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_valueLabel->setMinimumWidth(160);
    QFont monoFont("Courier");
    monoFont.setPointSize(10);
    m_valueLabel->setFont(monoFont);

    QLabel* windowLabel = new QLabel("Window:");
    m_windowSpinBox = new QSpinBox();
    m_windowSpinBox->setRange(10, 100000);
    m_windowSpinBox->setSingleStep(100);
    m_windowSpinBox->setSuffix(" pts");
    m_windowSpinBox->setToolTip(
        "Number of points to show and use for outlier-rejection std.\n"
        "Press Enter to apply.  Scroll/zoom on the plot to inspect history; double-click to return to live view.");

    m_resetButton = new QPushButton("Reset");
    m_resetButton->setMaximumWidth(70);
    m_resetButton->setToolTip("Clear all plot data and restart");

    m_redrawRoiButton = new QPushButton("Redraw ROI");
    m_redrawRoiButton->setToolTip("Draw a new ROI on the preview image for this diagnostic");

    topRow->addWidget(metricLabel);
    topRow->addWidget(m_metricCombo);
    topRow->addStretch();
    topRow->addWidget(m_valueLabel);
    topRow->addSpacing(10);
    topRow->addWidget(windowLabel);
    topRow->addWidget(m_windowSpinBox);
    topRow->addSpacing(6);
    topRow->addWidget(m_resetButton);
    topRow->addWidget(m_redrawRoiButton);

    // =========================================================================
    // RESTORE SAVED SETTINGS
    // =========================================================================
    {
        QSettings s;
        m_windowSpinBox->setValue(s.value(kKeyWindowPoints, 300).toInt());
        m_metricCombo->setCurrentIndex(
            qBound(0, s.value(kKeyMetric, 0).toInt(), m_metricCombo->count() - 1));
    }

    // =========================================================================
    // PLOT WIDGET
    // =========================================================================
    m_plot = new FocusPlotWidget();
    m_plot->setWindowPoints(m_windowSpinBox->value());

    // =========================================================================
    // LAYOUT
    // =========================================================================
    mainLayout->addLayout(topRow);
    mainLayout->addWidget(m_plot, 1);

    // =========================================================================
    // SIGNAL CONNECTIONS
    // =========================================================================
    connect(m_resetButton, &QPushButton::clicked,
            this, &FocusDiagnosticDialog::onResetClicked);

    connect(m_redrawRoiButton, &QPushButton::clicked,
            this, &FocusDiagnosticDialog::redrawRoiRequested);

    // Only apply window change when the user presses Enter (not on every arrow click).
    connect(m_windowSpinBox, &QSpinBox::editingFinished,
            this, &FocusDiagnosticDialog::onWindowPointsChanged);

    connect(m_metricCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        QSettings().setValue(kKeyMetric, index);
    });
}


// =============================================================================
// FocusDiagnosticDialog::closeEvent
// =============================================================================
void FocusDiagnosticDialog::closeEvent(QCloseEvent* event)
{
    emit dialogClosed();
    QDialog::closeEvent(event);
}


// =============================================================================
// FocusDiagnosticDialog::addFrame
// =============================================================================
void FocusDiagnosticDialog::addFrame(const QImage& image)
{
    if (image.isNull()) return;

    // Convert to Grayscale8 if needed
    QImage gray = image;
    if (gray.format() != QImage::Format_Grayscale8)
        gray = image.convertToFormat(QImage::Format_Grayscale8);

    // Downsample to max 512×512 for performance
    if (gray.width() > 512 || gray.height() > 512)
        gray = gray.scaled(512, 512, Qt::KeepAspectRatio, Qt::FastTransformation);

    const int idx = m_metricCombo->currentIndex();
    const double score = computeMetric(static_cast<Metric>(idx), gray);

    m_plot->addValue(score);
    m_valueLabel->setText(QString("Value: %1").arg(score, 0, 'g', 5));
}


// =============================================================================
// FocusDiagnosticDialog::onResetClicked
// =============================================================================
void FocusDiagnosticDialog::onResetClicked()
{
    m_plot->reset();
    m_valueLabel->setText("Value: --");
}


// =============================================================================
// FocusDiagnosticDialog::onWindowPointsChanged
// =============================================================================
void FocusDiagnosticDialog::onWindowPointsChanged()
{
    const int pts = m_windowSpinBox->value();
    QSettings().setValue(kKeyWindowPoints, pts);
    m_plot->setWindowPoints(pts);
}



// =============================================================================
// FocusDiagnosticDialog::computeMetric
// =============================================================================
double FocusDiagnosticDialog::computeMetric(Metric m, const QImage& img)
{
    if (img.isNull() || img.format() != QImage::Format_Grayscale8)
        return 0.0;

    const uchar* p = img.constBits();
    int w = img.width();
    int h = img.height();
    int stride = img.bytesPerLine();

    switch (m)
    {
        case Metric::Variance:   return computeVariance  (p, w, h, stride);
        case Metric::Brenner:    return computeBrenner   (p, w, h, stride);
        case Metric::Laplacian:  return computeLaplacian (p, w, h, stride);
        case Metric::Tenengrad:  return computeTenengrad (p, w, h, stride);
        case Metric::Entropy:    return computeEntropy   (p, w, h, stride);
        default:                 return 0.0;
    }
}


// =============================================================================
// computeVariance
// =============================================================================
double FocusDiagnosticDialog::computeVariance(const uchar* p, int w, int h, int stride)
{
    if (w <= 0 || h <= 0) return 0.0;

    int64_t sum = 0, sum2 = 0, count = 0;
    for (int y = 0; y < h; ++y)
    {
        const uchar* row = p + y * stride;
        for (int x = 0; x < w; ++x)
        {
            int64_t val = row[x];
            sum  += val;
            sum2 += val * val;
            count++;
        }
    }
    if (count == 0) return 0.0;
    const double mean  = static_cast<double>(sum)  / count;
    const double mean2 = static_cast<double>(sum2) / count;
    return mean2 - mean * mean;
}


// =============================================================================
// computeBrenner
// =============================================================================
double FocusDiagnosticDialog::computeBrenner(const uchar* p, int w, int h, int stride)
{
    if (w <= 0 || h <= 3) return 0.0;
    int64_t sum = 0, count = 0;
    for (int y = 0; y < h - 2; ++y)
    {
        const uchar* row1 = p + y       * stride;
        const uchar* row3 = p + (y + 2) * stride;
        for (int x = 0; x < w; ++x)
        {
            int64_t diff = static_cast<int64_t>(row3[x]) - static_cast<int64_t>(row1[x]);
            sum += diff * diff;
            count++;
        }
    }
    return (count > 0) ? static_cast<double>(sum) / count : 0.0;
}


// =============================================================================
// computeLaplacian
// =============================================================================
double FocusDiagnosticDialog::computeLaplacian(const uchar* p, int w, int h, int stride)
{
    if (w <= 2 || h <= 2) return 0.0;
    double sum = 0.0, count = 0.0;
    for (int y = 1; y < h - 1; ++y)
    {
        const uchar* rowUp  = p + (y - 1) * stride;
        const uchar* rowMid = p + y       * stride;
        const uchar* rowDn  = p + (y + 1) * stride;
        for (int x = 1; x < w - 1; ++x)
        {
            double lap = 4.0 * rowMid[x] - rowMid[x-1] - rowMid[x+1] - rowUp[x] - rowDn[x];
            sum += lap;
            count += 1.0;
        }
    }
    if (count == 0.0) return 0.0;
    const double mean = sum / count;
    double sumSq = 0.0;
    for (int y = 1; y < h - 1; ++y)
    {
        const uchar* rowUp  = p + (y - 1) * stride;
        const uchar* rowMid = p + y       * stride;
        const uchar* rowDn  = p + (y + 1) * stride;
        for (int x = 1; x < w - 1; ++x)
        {
            double lap  = 4.0 * rowMid[x] - rowMid[x-1] - rowMid[x+1] - rowUp[x] - rowDn[x];
            double diff = lap - mean;
            sumSq += diff * diff;
        }
    }
    return sumSq / count;
}


// =============================================================================
// computeTenengrad
// =============================================================================
double FocusDiagnosticDialog::computeTenengrad(const uchar* p, int w, int h, int stride)
{
    if (w <= 2 || h <= 2) return 0.0;
    int64_t sum = 0, count = 0;
    for (int y = 1; y < h - 1; ++y)
    {
        const uchar* rowUp  = p + (y - 1) * stride;
        const uchar* rowMid = p + y       * stride;
        const uchar* rowDn  = p + (y + 1) * stride;
        for (int x = 1; x < w - 1; ++x)
        {
            int64_t gx = -static_cast<int64_t>(rowUp[x-1]) + static_cast<int64_t>(rowUp[x+1])
                       - 2 * static_cast<int64_t>(rowMid[x-1]) + 2 * static_cast<int64_t>(rowMid[x+1])
                       - static_cast<int64_t>(rowDn[x-1]) + static_cast<int64_t>(rowDn[x+1]);
            int64_t gy = -static_cast<int64_t>(rowUp[x-1]) - 2*static_cast<int64_t>(rowUp[x]) - static_cast<int64_t>(rowUp[x+1])
                       + static_cast<int64_t>(rowDn[x-1]) + 2*static_cast<int64_t>(rowDn[x]) + static_cast<int64_t>(rowDn[x+1]);
            sum += gx * gx + gy * gy;
            count++;
        }
    }
    return (count > 0) ? static_cast<double>(sum) / count : 0.0;
}


// =============================================================================
// computeEntropy
// =============================================================================
double FocusDiagnosticDialog::computeEntropy(const uchar* p, int w, int h, int stride)
{
    if (w <= 0 || h <= 0) return 0.0;
    int hist[256] = {};
    int64_t count = 0;
    for (int y = 0; y < h; ++y)
    {
        const uchar* row = p + y * stride;
        for (int x = 0; x < w; ++x) { hist[row[x]]++; count++; }
    }
    if (count == 0) return 0.0;
    double entropy = 0.0;
    const double invCount = 1.0 / count;
    for (int i = 0; i < 256; ++i)
    {
        if (hist[i] > 0)
        {
            const double prob = hist[i] * invCount;
            entropy -= prob * std::log2(prob);
        }
    }
    return entropy;
}
