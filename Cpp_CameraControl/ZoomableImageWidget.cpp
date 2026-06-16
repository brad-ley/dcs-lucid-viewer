// =============================================================================
// ZoomableImageWidget.cpp
// =============================================================================
//
// Implementation of interactive zoom/pan image display.
// See ZoomableImageWidget.h for full feature description.
// =============================================================================

#include "ZoomableImageWidget.h"

#include <QPainter>        // QPainter — draws pixels onto the widget
#include <QPen>            // QPen — defines pen style (color, width, pattern) for drawing shapes
#include <QWheelEvent>     // QWheelEvent — mouse scroll wheel event
#include <QMouseEvent>     // QMouseEvent — mouse click/drag events
#include <QFontMetrics>    // QFontMetrics — measure rendered text size for overlay box sizing
#include <cmath>           // std::pow, std::max, std::min


// =============================================================================
// Constructor
// =============================================================================
ZoomableImageWidget::ZoomableImageWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setStyleSheet("background-color: #1e1e1e;");

    // We set the cursor to a hand so the user knows they can drag.
    setCursor(Qt::OpenHandCursor);
}


// =============================================================================
// setPixmap
// =============================================================================
void ZoomableImageWidget::setPixmap(const QPixmap& pixmap)
{
    // If this is the very first image we receive, fit it to the window so the user
    // sees the full image without needing to zoom out manually.
    bool wasNull = m_pixmap.isNull();

    m_pixmap = pixmap;

    if (wasNull && !pixmap.isNull())
        fitToWindow();  // First frame: auto-fit
    else
        update();       // Subsequent frames: preserve zoom/pan, just repaint
}


// =============================================================================
// fitToWindow
// =============================================================================
void ZoomableImageWidget::fitToWindow()
{
    if (m_pixmap.isNull() || width() <= 0 || height() <= 0)
        return;

    // Compute the scale that fits the image entirely within the widget,
    // maintaining the aspect ratio.  We take the smaller of the two axis scales.
    double scaleX = static_cast<double>(width())  / m_pixmap.width();
    double scaleY = static_cast<double>(height()) / m_pixmap.height();

    m_scale  = std::min(scaleX, scaleY);
    m_offset = QPointF(0, 0);  // Centered (no pan offset)

    update();
}


// =============================================================================
// imageToWidget — coordinate transform helper
// =============================================================================
QPointF ZoomableImageWidget::imageToWidget(const QPointF& imagePt) const
{
    // The image center (in widget coordinates) is: widget_center + m_offset
    // A pixel at (imagePt.x, imagePt.y) in image space is offset from the
    // image center by (imagePt - imageCenter) in image pixels, and those image
    // pixels are each m_scale screen pixels wide.

    QPointF widgetCenter(width() / 2.0, height() / 2.0);
    QPointF imageCenter(m_pixmap.width() / 2.0, m_pixmap.height() / 2.0);

    return widgetCenter + m_offset + (imagePt - imageCenter) * m_scale;
}


// =============================================================================
// widgetToImage — inverse coordinate transform helper
// =============================================================================
QPointF ZoomableImageWidget::widgetToImage(const QPointF& widgetPt) const
{
    QPointF widgetCenter(width() / 2.0, height() / 2.0);
    QPointF imageCenter(m_pixmap.width() / 2.0, m_pixmap.height() / 2.0);

    return imageCenter + (widgetPt - widgetCenter - m_offset) / m_scale;
}


// =============================================================================
// setPixelOverlay / clearPixelOverlay — pixel click readback overlay
// =============================================================================
void ZoomableImageWidget::setPixelOverlay(int imageX, int imageY, const QString& text)
{
    m_showPixelOverlay = true;
    m_pixelOverlayPos  = QPoint(imageX, imageY);
    m_pixelOverlayText = text;
    update();
}

void ZoomableImageWidget::clearPixelOverlay()
{
    m_showPixelOverlay = false;
    update();
}


// =============================================================================
// setStatusOverlay — show or hide a centred status message over the image
// =============================================================================
void ZoomableImageWidget::setStatusOverlay(const QString& text)
{
    m_statusOverlayText = text;
    update();
}


// =============================================================================
// setRoiOverlay — show a persistent confirmed-ROI rectangle
// =============================================================================
//
// imageRect is in image-pixel coordinates (same space as roiDrawn() emits).
// The rectangle is drawn as a solid green border so it visually distinguishes
// the final snapped region from the cyan dashed in-progress drawing rectangle.
void ZoomableImageWidget::setRoiOverlay(QRect imageRect)
{
    m_roiOverlay     = imageRect;
    m_showRoiOverlay = !imageRect.isEmpty();
    update();
}


