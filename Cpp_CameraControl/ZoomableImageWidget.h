// =============================================================================
// ZoomableImageWidget.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares ZoomableImageWidget — a display area that holds a QPixmap and lets
//   the user zoom and pan it interactively using the mouse.
//
// INTERACTION MODEL:
//   - Mouse wheel          : zoom in/out, always centered on the cursor
//   - Left-mouse drag      : pan the image around
//   - Double-click         : fit the image to the widget (reset view)
//
// HOW ZOOM WORKS (the math):
//   We keep two state variables:
//     m_scale  — current zoom factor (1.0 = original size, 2.0 = 2× zoom)
//     m_offset — how far the image center has been panned from the widget center
//
//   The image is drawn so that:
//     image center (in widget coords) = widget_center + m_offset
//
//   To zoom around the cursor position, we:
//     1. Find which image pixel is currently under the cursor  (widgetToImage)
//     2. Apply the new scale
//     3. Find where that same image pixel lands now            (imageToWidget)
//     4. Shift m_offset to bring it back under the cursor
//
// C++ CONCEPT — inheriting from QWidget:
//   We subclass QWidget and override paintEvent / wheelEvent / mousePressEvent etc.
//   Qt calls these "virtual" methods automatically when the user interacts with
//   the widget.  'override' is a C++11 keyword that tells the compiler to verify
//   these signatures match the base class — useful safety check.
// =============================================================================

#pragma once

#include <QWidget>
#include <QPixmap>
#include <QPointF>
#include <QRect>

class ZoomableImageWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ZoomableImageWidget(QWidget* parent = nullptr);

    // Replace the currently displayed frame.
    // The first time this is called (when the widget has no image yet), the image
    // is automatically scaled to fit in the window.
    void setPixmap(const QPixmap& pixmap);

    // Scale the image to fill the widget while preserving aspect ratio.
    // Called automatically on first image and when the user double-clicks.
    void fitToWindow();

    // Enable or disable ROI draw mode.  When enabled, left-click-drag draws a
    // rectangle on the image instead of panning.  After the mouse is released
    // roiDrawn() is emitted with the rectangle in image-pixel coordinates, and
    // ROI mode is automatically exited.
    void setRoiDrawMode(bool enabled);

    // Show a persistent green rectangle overlay in image-pixel coordinates.
    // Used after the user draws an ROI to display the snapped/confirmed region.
    // Calling setRoiDrawMode(true) clears this overlay automatically.
    void setRoiOverlay(QRect imageRect);

    // Remove the persistent ROI overlay (e.g., after Reset ROI).
    void clearRoiOverlay();

    // Show a dashed blue rectangle in image-pixel coordinates for the focus diagnostic ROI.
    // Independent of the green camera ROI — drawn on top, does not affect camera nodes.
    void setFocusRoiOverlay(QRect imageRect);

    // Remove the focus diagnostic ROI overlay.
    void clearFocusRoiOverlay();

    // Show a text overlay next to a specific image pixel.
    // (imageX, imageY) are in the coordinate space of the current pixmap.
    // text is displayed in a contrasted box near the pixel.
    // Stays visible until clearPixelOverlay() is called or a new click replaces it.
    void setPixelOverlay(int imageX, int imageY, const QString& text);

    // Remove the pixel click overlay.
    void clearPixelOverlay();

    // Show or hide a centred status message over the image (e.g. "Acquiring...").
    // Pass an empty string to hide the overlay.
    void setStatusOverlay(const QString& text);

signals:
    // Emitted when the user finishes drawing an ROI rectangle.
    // imageRect is in image-pixel coordinates (top-left = (0,0) of the current frame).
    void roiDrawn(QRect imageRect);

    // Emitted when the user clicks (without dragging) on the image.
    // imageX / imageY are in image-pixel coordinates (may be outside [0, imageSize] if
    // the user clicks outside the image area — callers should range-check if needed).
    void pixelClicked(int imageX, int imageY);

protected:
    // Qt event handlers — we override these to implement zoom/pan.

    // paintEvent: draws the scaled+panned pixmap onto the widget.
    void paintEvent(QPaintEvent* event) override;

    // wheelEvent: handles mouse scroll wheel → zoom.
    void wheelEvent(QWheelEvent* event) override;

    // mouse events: handles left-drag → pan (normal mode) or draw rect (ROI mode).
    void mousePressEvent(QMouseEvent* event)       override;
    void mouseMoveEvent(QMouseEvent* event)        override;
    void mouseReleaseEvent(QMouseEvent* event)     override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    // Convert a point in image-pixel coordinates to widget-pixel coordinates.
    // (Where on screen does pixel (ix, iy) in the image appear?)
    QPointF imageToWidget(const QPointF& imagePt) const;

    // Convert a point in widget-pixel coordinates to image-pixel coordinates.
    // (Which image pixel is currently under screen position (wx, wy)?)
    QPointF widgetToImage(const QPointF& widgetPt) const;

    QPixmap m_pixmap;              // Current frame (may be null before first frame)
    double  m_scale  = 1.0;        // Zoom factor: 1.0 = 1 image pixel per screen pixel
    QPointF m_offset;              // Pan: how far image center has moved from widget center
    bool    m_panning     = false;  // True once a left-drag has exceeded the movement threshold
    bool    m_dragStarted = false;  // True once m_panning has been committed (threshold crossed)
    QPointF m_pressPos;             // Widget position where the left button was pressed
    QPointF m_lastMouse;            // Mouse position at the previous mouseMoveEvent tick

    // ---- ROI draw mode ----
    bool    m_roiDrawMode  = false;  // True when in draw-ROI mode (pan is disabled)
    bool    m_roiDrawing   = false;  // True while the user is holding the mouse button
    QPointF m_roiStart;              // Widget-space start corner of the in-progress rect
    QPointF m_roiCurrent;            // Widget-space current mouse position while drawing

    // ---- Confirmed camera ROI overlay (green solid) ----
    QRect   m_roiOverlay;            // In image-pixel coordinates; null rect = no overlay
    bool    m_showRoiOverlay = false;

    // ---- Focus diagnostic ROI overlay (blue dashed) ----
    QRect   m_focusRoiOverlay;
    bool    m_showFocusRoiOverlay = false;

    // ---- Pixel click overlay (right-click readback) ----
    bool    m_showPixelOverlay = false;
    QPoint  m_pixelOverlayPos;   // In image-pixel coordinates
    QString m_pixelOverlayText;  // e.g. "[x, y, 42314 counts]"

    // ---- Centred status overlay ----
    QString m_statusOverlayText;  // Empty = not shown
};
