"""Launch regressions run in a fresh process using the selected Python env."""
from pathlib import Path
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(importlib.util.find_spec("PyQt6"), "PyQt6 not installed")
class ViewerRuntimeTests(unittest.TestCase):
    def test_viewer_imports_in_a_fresh_process(self):
        result = subprocess.run(
            [sys.executable, "-B", "-c", "import lucid_viewer; print('Viewer import OK')"],
            cwd=Path(__file__).resolve().parent, capture_output=True, text=True,
            timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(importlib.util.find_spec("torch") and
                         importlib.util.find_spec("hotopy"), "Nonlinear dependencies not installed")
    def test_pca_libraries_then_nonlinear_tv_in_fresh_process(self):
        script = """
import lucid_viewer
import numpy as np
app = lucid_viewer.QApplication([])
# Field correction exercises LAPACK before the user opens Density Retrieval.
np.linalg.svd(np.eye(4))
np.linalg.lstsq(np.eye(4), np.ones(4), rcond=None)
print("NumPy linear algebra ready", flush=True)
from density_retrieval import RetrievalParameters, retrieve_density
parameters = RetrievalParameters(solver="nonlinear", device="cpu",
                                 pixel_size_um=10, max_iter=2)
result = retrieve_density(np.ones((32, 32), np.float32), parameters)
np.testing.assert_allclose(result.thickness_m, 0, atol=1e-10)
print("PCA then nonlinear TV OK", flush=True)
"""
        result = subprocess.run(
            [sys.executable, "-B", "-X", "faulthandler", "-c", script],
            cwd=Path(__file__).resolve().parent,
            env=dict(os.environ, QT_QPA_PLATFORM="offscreen"),
            capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PCA then nonlinear TV OK", result.stdout)

    def test_cold_launch_without_console_keeps_blas_and_warning_streams_working(self):
        script = """
import sys, warnings
original_stdout = sys.stdout
sys.stdout = sys.stderr = sys.stdin = None
import lucid_viewer
import numpy as np
warnings.warn("No-console diagnostic test")
lucid_viewer._dbg("No console attached")
np.linalg.svd(np.eye(4))
np.linalg.lstsq(np.eye(4), np.ones(4), rcond=None)
app = lucid_viewer.QApplication([])
window = lucid_viewer.LucidViewer()
window.show()
app.processEvents()
window.close()
original_stdout.write("Cold no-console launch OK")
"""
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", PYTHONNOUSERSITE="1")
        environment.pop("CONDA_DLL_SEARCH_MODIFICATION_ENABLE", None)
        if sys.platform == "win32":
            environment["PATH"] = os.path.join(os.environ["SystemRoot"], "System32")
        with tempfile.TemporaryDirectory() as directory:
            # Isolate registry settings as in the ordinary launch regression.
            script = script.replace('window = lucid_viewer.LucidViewer()',
                'from PyQt6.QtCore import QSettings\nlucid_viewer.QSettings = lambda *a: QSettings(sys.argv[1], QSettings.Format.IniFormat)\nwindow = lucid_viewer.LucidViewer()')
            result = subprocess.run([sys.executable, "-B", "-X", "faulthandler", "-c", script,
                                     str(Path(directory) / "viewer.ini")],
                cwd=Path(__file__).resolve().parent, env=environment,
                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Cold no-console launch OK", result.stdout)

    def test_viewer_window_starts_and_closes_with_isolated_settings(self):
        script = """
import sys
import lucid_viewer
from PyQt6.QtCore import QSettings, QTimer
app = lucid_viewer.QApplication([])
lucid_viewer.QSettings = lambda *args: QSettings(sys.argv[1], QSettings.Format.IniFormat)
viewer = lucid_viewer.LucidViewer()
viewer.show()
QTimer.singleShot(200, viewer.close)
app.exec()
print("Viewer window launch OK")
"""
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable, "-B", "-c", script, str(Path(directory) / "viewer.ini")],
                cwd=Path(__file__).resolve().parent, env=environment,
                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Viewer window launch OK", result.stdout)


if __name__ == "__main__":
    unittest.main()

