"""Named spatial scales and pixel-anchored measurement overlays for the viewer."""
from viewer_runtime import prepare_qt_runtime
prepare_qt_runtime()

import json
import math
from dataclasses import asdict, dataclass, replace

import pyqtgraph as pg
from pyqtgraph.graphicsItems.ROI import Handle
from PyQt6.QtCore import QObject, QPointF, Qt, pyqtSignal
from PyQt6.QtGui import QFont, QKeySequence, QPainter, QShortcut
from PyQt6.QtWidgets import (
    QDialog, QDoubleSpinBox, QFormLayout, QHBoxLayout, QLabel, QLineEdit,
    QPushButton, QComboBox, QVBoxLayout,
)


@dataclass(frozen=True)
class Calibration:
    pixels: float
    known_um: float
    name: str = ''

    def __post_init__(self):
        if not all(math.isfinite(v) and v > 0 for v in (self.pixels, self.known_um)):
            raise ValueError('Both distances must be finite and greater than zero.')
        if not math.isfinite(self.known_um / self.pixels) or self.known_um / self.pixels <= 0:
            raise ValueError('The resulting scale must be finite and greater than zero.')

    @property
    def pitch_um(self):
        return self.known_um / self.pixels


class SpatialCalibration(QObject):
    """Manual scale overrides recording metadata; changing files never discards it."""
    changed = pyqtSignal()
    presets_changed = pyqtSignal()

    def __init__(self, settings, parent=None):
        super().__init__(parent)
        self.settings = settings
        self.presets = {}
        self.manual = None
        self.recording_pitch_um = None
        try:
            records = json.loads(settings.value('spatial_scale/presets', '[]'))
            for record in records:
                try:
                    scale = Calibration(**record)
                    name = scale.name.strip()
                    if name and not self.name_exists(name):
                        self.presets[name] = replace(scale, name=name)
                except (TypeError, ValueError, AttributeError, OverflowError):
                    continue
        except (TypeError, ValueError):
            pass
        try:
            record = json.loads(settings.value('spatial_scale/active', 'null'))
            if record:
                self.manual = Calibration(**record)
        except (TypeError, ValueError, OverflowError):
            pass

    @property
    def pitch_um(self):
        return self.manual.pitch_um if self.manual else self.recording_pitch_um

    @property
    def description(self):
        if self.manual:
            source = self.manual.name or 'Custom scale'
        else:
            source = 'Recording scale'
        return (f'{source} · {self.pitch_um:.6g} µm/px' if self.pitch_um else
                'Uncalibrated · px')

    def name_exists(self, name):
        return name.strip().casefold() in {key.casefold() for key in self.presets}

    def _persist_active(self):
        self.settings.setValue('spatial_scale/active',
                               json.dumps(asdict(self.manual) if self.manual else None))
        self.settings.sync()
        self.changed.emit()

    def _persist_presets(self):
        self.settings.setValue('spatial_scale/presets',
                               json.dumps([asdict(v) for v in self.presets.values()]))
        self.settings.sync()
        self.presets_changed.emit()

    def apply(self, pixels, known_um, name=''):
        self.manual = Calibration(float(pixels), float(known_um), name.strip())
        self._persist_active()

    def save(self, pixels, known_um, name):
        name = name.strip()
        scale = Calibration(float(pixels), float(known_um), name)
        if not name:
            raise ValueError('Enter a calibration name before saving.')
        if self.name_exists(name):
            raise ValueError('That name is already saved. Enter a unique name.')
        self.presets[name] = scale
        self._persist_presets()
        self.manual = scale
        self._persist_active()

    def delete(self, name):
        del self.presets[name]
        # Removing a preset must not silently change measurements already in use.
        if self.manual and self.manual.name.casefold() == name.casefold():
            self.manual = replace(self.manual, name='')
            self._persist_active()
        self._persist_presets()

    def use_recording_scale(self):
        self.manual = None
        self._persist_active()

    def set_recording_scale(self, pitch_um):
        try:
            pitch = float(pitch_um)
            pitch = pitch if math.isfinite(pitch) and pitch > 0 else None
        except (TypeError, ValueError, OverflowError):
            pitch = None
        if pitch != self.recording_pitch_um:
            self.recording_pitch_um = pitch
            if self.manual is None:
                self.changed.emit()


