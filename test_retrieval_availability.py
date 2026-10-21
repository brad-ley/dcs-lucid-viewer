"""Optional density dependencies must never prevent ordinary viewing or measurement."""
import builtins
import importlib
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
os.environ.setdefault('QT_QPA_PLATFORM','offscreen')
from viewer_runtime import prepare_qt_runtime
prepare_qt_runtime()
import numpy as np
from PyQt6.QtCore import QCoreApplication,QEvent,QPointF,QSettings
import lucid_viewer as viewer


class RetrievalAvailabilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app=viewer.QApplication.instance() or viewer.QApplication([])

    def setUp(self):
        self.directory=tempfile.TemporaryDirectory();self.addCleanup(self.directory.cleanup)
        settings=QSettings(str(Path(self.directory.name)/'settings.ini'),QSettings.Format.IniFormat)
        settings_patch=patch.object(viewer,'QSettings',lambda *a:settings)
        settings_patch.start();self.addCleanup(settings_patch.stop)
        self.win=viewer.LucidViewer();self.addCleanup(self.dispose)
        self.win.imview.setImage(np.ones((32,40),np.float32),autoLevels=False)
        self.win._measurements.set_image_shape((32,40))
        self.win.show()
        self.messages=[]
        warning=patch.object(viewer.QMessageBox,'warning',
                             lambda parent,title,message:self.messages.append((title,message)))
        warning.start();self.addCleanup(warning.stop)

    def dispose(self):
        self.win.close();self.win.deleteLater()
        QCoreApplication.sendPostedEvents(None,QEvent.Type.DeferredDelete)
        self.app.processEvents()

    def without(self,*names):
        original=importlib.util.find_spec
        return patch('importlib.util.find_spec',
                     side_effect=lambda name,*a,**k:None if name in names else original(name,*a,**k))

    def test_missing_core_dependencies_warn_before_opening_retrieval(self):
        with self.without('scipy','xraylib'):
            self.win._open_density_retrieval()
        self.assertIsNone(self.win._density_retrieval_window)
        self.assertTrue(self.win.isVisible())
        self.assertEqual(len(self.messages),1)
        self.assertIn('scipy',self.messages[0][1])
        self.assertIn('xraylib',self.messages[0][1])
        self.assertIn(sys.executable,self.messages[0][1])
        self.win._spatial_scale.apply(100,50)
        self.win.measure_btn.click()
        image=self.win.imview.imageItem
        self.win._measurements._line_dragged(image.mapToScene(QPointF(5,5)),
                                           image.mapToScene(QPointF(15,5)),True)
        self.assertIn('5 µm',self.win._measurements.lines[0].label.toPlainText())

    def test_basic_solver_stays_available_without_torch_or_hotopy(self):
        with self.without('torch','hotopy'):
            self.win._open_density_retrieval()
            window=self.win._density_retrieval_window
            self.assertIsNotNone(window)
            window.chk_auto.setChecked(False)
            self.assertFalse(self.messages)
            window.nonlin_action.trigger()
            self.assertTrue(window.paganin_action.isChecked())
            self.assertFalse(window.nonlin_action.isChecked())
            self.assertFalse(window.solver_group.isVisible())
            self.assertEqual(len(self.messages),1)
            self.assertIn('torch',self.messages[0][1])
            self.assertIn('hotopy',self.messages[0][1])
            self.assertFalse(window.is_busy)

    def test_import_dll_failure_warns_and_leaves_viewer_usable(self):
        original=builtins.__import__
        def broken_import(name,*args,**kwargs):
            if name=='density_retrieval_window':
                raise OSError('DLL load failed: missing procedure')
            return original(name,*args,**kwargs)
        with patch('builtins.__import__',side_effect=broken_import):
            try:
                self.win._open_density_retrieval()
            except Exception as exc:
                self.fail(f'Retrieval import escaped the Tools action: {exc}')
        self.assertIsNone(self.win._density_retrieval_window)
        self.assertEqual(len(self.messages),1)
        self.assertIn('missing procedure',self.messages[0][1])
        self.assertTrue(self.win.isVisible())

    def test_partial_window_initialization_failure_is_cleaned_up_and_retry_works(self):
        module=importlib.import_module('density_retrieval_window')
        with patch.object(module.DensityRetrievalWindow,'_build_ui',
                          side_effect=AttributeError('old runtime API unavailable')):
            try:
                self.win._open_density_retrieval()
            except Exception as exc:
                self.fail(f'Retrieval initialization escaped the Tools action: {exc}')
        QCoreApplication.sendPostedEvents(None,QEvent.Type.DeferredDelete)
        self.assertIsNone(self.win._density_retrieval_window)
        self.assertFalse(self.win.findChildren(module.DensityRetrievalWindow))
        self.assertEqual(len(self.messages),1)
        self.assertIn('old runtime API',self.messages[0][1])
        self.win._open_density_retrieval()
        self.assertIsNotNone(self.win._density_retrieval_window)
        self.win._density_retrieval_window.chk_auto.setChecked(False)

    def test_post_construction_failure_closes_window_and_retry_works(self):
        module=importlib.import_module('density_retrieval_window')
        with patch.object(module.DensityRetrievalWindow,'set_colormap',
                          side_effect=RuntimeError('incompatible display runtime')):
            self.win._open_density_retrieval()
        QCoreApplication.sendPostedEvents(None,QEvent.Type.DeferredDelete)
        self.assertIsNone(self.win._density_retrieval_window)
        self.assertFalse(self.win.findChildren(module.DensityRetrievalWindow))
        self.assertEqual(len(self.messages),1)
        self.assertIn('incompatible display runtime',self.messages[0][1])
        self.win._open_density_retrieval()
        self.assertIsNotNone(self.win._density_retrieval_window)
        self.win._density_retrieval_window.chk_auto.setChecked(False)

    def test_fresh_old_environment_never_imports_optional_retrieval_code(self):
        script=r'''
import importlib.abc,sys
class OldEnvironment(importlib.abc.MetaPathFinder):
    def find_spec(self,fullname,path=None,target=None):
        if fullname.split('.')[0] in ('scipy','xraylib','torch','hotopy'):
            raise ModuleNotFoundError('not installed: '+fullname,name=fullname)
sys.meta_path.insert(0,OldEnvironment())
import lucid_viewer
import numpy as np
from PyQt6.QtCore import QSettings,QCoreApplication,QEvent
app=lucid_viewer.QApplication([])
lucid_viewer.QSettings=lambda *a:QSettings(sys.argv[1],QSettings.Format.IniFormat)
win=lucid_viewer.LucidViewer()
win.imview.setImage(np.ones((8,8)),autoLevels=False)
messages=[]
lucid_viewer.QMessageBox.warning=lambda parent,title,message:messages.append(message)
lucid_viewer.QMessageBox.critical=lambda parent,title,message:messages.append(message)
win._open_density_retrieval()
assert win._density_retrieval_window is None
assert len(messages)==1 and 'xraylib' in messages[0] and 'scipy' in messages[0]
assert 'density_retrieval_window' not in sys.modules
assert 'density_retrieval' not in sys.modules
win.close();win.deleteLater()
QCoreApplication.sendPostedEvents(None,QEvent.Type.DeferredDelete)
print('Old environment: warning only; retrieval modules never loaded')
'''
        result=subprocess.run([sys.executable,'-B','-X','faulthandler','-c',script,
                               str(Path(self.directory.name)/'old.ini')],
                              cwd=Path(__file__).resolve().parent,
                              env=dict(os.environ,QT_QPA_PLATFORM='offscreen'),
                              capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('retrieval modules never loaded',result.stdout)


if __name__=='__main__':unittest.main()
