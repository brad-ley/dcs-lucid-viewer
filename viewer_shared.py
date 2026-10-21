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


def get_colormap(name):
    """Resolve a colormap by name, falling back to matplotlib's registry for
    names (e.g. 'inferno') not bundled with pyqtgraph itself. Returns None if
    neither lookup succeeds."""
    try:
        return pg.colormap.get(name)
    except Exception:
        try:
            return pg.colormap.get(name, source='matplotlib')
        except Exception:
            return None


def apply_colormap_to_histogram(histogram, cm):
    """Match a LUT/gradient editor to the main viewer with three color stops."""
    if cm is None:
        return
    gradient = histogram.gradient
    gradient.setColorMap(cm)
    colors = [cm.mapToQColor(p) for p in (0.0, 0.5, 1.0)]
    for tick in list(gradient.ticks):
        gradient.removeTick(tick, finish=False)
    for pos, color in zip((0.0, 0.5, 1.0), colors):
        gradient.addTick(pos, color, finish=False)
    gradient.updateGradient()


def apply_colormap_to_view(imview, cm):
    """Use the same colormap and compact gradient editor on every ImageView."""
    if cm is None:
        return
    imview.setColorMap(cm)
    apply_colormap_to_histogram(imview.getHistogramWidget().item, cm)


# "Nice" round scale-bar lengths, in micrometers, to choose from so the bar
# never reads something awkward like "37 um".
_SCALE_BAR_STEPS_UM = [1, 2, 5, 10, 20, 50, 100, 200, 500,
                       1000, 2000, 5000, 10000, 20000, 50000]


def _nice_scale_bar_length_um(view_width_um):
    """Largest step at or under ~22% of the visible width, so the bar reads
    cleanly without dominating the image."""
    target = view_width_um * 0.22
    chosen = _SCALE_BAR_STEPS_UM[0]
    for step in _SCALE_BAR_STEPS_UM:
        if step <= target:
            chosen = step
        else:
            break
    return chosen


def _update_scale_bar(view_box, bar_ref, pixel_size_um):
    """Keep one bottom-right bar sized to the currently visible field.

    pixel_size_um means µm per view coordinate: source views use µm/pixel;
    retrieval plots, whose coordinates are already mm, use 1000 µm/mm.
    Reuse the bar through zoom/pan so old scene items cannot accumulate.
    """
    (x0, x1), _ = view_box.viewRange()
    visible_width = abs(x1 - x0)
    valid = (pixel_size_um is not None and np.isfinite(pixel_size_um) and
             pixel_size_um > 0 and np.isfinite(visible_width) and visible_width > 0)
    bar = bar_ref[0]
    if not valid:
        if bar is not None:
            try:
                view_box.sigRangeChanged.disconnect(bar.updateBar)
            except (TypeError, RuntimeError):
                pass
            scene = bar.scene()
            # ScaleBar.setParentItem reanchors unless its offset is disabled.
            bar.offset = None
            bar.setParentItem(None)
            if scene is not None:
                scene.removeItem(bar)
            bar.deleteLater()
            bar_ref[0] = None
        return
    length_um = _nice_scale_bar_length_um(visible_width * pixel_size_um)
    length_view = length_um / pixel_size_um
    label = f'{length_um/1000:g} mm' if length_um >= 1000 else f'{length_um:g} µm'
    if bar is None:
        bar = pg.ScaleBar(size=length_view, suffix='m', offset=(-20, -20))
        bar.setParentItem(view_box)
        bar.anchor((1, 1), (1, 1), offset=(-20, -20))
        bar.setZValue(1000)
        bar_ref[0] = bar
    else:
        bar.size = length_view
    bar.text.setText(label)
    bar.updateBar()


def _connect_scale_bar_autoscale(view_box, bar_ref, pixel_size_getter):
    """Keep a scale bar sized to the visible field as the user zooms/pans.
    `pixel_size_getter` is called fresh on every range change (rather than
    passing a fixed value) since the current recording/calibration can
    change without the view itself re-emitting a range change."""
    view_box.sigRangeChanged.connect(
        lambda *_a: _update_scale_bar(view_box, bar_ref, pixel_size_getter()))




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
QPushButton:checked { background: #205a7a; border: 1px solid #4fc3f7; color: #eaf6ff; }
QComboBox {
    background: #3c3f41; border: 1px solid #555; border-radius: 3px; padding: 2px 4px; }
QMenuBar { background: #2b2b2b; }
QMenuBar::item:selected { background: #3c3f41; }
QMenu { background: #2b2b2b; border: 1px solid #555; }
QMenu::item:selected { background: #3c3f41; }
QStatusBar { color: #aaaaaa; font-size: 11px; }
QSplitter::handle { background: #444; }
QMessageBox { background: #2b2b2b; }
QHeaderView::section {
    background: #3c3f41; color: #dddddd; border: 1px solid #555; padding: 4px; }
QTableWidget {
    background: #2b2b2b; color: #dddddd; gridline-color: #444;
    selection-background-color: #3c5a7a; selection-color: #ffffff; }
"""
