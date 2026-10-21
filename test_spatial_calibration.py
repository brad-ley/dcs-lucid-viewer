"""Spatial calibration persistence and interactive measurement behavior."""
import importlib
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from viewer_runtime import prepare_qt_runtime
prepare_qt_runtime()
import numpy as np
from PyQt6.QtCore import QSettings, QPointF, Qt
from PyQt6.QtTest import QTest


class _Reader:
    def __init__(self, frames):
        self.frames = frames
        self.h, self.w = frames[0].shape
    def __len__(self):
        return len(self.frames)
    def __getitem__(self, index):
        return self.frames[index]
    def close(self):
        pass


class ScaleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import lucid_viewer
        cls.viewer_module = lucid_viewer
        cls.app = lucid_viewer.QApplication.instance() or lucid_viewer.QApplication([])

    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec('spatial_calibration'),
                             'Spatial calibration tool is missing')
        self.module = importlib.import_module('spatial_calibration')
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.ini = str(Path(self.directory.name) / 'scale.ini')
        self.settings = QSettings(self.ini, QSettings.Format.IniFormat)
        self.model = self.module.SpatialCalibration(self.settings)

    def test_unique_names_persist_without_overwriting_and_deleted_active_scale_survives(self):
        self.model.save(200, 50, ' Objective 10x ')
        self.assertAlmostEqual(self.model.pitch_um, .25)
        for name in ('Objective 10x', ' objective 10X ', ''):
            with self.assertRaises(ValueError):
                self.model.save(100, 90, name)
        restored = self.module.SpatialCalibration(QSettings(self.ini, QSettings.Format.IniFormat))
        self.assertEqual(list(restored.presets), ['Objective 10x'])
        self.assertAlmostEqual(restored.pitch_um, .25)
        restored.delete('Objective 10x')
        self.assertFalse(restored.presets)
        self.assertAlmostEqual(restored.pitch_um, .25)
        again = self.module.SpatialCalibration(QSettings(self.ini, QSettings.Format.IniFormat))
        self.assertFalse(again.presets)
        self.assertAlmostEqual(again.pitch_um, .25)

    def test_invalid_distances_do_not_change_active_calibration(self):
        self.model.apply(100, 20)
        for px, um in ((0, 1), (1, 0), (-1, 2), (float('nan'), 2), (2, float('inf'))):
            with self.assertRaises(ValueError):
                self.model.apply(px, um)
            self.assertAlmostEqual(self.model.pitch_um, .2)

    def test_manual_scale_overrides_metadata_until_automatic_is_selected(self):
        self.model.set_recording_scale(.46)
        self.assertAlmostEqual(self.model.pitch_um, .46)
        self.model.apply(200, 50)
        self.model.set_recording_scale(.92)
        self.assertAlmostEqual(self.model.pitch_um, .25)
        self.model.use_recording_scale()
        self.assertAlmostEqual(self.model.pitch_um, .92)
        self.model.set_recording_scale(None)
        self.assertIsNone(self.model.pitch_um)

    def viewer(self):
        settings_patch = patch.object(self.viewer_module, 'QSettings', lambda *a: self.settings)
        settings_patch.start()
        self.addCleanup(settings_patch.stop)
        win = self.viewer_module.LucidViewer()
        self.addCleanup(self.dispose, win)
        win._path = str(Path(self.directory.name) / 'recording.raw')
        win._reader = _Reader([np.ones((100, 160), np.uint16), np.full((100, 160), 2, np.uint16)])
        win.w_spin.setValue(160)
        win.h_spin.setValue(100)
        win.frame_slider.setRange(0, 1)
        win._show_frame(0)
        win.show()
        win.imview.getView().autoRange()
        QTest.qWait(30)
        return win

    def dispose(self, win):
        from PyQt6 import sip
        from PyQt6.QtCore import QCoreApplication, QEvent
        if sip.isdeleted(win):
            return
        win.close()
        win.deleteLater()
        # Drain DeferredDelete before another graphics scene is constructed.
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        self.app.processEvents()

    def test_dialog_is_nonmodal_validates_and_loads_saved_scale_as_draft(self):
        win = self.viewer()
        win._open_scale_dialog()
        dlg = win._scale_dialog
        self.assertFalse(dlg.isModal())
        dlg.pixels_input.setValue(200)
        dlg.known_input.setValue(50)
        self.assertFalse(dlg.save_btn.isEnabled())
        dlg.name_input.setText('10x')
        self.assertTrue(dlg.save_btn.isEnabled())
        dlg.save_btn.click()
        self.assertAlmostEqual(win._spatial_scale.pitch_um, .25)
        self.assertFalse(dlg.save_btn.isEnabled())
        dlg.name_input.setText(' 10X ')
        self.assertFalse(dlg.save_btn.isEnabled())
        dlg.pixels_input.setValue(100)
        dlg.known_input.setValue(75)
        dlg.name_input.setText('5x')
        dlg.save_btn.click()
        dlg.preset_combo.setCurrentIndex(dlg.preset_combo.findData('10x'))
        self.assertAlmostEqual(dlg.pixels_input.value(), 200)
        self.assertAlmostEqual(win._spatial_scale.pitch_um, .75)
        dlg.apply_btn.click()
        self.assertAlmostEqual(win._spatial_scale.pitch_um, .25)
        dlg.delete_btn.click()
        self.assertNotIn('10x', win._spatial_scale.presets)
        self.assertAlmostEqual(win._spatial_scale.pitch_um, .25)
        dlg.close()
        self.assertTrue(win.isVisible())

    def draw(self, win, start, end):
        image = win.imview.imageItem
        win._measurements._line_dragged(image.mapToScene(QPointF(*start)),
                                       image.mapToScene(QPointF(*end)), True)

    def test_measurements_use_pixel_geometry_and_relabel_after_scale_changes(self):
        win = self.viewer()
        win.measure_btn.click()
        self.draw(win, (20, 20), (50, 60))  # 30-40-50 triangle
        line = win._measurements.lines[0]
        self.assertIn('50', line.label.toPlainText())
        self.assertIn('px', line.label.toPlainText())
        win._spatial_scale.apply(200, 50)
        self.assertIn('12.5', line.label.toPlainText())
        self.assertIn('µm', line.label.toPlainText())
        line.roi.movePoint(line.roi.getHandles()[1], QPointF(20, 60))
        self.assertIn('10', line.label.toPlainText())
        self.assertAlmostEqual(line.length_px, 40)
        win._show_frame(1)
        self.assertEqual(len(win._measurements.lines), 1)
        win._path = str(Path(self.directory.name) / 'another.raw')
        win._reader = _Reader([np.ones((40, 60), np.uint16)])
        win._cache.clear()
        win._show_frame(0)
        self.assertEqual(len(win._measurements.lines), 1)
        self.assertAlmostEqual(line.length_px, 40)
        win.clear_measurements_btn.click()
        self.assertFalse(win._measurements.lines)
        self.assertIsNone(line.roi.scene())
        self.assertIsNone(line.label.scene())

    def test_calibration_line_updates_fields_and_closing_cancels_drawing(self):
        win = self.viewer()
        win._open_scale_dialog()
        dlg = win._scale_dialog
        dlg.draw_btn.click()
        self.draw(win, (20, 20), (50, 60))
        self.assertAlmostEqual(dlg.pixels_input.value(), 50)
        line = win._measurements.calibration_line
        line.roi.movePoint(line.roi.getHandles()[1], QPointF(20, 60))
        self.assertAlmostEqual(dlg.pixels_input.value(), 40)
        dlg.pixels_input.setValue(80)
        self.assertAlmostEqual(line.length_px, 80)
        dlg.draw_btn.click()
        dlg.close()
        self.assertIsNone(win._measurements.mode)
        self.assertIsNone(win._measurements.calibration_line)
        self.assertFalse(win.measure_btn.isChecked())

    def test_draw_gesture_uses_scene_to_image_coordinates_and_escape_cancels(self):
        win = self.viewer()
        win.measure_btn.click()
        view = win.imview.getView()
        scene_start = win.imview.imageItem.mapToScene(QPointF(25, 25))
        scene_end = win.imview.imageItem.mapToScene(QPointF(55, 65))
        viewport = win.imview.ui.graphicsView.viewport()
        start = win.imview.ui.graphicsView.mapFromScene(scene_start)
        end = win.imview.ui.graphicsView.mapFromScene(scene_end)
        QTest.mouseMove(viewport, start)
        QTest.qWait(25)  # pyqtgraph throttles mouse moves; let the initial hover settle.
        QTest.mousePress(viewport, Qt.MouseButton.LeftButton, pos=start)
        QTest.mouseMove(viewport, end, delay=25)
        QTest.qWait(25)
        QTest.mouseRelease(viewport, Qt.MouseButton.LeftButton, pos=end)
        QTest.qWait(30)
        self.assertEqual(len(win._measurements.lines), 1)
        self.assertAlmostEqual(win._measurements.lines[0].length_px, 50, delta=.8)
        win.measure_btn.click()
        QTest.keyClick(win, Qt.Key.Key_Escape)
        self.assertIsNone(win._measurements.mode)
        self.assertFalse(win.measure_btn.isChecked())

    def test_endpoint_can_be_dragged_in_the_image(self):
        win = self.viewer()
        win.measure_btn.click()
        self.draw(win, (25, 25), (55, 65))
        line = win._measurements.lines[0]
        image = win.imview.imageItem
        graphics = win.imview.ui.graphicsView
        viewport = graphics.viewport()
        start = graphics.mapFromScene(image.mapToScene(QPointF(55, 65)))
        end = graphics.mapFromScene(image.mapToScene(QPointF(25, 65)))
        QTest.mouseMove(viewport, start)
        QTest.qWait(30)
        QTest.mousePress(viewport, Qt.MouseButton.LeftButton, pos=start)
        QTest.qWait(20)
        QTest.mouseMove(viewport, end, delay=30)
        QTest.qWait(30)
        QTest.mouseRelease(viewport, Qt.MouseButton.LeftButton, pos=end)
        QTest.qWait(30)
        self.assertAlmostEqual(line.length_px, 40, delta=.8)

    def test_cancelling_replacement_calibration_line_restores_its_distance(self):
        win = self.viewer()
        win._open_scale_dialog()
        dlg = win._scale_dialog
        dlg.draw_btn.click()
        self.draw(win, (20, 20), (50, 60))
        original = win._measurements.calibration_line
        dlg.draw_btn.click()
        image = win.imview.imageItem
        win._measurements._line_dragged(image.mapToScene(QPointF(20, 20)),
                                       image.mapToScene(QPointF(100, 20)), False)
        self.assertAlmostEqual(dlg.pixels_input.value(), 80)
        win._measurements.cancel_drawing()
        self.assertIs(win._measurements.calibration_line, original)
        self.assertAlmostEqual(dlg.pixels_input.value(), 50)

    def test_recording_scale_clears_calibration_line_and_saved_selection(self):
        win = self.viewer()
        win._spatial_scale.save(100, 23, 'Manual')
        win._spatial_scale.set_recording_scale(.46)
        win._open_scale_dialog()
        dlg = win._scale_dialog
        dlg.preset_combo.setCurrentIndex(dlg.preset_combo.findData('Manual'))
        dlg.draw_btn.click()
        self.draw(win, (20, 20), (50, 60))
        dlg.automatic_btn.click()
        self.assertIsNone(win._measurements.calibration_line)
        self.assertIsNone(dlg.preset_combo.currentData())
        self.assertAlmostEqual(win._spatial_scale.pitch_um, .46)
        self.assertAlmostEqual(dlg.known_input.value() / dlg.pixels_input.value(), .46)

    def test_fft_uses_the_applied_sample_scale(self):
        win = self.viewer()
        win._spatial_scale.apply(100, 23)
        with patch.object(self.viewer_module._FFTDialog, 'exec', lambda self: 0):
            win._open_fft_dialog()
        dialog = win.findChildren(self.viewer_module._FFTDialog)[-1]
        self.assertAlmostEqual(dialog._pixel_size_um, .23)
        self.assertEqual(dialog._freq_unit, 'cycles/µm')

    def test_effective_scale_is_used_by_density_retrieval(self):
        win = self.viewer()
        win._path = 'test.raw'
        win._spatial_scale.apply(100, 23)
        win._open_density_retrieval()
        retrieval = win._density_retrieval_window
        retrieval.chk_auto.setChecked(False)
        self.assertAlmostEqual(retrieval.pixel_input.value(), .23)
        win._spatial_scale.apply(100, 46)
        self.assertAlmostEqual(retrieval.pixel_input.value(), .46)
        for pixels, known, expected in ((1, 2000, 2000), (2000, 1, .0005),
                                        (231.123456, 100, 0.4326691965007653745018419939)):
            win._spatial_scale.apply(pixels, known)
            self.assertAlmostEqual(retrieval.pixel_input.value(), expected, delta=1e-11)


if __name__ == '__main__':
    unittest.main()
