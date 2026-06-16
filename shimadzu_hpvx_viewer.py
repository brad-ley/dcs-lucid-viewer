"""
Shimadzu HPV-X series frame viewer (PyQt6 + pyqtgraph).

Frame dimensions are not assumed — supply them via the Format panel.
Run shimadzu_probe.py first to confirm header structure and dimensions.
"""

import sys
import os
import struct
import numpy as np

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout,
    QPushButton, QLabel, QFileDialog, QComboBox,
    QGroupBox, QGridLayout, QSpinBox,
    QSplitter, QMessageBox, QCheckBox,
)
from PyQt6.QtCore import Qt, QSettings
from PyQt6.QtGui import QAction, QKeySequence, QShortcut

import pyqtgraph as pg
from viewer_shared import COLORMAPS, _build_pixel_mask, ViewerMixin, _DARK_STYLE_BASE

# ── Shimadzu tag constants ────────────────────────────────────────────────────
TAG_REC_SPEED  = b'\x30\x30\x07\x30'
TAG_EXPOSURE   = b'\x30\x30\x08\x30'
TAG_TIMESTAMP  = b'\x40\x40\x0e\x40'
TAG_IMAGE_DATA = b'\xa0\xa0\x01\xa0'
TAG_SKIP       = 14   # bytes from tag start to payload

# ── File parsing ──────────────────────────────────────────────────────────────
def _image_data_len(raw: bytes, tag_offset: int) -> int:
    """
    bytes[6:10] in the section header stores a *sample count* (uint32-LE),
    not a byte count.  For int16 pixel data multiply by 2.
    Falls back to end-of-file if the result would exceed remaining bytes.
    """
    data_start = tag_offset + TAG_SKIP
    remaining  = len(raw) - data_start
    try:
        samples = struct.unpack_from('<I', raw, tag_offset + 6)[0]
    except struct.error:
        return remaining
    as_bytes = samples * 2
    return as_bytes if 0 < as_bytes <= remaining else remaining


def parse_dat(raw: bytes, frame_w: int, frame_h: int, bit_shift: int = 0):
    """
    Decode image frames from raw file bytes.

    Returns (frames ndarray [N, H, W] uint16, meta dict, diag dict).
    """
    meta = {}
    diag = {}

    def _ascii(tag, n=8):
        off = raw.find(tag)
        return raw[off + TAG_SKIP : off + TAG_SKIP + n].decode('ascii', errors='replace').strip() if off >= 0 else '—'

    meta['recording_speed'] = _ascii(TAG_REC_SPEED)
    meta['exposure_time']   = _ascii(TAG_EXPOSURE)

    off_ts = raw.find(TAG_TIMESTAMP)
    if off_ts >= 0:
        b = raw[off_ts + TAG_SKIP : off_ts + TAG_SKIP + 16]
        yr = int.from_bytes(b[0:2],  'little')
        mo = int.from_bytes(b[2:4],  'little')
        dy = int.from_bytes(b[6:8],  'little')
        hr = int.from_bytes(b[8:10], 'little')
        mn = int.from_bytes(b[10:12],'little')
        sc = int.from_bytes(b[12:14],'little')
        meta['timestamp'] = f'{yr}-{mo:02d}-{dy:02d}  {hr:02d}:{mn:02d}:{sc:02d}'
    else:
        meta['timestamp'] = '—'

    off_img = raw.find(TAG_IMAGE_DATA)
    if off_img < 0:
        raise ValueError('Image data tag not found — is this a Shimadzu HPV-X .dat file?')

    data_start  = off_img + TAG_SKIP
    data_len    = _image_data_len(raw, off_img)
    frame_bytes = frame_w * frame_h * 2
    n_frames    = data_len // frame_bytes

    diag.update(data_len=data_len, frame_bytes=frame_bytes, n_frames=n_frames)

    if n_frames == 0:
        raise ValueError(
            f'No complete frames fit in {data_len:,} bytes with '
            f'{frame_w}×{frame_h} px ({frame_bytes:,} B/frame).\n'
            'Check frame dimensions.'
        )

    raw_data = raw[data_start : data_start + n_frames * frame_bytes]
    flat     = np.frombuffer(raw_data, dtype=np.int16).astype(np.int32)
    frames   = flat.reshape(n_frames, frame_h, frame_w) >> bit_shift
    frames   = frames[:, ::-1, :]   # flip rows to match viewer orientation
    return frames.astype(np.uint16), meta, diag


