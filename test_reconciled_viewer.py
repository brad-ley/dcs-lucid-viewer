"""Integration contracts retained when combining master with the measurement branch."""
import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from viewer_runtime import prepare_qt_runtime
prepare_qt_runtime()
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import numpy as np
import pyqtgraph as pg
from PyQt6.QtCore import QCoreApplication,QEvent,QPointF,QSettings
from PyQt6.QtTest import QTest
import lucid_viewer as viewer


class _Reader:
    def __init__(self,frames):
        self.frames=frames
        self.h,self.w=frames[0].shape
        self.indices=[]
    def __len__(self): return len(self.frames)
    def __getitem__(self,index):
        self.indices.append(index)
        return self.frames[index]
    def close(self): pass


class ReconciledViewerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app=viewer.QApplication.instance() or viewer.QApplication([])

    def setUp(self):
        self.directory=tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root=Path(self.directory.name)
        self.settings=QSettings(str(self.root/'viewer.ini'),QSettings.Format.IniFormat)
        self.settings_patch=patch.object(viewer,'QSettings',lambda *a:self.settings)
        self.settings_patch.start();self.addCleanup(self.settings_patch.stop)
        self.win=viewer.LucidViewer();self.addCleanup(self.dispose,self.win)
        self.win._path=str(self.root/'recording.raw')
        self.win._reader=_Reader([np.ones((200,300),np.uint16)])
        self.win.w_spin.setValue(300);self.win.h_spin.setValue(200)
        self.win._show_frame(0)
        self.win.show();QTest.qWait(30)

    def dispose(self,window):
        from PyQt6 import sip
        if not sip.isdeleted(window):
            window.close();window.deleteLater()
        QCoreApplication.sendPostedEvents(None,QEvent.Type.DeferredDelete)
        self.app.processEvents()

    def test_manual_scale_updates_zooming_scale_bar_without_stacking(self):
        win=self.win;view=win.imview.getView()
        view.setAspectLocked(False)
        view.setXRange(0,200,padding=0)
        win._spatial_scale.apply(100,50)
        bar=win._scale_bar_ref[0]
        self.assertIsNotNone(bar,'Applying our calibration must update master\'s scale bar')
        self.assertAlmostEqual(bar.size,40)
        self.assertIn('20 µm',bar.text.toPlainText())
        for width in (20,100,200,50,20):
            view.setXRange(0,width,padding=0)
        QTest.qWait(20)
        self.assertAlmostEqual(win._scale_bar_ref[0].size,4)
        bars=[item for item in view.scene().items() if isinstance(item,pg.ScaleBar)]
        self.assertEqual(len(bars),1,'Zoom must replace or reuse a bar without orphaning old bars')
        win._spatial_scale.use_recording_scale()
        self.assertIsNone(win._scale_bar_ref[0])
        self.assertFalse([i for i in view.scene().items() if isinstance(i,pg.ScaleBar)])

    def test_fft_keeps_master_colormap_label_and_our_active_scale(self):
        win=self.win;win.cmap_combo.setCurrentText('viridis')
        win._spatial_scale.apply(100,50)
        with patch.object(viewer._FFTDialog,'exec',lambda self:0):win._open_fft_dialog()
        fft=win.findChildren(viewer._FFTDialog)[-1]
        self.assertAlmostEqual(fft._pixel_size_um,.5)
        expected=win.imview.getHistogramWidget().gradient.colorMap().getLookupTable(nPts=256)
        np.testing.assert_array_equal(fft._src_view.getHistogramWidget().gradient.colorMap().getLookupTable(nPts=256),expected)
        self.assertEqual(len(fft._src_view.getHistogramWidget().gradient.ticks),3)
        self.assertIsNotNone(fft._scale_bar_ref[0])
        fft._measure_chk.setChecked(True)
        fft._measure_line.setPos(10,20)
        self.assertIn('µm',fft._measure_label.toPlainText())
        self.assertTrue(fft._measure_label.textItem.font().bold())

    def test_density_views_follow_main_colormap_and_have_physical_scale_bars(self):
        win=self.win;win._spatial_scale.apply(100,50)
        win.cmap_combo.setCurrentText('viridis');win._open_density_retrieval()
        density=win._density_retrieval_window;density.chk_auto.setChecked(False)
        expected=win.imview.getHistogramWidget().gradient.colorMap().getLookupTable(nPts=256)
        for lut in (density.lut1,density.lut2):
            np.testing.assert_array_equal(lut.gradient.colorMap().getLookupTable(nPts=256),expected)
            self.assertEqual(len(lut.gradient.ticks),3)
        density.p1.getViewBox().setXRange(0,.2,padding=0)
        QTest.qWait(20)
        bars=[i for i in density.p1.scene().items() if isinstance(i,pg.ScaleBar)]
        self.assertEqual(len(bars),2)
        self.assertTrue(all('µm' in b.text.toPlainText() for b in bars))
        win.cmap_combo.setCurrentText('grey')
        expected=win.imview.getHistogramWidget().gradient.colorMap().getLookupTable(nPts=256)
        np.testing.assert_array_equal(density.lut2.gradient.colorMap().getLookupTable(nPts=256),expected)

    def test_camera_specific_pca_search_and_manual_scale_coexist(self):
        for camera,tag,prefix,pitch in (('orcafire','camera_bus','pca_background',4.6),
                                        ('lucid','save_format','white_field',2.74)):
            with self.subTest(camera=camera):
                sidecar={tag:'test','width':300,'height':200,'pixel_format':'Mono16',
                         'objective_magnification':10}
                (self.root/'recording.json').write_text(json.dumps(sidecar),encoding='utf-8')
                self.win._apply_hints(self.win._path)
                self.assertEqual(self.win._pca_field_keywords(),(prefix,))
                self.assertAlmostEqual(self.win._spatial_scale.pitch_um,pitch/10)
                folder=self.root/(prefix+'_2026-10-21_10-00-00');folder.mkdir(exist_ok=True)
                (folder/'field.json').write_text(json.dumps({'width':300,'height':200}),encoding='utf-8')
                np.zeros((2,200,300),np.uint16).tofile(folder/'field.raw')
                selected=self.win._find_field_folder(self.win._pca_field_keywords(),min_frames=2)
                self.assertEqual(os.path.normpath(selected),os.path.normpath(str(folder)))
        self.win._spatial_scale.apply(100,50)
        self.win._apply_hints(self.win._path)
        self.assertAlmostEqual(self.win._spatial_scale.pitch_um,.5)

    def test_cached_pca_restores_controls_and_autolevels_from_reload_path(self):
        win=self.win
        source=np.linspace(10,30,60000,dtype=np.float32).reshape(200,300)
        win._reader.frames[0]=source
        win._cache.clear();win._show_frame(0)
        folder=self.root/'pca_background';folder.mkdir()
        (folder/'field.json').write_text(json.dumps({'width':300,'height':200,'pixel_format':'Mono16'}),encoding='utf-8')
        np.zeros((2,200,300),np.uint16).tofile(folder/'field.raw')
        np.savez(win._pca_cache_path(str(folder),200,300),mean=np.full(60000,20,np.float32),
                 components=np.zeros((1,60000),np.float32),explained=np.array([1.]))
        win._stored_field_refs['white_pca_folder']=str(folder)
        win.white_chk.blockSignals(True);win.white_chk.setChecked(True);win.white_chk.blockSignals(False)
        win.white_combo.blockSignals(True);win.white_combo.setCurrentIndex(1);win.white_combo.blockSignals(False)
        win._pca_settings_btn.hide();win._pca_n_lbl.hide();win.pca_n_spin.hide()
        win._reload_fields()
        self.assertEqual(win._white_mode,'pca')
        self.assertIsNone(win._pca_worker)
        self.assertTrue(win._pca_settings_btn.isVisible())
        self.assertTrue(win.pca_n_spin.isVisible())
        self.assertFalse(win._corr_progress.isVisible())
        np.testing.assert_allclose(win.imview.image,source/20,rtol=2e-6)
        np.testing.assert_allclose(win.imview.getLevels(),(.53,1.47),atol=1e-4)

    def test_sampled_pca_temporal_weights_retain_original_frame_indices(self):
        reader=_Reader([np.full((2,3),i,np.uint16) for i in range(501)])
        worker=viewer._PcaTemporalWeightWorker(np.ones((1,6),np.float32),np.zeros(6),reader)
        result=[];errors=[]
        worker.done.connect(lambda coeffs,indices:result.append((coeffs,indices)))
        worker.error.connect(errors.append)
        worker.run()
        self.assertFalse(errors)
        self.assertEqual(len(result),1)
        coeffs,indices=result[0]
        self.assertEqual(len(indices),250)
        self.assertEqual((int(indices[0]),int(indices[-1])),(0,500))
        self.assertEqual(reader.indices,list(indices))
        np.testing.assert_array_equal(coeffs[0],indices*6)


if __name__=='__main__':unittest.main()
