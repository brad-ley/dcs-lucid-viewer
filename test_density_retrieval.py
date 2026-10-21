"""Numerical invariants and Qt integration checks for Density Retrieval.

Run numerical tests in any environment with numpy/scipy/xraylib; the nonlinear
tests additionally need torch/hotopy. CUDA checks require a working CUDA
runtime and otherwise skip. Qt tests skip if PyQt6 is unavailable.
"""
import importlib
import importlib.util
import os
import threading
import tempfile
import time
import unittest
from dataclasses import replace
from unittest.mock import patch

from viewer_runtime import prepare_qt_runtime

prepare_qt_runtime()

import numpy as np

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


def core_module(test):
    test.assertIsNotNone(importlib.util.find_spec("density_retrieval"),
                         "The density retrieval numerical module is missing")
    return importlib.import_module("density_retrieval")


class RetrievalMathTests(unittest.TestCase):
    def test_vacuum_has_zero_thickness(self):
        core = core_module(self)
        result = core.retrieve_density(np.ones((24, 32), np.float32),
                                       core.RetrievalParameters())
        np.testing.assert_allclose(result.thickness_m, 0, atol=1e-15)

    def test_uniform_material_recovers_known_thickness(self):
        import xraylib
        from scipy.constants import h, c, e
        core = core_module(self)
        p = core.RetrievalParameters()
        wavelength = h * c / (p.energy_keV * 1000 * e)
        mu = 4 * np.pi * xraylib.Refractive_Index_Im(
            p.material, p.energy_keV, p.density_g_cm3) / wavelength
        image = np.full((24, 32), np.exp(-mu * 1e-6), np.float32)
        before = image.copy()
        result = core.retrieve_density(image, p)
        np.testing.assert_allclose(result.thickness_m, 1e-6, rtol=2e-6)
        # 1 um of Bi at 9.8 g/cm3 is 0.98 mg/cm2.
        np.testing.assert_allclose(result.thickness_m * p.density_g_cm3 * 1e5,
                                   0.98, rtol=2e-6)
        np.testing.assert_array_equal(image, before)

    def test_exit_outputs_match_known_slab_and_keep_signed_contrast(self):
        import xraylib
        from scipy.constants import h, c, e
        core = core_module(self)
        p = core.RetrievalParameters()
        wavelength = h * c / (p.energy_keV * 1000 * e)
        delta = 1 - xraylib.Refractive_Index_Re(p.material, p.energy_keV, p.density_g_cm3)
        mu = 4 * np.pi * xraylib.Refractive_Index_Im(
            p.material, p.energy_keV, p.density_g_cm3) / wavelength
        for slab_m in (-1e-6, 0, 1e-6):
            with self.subTest(thickness_m=slab_m):
                image = np.full((24, 32), np.exp(-mu * slab_m), np.float32)
                result = core.retrieve_density(image, p)
                self.assertTrue(hasattr(result, "exit_phase_rad"), "Exit phase output is missing")
                self.assertTrue(hasattr(result, "exit_transmission"), "Exit transmission output is missing")
                np.testing.assert_allclose(result.exit_phase_rad,
                    -2 * np.pi * delta * slab_m / wavelength, rtol=2e-6, atol=1e-12)
                np.testing.assert_allclose(result.exit_transmission, image, rtol=2e-6)

    def test_single_frequency_matches_analytic_paganin_filter(self):
        import xraylib
        from scipy.constants import h, c, e
        core = core_module(self)
        p = core.RetrievalParameters(pixel_size_um=1.25)
        wavelength = h * c / (p.energy_keV * 1000 * e)
        delta = 1 - xraylib.Refractive_Index_Re(p.material, p.energy_keV,
                                                p.density_g_cm3)
        mu = 4 * np.pi * xraylib.Refractive_Index_Im(
            p.material, p.energy_keV, p.density_g_cm3) / wavelength
        cosine = np.cos(2 * np.pi * np.arange(63) * 3 / 63)
        image = np.tile(0.7 + 0.1 * cosine, (35, 1)).astype(np.float32)
        zeff = p.rr2_m * p.rr1_m / (p.rr1_m + p.rr2_m)
        frequency = 3 / (63 * p.pixel_size_um * 1e-6)
        amplitude = 0.1 / (1 + zeff * delta / mu * (2 * np.pi * frequency)**2)
        expected = np.tile(-np.log(0.7 + amplitude * cosine) / mu, (35, 1))
        result = core.retrieve_density(image, p)
        np.testing.assert_allclose(result.thickness_m, expected, rtol=2e-6)

    def test_invalid_images_and_parameters_are_rejected(self):
        core = core_module(self)
        p = core.RetrievalParameters()
        for image in (np.ones(8), np.ones((0, 3)), np.full((3, 3), np.nan)):
            with self.subTest(shape=image.shape):
                with self.assertRaises(ValueError):
                    core.retrieve_density(image, p)
        for p_bad in (replace(p, pixel_size_um=0), replace(p, rr1_m=0),
                      replace(p, density_g_cm3=-1), replace(p, material="not a material")):
            with self.subTest(parameters=p_bad):
                with self.assertRaises(ValueError):
                    core.retrieve_density(np.ones((8, 8)), p_bad)

    def test_small_positive_transmission_is_not_clipped(self):
        import xraylib
        from scipy.constants import h, c, e
        core = core_module(self)
        p = core.RetrievalParameters()
        wavelength = h * c / (p.energy_keV * 1000 * e)
        mu = 4 * np.pi * xraylib.Refractive_Index_Im(
            p.material, p.energy_keV, p.density_g_cm3) / wavelength
        for transmission in (1e-12, -0.1):
            with self.subTest(transmission=transmission):
                image = np.full((16, 16), transmission, np.float32)
                expected = -np.log(float(image[0, 0]) if transmission > 0 else 1e-10) / mu
                result = core.retrieve_density(image, p)
                np.testing.assert_allclose(result.thickness_m, expected, rtol=1e-6)

    def test_interrupted_request_does_not_run(self):
        core = core_module(self)
        with self.assertRaises(core.RetrievalCancelled):
            core.retrieve_density(np.ones((8, 8)), core.RetrievalParameters(),
                                  cancelled=lambda: True)

    @unittest.skipUnless(importlib.util.find_spec("hotopy") and
                         importlib.util.find_spec("torch"), "HoToPy not installed")
    def test_nonlinear_vacuum_on_cpu(self):
        core = core_module(self)
        p = core.RetrievalParameters(solver="nonlinear", device="cpu", max_iter=2,
                                     pixel_size_um=10.0)
        result = core.retrieve_density(np.ones((32, 32), np.float32), p)
        np.testing.assert_allclose(result.thickness_m, 0, atol=1e-10)
        np.testing.assert_allclose(result.exit_phase_rad, 0, atol=1e-7)
        np.testing.assert_allclose(result.exit_transmission, 1, atol=1e-7)
        self.assertEqual(result.device, "cpu")


    @unittest.skipUnless(importlib.util.find_spec("hotopy") and
                         importlib.util.find_spec("torch"), "HoToPy not installed")
    def test_nonlinear_cuda_matches_cpu(self):
        import torch
        if not torch.cuda.is_available():
            self.skipTest("CUDA runtime not available")
        core = core_module(self)
        p = core.RetrievalParameters(solver="nonlinear", device="cuda",
                                     pixel_size_um=2.0, rr2_m=0.2, max_iter=10)
        vacuum = core.retrieve_density(np.ones((64, 64), np.float32),
                                       replace(p, max_iter=2))
        self.assertEqual(vacuum.device, "cuda")
        np.testing.assert_allclose(vacuum.thickness_m, 0, atol=1e-10)
        y, x = np.mgrid[:128, :128]
        image = (0.85 + 0.03 * np.sin(x / 13) + 0.02 * np.cos(y / 17)).astype(np.float32)
        gpu = core.retrieve_density(image, p)
        cpu = core.retrieve_density(image, replace(p, device="cpu"))
        self.assertEqual(gpu.device, "cuda")  # A CPU fallback is not a GPU test.
        peak = float(np.max(np.abs(cpu.thickness_m)))
        np.testing.assert_allclose(gpu.thickness_m, cpu.thickness_m,
                                   rtol=2e-3, atol=peak * 2e-4)
        np.testing.assert_allclose(gpu.exit_phase_rad, cpu.exit_phase_rad,
                                   rtol=2e-3, atol=np.max(np.abs(cpu.exit_phase_rad)) * 2e-4)
        np.testing.assert_allclose(gpu.exit_transmission, cpu.exit_transmission,
                                   rtol=2e-3, atol=2e-4)