# ── Main window ───────────────────────────────────────────────────────────────
class ShimadzuViewer(ViewerMixin, QMainWindow):
    SETTINGS_ORG = 'ShimadzuViewer'
    SETTINGS_APP = 'HPVXViewer'

    def __init__(self):
        super().__init__()
        self._settings = QSettings(self.SETTINGS_ORG, self.SETTINGS_APP)
        self._raw       = None
        self._path      = None
        self.frames     = None
        self.n_frames   = 0
        self._roi_user_set = False
        self._sat_val   = 65535

        self.setWindowTitle('Shimadzu HPV-X Viewer')
        self.resize(1280, 800)
        self._build_ui()
        self._connect()
        self.setStyleSheet(_DARK_STYLE)
        self._restore_settings()

    # ── UI construction ───────────────────────────────────────────────────────
    def _build_ui(self):
        open_act = QAction('&Open .dat…', self)
        open_act.setShortcut(QKeySequence.StandardKey.Open)
        open_act.triggered.connect(self.open_file)
        self.menuBar().addMenu('&File').addAction(open_act)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        self.setCentralWidget(splitter)

        # ── Sidebar ───────────────────────────────────────────────────────────
        sidebar = QWidget()
        sidebar.setFixedWidth(220)
        sb = QVBoxLayout(sidebar)
        sb.setContentsMargins(8, 8, 8, 8)
        sb.setSpacing(8)

        open_btn = QPushButton('Open .dat file…')
        open_btn.clicked.connect(self.open_file)
        sb.addWidget(open_btn)

        # Format
        fmt_box  = QGroupBox('Frame format')
        fmt_grid = QGridLayout(fmt_box)
        fmt_grid.setSpacing(4)

        fmt_grid.addWidget(QLabel('Width (px):'), 0, 0)
        self.w_spin = QSpinBox()
        self.w_spin.setRange(1, 8192)
        self.w_spin.setValue(628)
        fmt_grid.addWidget(self.w_spin, 0, 1)

        fmt_grid.addWidget(QLabel('Height (px):'), 1, 0)
        self.h_spin = QSpinBox()
        self.h_spin.setRange(1, 8192)
        self.h_spin.setValue(480)
        fmt_grid.addWidget(self.h_spin, 1, 1)

        fmt_grid.addWidget(QLabel('Bit shift:'), 2, 0)
        self.shift_spin = QSpinBox()
        self.shift_spin.setRange(0, 15)
        self.shift_spin.setValue(0)
        self.shift_spin.setToolTip(
            'Right-shift applied to raw int16 values.\n'
            '0 = no shift (HPV-X3 default)\n'
            '6 = HPV-X2 (10-bit in upper bits)'
        )
        fmt_grid.addWidget(self.shift_spin, 2, 1)

        self.reload_btn = QPushButton('Re-parse with these dims')
        self.reload_btn.setEnabled(False)
        fmt_grid.addWidget(self.reload_btn, 3, 0, 1, 2)

        self.diag_label = QLabel('')
        self.diag_label.setWordWrap(True)
        self.diag_label.setStyleSheet('color: #888; font-size: 10px;')
        fmt_grid.addWidget(self.diag_label, 4, 0, 1, 2)
        sb.addWidget(fmt_box)

        # Metadata
        meta_box  = QGroupBox('File metadata')
        meta_grid = QGridLayout(meta_box)
        meta_grid.setSpacing(4)

        self._meta_vals = []
        for row, key in enumerate(['File:', 'Frames:', 'Size:', 'Speed:', 'Exposure:', 'Recorded:']):
            lbl = QLabel(key)
            lbl.setStyleSheet('font-weight: bold;')
            meta_grid.addWidget(lbl, row, 0)
            v = QLabel('—')
            v.setWordWrap(True)
            self._meta_vals.append(v)
            meta_grid.addWidget(v, row, 1)
        sb.addWidget(meta_box)

        # Display
        disp_box  = QGroupBox('Display')
        disp_grid = QGridLayout(disp_box)
        disp_grid.setSpacing(4)

        disp_grid.addWidget(QLabel('Colormap:'), 0, 0)
        self.cmap_combo = QComboBox()
        self.cmap_combo.addItems(COLORMAPS)
        disp_grid.addWidget(self.cmap_combo, 0, 1)

        auto_btn = QPushButton('Auto levels')
        auto_btn.clicked.connect(self._auto_levels)
        disp_grid.addWidget(auto_btn, 1, 0, 1, 2)

        self.mask_chk = QCheckBox('Pixel mask')
        self.mask_chk.setChecked(True)
        self.mask_chk.setToolTip(
            'Red = saturated  |  Yellow = dead (zero)\n'
            'Threshold derived from bit-shift setting'
        )
        disp_grid.addWidget(self.mask_chk, 2, 0, 1, 2)
        sb.addWidget(disp_box)

        sb.addStretch()
        splitter.addWidget(sidebar)

        # ── pyqtgraph ImageView ───────────────────────────────────────────────
        self.imview = pg.ImageView()
        splitter.addWidget(self.imview)
        splitter.setStretchFactor(1, 1)

        self._mask_item = pg.ImageItem()
        self._mask_item.setZValue(10)
        self.imview.getView().addItem(self._mask_item)

        self.statusBar().showMessage(
            'Set frame Width x Height in the Format panel, then open a .dat file.'
        )

    def _connect(self):
        self.reload_btn.clicked.connect(self._reparse)
        self.cmap_combo.currentTextChanged.connect(self._apply_colormap)
        self.imview.sigTimeChanged.connect(self._on_frame_changed)
        self.imview.ui.roiBtn.clicked.connect(self._on_roi_btn_clicked)
        # Fire ROI computation only on mouse release, not on every drag pixel
        self.imview.roi.sigRegionChanged.disconnect(self.imview.roiChanged)
        self.imview.roi.sigRegionChangeFinished.connect(self._roi_changed)
        self.imview.roi.sigRegionChangeFinished.connect(self._on_roi_moved)
        self.mask_chk.toggled.connect(self._toggle_mask)

        QShortcut(QKeySequence(Qt.Key.Key_Left),  self, lambda: self._step(-1))
        QShortcut(QKeySequence(Qt.Key.Key_Right), self, lambda: self._step(+1))
        QShortcut(QKeySequence(Qt.Key.Key_Home),  self, lambda: self._go(0))
        QShortcut(QKeySequence(Qt.Key.Key_End),   self, lambda: self._go(self.n_frames - 1))

    # ── File loading ──────────────────────────────────────────────────────────
    def open_file(self):
        last = self._settings.value('last_path', '')
        start_dir = os.path.dirname(last) if last else ''
        path, _ = QFileDialog.getOpenFileName(
            self, 'Open Shimadzu HPV-X data file',
            start_dir, 'Shimadzu data (*.dat);;All files (*)'
        )
        if not path:
            return
        with open(path, 'rb') as fh:
            self._raw  = fh.read()
        self._path = path
        self._reparse()

    def _reparse(self):
        if self._raw is None:
            return
        fw    = self.w_spin.value()
        fh    = self.h_spin.value()
        shift = self.shift_spin.value()
        try:
            frames, meta, diag = parse_dat(self._raw, fw, fh, shift)
        except Exception as exc:
            self.statusBar().showMessage(f'Parse error: {exc}')
            QMessageBox.warning(self, 'Parse error', str(exc))
            return

        self._sat_val = (1 << (16 - shift)) - 1
        self.frames   = frames
        self.n_frames = frames.shape[0]

        for lbl, val in zip(self._meta_vals, [
            os.path.basename(self._path),
            str(self.n_frames),
            f'{fw} × {fh} px',
            meta.get('recording_speed', '—'),
            meta.get('exposure_time',   '—'),
            meta.get('timestamp',       '—'),
        ]):
            lbl.setText(val)

        self.diag_label.setText(
            f"Data: {diag['data_len']:,} B\n"
            f"Frame: {diag['frame_bytes']:,} B"
        )
        self.reload_btn.setEnabled(True)

        self.imview.setImage(frames, autoLevels=True, autoRange=False)
        self._apply_colormap(self.cmap_combo.currentText())
        self.statusBar().showMessage(
            f'{self.n_frames} frames  |  {fw}×{fh} px  |  '
            f'shift={shift}  |  {os.path.basename(self._path)}'
        )

    # ── Navigation ────────────────────────────────────────────────────────────
    def _on_frame_changed(self, ind, _time):
        if self._path:
            self.statusBar().showMessage(
                f'Frame {ind + 1} / {self.n_frames}  |  '
                f'{self.w_spin.value()}×{self.h_spin.value()} px  |  '
                f'{os.path.basename(self._path)}'
            )
        self._update_mask(ind)

    def _step(self, delta):
        if self.frames is None:
            return
        self._go(self.imview.currentIndex + delta)

    def _go(self, idx):
        if self.frames is None:
            return
        self.imview.setCurrentIndex(max(0, min(idx, self.n_frames - 1)))

    # ── Pixel mask ────────────────────────────────────────────────────────────
    def _toggle_mask(self, checked: bool):
        self._mask_item.setVisible(checked)
        if checked and self.frames is not None:
            self._update_mask(self.imview.currentIndex)

    def _update_mask(self, ind: int):
        if not self.mask_chk.isChecked() or self.frames is None:
            self._mask_item.setImage(None)
            return
        rgba = _build_pixel_mask(self.frames[ind], self._sat_val)
        self._mask_item.setImage(rgba, autoLevels=False)

    # ── Display ───────────────────────────────────────────────────────────────
    def _apply_colormap(self, name: str):
        try:
            cm = pg.colormap.get(name)
        except Exception:
            try:
                cm = pg.colormap.get(name, source='matplotlib')
            except Exception:
                return
        self.imview.setColorMap(cm)

    def _auto_levels(self):
        if self.frames is not None:
            self.imview.autoLevels()

    # ── Settings ──────────────────────────────────────────────────────────────
    def _save_settings(self):
        s = self._settings
        s.setValue('geometry',     self.saveGeometry())
        s.setValue('windowState',  self.saveState())
        if self._path:
            s.setValue('last_path', self._path)
        s.setValue('frame_width',  self.w_spin.value())
        s.setValue('frame_height', self.h_spin.value())
        s.setValue('bit_shift',    self.shift_spin.value())
        s.setValue('colormap',     self.cmap_combo.currentText())
        s.setValue('mask_checked', self.mask_chk.isChecked())

    def _restore_settings(self):
        s = self._settings
        geom = s.value('geometry')
        if geom:
            self.restoreGeometry(geom)
        state = s.value('windowState')
        if state:
            self.restoreState(state)
        self.w_spin.setValue(    s.value('frame_width',  628, type=int))
        self.h_spin.setValue(    s.value('frame_height', 480, type=int))
        self.shift_spin.setValue(s.value('bit_shift',      0, type=int))
        cmap = s.value('colormap', 'grey')
        idx  = self.cmap_combo.findText(cmap)
        if idx >= 0:
            self.cmap_combo.setCurrentIndex(idx)
        self.mask_chk.setChecked(s.value('mask_checked', True, type=bool))
        last = s.value('last_path', '')
        if last:
            self.statusBar().showMessage(f'Last file: {last}')

    def closeEvent(self, event):
        self._save_settings()
        super().closeEvent(event)


_DARK_STYLE = _DARK_STYLE_BASE

if __name__ == '__main__':
    app = QApplication(sys.argv)
    win = ShimadzuViewer()
    win.show()
    sys.exit(app.exec())
