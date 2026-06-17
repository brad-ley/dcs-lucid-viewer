"""
Shared constants, helpers, and a ViewerMixin used by both
shimadzu_hpvx_viewer.py and lucid_viewer.py.
"""

import numpy as np
from PyQt6.QtGui import QTransform
import pyqtgraph as pg

pg.setConfigOptions(imageAxisOrder='row-major')
pg.setConfigOption('background', '#1e1e1e')
pg.setConfigOption('foreground', '#cccccc')

# Spatial pixel budget for ROI mean-over-time computation.
# If the image plane exceeds this, a downsampled copy is used instead.
_ROI_MAX_PIXELS = 600 * 600

COLORMAPS = ['grey', 'inferno', 'hot', 'plasma', 'magma', 'viridis', 'CET-L1', 'CET-L4']


def _build_pixel_mask(frame: np.ndarray, sat_val: int) -> np.ndarray:
    """Return RGBA uint8 (H, W, 4): red = saturated, yellow = zero-valued dead."""
    rgba      = np.zeros((*frame.shape[:2], 4), dtype=np.uint8)
    sat       = frame >= sat_val
    dead      = (frame == 0) & ~sat
    rgba[sat,  0]                  = 255   # R
    rgba[sat,  3]                  = 160   # A
    rgba[dead, 0] = rgba[dead, 1]  = 255   # R+G = yellow
    rgba[dead, 3]                  = 160   # A
    return rgba


class ViewerMixin:
    """ROI auto-positioning and memory-safe ROI computation shared by both viewers."""

    def _on_roi_btn_clicked(self):
        if self.imview.ui.roiBtn.isChecked() and not self._roi_user_set:
            self._auto_position_roi()

    def _auto_position_roi(self):
        img = self.imview.image
        if img is None:
            return
        axes = self.imview.axes
        h = img.shape[axes['y']]
        w = img.shape[axes['x']]
        self.imview.roi.setPos([w / 3, h / 3])
        self.imview.roi.setSize([w / 3, h / 3])

    def _on_roi_moved(self):
        self._roi_user_set = True

    def _roi_changed(self):
        """Compute the ROI mean-over-time plot, downsampling first if the image
        plane is large enough to cause a memory spike."""
        if not self.imview.hasTimeAxis():
            return
        img = self.imview.image
        if img is None:
            return

        axes  = self.imview.axes
        h, w  = img.shape[axes['y']], img.shape[axes['x']]

        if h * w <= _ROI_MAX_PIXELS:
            self.imview.roiChanged()
            return

        factor = int(np.ceil(np.sqrt(h * w / _ROI_MAX_PIXELS)))
        ds_idx = [slice(None)] * img.ndim
        ds_idx[axes['y']] = slice(None, None, factor)
        ds_idx[axes['x']] = slice(None, None, factor)
        ds_idx = tuple(ds_idx)

        orig_image = self.imview.image
        orig_disp  = self.imview.imageDisp
        orig_tr    = self.imview.imageItem.transform()

        scale_tr = QTransform()
        scale_tr.scale(factor, factor)

        self.imview.image     = orig_image[ds_idx]
        self.imview.imageDisp = orig_disp[ds_idx] if orig_disp is not None else None
        self.imview.imageItem.setTransform(orig_tr * scale_tr)
        try:
            self.imview.roiChanged()
        finally:
            self.imview.image     = orig_image
            self.imview.imageDisp = orig_disp
            self.imview.imageItem.setTransform(orig_tr)


# Base dark stylesheet (both viewers).
# Viewers that add their own widgets (e.g. QSlider) append extra rules.
_DARK_STYLE_BASE = """
QMainWindow, QWidget { background: #2b2b2b; color: #dddddd; }
QGroupBox { border: 1px solid #555; border-radius: 4px; margin-top: 6px;
             font-weight: bold; padding-top: 4px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; }
QPushButton { background: #3c3f41; border: 1px solid #555; border-radius: 3px;
               padding: 4px 10px; }
QPushButton:hover   { background: #4c5052; }
QPushButton:pressed { background: #5c6062; }
QPushButton:disabled { color: #666; }
QComboBox, QSpinBox {
    background: #3c3f41; border: 1px solid #555; border-radius: 3px; padding: 2px 4px; }
QMenuBar { background: #2b2b2b; }
QMenuBar::item:selected { background: #3c3f41; }
QMenu { background: #2b2b2b; border: 1px solid #555; }
QMenu::item:selected { background: #3c3f41; }
QStatusBar { color: #aaaaaa; font-size: 11px; }
QSplitter::handle { background: #444; }
QMessageBox { background: #2b2b2b; }
"""
