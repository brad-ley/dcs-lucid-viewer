// =============================================================================
// HistogramWidget.cpp
// =============================================================================
//
// Implementation of the rotated histogram + level handles.
// See HistogramWidget.h for full feature description.
// =============================================================================

#include "HistogramWidget.h"

#include <QPainter>        // QPainter — draws shapes, lines, text onto the widget
#include <QPen>            // QPen — defines line color and width
#include <QPolygon>        // QPolygon — a list of QPoints, used for the triangle markers
#include <QMouseEvent>     // QMouseEvent — mouse click/drag events
#include <QWheelEvent>     // QWheelEvent — scroll-wheel events used for zooming
#include <algorithm>       // std::max, std::min
#include <cmath>           // std::log1p, std::abs


// How many pixels away from a handle you can click and still grab it.
static constexpr int GRAB_RADIUS = 7;


// =============================================================================
// Constructor
// =============================================================================
HistogramWidget::HistogramWidget(QWidget* parent)
    : QWidget(parent)
{
    // Fixed width so the layout reserves exactly 70 px for this widget.
    setFixedWidth(70);

    // Dark background to match the image display area.
    setStyleSheet("background-color: #1e1e1e; border-left: 1px solid #333;");

    setMouseTracking(true);
    setToolTip("Scroll to zoom · Right-drag to pan · Double-click to reset view");
}


// =============================================================================
// updateHistogram
// =============================================================================
void HistogramWidget::updateHistogram(const QImage& image)
{
    m_bins.fill(0);

    if (image.isNull())
        return;

    if (image.format() == QImage::Format_Grayscale8)
    {
        // Fast path: iterate directly over the raw byte data.
        for (int y = 0; y < image.height(); ++y)
        {
            const uchar* line = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x)
                ++m_bins[line[x]];
        }
    }
    else
    {
        // For colour or other formats, convert to grayscale first.
        // This is slightly slower but rarely triggers (most cameras are mono).
        QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
        for (int y = 0; y < gray.height(); ++y)
        {
            const uchar* line = gray.constScanLine(y);
            for (int x = 0; x < gray.width(); ++x)
                ++m_bins[line[x]];
        }
    }

    // Find the peak bin count, used to scale the bar lengths.
    // We deliberately skip bins 0 and 255: these can be enormous (black borders,
    // saturated hot pixels) and would crush the useful mid-tone bars to nothing.
    m_maxBin = 1;
    for (int i = 1; i < 255; ++i)
        m_maxBin = std::max(m_maxBin, m_bins[i]);

    update();   // Ask Qt to repaint this widget
}


// =============================================================================
// Coordinate helpers
// =============================================================================

int HistogramWidget::levelToY(double level) const
{
    // Map the level through [m_viewMin, m_viewMax] → widget height.
    // Levels below viewMin appear at the bottom; above viewMax at the top.
    int h = height();
    if (h <= 1) return 0;
    double viewRange = m_viewMax - m_viewMin;
    if (viewRange <= 0.0) return 0;
    double fraction = (level - m_viewMin) / viewRange;
    fraction = std::max(0.0, std::min(1.0, fraction));  // clamp to widget edges
    return static_cast<int>((1.0 - fraction) * (h - 1));
}

double HistogramWidget::yToLevel(int y) const
{
    // Inverse of levelToY — maps a pixel Y back to a level in [0, 1].
    int h = height();
    if (h <= 1) return 0.0;
    double fraction = 1.0 - static_cast<double>(y) / (h - 1);
    // Map from [0, 1] fraction → [m_viewMin, m_viewMax] level range
    double level = m_viewMin + fraction * (m_viewMax - m_viewMin);
    return std::max(0.0, std::min(1.0, level));
}

int HistogramWidget::handleAtY(int y) const
{
    // Check high handle first (it's drawn on top, so it takes priority if they overlap).
    if (std::abs(y - levelToY(m_highLevel)) <= GRAB_RADIUS) return 1;
    if (std::abs(y - levelToY(m_lowLevel))  <= GRAB_RADIUS) return 0;
    return -1;
}