class LineDrawingViewBox(pg.ViewBox):
    line_dragged = pyqtSignal(object, object, bool)
    drawing_cancelled = pyqtSignal()

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.drawing_line = False

    def mouseDragEvent(self, event, axis=None):
        if self.drawing_line and axis is None and event.button() == Qt.MouseButton.LeftButton:
            event.accept()
            self.line_dragged.emit(event.buttonDownScenePos(), event.scenePos(), event.isFinish())
            return
        super().mouseDragEvent(event, axis=axis)

    def mouseClickEvent(self, event):
        if self.drawing_line:
            event.accept()
            if event.button() == Qt.MouseButton.RightButton:
                self.drawing_cancelled.emit()
            return
        super().mouseClickEvent(event)


class DimensionLine(pg.LineSegmentROI):
    """A cosmetic line with a subtle dark halo, legible on either bright or dark pixels."""
    def paint(self, painter, *args):
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        first, last = (h.pos() for h in self.endpoints)
        painter.setPen(pg.mkPen((10, 18, 24, 210), width=4.5))
        painter.drawLine(first, last)
        painter.setPen(self.currentPen)
        painter.drawLine(first, last)
        painter.restore()


class MeasurementLine:
    def __init__(self, image, start, end, number=None):
        self.number = number
        color = '#efbb66' if number is None else '#74cee8'
        handles = [Handle(4, typ='r', pen=pg.mkPen(color, width=1.5),
                          hoverPen=pg.mkPen('#ffffff', width=2)) for _ in range(2)]
        self.roi = DimensionLine([start, end], handles=handles, parent=image,
                                 pen=pg.mkPen(color, width=1.8),
                                 hoverPen=pg.mkPen('#ffffff', width=2),
                                 movable=True, rotatable=False, resizable=False)
        for handle in handles:
            handle.setParentItem(self.roi)
        self.roi.setZValue(30)
        self.label = pg.TextItem(color=color, anchor=(.5, 1.25),
                                 fill=pg.mkBrush(19, 28, 35, 235),
                                 border=pg.mkPen('#4a5b65', width=1))
        self.label.setFont(QFont('Segoe UI', 10, QFont.Weight.DemiBold))
        self.label.setParentItem(image)
        self.label.setZValue(31)
        self.label.setAcceptedMouseButtons(Qt.MouseButton.NoButton)

    @property
    def points(self):
        return [self.roi.mapToParent(h.pos()) for h in self.roi.getHandles()]

    @property
    def length_px(self):
        a, b = self.points
        return math.hypot(b.x() - a.x(), b.y() - a.y())

    def set_points(self, start, end):
        # ROI.movePoint takes parent/image coordinates, including after body translation.
        self.roi.movePoint(self.roi.getHandles()[0], start, finish=False)
        self.roi.movePoint(self.roi.getHandles()[1], end)

    def refresh(self, pitch_um):
        a, b = self.points
        self.label.setPos((a.x() + b.x()) / 2, (a.y() + b.y()) / 2)
        if self.number is None:
            text = f'Scale  {self.length_px:.3f} px'
        else:
            distance = self.length_px * pitch_um if pitch_um else self.length_px
            unit = 'µm' if pitch_um else 'px'
            text = f'M{self.number}  {distance:.5g} {unit}'
        self.label.setText(f' {text} ')

    def remove(self):
        for item in (self.roi, self.label):
            scene = item.scene()
            item.setParentItem(None)
            if scene is not None:
                scene.removeItem(item)


