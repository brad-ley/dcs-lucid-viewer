// =============================================================================
// HistogramWidget.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares HistogramWidget — a compact widget that displays a pixel-value
//   histogram and two draggable handles for adjusting the display contrast range.
//
// ORIENTATION:
//   The histogram is drawn ROTATED so it fits naturally to the RIGHT of an image:
//     - Vertical axis  = pixel value  (0 at bottom → 255 at top)
//     - Horizontal axis = bin count   (0 at left   → peak at right)
//
//   This matches pyqtgraph's ImageView layout, where the histogram panel runs
//   along the right edge of the image display.
//
// TWO DRAGGABLE HANDLES:
//   - Cyan  handle (LOW,  near bottom) = "black point": pixels at or below this
//     level are displayed as black.  Drag UP to crush shadows.
//   - Yellow handle (HIGH, near top)   = "white point": pixels at or above this
//     level are displayed as white.  Drag DOWN to clip highlights.
//
//   Both levels are normalized to 0.0–1.0.  They map to pixel values 0–255 in
//   the 8-bit display image.
//
// SIGNAL:
//   levelsChanged(double low, double high) is emitted whenever a handle moves.
//   PreviewDialog connects this signal to re-render the current frame with the
//   new contrast settings.
//
// C++ CONCEPT — std::array:
//   std::array<int, 256> is a fixed-size array of 256 ints, allocated on the
//   stack (not the heap).  It behaves like int[256] but supports range-for loops,
//   .fill(), .size(), etc.  No need to delete it manually.
// =============================================================================

#pragma once

#include <QWidget>
#include <QImage>
#include <QPair>
#include <array>


class HistogramWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HistogramWidget(QWidget* parent = nullptr);

    // Recompute histogram bins from the given image.
    // Accepts Format_Grayscale8 (fast path) or any other format (converted to gray first).
    // Call this once per new frame, before applying the contrast pipeline.
    void updateHistogram(const QImage& image);

    // Current level values, normalized 0.0–1.0.
    // These are the same values that were last emitted via levelsChanged().
    double lowLevel()  const { return m_lowLevel; }
    double highLevel() const { return m_highLevel; }

    // Compute percentile-based levels from the current histogram bins.
    // lowPct  = 3.0  → black point at the 3rd  percentile pixel value
    // highPct = 97.0 → white point at the 97th percentile pixel value
    // Returns {normalizedLow, normalizedHigh} in the range [0.0, 1.0].
    // Call this after updateHistogram(); the result is ready to pass to setLevels().
    QPair<double, double> computePercentileLevels(double lowPct, double highPct) const;

    // Set both handles programmatically WITHOUT emitting levelsChanged.
    // Used by auto-contrast so the display pipeline reads the new values without
    // triggering a redundant re-render loop.
    void setLevels(double low, double high);

signals:
    // Emitted while the user is dragging a handle.
    // low and high are both in the range [0.0, 1.0].
    void levelsChanged(double low, double high);

protected:
    void paintEvent(QPaintEvent* event)           override;
    void mousePressEvent(QMouseEvent* event)      override;
    void mouseMoveEvent(QMouseEvent* event)       override;
    void mouseReleaseEvent(QMouseEvent* event)    override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

    // Scroll wheel: zoom in/out on the intensity axis centered on the cursor position.
    // Double-click resets the view to the full 0–1 range.
    void wheelEvent(QWheelEvent* event)           override;

private:
    // Convert a normalized level (0.0–1.0) to a Y pixel coordinate in this widget,
    // taking the current zoom window [m_viewMin, m_viewMax] into account.
    // Levels outside the view are clamped to the widget edges.
    int    levelToY(double level) const;

    // Inverse of levelToY — maps a Y pixel back to a 0–1 level value within the
    // current zoom window, clamped to [0, 1].
    double yToLevel(int y) const;

    // Return 0 if the low handle is near y, 1 if the high handle is, or -1 if neither.
    int    handleAtY(int y) const;

    // -------------------------------------------------------------------------
    // Data
    // -------------------------------------------------------------------------

    std::array<int, 256> m_bins{};   // Histogram bin counts (one per 8-bit value)
    int    m_maxBin    = 1;          // Maximum bin count, used to normalize bar widths
    double m_lowLevel  = 0.0;        // Black-point handle position, 0–1
    double m_highLevel = 1.0;        // White-point handle position, 0–1
    int    m_dragging  = -1;         // Which handle is currently being dragged (-1 = none)

    // ---- Zoom / pan state ----
    //
    // The vertical axis normally covers the full intensity range [0.0, 1.0].
    // Scrolling the wheel narrows this window so bars in a small intensity band
    // are spread across the full widget height, making fine-grained handle
    // placement much easier.  Right-click-drag pans the window up or down.
    // Double-clicking resets the window to the full [0, 1] range.
    double m_viewMin = 0.0;          // Bottom of the visible intensity range
    double m_viewMax = 1.0;          // Top of the visible intensity range
    bool   m_isPanning = false;      // True while a right-button drag is in progress
    int    m_panStartY = 0;          // Widget Y where the right-drag began
    double m_panStartMin = 0.0;      // m_viewMin at the start of the current pan drag
    double m_panStartMax = 1.0;      // m_viewMax at the start of the current pan drag
};
