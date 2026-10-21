"""Median synthetic-white math, worker lifetime, and export invariants."""
import importlib
import importlib.util
import json
import os
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from viewer_runtime import prepare_qt_runtime
prepare_qt_runtime()
import numpy as np


class MedianMathTests(unittest.TestCase):
    def core(self):
        self.assertIsNotNone(importlib.util.find_spec("median_white_field"),
                             "Median white-field implementation is missing")
        return importlib.import_module("median_white_field")

    def test_flat_fields_normalize_to_one_at_any_positive_scale(self):
        core = self.core()
        for level in (0.02, 100, 1800):
            with self.subTest(level=level):
                result = core.correct_median_white(np.full((64, 80), level), 100, device="cpu")
                np.testing.assert_allclose(result.image, 1, atol=2e-6)
                self.assertEqual(result.image.dtype, np.float32)

    def test_gain_scaled_dark_is_subtracted_before_synthetic_field(self):
        core = self.core()
        frame = np.full((64, 80), 120, np.uint16)
        frame[30:34, 38:42] = 70
        original = frame.copy()
        result = core.correct_median_white(frame, 100, dark=np.full(frame.shape, 10),
                                           dark_gain=0, data_gain=20*np.log10(2), device="cpu")
        np.testing.assert_allclose(result.image[30:34, 38:42], .5, atol=2e-6)
        np.testing.assert_allclose(result.image[:10], 1, atol=2e-6)
        np.testing.assert_array_equal(frame, original)
        scaled = core.correct_median_white(frame.astype(np.float32)*.4, 100,
                                           dark=np.full(frame.shape, 8), device="cpu")
        np.testing.assert_allclose(scaled.image, result.image, atol=2e-6)

    def test_negative_pixels_remain_signed_and_zero_background_is_safe(self):
        core = self.core()
        frame = np.full((40, 48), 10, np.float32)
        frame[20, 24] = -5
        result = core.correct_median_white(frame, 100, device="cpu")
        self.assertAlmostEqual(float(result.image[20, 24]), -.5, places=5)
        result = core.correct_median_white(np.zeros((12, 16)), 100, device="cpu")
        np.testing.assert_array_equal(result.image, 0)
        self.assertTrue(np.isfinite(result.image).all())

    def test_bad_inputs_and_cancelled_work_do_not_run(self):
        core = self.core()
        for image, size in ((np.ones(8), 100), (np.ones((8, 8)), 0),
                            (np.full((8, 8), np.nan), 100)):
            with self.assertRaises(ValueError):
                core.correct_median_white(image, size, device="cpu")
        with self.assertRaises(core.MedianCancelled):
            core.correct_median_white(np.ones((8, 8)), 100, cancelled=lambda: True)

    @unittest.skipUnless(importlib.util.find_spec("torch"), "PyTorch not installed")
    def test_cuda_matches_cpu_even_window_and_reflected_edges(self):
        core = self.core()
        import torch
        if not torch.cuda.is_available():
            self.skipTest("CUDA not available")
        image = np.random.default_rng(9).uniform(1, 50, (37, 43)).astype(np.float32)
        for size in (4, 13, 100):
            cpu = core.correct_median_white(image, size, device="cpu")
            gpu = core.correct_median_white(image, size, device="cuda")
            self.assertEqual(gpu.backend, "cuda")
            np.testing.assert_allclose(gpu.image, cpu.image, rtol=2e-6, atol=2e-6)


class _Reader:
    def __init__(self, frames):
        self.frames = frames
        self.h, self.w = frames[0].shape
        self.closed = False
    def __len__(self):
        return len(self.frames)
    def __getitem__(self, index):
        return self.frames[index]
    def close(self):
        self.closed = True


class MedianViewerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import lucid_viewer
        cls.module = lucid_viewer
        cls.app = lucid_viewer.QApplication.instance() or lucid_viewer.QApplication([])

    def setUp(self):
        from PyQt6.QtCore import QSettings
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        ini = str(Path(self.directory.name) / "settings.ini")
        self.settings_patch = patch.object(self.module, "QSettings",
            lambda *args: QSettings(ini, QSettings.Format.IniFormat))
        self.settings_patch.start()
        self.addCleanup(self.settings_patch.stop)
        self.win = self.module.LucidViewer()
        self.addCleanup(self.dispose, self.win)
        frames = []
        for value in (60, 35):
            frame = np.full((64, 80), 110, np.uint16)
            frame[30:34, 38:42] = value
            frames.append(frame)
        self.reader = _Reader(frames)
        self.win._reader = self.reader
        self.win._path = str(Path(self.directory.name) / "recording.raw")
        self.win.w_spin.setValue(80)
        self.win.h_spin.setValue(64)
        self.win.frame_slider.setRange(0, 1)
        self.win._show_frame(0)
        self.win.show()

    def wait_for(self, condition, timeout=10):
        from PyQt6.QtTest import QTest
        limit = time.monotonic() + timeout
        while not condition() and time.monotonic() < limit:
            QTest.qWait(10)
        self.assertTrue(condition(), "Qt operation timed out")

    def dispose(self, win):
        from PyQt6 import sip
        from PyQt6.QtCore import QCoreApplication, QEvent
        if sip.isdeleted(win):
            return
        win.close()
        self.wait_for(lambda: getattr(win, "_median_worker", None) is None)
        win.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        self.app.processEvents()

    def enable_median(self):
        index = self.win.white_combo.findText("Median Filter")
        self.assertGreaterEqual(index, 0, "Median Filter mode is missing")
        self.win.white_combo.setCurrentIndex(index)
        self.win.white_chk.setChecked(True)

    def ready(self):
        return not getattr(self.win, "_median_display_pending", True) and getattr(self.win, "_median_worker", 1) is None

    def test_mode_needs_no_white_folder_and_patch_control_is_contextual(self):
        self.win._check_white_options()
        self.enable_median()
        self.assertTrue(self.win.median_patch_spin.isVisible())
        self.assertEqual(self.win.median_patch_spin.value(), 100)
        self.wait_for(self.ready)
        self.assertEqual(self.win._white_mode, "median")
        self.win.white_chk.setChecked(False)
        self.assertFalse(self.win.median_patch_spin.isVisible())
        np.testing.assert_array_equal(self.win.imview.image, self.reader[0])

    def test_display_density_and_metadata_use_the_relative_corrected_image(self):
        self.win._dark_field = np.full((64, 80), 10, np.float32)
        self.enable_median()
        self.wait_for(self.ready)
        self.assertAlmostEqual(float(self.win.imview.image[31, 39]), .5, places=5)
        self.win._open_density_retrieval()
        child = self.win._density_retrieval_window
        child.chk_auto.setChecked(False)
        np.testing.assert_array_equal(child._frame, self.win.imview.image)
        meta = self.win._correction_metadata()
        self.assertEqual(meta["white_field_mode"], "median")
        self.assertFalse(meta["absolute_transmission"])
        self.assertEqual(meta["median_patch_size_px"], 100)
        child.close()

    def test_new_frame_discards_stale_result_without_stalling_qt(self):
        from PyQt6.QtCore import QTimer
        core = importlib.import_module("median_white_field")
        entered, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)
        original = core.correct_median_white
        def delayed(*args, **kwargs):
            entered.set()
            release.wait(3)
            return original(*args, **kwargs)
        with patch.object(self.module, "correct_median_white", delayed):
            self.enable_median()
            self.wait_for(entered.is_set)
            ticks = []
            timer = QTimer()
            timer.setInterval(5)
            timer.timeout.connect(lambda: ticks.append(1))
            timer.start()
            self.win._show_frame(1)
            from PyQt6.QtTest import QTest
            QTest.qWait(40)
            self.assertGreater(len(ticks), 0)
            release.set()
            self.wait_for(self.ready)
            timer.stop()
        self.assertEqual(self.win.frame_slider.value(), 1)
        self.assertAlmostEqual(float(self.win.imview.image[31, 39]), 35/110, places=5)

    def test_close_while_median_is_busy_waits_asynchronously(self):
        core = importlib.import_module("median_white_field")
        entered, release = threading.Event(), threading.Event()
        self.addCleanup(release.set)
        original = core.correct_median_white
        def delayed(*args, **kwargs):
            entered.set()
            release.wait(3)
            return original(*args, **kwargs)
        with patch.object(self.module, "correct_median_white", delayed):
            self.enable_median()
            self.wait_for(entered.is_set)
            start = time.monotonic()
            self.win.close()
            self.assertLess(time.monotonic()-start, .25)
            self.assertFalse(self.reader.closed)
            release.set()
            self.wait_for(lambda: self.reader.closed)

    def test_late_flat_result_cannot_override_median_mode(self):
        self.enable_median()
        self.wait_for(self.ready)
        self.win._on_white_flat_loaded(np.full((64, 80), 500), "old white", "")
        self.assertEqual(self.win._white_mode, "median")
        self.assertAlmostEqual(float(self.win.imview.image[31, 39]), 60/110, places=5)

    def test_store_and_restore_median_choice_without_reference_folder(self):
        self.enable_median()
        self.win.median_patch_spin.setValue(77)
        self.wait_for(self.ready)
        self.win._store_fields_ref()
        refs = json.loads((Path(self.directory.name)/"metadata.json").read_text())["field_references"]
        self.assertEqual(refs["white_mode"], "median")
        self.assertEqual(refs["median_patch_size_px"], 77)
        self.win.white_chk.setChecked(False)
        self.win._stored_field_refs = refs
        self.win._apply_stored_field_refs()
        self.win._reload_fields()
        self.wait_for(self.ready)
        self.assertEqual(self.win._white_mode, "median")
        self.assertEqual(self.win.median_patch_spin.value(), 77)

    def test_auto_levels_reuses_the_committed_median_image(self):
        self.enable_median()
        self.wait_for(self.ready)
        with patch.object(self.module, "correct_median_white",
                          side_effect=AssertionError("Median was recomputed on the GUI thread")):
            self.win._auto_levels()
        low, high = self.win.imview.getLevels()
        self.assertTrue(np.isfinite([low, high]).all())
        self.assertGreater(high, low)

    def test_playback_commits_images_when_filtering_is_slower_than_timer(self):
        from PyQt6.QtTest import QTest
        core=importlib.import_module("median_white_field")
        original=core.correct_median_white
        seen=set()
        def record():
            image=self.win.imview.image
            if image is not None and image.dtype == np.float32:
                seen.add(round(float(image[31,39]), 3))
        self.win.imview.imageItem.sigImageChanged.connect(record)
        def delayed(*args, **kwargs):
            time.sleep(.18)
            return original(*args, **kwargs)
        with patch.object(self.module, "correct_median_white", delayed):
            self.enable_median()
            self.win._start_play()
            QTest.qWait(750)
            self.win._stop_play()
            self.assertGreaterEqual(len(seen), 2, "Playback kept cancelling every median result")
            self.wait_for(self.ready)

    def test_late_pca_error_does_not_disable_selected_median_mode(self):
        self.enable_median()
        self.wait_for(self.ready)
        self.win._on_pca_error("Failure from previous PCA request")
        self.assertEqual(self.win._white_mode, "median")
        self.assertTrue(self.win.white_chk.isChecked())

    def test_patch_size_and_mode_restore_from_application_settings(self):
        self.enable_median()
        self.win.median_patch_spin.setValue(77)
        self.wait_for(self.ready)
        self.win._save_settings()
        other=self.module.LucidViewer()
        self.addCleanup(self.dispose, other)
        other.show()
        self.assertEqual(other.white_combo.currentText(), "Median Filter")
        self.assertTrue(other.white_chk.isChecked())
        self.assertTrue(other.median_patch_spin.isVisible())
        self.assertEqual(other.median_patch_spin.value(), 77)

    def test_gif_cancellation_during_median_is_reported_without_escaping(self):
        core=importlib.import_module("median_white_field")
        worker=self.module.GifExportWorker(str(Path(self.directory.name)/"cancel.gif"),
            self.reader, {}, 2, 1, 1, 10, (0,1), median_patch_size=100)
        errors=[]
        worker.export_error.connect(errors.append)
        def cancel_during_frame(*args, **kwargs):
            worker.cancel()
            return core.correct_median_white(*args, **kwargs)
        with patch.object(self.module, "correct_median_white", cancel_during_frame):
            try:
                worker.run()
            except core.MedianCancelled:
                self.fail("Cancellation escaped the GIF worker")
        self.assertEqual(errors, ["GIF export cancelled."])

    def test_imagej_sidecar_preserves_relative_contrast_provenance(self):
        output=str(Path(self.directory.name)/"imagej.tiff")
        correction=dict(white_field_mode="median", absolute_transmission=False,
                        median_patch_size_px=100)
        worker=self.module.TiffExportWorker(output, self.reader, {}, 2,
            None, None, None, None, None, json.dumps({"correction":correction}),
            False, imagej=True, median_patch_size=100)
        errors=[]
        worker.export_error.connect(errors.append)
        worker.run()
        self.assertEqual(errors, [])
        info=json.loads((Path(self.directory.name)/"imagej_transmission.json").read_text())
        self.assertEqual(info.get("correction"), correction)

    def test_average_export_corrects_each_frame_before_averaging(self):
        import tifffile
        first = np.full((64, 80), 110, np.uint16)
        second = np.full((64, 80), 210, np.uint16)
        first[30:34, 38:42] = second[30:34, 38:42] = 60
        reader = _Reader([first, second])
        output = str(Path(self.directory.name)/"mean.tiff")
        worker = self.module.AverageExportWorker(output, reader, {}, 2, 16,
            np.full(first.shape, 10, np.float32), None, None, None, None, "{}",
            median_patch_size=100)
        errors = []
        worker.export_error.connect(errors.append)
        worker.run()
        self.assertEqual(errors, [])
        with tifffile.TiffFile(output) as file:
            encoded = file.asarray()
            meta = json.loads(file.pages[0].description)
        decoded = encoded/meta["code_max"]*(meta["transmission_max"]-meta["transmission_min"])+meta["transmission_min"]
        self.assertAlmostEqual(float(decoded[31, 39]), .375, places=4)
        self.assertAlmostEqual(float(decoded[0, 0]), 1, places=4)


if __name__ == "__main__":
    unittest.main()