// =============================================================================
// clearRoiOverlay — remove the persistent ROI rectangle
// =============================================================================
void ZoomableImageWidget::clearRoiOverlay()
{
    m_showRoiOverlay = false;
    m_roiOverlay     = QRect();
    update();
}


// =============================================================================
// setFocusRoiOverlay / clearFocusRoiOverlay — dashed blue focus diagnostic ROI
// =============================================================================
void ZoomableImageWidget::setFocusRoiOverlay(QRect imageRect)
{
    m_focusRoiOverlay     = imageRect;
    m_showFocusRoiOverlay = !imageRect.isEmpty();
    update();
}

void ZoomableImageWidget::clearFocusRoiOverlay()
{
    m_showFocusRoiOverlay = false;
    m_focusRoiOverlay     = QRect();
    update();
}


// =============================================================================
// setRoiDrawMode — enter or exit ROI rectangle-draw mode
// =============================================================================
//
// In ROI mode, left-click-drag draws a rectangle instead of panning.
// The cursor changes to a crosshair to signal the mode change.
// Calling with enabled=false restores normal pan/zoom behaviour.
void ZoomableImageWidget::setRoiDrawMode(bool enabled)
{
    m_roiDrawMode  = enabled;
    m_roiDrawing   = false;
    m_roiStart     = {};
    m_roiCurrent   = {};
    m_panning      = false;  // Cancel any in-progress pan when mode switches

    // Starting a new draw clears the previous confirmed-ROI overlay so the display
    // doesn't show a stale rectangle while the user is drawing the replacement.
    if (enabled)
        clearRoiOverlay();

    setCursor(enabled ? Qt::CrossCursor : Qt::OpenHandCursor);
    update();
}