// =============================================================================
// paintEvent
// =============================================================================
void HistogramWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const int w = width();
    const int h = height();

    p.fillRect(rect(), QColor("#1e1e1e"));

    if (h <= 0 || w <= 0)
        return;

    // ------------------------------------------------------------------
    // Draw histogram bars
    // ------------------------------------------------------------------
    //
    // There are 256 bins.  Each bin corresponds to a horizontal strip of
    // height (h / 256) pixels.  Bin 0 is at the bottom; bin 255 at the top.
    //
    // Bar length = log-scaled fraction of m_maxBin, drawn left-to-right.
    // We use a log scale so that large peaks don't completely hide the tails.
    //
    // C++ CONCEPT — std::log1p:
    //   log1p(x) = ln(1 + x).  Using 1+x avoids log(0) when a bin is empty.
    //   The base-e log is fine here because we only care about the ratio
    //   log(count) / log(maxCount), which is scale-invariant.

    const double logMax = std::log1p(static_cast<double>(m_maxBin));

    for (int bin = 0; bin < 256; ++bin)
    {
        // Each bin spans the intensity range [bin/255, (bin+1)/255].
        // Use levelToY() so the strip positions respect the current zoom window.
        int yBot = levelToY( bin      / 255.0);  // bottom edge (larger Y = lower on screen)
        int yTop = levelToY((bin + 1) / 255.0);  // top edge

        int stripTop    = yTop;
        int stripBottom = yBot;
        int stripHeight = stripBottom - stripTop;
        if (stripHeight <= 0) stripHeight = 1;

        // Skip strips that are entirely outside the widget
        if (stripBottom < 0 || stripTop >= h) continue;

        // Clip to widget bounds
        int clippedTop    = std::max(0, stripTop);
        int clippedHeight = std::min(h, stripBottom) - clippedTop;
        if (clippedHeight <= 0) continue;

        // Bar width proportional to log(count / maxCount), occupying the widget width.
        // Reserve 2 px on the right for the edge border.
        double logCount = std::log1p(static_cast<double>(m_bins[bin]));
        int barWidth = (logMax > 0)
            ? static_cast<int>((logCount / logMax) * (w - 2))
            : 0;
        if (barWidth < 0) barWidth = 0;

        // Colour: brighter (light blue) inside the level range, muted outside.
        double level = bin / 255.0;
        bool inRange = (level >= m_lowLevel && level <= m_highLevel);
        QColor barColor = inRange ? QColor(160, 190, 220) : QColor(70, 80, 95);

        p.fillRect(0, clippedTop, barWidth, clippedHeight, barColor);
    }

    // ------------------------------------------------------------------
    // Shade regions outside the level handles
    // ------------------------------------------------------------------
    //
    // Below the low handle → dark overlay  (pixels clipped to black)
    // Above the high handle → light overlay (pixels clipped to white)

    int yLow  = levelToY(m_lowLevel);
    int yHigh = levelToY(m_highLevel);

    if (yLow < h)
        p.fillRect(0, yLow, w, h - yLow, QColor(0, 0, 0, 120));   // Dark: crushed shadows

    if (yHigh > 0)
        p.fillRect(0, 0, w, yHigh, QColor(255, 255, 255, 35));     // Light: clipped highlights

    // ------------------------------------------------------------------
    // Draw the handle lines and triangle markers
    // ------------------------------------------------------------------

    auto drawHandle = [&](double level, const QColor& lineColor, const QString& label)
    {
        int y = levelToY(level);

        // Horizontal line across the full width
        p.setPen(QPen(lineColor, 2));
        p.drawLine(0, y, w, y);

        // Solid triangle on the left edge pointing right — acts as a drag handle grip
        QPolygon tri;
        tri << QPoint(0, y - 5)
            << QPoint(0, y + 5)
            << QPoint(8, y);
        p.setBrush(lineColor);
        p.setPen(Qt::NoPen);
        p.drawPolygon(tri);

        // Label ("Lo" / "Hi") just to the right of the triangle
        p.setPen(lineColor);
        QFont f = p.font();
        f.setPixelSize(9);
        f.setBold(true);
        p.setFont(f);
        // Position label above the line for Hi, below for Lo, to avoid overlap
        int labelY = (level > 0.5) ? y + 11 : y - 3;
        p.drawText(10, labelY, label);
    };

    drawHandle(m_lowLevel,  QColor(80,  200, 255), "Lo");   // Cyan  — black point
    drawHandle(m_highLevel, QColor(255, 210, 80),  "Hi");   // Yellow — white point
}


