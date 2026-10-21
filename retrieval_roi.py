"""Pixel-aligned processing ROI controls for a full-resolution input image."""
from viewer_runtime import prepare_qt_runtime

prepare_qt_runtime()

import math
import pyqtgraph as pg
from PyQt6.QtCore import Qt, QRectF, pyqtSignal
from PyQt6.QtGui import QKeySequence, QShortcut
from PyQt6.QtWidgets import (
    QGroupBox, QVBoxLayout, QFormLayout, QHBoxLayout, QLabel, QSpinBox, QPushButton,
)


class ROISelectionViewBox(pg.ViewBox):
    rectangle_drawn = pyqtSignal(object, object, bool)

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.drawing_roi = False

    def mouseDragEvent(self, event, axis=None):
        if self.drawing_roi and axis is None and event.button() == Qt.MouseButton.LeftButton:
            event.accept()
            self.rectangle_drawn.emit(event.buttonDownScenePos(), event.scenePos(),
                                      event.isFinish())
            return
        super().mouseDragEvent(event, axis=axis)


class ProcessingROI(QGroupBox):
    """Keep an integer crop, its edit fields and an image-child rectangle in sync.

    region_changing invalidates running results during a drag. region_changed
    requests calculation only once the drag/field edit is committed.
    """
    region_changing = pyqtSignal()
    region_changed = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__("Processing ROI", parent)
        self.setCheckable(True)
        self.setChecked(False)
        self.setEnabled(False)
        self._shape = None
        self._bounds = (0, 0, 1, 1)
        self._syncing = False
        self._dirty = False
        self._image = None
        self._view = None
        self.rect = None
        self._draw_original = None
        self._draw_complete = False

        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.x_input = self._spin(form, "X (px, from 0):", 0)
        self.y_input = self._spin(form, "Y (px, from 0):", 0)
        self.width_input = self._spin(form, "Width (px):", 1)
        self.height_input = self._spin(form, "Height (px):", 1)
        layout.addLayout(form)
        buttons = QHBoxLayout()
        self.draw_btn = QPushButton("Draw Box")
        self.draw_btn.setCheckable(True)
        self.draw_btn.setToolTip("Drag on the input image. Press Escape or click again to cancel.")
        self.full_btn = QPushButton("Full Image")
        buttons.addWidget(self.draw_btn)
        buttons.addWidget(self.full_btn)
        layout.addLayout(buttons)
        self.info = QLabel("No active image.")
        self.info.setWordWrap(True)
        layout.addWidget(self.info)
        note = QLabel("Drag/resize the box or edit pixel bounds. Include a margin "
                      "around features to limit crop-edge effects.")
        note.setWordWrap(True)
        layout.addWidget(note)

        for spin in (self.x_input, self.y_input, self.width_input, self.height_input):
            spin.valueChanged.connect(self._fields_changed)
        self.toggled.connect(self._enabled_changed)
        self.draw_btn.toggled.connect(self._drawing_toggled)
        self.full_btn.clicked.connect(self.full_image)
        self._escape = QShortcut(QKeySequence(Qt.Key.Key_Escape), self)
        self._escape.activated.connect(lambda: self.draw_btn.setChecked(False))

    @staticmethod
    def _spin(form, label, low):
        spin = QSpinBox()
        spin.setRange(low, 1)
        spin.setKeyboardTracking(False)
        form.addRow(label, spin)
        return spin

    def attach(self, image, view):
        self._image = image
        self._view = view
        # Parenting to the image keeps the ROI in original pixel coordinates
        # when calibration or plot zoom changes. It does not affect auto-range.
        self.rect = pg.RectROI(
            [0, 0], [1, 1], parent=image, rotatable=False,
            translateSnap=True, scaleSnap=True, snapSize=1,
            pen=pg.mkPen("#4fc3f7", width=2), hoverPen=pg.mkPen("#ffee58", width=2))
        for position, center in (
                ([0, 0], [1, 1]), ([0, 1], [1, 0]), ([1, 0], [0, 1]),
                ([0, 0.5], [1, 0.5]), ([0.5, 0], [0.5, 1]),
                ([1, 0.5], [0, 0.5]), ([0.5, 1], [0.5, 0])):
            self.rect.addScaleHandle(position, center)
        self.rect.hide()
        self.rect.sigRegionChanged.connect(self._rectangle_changed)
        self.rect.sigRegionChangeFinished.connect(self._rectangle_finished)
        view.rectangle_drawn.connect(self._draw_rectangle)

    def set_image_shape(self, shape, reset=False):
        if shape is None:
            self._shape = None
            self._stop_drawing()
            self.setEnabled(False)
            self.rect.hide()
            self.info.setText("No active image.")
            return
        shape = tuple(shape[:2])
        reset = reset or shape != self._shape
        self._shape = shape
        self.setEnabled(True)
        if reset:
            self._stop_drawing()
            self.blockSignals(True)
            self.setChecked(False)
            self.blockSignals(False)
            height, width = shape
            self._bounds = (0, 0, width, height)
            self._dirty = False
        self._sync_controls()
        self.rect.setVisible(self.isChecked() and not self.draw_btn.isChecked())

    def bounds(self):
        if self._shape is None:
            return None
        if self.isChecked():
            return self._bounds
        height, width = self._shape
        return (0, 0, width, height)

    def _clamped(self, bounds):
        height, width = self._shape
        x, y, roi_width, roi_height = (int(value) for value in bounds)
        x = max(0, min(x, width - 1))
        y = max(0, min(y, height - 1))
        return (x, y, max(1, min(roi_width, width - x)),
                max(1, min(roi_height, height - y)))

    def _set_bounds(self, bounds, finished=True, sync_rectangle=True):
        if self._shape is None:
            return
        bounds = self._clamped(bounds)
        changed = bounds != self._bounds
        self._bounds = bounds
        self._sync_controls(sync_rectangle)
        if changed and self.isChecked():
            self._dirty = True
            self.region_changing.emit()
        if finished and self._dirty:
            self._dirty = False
            self.region_changed.emit()

    def _sync_controls(self, sync_rectangle=True):
        if self._shape is None:
            return
        height, width = self._shape
        x, y, roi_width, roi_height = self._bounds
        self._syncing = True
        try:
            for spin, low, high, value in (
                    (self.x_input, 0, width - 1, x),
                    (self.y_input, 0, height - 1, y),
                    (self.width_input, 1, width - x, roi_width),
                    (self.height_input, 1, height - y, roi_height)):
                spin.setRange(low, high)
                spin.setValue(value)
            if sync_rectangle:
                self.rect.maxBounds = QRectF(0, 0, width, height)
                self.rect.setPos((x, y), finish=False)
                self.rect.setSize((roi_width, roi_height), finish=False)
            pixels = roi_width * roi_height if self.isChecked() else width * height
            processed_width = roi_width if self.isChecked() else width
            processed_height = roi_height if self.isChecked() else height
            self.info.setText(f"{processed_width} × {processed_height} px "
                              f"({100 * pixels / (width * height):.1f}% of full image)")
        finally:
            self._syncing = False

    def _fields_changed(self, *_args):
        if not self._syncing:
            self._set_bounds((self.x_input.value(), self.y_input.value(),
                              self.width_input.value(), self.height_input.value()))

    def _rectangle_changed(self, *_args):
        if self._syncing or not self.isChecked():
            return
        pos, size = self.rect.pos(), self.rect.size()
        self._set_bounds((round(pos.x()), round(pos.y()),
                          round(size.x()), round(size.y())),
                         finished=False, sync_rectangle=False)

    def _rectangle_finished(self, *_args):
        if self._syncing or not self.isChecked():
            return
        self._set_bounds(self._bounds, finished=True)

    def _enabled_changed(self, checked):
        if self._shape is None:
            return
        if not checked:
            self.draw_btn.setChecked(False)
        self.rect.setVisible(checked and not self.draw_btn.isChecked())
        self._sync_controls()
        self._dirty = False
        self.region_changing.emit()
        self.region_changed.emit()

    def full_image(self):
        if self._shape is None:
            return
        self._stop_drawing()
        self.setChecked(False)
        height, width = self._shape
        self._set_bounds((0, 0, width, height))
        self._view.autoRange()

    def _stop_drawing(self):
        self._draw_original = None
        self.draw_btn.blockSignals(True)
        self.draw_btn.setChecked(False)
        self.draw_btn.blockSignals(False)
        if self._view is not None:
            self._view.drawing_roi = False
            self._view.unsetCursor()

    def _drawing_toggled(self, drawing):
        if self._shape is None:
            return
        self._view.drawing_roi = drawing
        if drawing:
            self._draw_original = self._bounds
            self._draw_complete = False
            self._view.setCursor(Qt.CursorShape.CrossCursor)
            self.rect.hide()
            self._dirty = True
            self.region_changing.emit()
        else:
            self._view.unsetCursor()
            if not self._draw_complete and self._draw_original is not None:
                self._set_bounds(self._draw_original)
            self._draw_original = None
            self.rect.setVisible(self.isChecked())

    def _draw_rectangle(self, start, end, finished):
        if self._shape is None or not self.draw_btn.isChecked():
            return
        rect = QRectF(self._image.mapFromScene(start),
                      self._image.mapFromScene(end)).normalized()
        x = math.floor(rect.left() + 1e-8)
        y = math.floor(rect.top() + 1e-8)
        x_end = math.ceil(rect.right() - 1e-8)
        y_end = math.ceil(rect.bottom() - 1e-8)
        # Clamp each endpoint before deriving dimensions for out-of-image drags.
        height, width = self._shape
        x = max(0, min(x, width - 1))
        y = max(0, min(y, height - 1))
        x_end = max(x + 1, min(x_end, width))
        y_end = max(y + 1, min(y_end, height))
        self._set_bounds((x, y, x_end - x, y_end - y), finished=finished)
        if finished:
            self._draw_complete = True
            self.draw_btn.setChecked(False)