@unittest.skipUnless(importlib.util.find_spec("PyQt6"), "PyQt6 not installed")
class RetrievalWindowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PyQt6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def window(self):
        self.assertIsNotNone(importlib.util.find_spec("density_retrieval_window"),
                             "The density retrieval window is missing")
        module = importlib.import_module("density_retrieval_window")
        win = module.DensityRetrievalWindow()
        self.addCleanup(self.close_widget, win)
        return win

    def close_widget(self, widget):
        from PyQt6 import sip
        from PyQt6.QtCore import QCoreApplication, QEvent
        if sip.isdeleted(widget):
            return
        widget.close()
        if hasattr(widget, "is_busy"):
            self.wait_for(lambda: not widget.is_busy, timeout=10)
        if not sip.isdeleted(widget):
            widget.deleteLater()
        # Destroy Qt-owned plots while their Python wrappers are still alive.
        # Letting test-local main viewers fall into cyclic GC can run named
        # ViewBox destruction callbacks midway through constructing another view.
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        self.app.processEvents()

    def wait_for(self, predicate, timeout=5):
        from PyQt6.QtTest import QTest
        deadline = time.monotonic() + timeout
        while not predicate() and time.monotonic() < deadline:
            QTest.qWait(10)
        self.assertTrue(predicate(), "Qt operation did not finish before timeout")

    def set_roi(self, win, x, y, width, height):
        self.assertIsNotNone(getattr(win, "roi_group", None),
                             "The Processing ROI controls are missing")
        win.roi_group.setChecked(True)
        win.roi_group.width_input.setValue(width)
        win.roi_group.height_input.setValue(height)
        win.roi_group.x_input.setValue(x)
        win.roi_group.y_input.setValue(y)

    def test_queued_mouse_readout_is_ignored_after_close(self):
        from PyQt6.QtCore import QCoreApplication, QEvent, QPointF
        win = self.window()
        win.chk_auto.setChecked(False)
        win.set_frame(np.ones((24, 32)), "Closing frame")
        win.close()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        try:
            win._mouse_moved(QPointF(1, 1))
        except RuntimeError as exc:
            self.fail(f"A queued mouse callback accessed a deleted window: {exc}")

    def test_output_selector_reuses_result_and_preserves_roi_readout(self):
        from PyQt6.QtCore import QPointF
        from PyQt6.QtTest import QTest
        win = self.window()
        self.assertIsNotNone(getattr(win, "output_input", None), "Output selector is missing")
        win.set_frame(np.full((48, 64), 0.8), "Output frame", pixel_size_um=0.5)
        self.set_roi(win, 13, 9, 20, 16)
        win.show()
        win.run_retrieval()
        self.wait_for(lambda: not win.is_busy and win.retrieved_map is not None)
        result, revision = win._result, win._revision
        outputs = (
            ("areal_density", result.thickness_m * 9.8e5, "mg/cm²", "Areal Density"),
            ("thickness", result.thickness_m * 1e6, "µm", "Thickness"),
            ("exit_phase", result.exit_phase_rad, "rad", "Exit Phase"),
            ("exit_transmission", result.exit_transmission, "I/I₀", "Transmission"),
        )
        self.assertEqual(win.output_input.currentData(), "areal_density")
        with patch("density_retrieval_window.retrieve_density",
                   side_effect=AssertionError("Changing the display reran retrieval")):
            for key, expected, unit, title in outputs:
                with self.subTest(output=key):
                    index = win.output_input.findData(key)
                    self.assertGreaterEqual(index, 0)
                    win.output_input.setCurrentIndex(index)
                    QTest.qWait(200)
                    self.assertIs(win._result, result)
                    self.assertEqual(win._revision, revision)
                    self.assertFalse(win.is_busy)
                    np.testing.assert_allclose(win.retrieved_map, expected)
                    np.testing.assert_array_equal(win.img2.image, win.retrieved_map)
                    self.assertIn(title, win.p2.titleLabel.text)
                    origin = win.img2.transform().map(QPointF(0, 0))
                    self.assertAlmostEqual(origin.x(), 13 * 0.0005)
                    self.assertAlmostEqual(origin.y(), 9 * 0.0005)
                    win._mouse_moved(win.img1.mapToScene(QPointF(17.5, 12.5)))
                    self.assertIn(f"Retrieved: {win.retrieved_map[3, 4]:.4f} {unit}",
                                  win.statusBar().currentMessage())

    def test_output_can_change_while_retrieval_is_running(self):
        core = core_module(self)
        win = self.window()
        self.assertIsNotNone(getattr(win, "output_input", None), "Output selector is missing")
        win.chk_auto.setChecked(False)
        win.set_frame(np.full((24, 32), 0.8), "Busy output")
        entered, release = threading.Event(), threading.Event()
        def controlled_retrieval(image, params, cancelled):
            entered.set()
            release.wait(5)
            return core.retrieve_density(image, params, cancelled=cancelled)
        with patch("density_retrieval_window.retrieve_density", controlled_retrieval):
            try:
                win.run_retrieval()
                self.wait_for(entered.is_set)
                revision = win._revision
                win.output_input.setCurrentIndex(win.output_input.findData("exit_transmission"))
                self.assertEqual(win._revision, revision)
            finally:
                release.set()
            self.wait_for(lambda: not win.is_busy and win.retrieved_map is not None)
        np.testing.assert_allclose(win.retrieved_map, 0.8, rtol=2e-6)
        self.assertIn("Transmission", win.p2.titleLabel.text)

    def test_roi_is_cropped_before_paganin_and_input_stays_full(self):
        core = core_module(self)
        win = self.window()
        win.chk_auto.setChecked(False)
        # Invalid unused borders must not reach the numerical solver.
        image = np.full((48, 64), np.nan, np.float32)
        image[9:25, 13:33] = 0.8
        win.set_frame(image, "ROI frame", pixel_size_um=0.5, source_key="A")
        self.set_roi(win, 13, 9, 20, 16)
        win.run_retrieval()
        self.wait_for(lambda: not win.is_busy and win.retrieved_map is not None)
        expected = core.retrieve_density(np.full((16, 20), 0.8, np.float32),
                                         win.parameters())
        np.testing.assert_allclose(win.retrieved_map, expected.thickness_m * 9.8e5)
        self.assertEqual(win.retrieved_map.shape, (16, 20))
        np.testing.assert_array_equal(win.img1.image, image)
        self.assertEqual(win._result_bounds, (13, 9, 20, 16))

    @unittest.skipUnless(importlib.util.find_spec("hotopy") and
                         importlib.util.find_spec("torch"), "HoToPy not installed")
    def test_roi_is_cropped_before_nonlinear_solver(self):
        win = self.window()
        win.chk_auto.setChecked(False)
        win.nonlin_action.trigger()
        win.device_input.setCurrentText("cpu")
        win.rr2_input.setValue(0.2)
        win.max_iter_input.setValue(2)
        image = np.full((64, 80), np.nan, np.float32)
        image[8:28, 10:34] = 1
        win.set_frame(image, "Nonlinear ROI", pixel_size_um=2)
        self.set_roi(win, 10, 8, 24, 20)
        win.run_retrieval()
        self.wait_for(lambda: not win.is_busy and win.retrieved_map is not None, timeout=30)
        self.assertEqual(win.retrieved_map.shape, (20, 24))
        np.testing.assert_allclose(win.retrieved_map, 0, atol=1e-4)

    def test_roi_box_and_pixel_fields_stay_synchronized(self):
        win = self.window()
        win.chk_auto.setChecked(False)
        win.set_frame(np.ones((48, 64)), "Frame", pixel_size_um=0.5)
        self.set_roi(win, 3, 4, 12, 10)
        self.assertEqual(tuple(win.roi_group.rect.pos()), (3, 4))
        self.assertEqual(tuple(win.roi_group.rect.size()), (12, 10))
        win.roi_group.rect.setPos((7, 9))
        win.roi_group.rect.setSize((15, 11))
        self.assertEqual((win.roi_group.x_input.value(), win.roi_group.y_input.value(),
                          win.roi_group.width_input.value(), win.roi_group.height_input.value()),
                         (7, 9, 15, 11))
        win.roi_group.x_input.setValue(4)
        win.roi_group.width_input.setValue(8)
        self.assertEqual(tuple(win.roi_group.rect.pos()), (4, 9))
        self.assertEqual(tuple(win.roi_group.rect.size()), (8, 11))
        # Calibration changes physical coordinates, not selected pixel bounds.
        win.pixel_input.setValue(1.2)
        self.assertEqual(tuple(win.roi_group.rect.pos()), (4, 9))
        self.assertEqual(tuple(win.roi_group.rect.size()), (8, 11))
        win.roi_group.x_input.setValue(63)
        win.roi_group.y_input.setValue(47)
        self.assertEqual((win.roi_group.width_input.value(), win.roi_group.height_input.value()), (1, 1))

    def test_roi_result_uses_original_coordinates_and_hover_indices(self):
        from PyQt6.QtCore import QPointF
        from PyQt6.QtTest import QTest
        win = self.window()
        win.chk_auto.setChecked(False)
        win.set_frame(np.full((48, 64), 0.8), "Frame", pixel_size_um=0.5)
        self.set_roi(win, 13, 9, 20, 16)
        win.show()
        win.run_retrieval()
        self.wait_for(lambda: not win.is_busy and win.retrieved_map is not None)
        origin = win.img2.transform().map(QPointF(0, 0))
        self.assertAlmostEqual(origin.x(), 13 * 0.0005)
        self.assertAlmostEqual(origin.y(), 9 * 0.0005)
        QTest.qWait(20)
        win._mouse_moved(win.img1.mapToScene(QPointF(17.5, 12.5)))
        self.assertIn(f"Retrieved: {win.retrieved_map[3, 4]:.4f}",
                      win.statusBar().currentMessage())
        win._mouse_moved(win.img1.mapToScene(QPointF(1.5, 1.5)))
        self.assertNotIn("Retrieved:", win.statusBar().currentMessage())

    def test_roi_preserved_between_frames_and_reset_for_new_source_or_size(self):
        win = self.window()
        win.chk_auto.setChecked(False)
        image = np.ones((48, 64))
        win.set_frame(image, "A1", source_key="A")
        self.set_roi(win, 5, 6, 12, 10)
        win.set_frame(image, "A2", source_key="A")
        self.assertTrue(win.roi_group.isChecked())
        self.assertEqual((win.roi_group.x_input.value(), win.roi_group.y_input.value(),
                          win.roi_group.width_input.value(), win.roi_group.height_input.value()),
                         (5, 6, 12, 10))
        win.set_frame(image, "B1", source_key="B")
        self.assertFalse(win.roi_group.isChecked())
        self.set_roi(win, 5, 6, 12, 10)
        win.set_frame(np.ones((20, 30)), "B resized", source_key="B")
        self.assertFalse(win.roi_group.isChecked())
        self.assertEqual((win.roi_group.width_input.value(), win.roi_group.height_input.value()), (30, 20))

    def test_full_image_button_restores_full_processing(self):
        win = self.window()
        win.chk_auto.setChecked(False)
        win.set_frame(np.full((48, 64), 0.8), "Frame")
        self.set_roi(win, 5, 6, 12, 10)
        win.roi_group.full_btn.click()
        self.assertFalse(win.roi_group.isChecked())
        win.run_retrieval()
        self.wait_for(lambda: not win.is_busy and win.retrieved_map is not None)
        self.assertEqual(win.retrieved_map.shape, (48, 64))
        self.assertEqual(win._result_bounds, (0, 0, 64, 48))

    def test_roi_change_during_retrieval_discards_old_crop(self):
        core = core_module(self)
        win = self.window()
        win.chk_auto.setChecked(False)
        win.set_frame(np.full((48, 64), 0.8), "Frame")
        self.set_roi(win, 2, 3, 24, 20)
        entered, release = threading.Event(), threading.Event()

        def controlled_retrieval(image, params, cancelled):
            result = core.retrieve_density(image, params)
            entered.set()
            release.wait(5)
            return result

        with patch("density_retrieval_window.retrieve_density", controlled_retrieval):
            win.run_retrieval()
            self.wait_for(entered.is_set)
            self.set_roi(win, 5, 6, 12, 10)
            win.run_retrieval()
            self.assertIsNone(win.retrieved_map)
            release.set()
            self.wait_for(lambda: not win.is_busy and win.retrieved_map is not None)
        self.assertEqual(win.retrieved_map.shape, (10, 12))
        self.assertEqual(win._result_bounds, (5, 6, 12, 10))

    def test_draw_roi_box_on_uncropped_input(self):
        from PyQt6.QtCore import QPointF, Qt
        from PyQt6.QtTest import QTest
        win = self.window()
        win.chk_auto.setChecked(False)
        win.set_frame(np.ones((120, 160)), "Draw frame", pixel_size_um=1)
        self.set_roi(win, 0, 0, 160, 120)
        win.show()
        QTest.qWait(50)
        win.roi_group.draw_btn.click()
        graphics_view = win.p1.scene().views()[0]
        start = graphics_view.mapFromScene(win.img1.mapToScene(QPointF(20, 25)))
        end = graphics_view.mapFromScene(win.img1.mapToScene(QPointF(100, 85)))
        viewport = graphics_view.viewport()
        QTest.mouseMove(viewport, start)
        QTest.qWait(20)  # Let the scene register the new hover/drag target.
        QTest.mousePress(viewport, Qt.MouseButton.LeftButton, pos=start)
        QTest.mouseMove(viewport, (start + end) / 2, delay=20)
        QTest.mouseMove(viewport, end, delay=20)
        QTest.mouseRelease(viewport, Qt.MouseButton.LeftButton, pos=end)
        self.assertFalse(win.roi_group.draw_btn.isChecked())
        for actual, expected in zip(
                (win.roi_group.x_input.value(), win.roi_group.y_input.value(),
                 win.roi_group.width_input.value(), win.roi_group.height_input.value()),
                (20, 25, 80, 60)):
            self.assertLessEqual(abs(actual - expected), 2)
        self.assertEqual(win.img1.image.shape, (120, 160))

    def test_source_is_a_private_copy_and_manual_retrieval_works(self):
        win = self.window()
        win.chk_auto.setChecked(False)
        image = np.full((24, 32), 0.8, np.float32)
        win.set_frame(image, "Frame 1", pixel_size_um=0.5, source_key="recording")
        image[:] = 0.1
        np.testing.assert_array_equal(win._frame, np.full((24, 32), 0.8, np.float32))
        self.assertEqual(win.pixel_input.value(), 0.5)
        win.run_retrieval()
        self.wait_for(lambda: win.retrieved_map is not None and not win.is_busy)
        self.assertEqual(win.retrieved_map.shape, image.shape)
        self.assertTrue(np.all(win.retrieved_map > 0))
        np.testing.assert_array_equal(win.img1.image, win._frame)

    @unittest.skipUnless(importlib.util.find_spec("hotopy") and
                         importlib.util.find_spec("torch"), "HoToPy not installed")
    def test_cuda_retrieval_runs_in_worker_with_responsive_qt(self):
        import torch
        from PyQt6.QtCore import QTimer
        if not torch.cuda.is_available():
            self.skipTest("CUDA runtime not available")
        win = self.window()
        win.chk_auto.setChecked(False)
        win.nonlin_action.trigger()
        win.device_input.setCurrentText("cuda")
        win.pixel_input.setValue(2.0)
        win.rr2_input.setValue(0.2)
        win.max_iter_input.setValue(5)
        y, x = np.mgrid[:128, :128]
        image = (0.85 + 0.03 * np.sin(x / 13) + 0.02 * np.cos(y / 17)).astype(np.float32)
        win.set_frame(image, "CUDA validation", pixel_size_um=2.0)
        self.set_roi(win, 20, 24, 64, 48)
        ticks_during_work = []
        heartbeat = QTimer(win)
        heartbeat.setInterval(1)
        heartbeat.timeout.connect(lambda: ticks_during_work.append(win.is_busy))
        heartbeat.start()
        win.run_retrieval()
        self.wait_for(lambda: not win.is_busy and win.retrieved_map is not None, timeout=30)
        heartbeat.stop()
        self.assertEqual(win._result.device, "cuda")
        self.assertEqual(win.retrieved_map.shape, (48, 64))
        self.assertTrue(any(ticks_during_work), "Qt did not process events during CUDA work")
        self.assertTrue(np.isfinite(win.retrieved_map).all())

    def test_pixel_calibration_resets_for_a_new_uncalibrated_recording(self):
        win = self.window()
        win.chk_auto.setChecked(False)
        image = np.ones((24, 32))
        win.set_frame(image, "A", pixel_size_um=0.65, source_key="A")
        win.pixel_input.setValue(0.8)
        win.set_frame(image, "A frame 2", pixel_size_um=0.65, source_key="A")
        self.assertEqual(win.pixel_input.value(), 0.8)
        win.clear_frame()
        win.set_frame(image, "B", source_key="B")
        self.assertEqual(win.pixel_input.value(), 0.274)

    def test_new_requests_discard_stale_results_without_blocking_qt(self):
        core = core_module(self)
        win = self.window()
        win.chk_auto.setChecked(False)
        entered = threading.Event()
        release = threading.Event()

        def controlled_retrieval(image, params, cancelled):
            entered.set()
            release.wait(5)
            return core.retrieve_density(image, params, cancelled=cancelled)

        with patch("density_retrieval_window.retrieve_density", controlled_retrieval):
            win.set_frame(np.full((24, 32), 0.9), "Old frame")
            win.run_retrieval()
            self.wait_for(entered.is_set)
            # This UI interaction occurs while calculation is blocked in a worker.
            win.set_frame(np.full((24, 32), 0.7), "New frame")
            win.run_retrieval()
            self.assertIsNone(win.retrieved_map)
            release.set()
            self.wait_for(lambda: not win.is_busy and win.retrieved_map is not None)
        expected = core.retrieve_density(np.full((24, 32), 0.7, np.float32),
                                         win.parameters())
        np.testing.assert_allclose(win.retrieved_map,
                                   expected.thickness_m * 9.8e5)

    def test_result_that_finishes_after_a_frame_change_is_discarded(self):
        core = core_module(self)
        win = self.window()
        win.chk_auto.setChecked(False)
        entered, release = threading.Event(), threading.Event()

        def completed_before_cancellation(image, params, cancelled):
            # Represent a result that passed its final cancellation check just
            # before the GUI changed frames. Revision checking must still reject it.
            result = core.retrieve_density(image, params)
            entered.set()
            release.wait(5)
            return result

        with patch("density_retrieval_window.retrieve_density", completed_before_cancellation):
            win.set_frame(np.full((24, 32), 0.9), "Old frame")
            win.run_retrieval()
            self.wait_for(entered.is_set)
            win.set_frame(np.full((24, 32), 0.7), "New frame")
            release.set()
            self.wait_for(lambda: not win.is_busy)
        self.assertIsNone(win.retrieved_map)

    def test_auto_retrieval_uses_latest_frame(self):
        core = core_module(self)
        win = self.window()
        win.set_frame(np.full((24, 32), 0.9), "Old frame")
        win.set_frame(np.full((24, 32), 0.7), "New frame")
        self.wait_for(lambda: not win.is_busy and win.retrieved_map is not None)
        expected = core.retrieve_density(np.full((24, 32), 0.7, np.float32),
                                         win.parameters())
        np.testing.assert_allclose(win.retrieved_map, expected.thickness_m * 9.8e5)

    def test_main_viewer_close_waits_asynchronously_for_worker(self):
        from lucid_viewer import LucidViewer
        core = core_module(self)
        with patch.object(LucidViewer, "_restore_settings"):
            viewer = LucidViewer()
        # Keep all saves isolated, including cleanup after this test.
        saved = []
        viewer._save_settings = lambda: saved.append(True)
        self.addCleanup(self.close_widget, viewer)
        viewer.imview.setImage(np.full((24, 32), 0.8))
        viewer._open_density_retrieval()
        win = viewer._density_retrieval_window
        win.chk_auto.setChecked(False)
        entered, release = threading.Event(), threading.Event()

        def controlled_retrieval(image, params, cancelled):
            entered.set()
            release.wait(5)
            return core.retrieve_density(image, params, cancelled=cancelled)

        with patch("density_retrieval_window.retrieve_density", controlled_retrieval):
            win.run_retrieval()
            self.wait_for(entered.is_set)
            start = time.monotonic()
            viewer.close()
            self.assertLess(time.monotonic() - start, 0.25)
            self.assertFalse(saved)
            release.set()
            self.wait_for(lambda: bool(saved))
        self.assertIsNone(viewer._density_retrieval_window)

    def test_close_while_busy_is_deferred_without_waiting(self):
        core = core_module(self)
        win = self.window()
        win.chk_auto.setChecked(False)
        entered = threading.Event()
        release = threading.Event()
        closed = []

        def controlled_retrieval(image, params, cancelled):
            entered.set()
            release.wait(5)
            return core.retrieve_density(image, params, cancelled=cancelled)

        win.closed.connect(lambda: closed.append(True))
        with patch("density_retrieval_window.retrieve_density", controlled_retrieval):
            win.set_frame(np.ones((24, 32)), "Frame 1")
            win.run_retrieval()
            self.wait_for(entered.is_set)
            start = time.monotonic()
            win.close()
            self.assertLess(time.monotonic() - start, 0.25)
            self.assertFalse(closed)
            release.set()
            self.wait_for(lambda: bool(closed))
        self.assertFalse(win.is_busy)

    def test_tools_action_uses_corrected_image_and_follows_frame_changes(self):
        from lucid_viewer import LucidViewer
        from PyQt6.QtCore import QSettings
        # Isolate settings even if cleanup happens after the patch exits.
        with tempfile.TemporaryDirectory() as settings_dir, \
             patch("lucid_viewer.QSettings", return_value=QSettings(
                 os.path.join(settings_dir, "viewer.ini"), QSettings.Format.IniFormat)):
            viewer = LucidViewer()
            viewer._save_settings = lambda: None
            self.addCleanup(self.close_widget, viewer)
            viewer._path = "synthetic.raw"
            viewer._reader = [np.full((24, 32), 6, np.uint16),
                              np.full((24, 32), 4, np.uint16)]
            viewer._dark_field = np.full((24, 32), 2, np.float32)
            viewer._white_field = np.full((24, 32), 10, np.float32)
            viewer.frame_slider.setRange(0, 1)
            viewer._show_frame(0)
            tools = next(a.menu() for a in viewer.menuBar().actions()
                         if a.text().replace("&", "") == "Tools")
            action = next((a for a in tools.actions()
                           if "Density Retrieval" in a.text()), None)
            self.assertIsNotNone(action, "Tools menu has no Density Retrieval action")
            action.trigger()
            win = viewer._density_retrieval_window
            win.chk_auto.setChecked(False)
            np.testing.assert_allclose(win._frame, 0.5)
            viewer._show_frame(1)
            np.testing.assert_allclose(win._frame, 0.25)
            viewer.close()


if __name__ == "__main__":
    unittest.main()