// =============================================================================
// computePercentileLevels
// =============================================================================
//
// Computes black-point and white-point levels from the existing histogram bins
// using percentile statistics.  No image scan needed — we reuse the bins that
// updateHistogram() already built.
//
// HOW PERCENTILES WORK HERE:
//   We walk the cumulative sum of bins from left (darkest) to right (brightest).
//   The Nth percentile is the first bin where the running total >= N% of all pixels.
//   Example: lowPct=3 finds the bin below which only 3% of pixels fall — a robust
//   black point that ignores stray dark noise.
//
// C++ CONCEPT — long long:
//   A plain 'int' can hold up to ~2 billion.  A 12 MP image has 12 million pixels,
//   well within int range.  But when we accumulate the full bin sum, using long long
//   (64-bit integer) makes the intent clear and future-proofs for large frames.
// =============================================================================
QPair<double, double> HistogramWidget::computePercentileLevels(
    double lowPct, double highPct) const
{
    // Count total pixels from the bins
    long long total = 0;
    for (int c : m_bins) total += c;

    if (total == 0)
        return {0.0, 1.0};   // No data → full range

    // How many pixels must be below each threshold
    const long long lowTarget  = static_cast<long long>(total * lowPct  / 100.0);
    const long long highTarget = static_cast<long long>(total * highPct / 100.0);

    int lowBin  = 0;
    int highBin = 255;

    // Walk from dark to bright to find the low (black-point) bin
    long long cumulative = 0;
    for (int i = 0; i < 256; ++i)
    {
        cumulative += m_bins[i];
        if (cumulative >= lowTarget)
        {
            lowBin = i;
            break;
        }
    }

    // Walk from dark to bright to find the high (white-point) bin
    cumulative = 0;
    for (int i = 0; i < 256; ++i)
    {
        cumulative += m_bins[i];
        if (cumulative >= highTarget)
        {
            highBin = i;
            break;
        }
    }

    // Guarantee at least a 1-bin gap so the remap LUT never divides by zero
    if (highBin <= lowBin)
        highBin = std::min(255, lowBin + 1);

    return { lowBin / 255.0, highBin / 255.0 };
}


// =============================================================================
// setLevels
// =============================================================================
//
// Programmatically move both handles without emitting levelsChanged.
// Called by PreviewDialog's auto-contrast path, which reads the new levels
// immediately afterward via lowLevel()/highLevel() — no signal needed.
// =============================================================================
void HistogramWidget::setLevels(double low, double high)
{
    m_lowLevel  = std::max(0.0, std::min(low,  1.0));
    m_highLevel = std::max(0.0, std::min(high, 1.0));

    // Ensure separation so the LUT denominator is never zero
    if (m_highLevel <= m_lowLevel)
        m_highLevel = std::min(1.0, m_lowLevel + 0.01);

    update();   // Repaint so handles move visually
}


// =============================================================================
// Mouse events — drag handles
// =============================================================================

void HistogramWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        m_dragging = handleAtY(event->pos().y());
    }
    else if (event->button() == Qt::RightButton)
    {
        // Begin a pan drag: record the Y pixel and the view extents at drag start
        // so we can compute the offset continuously in mouseMoveEvent.
        m_isPanning      = true;
        m_panStartY      = event->pos().y();
        m_panStartMin    = m_viewMin;
        m_panStartMax    = m_viewMax;
        setCursor(Qt::ClosedHandCursor);
    }
}

void HistogramWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging == 0)
    {
        // Dragging the LOW (black-point) handle.
        // Clamp so it stays at least 1% below the high handle.
        double newLevel = yToLevel(event->pos().y());
        m_lowLevel = std::min(newLevel, m_highLevel - 0.01);
        update();
        emit levelsChanged(m_lowLevel, m_highLevel);
    }
    else if (m_dragging == 1)
    {
        // Dragging the HIGH (white-point) handle.
        // Clamp so it stays at least 1% above the low handle.
        double newLevel = yToLevel(event->pos().y());
        m_highLevel = std::max(newLevel, m_lowLevel + 0.01);
        update();
        emit levelsChanged(m_lowLevel, m_highLevel);
    }
    else if (m_isPanning)
    {
        // Right-drag pan: convert the pixel delta to a level-space offset and shift
        // the view window, clamping so it cannot be dragged outside [0, 1].
        int h = height();
        if (h <= 1) return;

        double viewRange  = m_viewMax - m_viewMin;
        // Positive dy (mouse moved down) means we panned toward lower intensities.
        double levelDelta = static_cast<double>(event->pos().y() - m_panStartY)
                            / (h - 1) * viewRange;

        double newMin = m_panStartMin + levelDelta;
        double newMax = m_panStartMax + levelDelta;

        // Clamp: don't let the view scroll past [0, 1]
        if (newMin < 0.0) { newMax -= newMin; newMin = 0.0; }
        if (newMax > 1.0) { newMin -= (newMax - 1.0); newMax = 1.0; }
        m_viewMin = std::max(0.0, newMin);
        m_viewMax = std::min(1.0, newMax);
        update();
    }
    else
    {
        // No drag active — update cursor to hint at what actions are available.
        // Show a resize cursor near a handle, open hand otherwise.
        if (handleAtY(event->pos().y()) >= 0)
            setCursor(Qt::SizeVerCursor);
        else
            setCursor(Qt::OpenHandCursor);
    }
}

void HistogramWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        m_dragging = -1;
    else if (event->button() == Qt::RightButton)
    {
        m_isPanning = false;
        setCursor(Qt::OpenHandCursor);
    }
}


// =============================================================================
// wheelEvent — zoom the intensity axis centered on the cursor position
// =============================================================================
//
// Each notch of the scroll wheel shrinks (zoom in) or expands (zoom out) the
// visible intensity window by 20 %.  The intensity value under the cursor stays
// fixed so the zoom feels anchored to whatever the user is looking at.
//
// C++ CONCEPT — QWheelEvent::angleDelta:
//   angleDelta().y() returns the vertical scroll amount in units of 1/8 degree.
//   A single "notch" is typically 120 units (= 15 degrees).  Positive = scroll up.
void HistogramWidget::wheelEvent(QWheelEvent* event)
{
    double cursorLevel = yToLevel(static_cast<int>(event->position().y()));

    // Each notch zooms by 20%.  Scroll up (positive) → zoom in (smaller range).
    double factor = (event->angleDelta().y() > 0) ? 0.8 : 1.25;

    double oldRange = m_viewMax - m_viewMin;
    double newRange = std::max(0.04, std::min(1.0, oldRange * factor)); // keep range in [4%, 100%]

    // Expand/contract around the cursor's intensity level
    double cursorFrac = (oldRange > 0.0) ? (cursorLevel - m_viewMin) / oldRange : 0.5;
    double newMin     = cursorLevel - cursorFrac * newRange;
    double newMax     = newMin + newRange;

    // Clamp so the window stays inside [0, 1]
    if (newMin < 0.0) { newMax -= newMin; newMin = 0.0; }
    if (newMax > 1.0) { newMin -= (newMax - 1.0); newMax = 1.0; }
    m_viewMin = std::max(0.0, newMin);
    m_viewMax = std::min(1.0, newMax);

    update();
    event->accept();
}


// =============================================================================
// mouseDoubleClickEvent — reset the zoom window to the full [0, 1] range
// =============================================================================
void HistogramWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    Q_UNUSED(event)
    m_viewMin = 0.0;
    m_viewMax = 1.0;
    update();
}