class MeasurementOverlay(QObject):
    distance_changed = pyqtSignal(float)
    drawing_changed = pyqtSignal(object)
    count_changed = pyqtSignal(int)
    image_available_changed = pyqtSignal(bool)
    message = pyqtSignal(str)

    def __init__(self, imview, scale, parent=None):
        super().__init__(parent)
        self.image = imview.imageItem
        self.view = imview.getView()
        self.scale = scale
        self.lines = []
        self.calibration_line = None
        self.mode = None
        self._shape = None
        self._draft = None
        self._next_number = 1
        self.view.line_dragged.connect(self._line_dragged)
        self.view.drawing_cancelled.connect(self.cancel_drawing)
        scale.changed.connect(self.refresh_labels)

    def set_image_shape(self, shape):
        shape = tuple(shape[:2]) if shape is not None else None
        if shape != self._shape:
            self.cancel_drawing()
        self._shape = shape
        self.image_available_changed.emit(shape is not None)

    def start_drawing(self, mode):
        self.cancel_drawing()
        if self._shape is None:
            self.message.emit('Open an image before drawing a line.')
            return
        self.mode = mode
        self.view.drawing_line = True
        self.view.setCursor(Qt.CursorShape.CrossCursor)
        # Existing measurement handles must not steal the start of a new line.
        for line in self.lines + ([self.calibration_line] if self.calibration_line else []):
            line.roi.setEnabled(False)
        self.drawing_changed.emit(mode)
        self.message.emit('Drag a line on the image. Escape or right-click cancels.')

    def cancel_drawing(self):
        if self._draft:
            self._draft.remove()
            self._draft = None
            if self.mode == 'calibrate' and self.calibration_line:
                self.distance_changed.emit(self.calibration_line.length_px)
        self.mode = None
        self.view.drawing_line = False
        self.view.unsetCursor()
        for line in self.lines + ([self.calibration_line] if self.calibration_line else []):
            line.roi.setEnabled(True)
        self.drawing_changed.emit(None)

    def _line_dragged(self, scene_start, scene_end, finished):
        if self.mode is None or self._shape is None:
            return
        start = self.image.mapFromScene(scene_start)
        end = self.image.mapFromScene(scene_end)
        height, width = self._shape
        if not self._draft and not (0 <= start.x() <= width and 0 <= start.y() <= height):
            return
        end = QPointF(max(0, min(end.x(), width)), max(0, min(end.y(), height)))
        if self._draft is None:
            self._draft = MeasurementLine(self.image, start, end,
                                         self._next_number if self.mode == 'measure' else None)
            self._draft.roi.setEnabled(False)
        else:
            self._draft.set_points(start, end)
        self._draft.refresh(self.scale.pitch_um)
        if self.mode == 'calibrate':
            self.distance_changed.emit(self._draft.length_px)
        if not finished:
            return
        line = self._draft
        self._draft = None
        if line.length_px < 1e-6:
            line.remove()
            self.cancel_drawing()
            return
        if self.mode == 'calibrate':
            self.clear_calibration_line()
            self.calibration_line = line
            line.roi.sigRegionChanged.connect(self._calibration_changed)
        else:
            self.lines.append(line)
            self._next_number += 1
            line.roi.sigRegionChanged.connect(self.refresh_labels)
            self.count_changed.emit(len(self.lines))
        self.cancel_drawing()
        self.message.emit('Drag endpoints to adjust the line. Measure adds another; Clear Measurements removes all.')

    def _calibration_changed(self, *_args):
        if self.calibration_line:
            self.calibration_line.refresh(self.scale.pitch_um)
            self.distance_changed.emit(self.calibration_line.length_px)

    def resize_calibration_line(self, pixels):
        line = self.calibration_line
        if line is None or pixels <= 0 or line.length_px <= 0:
            return
        a, b = line.points
        ratio = pixels / line.length_px
        line.set_points(a, QPointF(a.x() + (b.x() - a.x()) * ratio,
                                  a.y() + (b.y() - a.y()) * ratio))

    def refresh_labels(self, *_args):
        for line in self.lines + ([self.calibration_line] if self.calibration_line else []):
            line.refresh(self.scale.pitch_um)

    def clear_calibration_line(self):
        if self.calibration_line:
            self.calibration_line.remove()
            self.calibration_line = None

    def clear_measurements(self):
        self.cancel_drawing()
        for line in self.lines:
            line.remove()
        self.lines.clear()
        self._next_number = 1
        self.count_changed.emit(0)