// =============================================================================
// paintEvent — draw the image
// =============================================================================
void ZoomableImageWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);

    // Fill background dark gray (matches the rest of the preview dialog)
    p.fillRect(rect(), QColor("#1e1e1e"));

    if (m_pixmap.isNull())
    {
        // No image yet — show placeholder text
        p.setPen(QColor("#555"));
        p.drawText(rect(), Qt::AlignCenter, "Waiting for preview");
        return;
    }

    // The top-left corner of the scaled image in widget coordinates is found by
    // converting the image's (0, 0) pixel to widget space.
    QPointF topLeft = imageToWidget(QPointF(0, 0));

    int scaledW = static_cast<int>(m_pixmap.width()  * m_scale);
    int scaledH = static_cast<int>(m_pixmap.height() * m_scale);

    // Draw the pixmap scaled.  Qt handles the actual scaling internally using
    // bilinear or nearest-neighbor filtering based on the rendering hints below.
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_scale < 1.0);

    p.drawPixmap(QRect(topLeft.toPoint(), QSize(scaledW, scaledH)), m_pixmap);

    // ---- ROI draw overlay (in-progress) ----
    // While the user is dragging a rectangle, draw a semi-transparent cyan box
    // so they can see exactly which region they're selecting.
    if (m_roiDrawMode && m_roiDrawing)
    {
        QRectF roiRect = QRectF(m_roiStart, m_roiCurrent).normalized();

        // Semi-transparent cyan fill so the image underneath is still visible
        p.fillRect(roiRect, QColor(0, 200, 200, 40));

        // Dashed cyan border — 2 px wide so it's clearly visible even on light images
        QPen pen(QColor(0, 220, 220), 2, Qt::DashLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(roiRect);
    }

    // ---- Confirmed ROI overlay (snapped result after drawing) ----
    // Shown as a solid green border after the user releases the mouse, so they
    // can see the snapped/adjusted rectangle before clicking Apply.
    // Cleared automatically when a new draw begins.
    if (m_showRoiOverlay && !m_roiOverlay.isEmpty())
    {
        // Convert image-pixel corners to widget-pixel coordinates so the rectangle
        // stays correctly positioned as the user zooms and pans.
        QPointF tl = imageToWidget(QPointF(m_roiOverlay.left(),  m_roiOverlay.top()));
        QPointF br = imageToWidget(QPointF(m_roiOverlay.right(), m_roiOverlay.bottom()));
        QRectF displayRect = QRectF(tl, br).normalized();

        p.fillRect(displayRect, QColor(0, 220, 0, 30));

        QPen pen(QColor(0, 230, 0), 2, Qt::SolidLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(displayRect);
    }

    // ---- Focus diagnostic ROI overlay (dashed blue) ----
    if (m_showFocusRoiOverlay && !m_focusRoiOverlay.isEmpty())
    {
        QPointF tl = imageToWidget(QPointF(m_focusRoiOverlay.left(),  m_focusRoiOverlay.top()));
        QPointF br = imageToWidget(QPointF(m_focusRoiOverlay.right(), m_focusRoiOverlay.bottom()));
        QRectF displayRect = QRectF(tl, br).normalized();

        p.fillRect(displayRect, QColor(0, 160, 220, 30));

        QPen focusPen(QColor(0, 180, 255), 2, Qt::DashLine);
        p.setPen(focusPen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(displayRect);

        QFont labelFont = p.font();
        labelFont.setPointSize(8);
        p.setFont(labelFont);
        p.setPen(QColor(0, 200, 255));
        p.drawText(displayRect.topLeft() + QPointF(4, -3), "Focus ROI");
    }

    // ---- Pixel click readback overlay ----
    //
    // Draws a small crosshair centered on the clicked pixel and a text label
    // "[x, y, N counts]" in a contrasted box next to it.
    if (m_showPixelOverlay && !m_pixelOverlayText.isEmpty())
    {
        // Map the clicked image pixel to the current widget coordinate system.
        // QPointF(x+0.5, y+0.5) puts us at the pixel center rather than its top-left corner.
        QPointF center = imageToWidget(QPointF(m_pixelOverlayPos.x() + 0.5,
                                               m_pixelOverlayPos.y() + 0.5));

        // --- Crosshair ---
        // 8-pixel arms in screen space so the indicator is always a constant visible size
        // regardless of how far the user has zoomed in or out.
        const int ARM = 8;
        QPen crossPen(QColor(255, 220, 0), 1.5);  // bright yellow, 1.5 px wide
        p.setPen(crossPen);
        p.drawLine(QPointF(center.x() - ARM, center.y()),
                   QPointF(center.x() + ARM, center.y()));
        p.drawLine(QPointF(center.x(), center.y() - ARM),
                   QPointF(center.x(), center.y() + ARM));

        // Small circle around the exact pixel
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, 4.0, 4.0);

        // --- Text label ---
        // Size the box to fit the text, then position it just above-right of the crosshair.
        // If that would go off the right edge, flip to the left; if off the top, flip down.
        QFont labelFont = p.font();
        labelFont.setPointSize(9);
        p.setFont(labelFont);
        QFontMetrics fm(labelFont);
        QRect textRect = fm.boundingRect(m_pixelOverlayText);

        const int PAD   = 4;   // padding inside the box
        const int boxW  = textRect.width()  + 2 * PAD;
        const int boxH  = textRect.height() + 2 * PAD;
        const int OFFSET = ARM + 4;  // distance from crosshair center to box edge

        // Choose which side to draw on so the box stays inside the widget.
        double boxLeft = center.x() + OFFSET;
        if (boxLeft + boxW > width())
            boxLeft = center.x() - OFFSET - boxW;

        double boxTop = center.y() - OFFSET - boxH;
        if (boxTop < 0)
            boxTop = center.y() + OFFSET;

        QRectF boxRect(boxLeft, boxTop, boxW, boxH);

        // Semi-transparent dark background so text is readable over any image content
        p.fillRect(boxRect, QColor(0, 0, 0, 180));
        QPen boxPen(QColor(255, 220, 0), 1);
        p.setPen(boxPen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(boxRect);

        // Draw the text itself in bright yellow
        p.setPen(QColor(255, 220, 0));
        p.drawText(boxRect.adjusted(PAD, PAD, -PAD, -PAD),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   m_pixelOverlayText);
    }

    // ---- Centred status overlay (e.g. "Acquiring — no preview available") ----
    if (!m_statusOverlayText.isEmpty())
    {
        QFont f = p.font();
        f.setPointSize(14);
        f.setBold(true);
        p.setFont(f);
        QFontMetrics fm(f);
        QRect textBound = fm.boundingRect(m_statusOverlayText);

        const int PAD  = 16;
        const int boxW = textBound.width()  + 2 * PAD;
        const int boxH = textBound.height() + 2 * PAD;
        QRect boxRect(
            (width()  - boxW) / 2,
            (height() - boxH) / 2,
            boxW, boxH
        );

        p.fillRect(boxRect, QColor(0, 0, 0, 180));
        QPen borderPen(QColor(200, 200, 200, 180), 1);
        p.setPen(borderPen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(boxRect);

        p.setPen(QColor(220, 220, 220));
        p.drawText(boxRect, Qt::AlignCenter, m_statusOverlayText);
    }
}


// =============================================================================
// wheelEvent — zoom centered on cursor
// =============================================================================
void ZoomableImageWidget::wheelEvent(QWheelEvent* event)
{
    // angleDelta().y() is ±120 per scroll notch on most mice.
    // We use std::pow so that equal numbers of in/out notches return to the same zoom.
    // Factor of 1.2 means each notch zooms by 20%.
    double factor   = std::pow(1.2, event->angleDelta().y() / 120.0);
    double newScale = m_scale * factor;

    // Clamp zoom range: 5% (very zoomed out) to 3200% (very zoomed in)
    newScale = std::max(0.05, std::min(32.0, newScale));
    if (newScale == m_scale)
    {
        event->accept();
        return;
    }

    // The key zoom math: keep the image point under the cursor stationary.
    //
    // Before scale change:  cursorPt (widget) → imagePt (image)
    // After scale change:   imagePt (image)   → newCursorPt (widget)  [different!]
    // We adjust m_offset to close the gap between cursorPt and newCursorPt.

    QPointF cursorPt = event->position();
    QPointF imagePt  = widgetToImage(cursorPt);   // Which image pixel is under cursor?

    m_scale = newScale;

    QPointF newCursorPt = imageToWidget(imagePt);  // Where did that pixel move to?
    m_offset += (cursorPt - newCursorPt);           // Nudge offset to keep it in place

    update();
    event->accept();
}


// =============================================================================
// mousePressEvent — begin pan OR begin ROI rectangle draw
// =============================================================================
void ZoomableImageWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        if (m_roiDrawMode)
        {
            // ROI mode: start drawing a selection rectangle
            m_roiDrawing  = true;
            m_roiStart    = event->position();
            m_roiCurrent  = event->position();
        }
        else
        {
            // Normal mode: record the press position but don't commit to pan yet.
            // We wait until the mouse moves past the drag threshold (see mouseMoveEvent)
            // so that a stationary click can be used for pixel readback instead.
            m_pressPos    = event->position();
            m_lastMouse   = event->position();
            m_dragStarted = false;
            m_panning     = false;
        }
    }
}


// =============================================================================
// mouseMoveEvent — drag to pan OR update ROI rectangle
// =============================================================================
void ZoomableImageWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_roiDrawMode && m_roiDrawing)
    {
        // Extend the ROI rectangle to the current cursor position and repaint
        // the overlay each frame so the user sees it update smoothly.
        m_roiCurrent = event->position();
        update();
    }
    else if (event->buttons() & Qt::LeftButton)
    {
        if (!m_dragStarted)
        {
            // Check whether the mouse has moved far enough to commit to a pan.
            // 4 pixels is enough to distinguish an intentional drag from a slightly
            // shaky click, while still feeling responsive.
            QPointF delta = event->position() - m_pressPos;
            if (delta.x() * delta.x() + delta.y() * delta.y() > 4.0 * 4.0)
            {
                m_dragStarted = true;
                m_panning     = true;
                m_lastMouse   = event->position();
                setCursor(Qt::ClosedHandCursor);
            }
        }
        else if (m_panning)
        {
            // Pan: move the image by the same amount the cursor moved
            QPointF delta = event->position() - m_lastMouse;
            m_offset     += delta;
            m_lastMouse   = event->position();
            update();
        }
    }
}