class SetScaleDialog(QDialog):
    def __init__(self, scale, overlay, parent=None):
        super().__init__(parent)
        self.setWindowTitle('Set Scale')
        self.setModal(False)
        self.setMinimumWidth(420)
        self.scale = scale
        self.overlay = overlay
        self._syncing = False
        self._selected_name = None
        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(12)
        stored = QHBoxLayout()
        self.preset_combo = QComboBox()
        self.preset_combo.setMinimumContentsLength(22)
        self.delete_btn = QPushButton('Delete Stored')
        self.delete_btn.setToolTip('Remove the selected saved calibration. The active scale stays in use.')
        stored.addWidget(self.preset_combo, 1)
        stored.addWidget(self.delete_btn)
        layout.addLayout(stored)
        form = QFormLayout()
        form.setSpacing(10)
        self.pixels_input = self._distance_input()
        self.known_input = self._distance_input()
        self.name_input = QLineEdit()
        self.name_input.setMaxLength(120)
        self.name_input.setPlaceholderText('e.g. 10× objective, camera A')
        form.addRow('Distance in pixels:', self.pixels_input)
        form.addRow('Known distance (µm):', self.known_input)
        form.addRow('Calibration name:', self.name_input)
        layout.addLayout(form)
        self.draw_btn = QPushButton('Draw Calibration Line')
        self.draw_btn.setCheckable(True)
        self.draw_btn.setToolTip('Drag across a known feature in the main image, then adjust either endpoint.')
        layout.addWidget(self.draw_btn)
        self.preview = QLabel()
        self.preview.setStyleSheet('font-size: 16px; font-weight: 600; color: #efbb66; padding: 4px 0;')
        layout.addWidget(self.preview)
        self.feedback = QLabel()
        self.feedback.setWordWrap(True)
        self.feedback.setMinimumHeight(34)
        layout.addWidget(self.feedback)
        actions = QHBoxLayout()
        self.apply_btn = QPushButton('Apply Scale')
        self.save_btn = QPushButton('Save Calibration')
        self.save_btn.setStyleSheet('QPushButton:enabled { border-color: #7daabc; }')
        actions.addWidget(self.apply_btn)
        actions.addWidget(self.save_btn)
        layout.addLayout(actions)
        self.automatic_btn = QPushButton('Use Recording Scale')
        layout.addWidget(self.automatic_btn)
        self.active_label = QLabel()
        self.active_label.setTextFormat(Qt.TextFormat.PlainText)
        self.active_label.setWordWrap(True)
        self.active_label.setStyleSheet('color: #a9b5bc;')
        layout.addWidget(self.active_label)
        note = QLabel('Scale applies to measurements, FFT and density retrieval. It assumes square pixels. '
                      'Manual scale stays active across recordings until you choose recording scale.')
        note.setWordWrap(True)
        note.setStyleSheet('color: #a9b5bc;')
        layout.addWidget(note)
        self.preset_combo.currentIndexChanged.connect(self._load_preset)
        self.delete_btn.clicked.connect(self._delete)
        self.pixels_input.valueChanged.connect(self._pixels_edited)
        self.known_input.valueChanged.connect(self._validate)
        self.name_input.textChanged.connect(self._validate)
        self.draw_btn.toggled.connect(self._drawing_toggled)
        self.apply_btn.clicked.connect(self._apply)
        self.save_btn.clicked.connect(self._save)
        self.automatic_btn.clicked.connect(self._automatic)
        overlay.distance_changed.connect(self._line_distance)
        overlay.drawing_changed.connect(self._drawing_changed)
        overlay.image_available_changed.connect(self.draw_btn.setEnabled)
        scale.changed.connect(self._active_changed)
        scale.presets_changed.connect(self._reload_presets)
        self._escape = QShortcut(QKeySequence(Qt.Key.Key_Escape), self)
        self._escape.activated.connect(overlay.cancel_drawing)
        self._reload_presets()
        self._fill_current()
        self._active_changed()
        self.draw_btn.setEnabled(overlay._shape is not None)

    @staticmethod
    def _distance_input():
        field = QDoubleSpinBox()
        field.setDecimals(6)
        field.setRange(0, 1e9)
        field.setGroupSeparatorShown(True)
        field.setKeyboardTracking(False)
        return field

    def _reload_presets(self):
        selected = self.preset_combo.currentData()
        self.preset_combo.blockSignals(True)
        self.preset_combo.clear()
        self.preset_combo.addItem('New / current scale', None)
        for name in sorted(self.scale.presets, key=str.casefold):
            self.preset_combo.addItem(name, name)
        self.preset_combo.setCurrentIndex(max(0, self.preset_combo.findData(selected)))
        self.preset_combo.blockSignals(False)
        self._selected_name = self.preset_combo.currentData()
        self._validate()

    def _set_fields(self, pixels, known_um, name):
        self._syncing = True
        try:
            self.pixels_input.setValue(pixels)
            self.known_input.setValue(known_um)
            self.name_input.setText(name)
        finally:
            self._syncing = False
        self._validate()

    def _fill_current(self):
        scale = self.scale.manual
        if scale:
            self._set_fields(scale.pixels, scale.known_um, scale.name)
        else:
            self._set_fields(100, 100 * self.scale.pitch_um if self.scale.pitch_um else 0, '')

    def _load_preset(self, *_args):
        self.overlay.cancel_drawing()
        self.overlay.clear_calibration_line()
        self._selected_name = self.preset_combo.currentData()
        if self._selected_name:
            scale = self.scale.presets[self._selected_name]
            self._set_fields(scale.pixels, scale.known_um, scale.name)
        else:
            self._fill_current()

    def _validate(self, *_args):
        pixels, known = self.pixels_input.value(), self.known_input.value()
        valid = pixels > 0 and known > 0
        name = self.name_input.text().strip()
        unique = bool(name) and not self.scale.name_exists(name)
        self.apply_btn.setEnabled(valid)
        self.save_btn.setEnabled(valid and unique)
        self.delete_btn.setEnabled(self._selected_name in self.scale.presets)
        self.preview.setText(f'{known / pixels:.6g} µm / pixel' if valid else 'Enter both distances')
        if not valid:
            text = 'Enter positive distances, or draw a line to fill the pixel distance.'
        elif not name:
            text = 'Apply now, or enter a unique name to save this scale for reuse.'
        elif not unique:
            text = 'This name is already saved. Apply it, or enter a different name to save a copy.'
        else:
            text = 'Save Calibration stores this scale and applies it immediately.'
        self.feedback.setText(text)

    def _pixels_edited(self, pixels):
        if not self._syncing:
            self._syncing = True
            try:
                self.overlay.resize_calibration_line(pixels)
            finally:
                self._syncing = False
        self._validate()

    def _line_distance(self, pixels):
        if self._syncing:
            return
        self._syncing = True
        try:
            self.pixels_input.setValue(pixels)
        finally:
            self._syncing = False
        self._validate()

    def _drawing_toggled(self, checked):
        if checked:
            self.overlay.start_drawing('calibrate')
        elif self.overlay.mode == 'calibrate':
            self.overlay.cancel_drawing()

    def _drawing_changed(self, mode):
        self.draw_btn.blockSignals(True)
        self.draw_btn.setChecked(mode == 'calibrate')
        self.draw_btn.setText('Draw on Image · Escape to Cancel' if mode == 'calibrate' else
                             'Draw Calibration Line')
        self.draw_btn.blockSignals(False)

    def _apply(self):
        # An edited preset is a custom scale until saved with its own unique name.
        name = self.name_input.text().strip()
        preset = self.scale.presets.get(name)
        if not preset or (preset.pixels, preset.known_um) != (self.pixels_input.value(), self.known_input.value()):
            name = ''
        self.scale.apply(self.pixels_input.value(), self.known_input.value(), name)
        self.feedback.setText('Scale applied. Existing measurement labels have been updated.')

    def _save(self):
        self.scale.save(self.pixels_input.value(), self.known_input.value(), self.name_input.text())
        self.preset_combo.setCurrentIndex(self.preset_combo.findData(self.name_input.text().strip()))
        self.feedback.setText('Calibration saved and applied.')

    def _delete(self):
        name = self._selected_name
        if name in self.scale.presets:
            self.scale.delete(name)
            self.feedback.setText('Stored calibration deleted. The active scale is unchanged.')

    def _automatic(self):
        self.overlay.cancel_drawing()
        self.overlay.clear_calibration_line()
        self.preset_combo.setCurrentIndex(0)
        self.scale.use_recording_scale()
        self._fill_current()
        self.feedback.setText('Recording scale selected. Unknown recording scale uses pixel units.')

    def _active_changed(self):
        self.active_label.setText(f'Active: {self.scale.description}')

    def closeEvent(self, event):
        if self.overlay.mode == 'calibrate':
            self.overlay.cancel_drawing()
        self.overlay.clear_calibration_line()
        super().closeEvent(event)