// =============================================================================
// mouseReleaseEvent — end pan OR finalise ROI rectangle
// =============================================================================
void ZoomableImageWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        if (m_roiDrawMode && m_roiDrawing)
        {
            m_roiDrawing = false;

            // Convert the two widget-space corners to image-pixel coordinates.
            // widgetToImage() applies the inverse of the current zoom+pan transform.
            QPointF imgA = widgetToImage(m_roiStart);
            QPointF imgB = widgetToImage(m_roiCurrent);

            // Build a normalised QRect (ensures top-left is above/left of bottom-right
            // regardless of which direction the user dragged).
            QRect imageRect = QRectF(imgA, imgB).normalized().toRect();

            // Clamp the rectangle to the image bounds so we don't emit out-of-range coords.
            if (!m_pixmap.isNull())
            {
                QRect imageBounds(0, 0, m_pixmap.width(), m_pixmap.height());
                imageRect = imageRect.intersected(imageBounds);
            }

            // Only emit for rectangles at least 2×2 pixels — ignore accidental single clicks.
            if (imageRect.width() >= 2 && imageRect.height() >= 2)
                emit roiDrawn(imageRect);

            // Exit ROI mode in the widget — the caller (PreviewDialog) will also update
            // its button state, but resetting here ensures the cursor/state are correct
            // even if nobody is connected to the signal.
            setRoiDrawMode(false);
        }
        else
        {
            if (!m_dragStarted && !m_pixmap.isNull())
            {
                // The mouse didn't move past the drag threshold — treat as a pixel click.
                // Use the original press position (not release position) for accuracy.
                QPointF imgPt = widgetToImage(m_pressPos);
                emit pixelClicked(static_cast<int>(imgPt.x()), static_cast<int>(imgPt.y()));
            }

            m_panning     = false;
            m_dragStarted = false;
            setCursor(Qt::OpenHandCursor);
        }
    }
}


// =============================================================================
// mouseDoubleClickEvent — fit image to window
// =============================================================================
void ZoomableImageWidget::mouseDoubleClickEvent(QMouseEvent*)
{
    // Reset zoom and pan so the whole image is visible again
    fitToWindow();
}
