"""
LucidLabs ATX245 mono image viewer (PyQt6 + pyqtgraph).

Supports pixel formats: Mono8, Mono10, Mono10p, Mono10Packed,
                        Mono12, Mono12p, Mono12Packed, Mono16
File formats: .raw (binary), .tiff / .tif (via tifffile), .mp4 / .avi (via opencv)

Frame dimensions default to ATX245 full-sensor (5328 × 4608).
For .raw files, supply Width × Height and pixel format so the parser can
compute bytes-per-frame and derive the frame count automatically.
"""

import sys
import os
import re
import json
import time
import numpy as np

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QPushButton, QLabel, QFileDialog, QComboBox,
    QGroupBox, QGridLayout, QSpinBox, QFormLayout,
    QSplitter, QMessageBox, QSlider, QCheckBox,
    QDialog, QProgressBar, QDialogButtonBox,
)
from PyQt6.QtCore import Qt, QSettings, QUrl, QThread, QTimer, pyqtSignal
from PyQt6.QtGui import QAction, QKeySequence, QShortcut, QIcon
from PyQt6.QtGui import QDesktopServices, QPainter, QColor, QPen

import pyqtgraph as pg
from viewer_shared import COLORMAPS, _build_pixel_mask, ViewerMixin, _DARK_STYLE_BASE

_FORMAT_BITS = {
    'Mono8': 8, 'Mono10': 10, 'Mono10p': 10, 'Mono10Packed': 10,
    'Mono12': 12, 'Mono12p': 12, 'Mono12Packed': 12, 'Mono16': 16,
}

PIXEL_FORMATS = [
    'Mono8',
    'Mono10',
    'Mono10p',
    'Mono10Packed',
    'Mono12',
    'Mono12p',
    'Mono12Packed',
    'Mono16',
]

ATX245_W = 5328
ATX245_H = 4608


# ── Pixel-format unpacking ────────────────────────────────────────────────────

def frame_byte_count(fmt: str, w: int, h: int) -> float:
    """Return bytes per frame (may be fractional for packed formats)."""
    n = w * h
    return {
        'Mono8':        n,
        'Mono10':       n * 2,
        'Mono10p':      n * 10 / 8,   # PFNC: 4 pixels in 5 bytes (true 10-bit pack)
        'Mono10Packed': n * 3  / 2,   # Lucid native: 2 pixels in 3 bytes
        'Mono12':       n * 2,
        'Mono12p':      n * 12 / 8,   # 12 bits per pixel, packed (3 bytes per 2 px)
        'Mono12Packed': n * 12 / 8,   # 12 bits per pixel, packed (3 bytes per 2 px)
        'Mono16':       n * 2,
    }[fmt]


def unpack_frame(raw_bytes: bytes, fmt: str, w: int, h: int) -> np.ndarray:
    """
    Unpack a single frame's raw bytes into a uint16 [H, W] array.
    """
    n = w * h
    buf = np.frombuffer(raw_bytes, dtype=np.uint8)

    if fmt == 'Mono8':
        return buf.astype(np.uint16).reshape(h, w)

    if fmt in ('Mono10', 'Mono12', 'Mono16'):
        return np.frombuffer(raw_bytes, dtype='<u2').reshape(h, w).copy()

    if fmt == 'Mono12Packed':
        # GigE Vision MSB-aligned: 2 pixels in 3 bytes
        pad = (-len(buf)) % 3
        if pad:
            buf = np.concatenate([buf, np.zeros(pad, dtype=np.uint8)])
        chunks = buf[:len(buf) - len(buf) % 3].reshape(-1, 3)
        out = np.empty(n, dtype=np.uint16)
        out[0::2] = (chunks[:, 0].astype(np.uint16) << 4) | (chunks[:, 1] & 0x0F)
        out[1::2] = (chunks[:, 2].astype(np.uint16) << 4) | ((chunks[:, 1] & 0xF0) >> 4)
        return out.reshape(h, w)

    if fmt == 'Mono12p':
        # USB3 Vision/PFNC LSB-aligned: 2 pixels in 3 bytes
        pad = (-len(buf)) % 3
        if pad:
            buf = np.concatenate([buf, np.zeros(pad, dtype=np.uint8)])
        chunks = buf[:len(buf) - len(buf) % 3].reshape(-1, 3)
        out = np.empty(n, dtype=np.uint16)
        out[0::2] = chunks[:, 0].astype(np.uint16) | ((chunks[:, 1] & 0x0F).astype(np.uint16) << 8)
        out[1::2] = ((chunks[:, 1] & 0xF0).astype(np.uint16) >> 4) | (chunks[:, 2].astype(np.uint16) << 4)
        return out.reshape(h, w)

    if fmt == 'Mono10Packed':
        # Lucid Vision native layout: 2 pixels in 3 bytes
        # p0 = (byte[0] << 2) | (byte[1] & 0x03)
        # p1 = (byte[2] << 2) | (byte[1] >> 6)
        pad = (-len(buf)) % 3
        if pad:
            buf = np.concatenate([buf, np.zeros(pad, dtype=np.uint8)])
        chunks = buf[:len(buf) - len(buf) % 3].reshape(-1, 3)
        out = np.empty(n, dtype=np.uint16)
        out[0::2] = (chunks[:, 0].astype(np.uint16) << 2) | (chunks[:, 1].astype(np.uint16) & 0x03)
        out[1::2] = (chunks[:, 2].astype(np.uint16) << 2) | (chunks[:, 1].astype(np.uint16) >> 6)
        return out.reshape(h, w)

    if fmt == 'Mono10p':
        # GenICam LSB-aligned: 4 pixels per 5 bytes
        pad = (-len(buf)) % 5
        if pad:
            buf = np.concatenate([buf, np.zeros(pad, dtype=np.uint8)])
        chunks = buf[:len(buf) - len(buf) % 5].reshape(-1, 5)
        out = np.empty((len(chunks), 4), dtype=np.uint16)
        out[:, 0] =  chunks[:, 0].astype(np.uint16)        | ((chunks[:, 1] & 0x03).astype(np.uint16) << 8)
        out[:, 1] = (chunks[:, 1].astype(np.uint16) >> 2)  | ((chunks[:, 2] & 0x0F).astype(np.uint16) << 6)
        out[:, 2] = (chunks[:, 2].astype(np.uint16) >> 4)  | ((chunks[:, 3] & 0x3F).astype(np.uint16) << 4)
        out[:, 3] = (chunks[:, 3].astype(np.uint16) >> 6)  | ( chunks[:, 4].astype(np.uint16) << 2)
        return out.ravel()[:n].reshape(h, w)

    raise ValueError(f'Unknown pixel format: {fmt}')


# ── Frame readers ─────────────────────────────────────────────────────────────

class RawReader:
    """Lazy reader for a single .raw file containing concatenated frames."""

    def __init__(self, path: str, fmt: str, w: int, h: int):
        self.path  = path
        self.fmt   = fmt
        self.w     = w
        self.h     = h
        self._mmap = None
        bpf = frame_byte_count(fmt, w, h)
        if bpf != int(bpf):
            raise ValueError(
                f'{fmt} requires {bpf:.2f} bytes/frame — not an integer. '
                'Check dimensions.'
            )
        self.bpf      = int(bpf)
        file_size     = os.path.getsize(path)
        self.n_frames = file_size // self.bpf
        if self.n_frames == 0:
            raise ValueError(
                f'No complete frames fit in {file_size:,} bytes with '
                f'{w}×{h} px {fmt} ({self.bpf:,} B/frame).'
            )
        self._data = np.memmap(path, dtype=np.uint8, mode='r')

    def __len__(self):
        return self.n_frames

    def __getitem__(self, idx: int) -> np.ndarray:
        start = idx * self.bpf
        raw   = bytes(self._data[start : start + self.bpf])
        return unpack_frame(raw, self.fmt, self.w, self.h)

    @property
    def shape(self):
        return (self.n_frames, self.h, self.w)

    def close(self):
        del self._data


class TiffReader:
    """Lazy reader for multi-frame TIFF (via tifffile page API)."""

    def __init__(self, path: str):
        import tifffile
        self._tf     = tifffile.TiffFile(path)
        self._pages  = self._tf.pages

        # Use the first series for authoritative shape if available
        if self._tf.series:
            shape = self._tf.series[0].shape
        else:
            p = self._pages[0]
            shape = (len(self._pages), p.shape[0], p.shape[1])

        if len(shape) == 2:
            self.n_frames = 1
            self.h, self.w = shape
        else:
            self.n_frames = shape[0]
            self.h        = shape[-2]
            self.w        = shape[-1]

        # Series shape can report 1 frame even when there are many pages
        # (e.g. BigTIFF, some scientific formats). Page count is ground truth.
        n_pages = len(self._pages)
        if self.n_frames == 1 and n_pages > 1:
            self.n_frames = n_pages
        # Single-page 3D TIFF: all frames packed into one page
        elif self.n_frames == 1 and n_pages == 1:
            p_shape = self._pages[0].shape
            if len(p_shape) == 3:
                self.n_frames = p_shape[0]
                self.h, self.w = p_shape[1], p_shape[2]

    def __len__(self):
        return self.n_frames

    def __getitem__(self, idx: int) -> np.ndarray:
        if len(self._pages) == 1:
            # Single multi-dimensional page (e.g. 3D array packed into one page)
            data = self._pages[0].asarray()
            frame = data[idx] if data.ndim == 3 else data
        else:
            frame = self._pages[idx].asarray()
        if frame.dtype != np.uint16:
            frame = frame.astype(np.uint16)
        return frame

    @property
    def shape(self):
        return (self.n_frames, self.h, self.w)

    def close(self):
        self._tf.close()


class VideoReader:
    """Lazy reader for .mp4 / .avi (via opencv)."""

    def __init__(self, path: str):
        import cv2
        self._cv2     = cv2
        self._cap     = cv2.VideoCapture(path)
        if not self._cap.isOpened():
            raise IOError(
                f'OpenCV could not open {os.path.basename(path)}.\n'
                'Check that the file exists and the codec is installed.'
            )
        self.n_frames = int(self._cap.get(cv2.CAP_PROP_FRAME_COUNT))
        self.fps      = self._cap.get(cv2.CAP_PROP_FPS) or 0.0
        self.w        = int(self._cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        self.h        = int(self._cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        if self.n_frames <= 0 or self.w <= 0 or self.h <= 0:
            raise IOError(
                f'Could not read video properties from {os.path.basename(path)}.\n'
                f'Reported: {self.n_frames} frames, {self.w}×{self.h} px.'
            )
        self._cur_idx = -1

    def __len__(self):
        return self.n_frames

    def __getitem__(self, idx: int) -> np.ndarray:
        cv2 = self._cv2
        if idx != self._cur_idx + 1:
            self._cap.set(cv2.CAP_PROP_POS_FRAMES, idx)
        ret, frame = self._cap.read()
        if not ret:
            raise IndexError(f'Could not read frame {idx}')
        self._cur_idx = idx
        if frame.ndim == 3:
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        return frame.astype(np.uint16)

    @property
    def shape(self):
        return (self.n_frames, self.h, self.w)

    def close(self):
        self._cap.release()


# ── Sidecar / filename hints ──────────────────────────────────────────────────

def _load_sidecar(path: str):
    """
    Look for a JSON sidecar next to *path* and return (data_dict, status_str).
    Tries <path>.json first, then <stem>.json (strip data-file extension).
    Tries UTF-8-sig (handles BOM), UTF-16, and latin-1 as fallbacks.
    """
    candidates = [path + '.json']
    stem = os.path.splitext(path)[0]
    alt  = stem + '.json'
    if alt not in candidates:
        candidates.append(alt)
    # Also check for a metadata.json sibling in the same acquisition folder
    candidates.append(os.path.join(os.path.dirname(path), 'metadata.json'))

    for jspath in candidates:
        if not os.path.isfile(jspath):
            continue
        for enc in ('utf-8-sig', 'utf-16', 'latin-1'):
            try:
                with open(jspath, 'r', encoding=enc) as fh:
                    data = json.load(fh)
                return data, f'Sidecar loaded ({os.path.basename(jspath)}, {enc})'
            except (UnicodeDecodeError, UnicodeError):
                continue
            except Exception as exc:
                return {}, f'Sidecar parse error: {exc}'

    base = os.path.basename(path)
    return {}, f'No sidecar ({os.path.splitext(base)[0]}.json)'


def _load_timestamps(path: str):
    """
    Look for frame_data.csv (preferred) or timestamps.csv in the same directory
    as *path*.  Returns (timestamps, unit, gains, exposures, line_statuses) where
    timestamps is a 1-D float64 array of relative times in the best-fit unit,
    unit is a string ('µs', 'ms', 's', 'min'), gains and exposures are 1-D
    float64 arrays or None, and line_statuses is a 1-D int64 array or None.
    Returns (None, '', None, None, None) on failure.
    """
    dir_ = os.path.dirname(path)
    frame_data_path = os.path.join(dir_, 'frame_data.csv')
    csv_path = os.path.join(dir_, 'timestamps.csv')
    if os.path.isfile(frame_data_path):
        csv_path = frame_data_path
    elif not os.path.isfile(csv_path):
        return None, '', None, None, None
    try:
        import csv

        # Read all non-empty rows
        with open(csv_path, newline='', encoding='utf-8-sig') as fh:
            reader = csv.reader(fh)
            rows = [row for row in reader if any(cell.strip() for cell in row)]
            
        if not rows:
            return None, '', None, None, None

        # Check if the first row is a header row
        has_header = False
        header_row = []
        try:
            for cell in rows[0]:
                if cell.strip():
                    float(cell.strip())
        except ValueError:
            has_header = True
            header_row = [cell.strip().lower() for cell in rows[0]]
            
        # Group numeric values by column index
        num_cols = max(len(row) for row in rows)
        col_values = {i: [] for i in range(num_cols)}
        start_idx = 1 if has_header else 0
        
        for row in rows[start_idx:]:
            for i in range(num_cols):
                if i < len(row):
                    val_str = row[i].strip()
                    if val_str:
                        try:
                            col_values[i].append(float(val_str))
                        except ValueError:
                            pass
                            
        # Decide which column contains the timestamp
        time_col_idx = None
        
        # 1. Try to find a header match if we have headers
        if has_header:
            time_headers = ['timestamp', 'time', 'ts', 'ticks', 'system', 'device', 't']
            for idx, h_name in enumerate(header_row):
                if any(th in h_name for th in time_headers):
                    if len(col_values[idx]) >= 2:
                        time_col_idx = idx
                        break
                        
        # 2. If no header match or no header, analyze the data columns
        if time_col_idx is None:
            candidate_indices = [idx for idx in col_values if len(col_values[idx]) >= 2]
            if len(candidate_indices) > 1:
                # Filter out columns that look like a simple sequential frame index (step of exactly 1.0)
                non_index_candidates = []
                for idx in candidate_indices:
                    vals = np.array(col_values[idx], dtype=np.float64)
                    diffs = np.diff(vals)
                    if not np.allclose(diffs, 1.0):
                        non_index_candidates.append(idx)
                if non_index_candidates:
                    time_col_idx = non_index_candidates[0]
                else:
                    time_col_idx = candidate_indices[-1]
            elif candidate_indices:
                time_col_idx = candidate_indices[0]
                
        if time_col_idx is None or len(col_values[time_col_idx]) < 2:
            return None, '', None, None, None

        col_header = ""
        if has_header and time_col_idx < len(header_row):
            col_header = header_row[time_col_idx]

        # Extract gain, exposure, and line_status_all columns if present
        gains = None
        exposures = None
        line_statuses = None
        if has_header:
            for g_idx, h_name in enumerate(header_row):
                if 'gain' in h_name and g_idx != time_col_idx:
                    g_vals = col_values.get(g_idx, [])
                    if g_vals:
                        gains = np.array(g_vals, dtype=np.float64)
                    break
            for e_idx, h_name in enumerate(header_row):
                if 'exposure' in h_name and e_idx != time_col_idx:
                    e_vals = col_values.get(e_idx, [])
                    if e_vals:
                        exposures = np.array(e_vals, dtype=np.float64)
                        # Convert µs to ms if the header says _us and values are large
                        if '_us' in h_name or 'us' in h_name:
                            exposures = exposures / 1000.0  # store as ms
                    break
            for l_idx, h_name in enumerate(header_row):
                if 'line_status' in h_name and l_idx != time_col_idx:
                    # Re-parse from raw rows as binary strings (e.g. '00000101' → 5).
                    # The generic float() pass above would turn '00000101' into 101.
                    l_vals = []
                    for row in rows[start_idx:]:
                        if l_idx < len(row):
                            val_str = row[l_idx].strip()
                            if val_str:
                                try:
                                    l_vals.append(int(val_str, 2))
                                except ValueError:
                                    try:
                                        l_vals.append(int(val_str))
                                    except ValueError:
                                        pass
                    if l_vals:
                        line_statuses = np.array(l_vals, dtype=np.int64)
                    break

        raw_vals = np.array(col_values[time_col_idx], dtype=np.float64)

        # Apply pre-scaling if column header hints at the unit
        pre_scale = 1.0
        if "_ns" in col_header:
            pre_scale = 1e-9
        elif "_us" in col_header:
            pre_scale = 1e-6
        elif "_ms" in col_header:
            pre_scale = 1e-3

        raw_vals *= pre_scale
        ts = raw_vals - raw_vals[0]     # make relative to frame 0

        # Auto-detect raw unit based on magnitude of the first timestamp and frame intervals
        first_val = raw_vals[0]
        mean_diff = np.mean(np.diff(raw_vals)) if len(raw_vals) >= 2 else 0
        max_val = raw_vals[-1]

        scale = 1.0
        if first_val > 1e8:
            # Absolute Epoch timestamps
            if first_val > 1e17:    # Epoch nanoseconds (~1.7e18)
                scale = 1e-9
            elif first_val > 1e14:  # Epoch microseconds (~1.7e15)
                scale = 1e-6
            elif first_val > 1e11:  # Epoch milliseconds (~1.7e12)
                scale = 1e-3
            else:                   # Epoch seconds (~1.7e9)
                scale = 1.0
        else:
            # Relative timestamps
            if mean_diff > 5e7:
                scale = 1e-9  # Nanoseconds
            elif mean_diff > 5e4:
                scale = 1e-6  # Microseconds
            elif mean_diff > 50:
                scale = 1e-3  # Milliseconds
            elif mean_diff > 1.5:
                if max_val > 100:
                    scale = 1e-3  # Milliseconds

        ts *= scale
        span = ts[-1] - ts[0]

        # Choose display unit from span (now in seconds)
        if span < 0.5:
            return ts * 1e3, 'ms', gains, exposures, line_statuses
        if span < 120:
            return ts,       's',  gains, exposures, line_statuses
        return ts / 60,      'min', gains, exposures, line_statuses
    except Exception:
        return None, '', None, None, None


def _guess_from_filename(name: str):
    """Try to extract pixel format from filename. Returns fmt or None."""
    for f in reversed(PIXEL_FORMATS):          # try longer names first
        if f.lower() in name.lower():
            return f
    return None


def _folder_timestamp(name: str) -> int:
    """Extract a compact date-time integer from a folder name for proximity comparison.

    Tries patterns in order:
      1. YYYYMMDD + HHMMSS  → e.g. 'exp_20240117_173400' → 20240117173400
      2. YYYYMMDD alone     → e.g. 'exp_20240117'         → 20240117000000
      3. Returns 0 if no recognisable date found.

    Using pattern matching (not raw digit concat) avoids camera-model numbers
    like 'ATX245' or '12bit' corrupting the comparison.
    """
    # 8-digit date followed immediately or after one separator by 6-digit time
    m = re.search(r'(\d{8})[_\-\s]?(\d{6})', name)
    if m:
        return int(m.group(1) + m.group(2))
    # 8-digit date alone (no time component)
    m = re.search(r'\d{8}', name)
    if m:
        return int(m.group(0)) * 1_000_000  # pad to same scale as full datetime
    return 0


# ── Field correction (shared math) ────────────────────────────────────────────

def _field_correct_float(frame, dark, white,
                         dark_gain=None, white_gain=None, data_gain=None):
    """
    Apply dark-field subtraction and/or white-field division and return the
    result as a float32 array — the physically meaningful x-ray transmission.

        dark + white :  (data - dark) / (white - dark)   → transmission (≈0..1)
        white only   :  data / white                     → transmission (≈0..1)
        dark only    :  data - dark                       → dark-subtracted counts

    No 12-bit re-mapping, shifting, or clipping is performed here — callers that
    need an integer encoding (e.g. TIFF export) map this float result to codes
    themselves and record the float min/max so the mapping is reversible.

    Returns None if neither a dark nor a white field is supplied.
    """
    if dark is None and white is None:
        return None
    f = frame.astype(np.float32)

    # Scale field frames to match the data gain if all gains are known.
    # Gains are in dB: linear_gain = 10^(dB/20) for voltage/pixel signal.
    if data_gain is not None:
        if dark is not None and dark_gain is not None:
            dark  = dark  * np.float32(10 ** ((data_gain - dark_gain)  / 20.0))
        if white is not None and white_gain is not None:
            white = white * np.float32(10 ** ((data_gain - white_gain) / 20.0))

    if dark is not None and white is not None:
        denom = white - dark
        valid = denom >= np.float32(1.0)
        safe  = np.where(valid, denom, np.float32(1.0))
        out = np.where(valid, (f - dark) / safe, np.float32(0.0))
    elif dark is not None:
        out = f - dark
    else:
        out = f / np.where(white >= np.float32(1.0), white, np.float32(1.0))
    return out.astype(np.float32)


def _compute_pca_from_frames(frames, n_components=20, progress_cb=None):
    """
    PCA on a list of (H, W) float32 frames using the frame-space covariance
    trick (K << N).  All heavy work is a small number of BLAS sgemm calls.
    Returns (mean, components, explained_variance_ratio).
      mean:       (H*W,) float32
      components: (n_out, H*W) float32
      explained:  (n_out,) float32
    """
    def _prog(msg):
        if progress_cb:
            progress_cb(msg)

    K = len(frames)
    if K < 2:
        raise ValueError('PCA requires at least 2 white-field frames.')
    H, W = frames[0].shape
    N = H * W
    n_components = min(n_components, K - 1)

    # Stack frames into one (K, N) array — single allocation, no per-frame copies
    _prog(f'PCA: stacking {K} frames…')
    X = np.empty((K, N), dtype=np.float32)
    for i, f in enumerate(frames):
        X[i] = f.ravel()

    # Mean and centering in-place (avoids a second 2 GB alloc)
    _prog('PCA: centering…')
    mu32 = X.mean(axis=0)          # (N,) float32, one BLAS call
    X   -= mu32                    # in-place: no extra copy

    # Frame-space covariance (K, K) — cost: K² × N, one sgemm
    _prog('PCA: computing frame covariance…')
    C = X.dot(X.T).astype(np.float64) / max(K - 1, 1)

    _prog('PCA: eigendecomposition…')
    eigenvalues, eigenvectors = np.linalg.eigh(C)  # ascending
    order        = np.argsort(eigenvalues)[::-1]
    eigenvalues  = eigenvalues[order]
    eigenvectors = eigenvectors[:, order]           # (K, K)

    # Pixel-space components — ONE sgemm: (n_comp, K) @ (K, N) → (n_comp, N)
    _prog(f'PCA: projecting {n_components} components…')
    evecs = np.ascontiguousarray(
        eigenvectors[:, :n_components].T.astype(np.float32))  # (n_comp, K)
    V = evecs.dot(X)                                           # (n_comp, N)

    # Normalise rows
    norms = np.linalg.norm(V, axis=1, keepdims=True)
    np.maximum(norms, 1e-10, out=norms)
    V /= norms
    components = V                                             # (n_comp, N) float32

    total_var = float(max(eigenvalues[eigenvalues > 0].sum(), 1e-12))
    explained = (eigenvalues[:n_components] / total_var).astype(np.float32)
    return mu32, components, explained


# ── TIFF export worker ────────────────────────────────────────────────────────

class TiffExportWorker(QThread):
    progress     = pyqtSignal(int, int)  # frames_done, total
    phase        = pyqtSignal(str)       # current phase label ('Analyzing'/'Exporting')
    export_done  = pyqtSignal(str)       # success message
    export_error = pyqtSignal(str)       # error message

    def __init__(self, out_path, reader, cache, n,
                 dark_field, dark_field_gain,
                 white_field, white_field_gain,
                 gains, desc, bigtiff, compression='None', pixel_bits=16,
                 timestamps=None, ts_unit='s', trigger_t0=None, imagej=False,
                 pca_mean=None, pca_components=None, pca_n_components=5):
        super().__init__()
        self._out_path         = out_path
        self._reader           = reader
        self._cache            = dict(cache)
        self._n                = n
        self._dark_field       = dark_field
        self._dark_field_gain  = dark_field_gain
        self._white_field      = white_field
        self._white_field_gain = white_field_gain
        self._gains            = gains
        self._desc             = desc
        self._bigtiff          = bigtiff
        self._compression      = compression
        self._pixel_bits       = pixel_bits
        self._timestamps       = timestamps
        self._ts_unit          = ts_unit
        self._trigger_t0       = trigger_t0
        self._imagej           = imagej
        self._pca_mean         = pca_mean
        self._pca_components   = pca_components
        self._pca_n_components = pca_n_components
        self._cancelled        = False

    def cancel(self):
        self._cancelled = True

    def _corrected_float(self, frame, data_gain):
        """Float32 x-ray transmission for a frame, or None if no correction."""
        if self._pca_mean is not None and self._pca_components is not None:
            mu   = self._pca_mean
            comp = self._pca_components
            n    = min(self._pca_n_components, comp.shape[0])
            f = frame.astype(np.float32)
            dark = self._dark_field
            if dark is not None and data_gain is not None and self._dark_field_gain is not None:
                dark = dark * np.float32(10 ** ((data_gain - self._dark_field_gain) / 20.0))
            if dark is not None:
                f = f - dark
            d_c        = f.ravel() - mu
            coeffs     = comp[:n] @ d_c
            bg         = (mu + comp[:n].T @ coeffs).reshape(frame.shape)
            bg         = np.where(bg >= np.float32(1.0), bg, np.float32(1.0))
            return (f / bg).astype(np.float32)
        return _field_correct_float(
            frame, self._dark_field, self._white_field,
            self._dark_field_gain, self._white_field_gain, data_gain,
        )

    def _correct(self, frame, data_gain):
        """
        Apply correction and map the float transmission to integer codes using
        THIS frame's own min/max (a per-frame conversion factor — no whole-stack
        pre-pass needed):

            code = (transmission - fmin) / (fmax - fmin) * code_max

        Returns (frame_to_write, fmin, fmax).  When no correction is active the
        raw frame is returned unchanged and (fmin, fmax) are None.
        """
        out = self._corrected_float(frame, data_gain)
        if out is None:
            return frame, None, None
        code_max = np.float32((1 << self._pixel_bits) - 1)
        fmin = float(out.min())
        fmax = float(out.max())
        if not np.isfinite(fmin) or not np.isfinite(fmax) or fmax <= fmin:
            # Flat frame: nothing to scale — store zeros and a unit range so the
            # decode formula stays well-defined (transmission ≈ fmin everywhere).
            return np.zeros(out.shape, dtype=np.uint16), fmin, fmin + 1.0
        span   = np.float32(fmax - fmin)
        mapped = (out - np.float32(fmin)) / span * code_max
        return np.clip(mapped, 0, code_max).astype(np.uint16), fmin, fmax

    _PER_FRAME_DECODE = (
        'per frame i: transmission = code / code_max * '
        '(transmission_max[i] - transmission_min[i]) + transmission_min[i]'
    )

    def _write_transmission_sidecar(self, tmins, tmaxs, code_max):
        """
        Write the per-frame transmission_min/transmission_max arrays to a JSON
        sidecar next to the exported file.  Used for ImageJ exports, whose strict
        TIFF metadata layout can't safely carry the arrays in-file.  The sidecar
        is named '<output-stem>_transmission.json'.
        """
        side = os.path.splitext(self._out_path)[0] + '_transmission.json'
        info = {
            'source':           os.path.basename(self._out_path),
            'transmission_min': [round(float(v), 8) for v in tmins],
            'transmission_max': [round(float(v), 8) for v in tmaxs],
            'code_max':         int(code_max),
            'decode':           self._PER_FRAME_DECODE,
        }
        with open(side, 'w', encoding='utf-8') as fh:
            json.dump(info, fh, indent=2)

    def run(self):
        try:
            import tifffile
            import warnings
            n = self._n
            cmp_str = self._compression.lower() if self._compression != 'None' else None
            write_kwargs = {}
            if cmp_str:
                write_kwargs['compression'] = cmp_str
                write_kwargs['predictor']   = 2
            # ImageJ mode requires contiguous=True, which is incompatible with compression
            imagej_write_kwargs = {}

            corr_active = (self._dark_field is not None or self._white_field is not None
                           or (self._pca_mean is not None and self._pca_components is not None))
            code_max    = int((1 << self._pixel_bits) - 1)
            self.phase.emit('Exporting')

            if self._imagej:
                # ImageJ mode: write frame-by-frame with contiguous=True so
                # tifffile lays out a contiguous series (required by imagej=True)
                # without loading the full stack into RAM first.
                # tifffile emits a harmless "truncating ImageJ file" warning when
                # writing multi-page ImageJ TIFFs incrementally — suppress it.
                #
                # ImageJ's TIFF reader is strict about its own metadata layout, so
                # we write the stack with no embedded extras (exactly the layout
                # ImageJ expects) and record the per-frame transmission_min/max in
                # a JSON sidecar next to the file instead.
                tmins = [0.0] * n
                tmaxs = [0.0] * n
                with warnings.catch_warnings():
                    warnings.filterwarnings('ignore', message='truncating ImageJ file',
                                            category=UserWarning)
                    with tifffile.TiffWriter(self._out_path, bigtiff=False, imagej=True) as tif:
                        for i in range(n):
                            if self._cancelled:
                                self.export_error.emit('Export cancelled.')
                                return
                            frame = self._cache[i] if i in self._cache else self._reader[i]
                            data_gain = (self._gains[i]
                                         if self._gains is not None and i < len(self._gains)
                                         else None)
                            out, fmin, fmax = self._correct(frame, data_gain)
                            if fmin is not None:
                                tmins[i] = fmin
                                tmaxs[i] = fmax
                            tif.write(out, contiguous=True, **imagej_write_kwargs)
                            self.progress.emit(i + 1, n)
                if corr_active:
                    self._write_transmission_sidecar(tmins, tmaxs, code_max)
            else:
                u = self._ts_unit
                with tifffile.TiffWriter(self._out_path, bigtiff=self._bigtiff) as tif:
                    for i in range(n):
                        if self._cancelled:
                            self.export_error.emit('Export cancelled.')
                            return
                        frame = self._cache[i] if i in self._cache else self._reader[i]
                        data_gain = (self._gains[i]
                                     if self._gains is not None and i < len(self._gains)
                                     else None)

                        out, fmin, fmax = self._correct(frame, data_gain)

                        # Build per-frame metadata dict with trigger-relative
                        # timestamp and this frame's own transmission mapping.
                        fm = {'frame_index': i}
                        ts = self._timestamps
                        if ts is not None and i < len(ts):
                            fm[f'timestamp_{u}'] = round(float(ts[i]), 6)
                            if self._trigger_t0 is not None:
                                fm[f't_trigger_{u}'] = round(float(ts[i] - self._trigger_t0), 6)
                        if fmin is not None:
                            fm['transmission_min'] = round(fmin, 8)
                            fm['transmission_max'] = round(fmax, 8)
                            fm['code_max']         = code_max

                        if i == 0 and self._desc:
                            try:
                                merged = json.loads(self._desc)
                                merged.update(fm)
                                # Record the per-frame decode convention once in
                                # the stack-level correction metadata on page 0.
                                if corr_active:
                                    corr = merged.get('correction')
                                    if not isinstance(corr, dict):
                                        corr = {}
                                    corr['code_max'] = code_max
                                    corr['decode']   = self._PER_FRAME_DECODE
                                    corr['note'] = ('transmission_min/transmission_max '
                                                    'are stored per page in each frame.')
                                    merged['correction'] = corr
                                desc = json.dumps(merged, indent=2)
                            except Exception:
                                desc = self._desc
                        else:
                            desc = json.dumps(fm) if fm else None

                        tif.write(out, description=desc, **write_kwargs)
                        self.progress.emit(i + 1, n)

            self.export_done.emit(
                f'Exported {n} frames → {os.path.basename(self._out_path)}')
        except Exception as exc:
            self.export_error.emit(str(exc))


class PcaComputeWorker(QThread):
    pca_done     = pyqtSignal(object, object, object)  # mean, components, explained
    pca_error    = pyqtSignal(str)
    pca_progress = pyqtSignal(str)                     # status message for UI

    def __init__(self, folder, fmt, w, h, n_components=20, parent=None):
        super().__init__(parent)
        self._folder       = folder
        self._fmt          = fmt
        self._w            = w
        self._h            = h
        self._n_components = n_components

    def run(self):
        try:
            frames = self._load_frames()
            if len(frames) < 2:
                self.pca_error.emit(f'Need ≥2 white-field frames, found {len(frames)}')
                return
            K = len(frames)
            H, W = frames[0].shape
            mem_gb = K * H * W * 4 / 1e9
            self.pca_progress.emit(f'Computing PCA ({K} frames, ~{mem_gb:.1f} GB)…')
            n_store = min(20, K - 1)
            mu, comp, expl = _compute_pca_from_frames(
                frames, n_store,
                progress_cb=lambda msg: self.pca_progress.emit(msg))
            self.pca_done.emit(mu, comp, expl)
        except Exception as exc:
            self.pca_error.emit(str(exc))

    def _load_frames(self):
        """Load every frame from folder as individual float32 arrays."""
        try:
            all_files = sorted(os.listdir(self._folder))
        except OSError:
            return []
        raw_files = [f for f in all_files if os.path.splitext(f)[1].lower() == '.raw']
        tif_files = [f for f in all_files
                     if os.path.splitext(f)[1].lower() in ('.tiff', '.tif')]
        files = raw_files if raw_files else tif_files
        if not files:
            return []
        frames = []
        n = len(files)
        for i, fname in enumerate(files):
            self.pca_progress.emit(f'Loading white-field frames ({i + 1}/{n})…')
            fpath = os.path.join(self._folder, fname)
            ext   = os.path.splitext(fname)[1].lower()
            try:
                if ext == '.raw':
                    sidecar, _ = _load_sidecar(fpath)
                    fmt = (sidecar.get('pixel_format') or sidecar.get('pixelformat')
                           or self._fmt)
                    w = int(sidecar.get('width',  self._w))
                    h = int(sidecar.get('height', self._h))
                    r = RawReader(fpath, fmt, w, h)
                    for j in range(len(r)):
                        frames.append(r[j].astype(np.float32))
                    r.close()
                elif ext in ('.tiff', '.tif'):
                    import tifffile
                    data = tifffile.imread(fpath)
                    if data.ndim == 2:
                        frames.append(data.astype(np.float32))
                    else:
                        for j in range(data.shape[0]):
                            frames.append(data[j].astype(np.float32))
            except Exception as e:
                print(f'[pca] error loading {fname}: {e}')
        return frames


TIFF_COMPRESSIONS = ['None', 'LZW', 'Deflate', 'ZSTD']


class _ExportSettingsDialog(QDialog):
    def __init__(self, current_compression, imagej=False, parent=None, stylesheet=''):
        super().__init__(parent)
        self.setWindowTitle('Export Settings')
        if stylesheet:
            self.setStyleSheet(stylesheet)
        layout = QVBoxLayout(self)
        layout.setSpacing(10)
        layout.setContentsMargins(14, 14, 14, 14)

        form = QFormLayout()
        form.setSpacing(8)
        self._cmp_combo = QComboBox()
        self._cmp_combo.addItems(TIFF_COMPRESSIONS)
        idx = self._cmp_combo.findText(current_compression, Qt.MatchFlag.MatchFixedString)
        if idx >= 0:
            self._cmp_combo.setCurrentIndex(idx)
        form.addRow('TIFF compression:', self._cmp_combo)

        self._imagej_chk = QCheckBox()
        self._imagej_chk.setChecked(imagej)
        self._imagej_chk.setToolTip(
            'Write as ImageJ-compatible TIFF (imagej=True).\n'
            'Allows the file to be opened directly in ImageJ/Fiji.\n\n'
            'Note: compression is disabled in this mode (contiguous layout required).\n'
            'Per-frame timestamp metadata is not written in this mode; field-correction\n'
            'conversion factors go to a <name>_transmission.json sidecar instead.'
        )
        form.addRow('ImageJ-compatible:', self._imagej_chk)
        layout.addLayout(form)

        note = QLabel('LZW / Deflate / ZSTD use predictor=2 (horizontal diff) for better ratios.\n'
                      'Compression is unavailable in ImageJ mode.')
        note.setWordWrap(True)
        note.setStyleSheet('color: #888; font-size: 10px;')
        layout.addWidget(note)

        self._imagej_chk.toggled.connect(self._on_imagej_toggled)
        self._on_imagej_toggled(imagej)

        btns = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok |
                                QDialogButtonBox.StandardButton.Cancel)
        btns.accepted.connect(self.accept)
        btns.rejected.connect(self.reject)
        layout.addWidget(btns)
        self.adjustSize()

    def _on_imagej_toggled(self, checked: bool):
        self._cmp_combo.setEnabled(not checked)
        if checked:
            self._cmp_combo.setCurrentIndex(0)  # reset to 'None'

    def selected_compression(self):
        return self._cmp_combo.currentText()

    def selected_imagej(self):
        return self._imagej_chk.isChecked()


class _TriggerConfigDialog(QDialog):
    def __init__(self, current_bit, parent=None, stylesheet=''):
        super().__init__(parent)
        self.setWindowTitle('Trigger Settings')
        if stylesheet:
            self.setStyleSheet(stylesheet)
        layout = QVBoxLayout(self)
        layout.setSpacing(10)
        layout.setContentsMargins(14, 14, 14, 14)

        form = QFormLayout()
        form.setSpacing(8)
        self._bit_spin = QSpinBox()
        self._bit_spin.setRange(0, 15)
        self._bit_spin.setValue(current_bit)
        self._bit_spin.setToolTip(
            'Zero-indexed bit position in line_status_all that goes high on event trigger.\n'
            'Example: bit 2 detects values where (line_status_all & 4) != 0.'
        )
        form.addRow('Trigger bit (0-indexed):', self._bit_spin)
        layout.addLayout(form)

        note = QLabel(
            'The trigger bit is the 0-indexed position in the line_status_all bitmask\n'
            'that goes high when an external event trigger is received.'
        )
        note.setWordWrap(True)
        note.setStyleSheet('color: #888; font-size: 10px;')
        layout.addWidget(note)

        btns = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok |
                                QDialogButtonBox.StandardButton.Cancel)
        btns.accepted.connect(self.accept)
        btns.rejected.connect(self.reject)
        layout.addWidget(btns)
        self.adjustSize()

    def selected_bit(self):
        return self._bit_spin.value()


class _ExportProgressDialog(QDialog):
    cancel_requested = pyqtSignal()

    def __init__(self, n, parent=None, stylesheet=''):
        super().__init__()   # no parent → independent top-level, can go behind main window
        self.setWindowTitle('Exporting TIFF…')
        self.setMinimumWidth(380)
        if stylesheet:
            self.setStyleSheet(stylesheet + """
                QProgressBar {
                    background: #3c3f41; border: 1px solid #555; border-radius: 3px;
                    text-align: center; color: #ddd;
                }
                QProgressBar::chunk { background: #5a8a5a; border-radius: 2px; }
            """)
        layout = QVBoxLayout(self)
        layout.setSpacing(8)
        layout.setContentsMargins(12, 12, 12, 12)
        self._phase = 'Exporting'
        self._label = QLabel('Preparing…')
        layout.addWidget(self._label)
        self._bar = QProgressBar()
        self._bar.setRange(0, n)
        self._bar.setValue(0)
        layout.addWidget(self._bar)
        btn = QPushButton('Cancel')
        btn.clicked.connect(self.cancel_requested)
        layout.addWidget(btn)
        self.adjustSize()
        if parent is not None:
            geo = parent.frameGeometry()
            self.move(geo.center() - self.rect().center())

    def set_phase(self, phase: str):
        self._phase = phase
        self._label.setText(f'{phase}…')

    def update_progress(self, done, total):
        if self._bar.maximum() != total:
            self._bar.setRange(0, total)
        self._bar.setValue(done)
        self._label.setText(f'{self._phase} frame {done} / {total}…')


# ── Trigger-aware slider ──────────────────────────────────────────────────────

class TriggerSlider(QSlider):
    """QSlider that draws vertical tick marks at trigger-frame positions."""

    _TICK_COLOR = QColor(255, 80, 30, 210)

    def __init__(self, orientation, parent=None):
        super().__init__(orientation, parent)
        self._trigger_frames = np.empty(0, dtype=np.intp)

    def set_triggers(self, frames):
        self._trigger_frames = np.asarray(frames, dtype=np.intp)
        self.update()

    def paintEvent(self, event):
        super().paintEvent(event)
        if len(self._trigger_frames) == 0 or self.maximum() <= 0:
            return
        from PyQt6.QtWidgets import QStyleOptionSlider
        opt = QStyleOptionSlider()
        self.initStyleOption(opt)
        groove = self.style().subControlRect(
            self.style().ComplexControl.CC_Slider, opt,
            self.style().SubControl.SC_SliderGroove, self,
        )
        x0, x1 = groove.left(), groove.right()
        total   = self.maximum() - self.minimum()
        pen = QPen(self._TICK_COLOR)
        pen.setWidth(2)
        painter = QPainter(self)
        painter.setPen(pen)
        for f in self._trigger_frames:
            t = (int(f) - self.minimum()) / total
            x = round(x0 + t * (x1 - x0))
            painter.drawLine(x, 0, x, self.height())
        painter.end()


# ── Pixel inspector ───────────────────────────────────────────────────────────

class PixelInspector:
    """Shows [x, y, counts] as a cyan overlay on image click."""

    _CYAN = pg.mkColor(0, 255, 255)

    def __init__(self, imview: pg.ImageView):
        self._imview = imview
        self._frame  = None
        self._x      = None
        self._y      = None
        self._text_item = pg.TextItem(color=self._CYAN, anchor=(0, 1))
        self._text_item.setFont(pg.QtGui.QFont('monospace', 10, pg.QtGui.QFont.Weight.Bold))
        self._text_item.hide()
        imview.getView().addItem(self._text_item)
        imview.scene.sigMouseClicked.connect(self._on_click)

    def update(self, frame: np.ndarray):
        self._frame = frame
        self._refresh()

    def clear(self):
        self._frame = None
        self._x = self._y = None
        self._text_item.hide()

    @staticmethod
    def _fmt_val(frame, y, x) -> str:
        """Integer for raw counts, 4-decimal float for corrected transmission."""
        v = frame[y, x]
        if np.issubdtype(frame.dtype, np.floating):
            return f'{float(v):.4f}'
        return f'{int(v)}'

    def _refresh(self):
        if self._frame is None or self._x is None:
            return
        h, w = self._frame.shape[0], self._frame.shape[1]
        if 0 <= self._x < w and 0 <= self._y < h:
            val = self._fmt_val(self._frame, self._y, self._x)
            self._text_item.setText(f'[{self._x}, {self._y}, {val}]')

    def _on_click(self, event):
        if event.button() != Qt.MouseButton.LeftButton:
            return
        if self._frame is None:
            return
        pos = self._imview.imageItem.mapFromScene(event.scenePos())
        x, y = int(pos.x()), int(pos.y())
        h, w = self._frame.shape[0], self._frame.shape[1]
        if 0 <= x < w and 0 <= y < h:
            self._x, self._y = x, y
            val = self._fmt_val(self._frame, y, x)
            self._text_item.setText(f'[{x}, {y}, {val}]')
            self._text_item.setPos(x, y)
            self._text_item.show()
        else:
            self._x = self._y = None
            self._text_item.hide()


# ── Main window ───────────────────────────────────────────────────────────────

class LucidViewer(ViewerMixin, QMainWindow):
    SETTINGS_ORG = 'LucidViewer'
    SETTINGS_APP = 'ATXViewer'

    def __init__(self):
        super().__init__()
        self._settings          = QSettings(self.SETTINGS_ORG, self.SETTINGS_APP)
        self._reader            = None
        self._path              = None
        self._sidecar_fps       = None
        self._sidecar_status    = ''
        self._sidecar_acq_time  = ''
        self._sidecar_notes     = ''
        self._timestamps        = None   # 1-D float64, relative, display units
        self._ts_unit           = ''
        self._gains             = None   # 1-D float64, per-frame gain dB (from frame_data.csv)
        self._exposures         = None   # 1-D float64, per-frame exposure ms (from frame_data.csv)
        self._line_statuses     = None   # 1-D int64, per-frame line_status_all bitmask
        self._trigger_bit       = 2      # 0-indexed bit in line_status_all for event trigger
        self._trigger_frames    = np.empty(0, dtype=np.intp)  # indices where trigger bit is high
        self._trigger_t0        = None   # timestamp (display units) of first trigger
        self._cache             = {}     # idx -> ndarray, small LRU-ish cache
        self._cache_max         = 8
        self._roi_user_set      = False
        self._export_worker      = None   # TiffExportWorker while an export is running
        self._export_compression = 'None'
        self._export_imagej      = False
        self._dark_field        = None   # float32 (H, W) averaged dark frame, or None
        self._dark_field_gain   = None   # float, average gain (dB) the dark was captured at
        self._dark_field_error  = None   # str error message, or None
        self._white_field       = None   # float32 (H, W) averaged white frame, or None
        self._white_field_gain  = None   # float, average gain (dB) the white was captured at
        self._white_field_error = None   # str error message, or None
        self._white_mode        = 'none' # 'none' | 'flat' | 'pca'
        self._pca_mean          = None   # (H*W,) float32 mean of white frames
        self._pca_components    = None   # (n_stored, H*W) float32 eigenvectors
        self._pca_shape         = None   # (H, W) for which PCA was computed
        self._pca_folder        = None   # folder path used for cached PCA
        self._pca_n_components  = 5      # number of components to use for correction
        self._pca_worker        = None   # PcaComputeWorker while computing
        self._stored_field_refs = {}     # field_references loaded from sidecar
        self._play_timer        = None   # QTimer driving playback
        self._play_start_wall   = 0.0    # time.monotonic() when play started
        self._play_start_ts_idx = 0      # frame index at play start
        self._auto_levels_done  = False  # True after first frame of each file is shown

        self.setWindowTitle('LucidLabs ATX245 Viewer')
        self.resize(1300, 860)
        _ico = os.path.join(os.path.dirname(__file__), 'assets', 'projector.ico')
        if os.path.isfile(_ico):
            self.setWindowIcon(QIcon(_ico))
        self._build_ui()
        self._pixel_inspector = PixelInspector(self.imview)
        self._connect()
        self.setStyleSheet(_DARK_STYLE)
        self._restore_settings()

    # ── UI ────────────────────────────────────────────────────────────────────
    def _build_ui(self):
        file_menu = self.menuBar().addMenu('&File')
        open_act = QAction('&Open file…', self)
        open_act.setShortcut(QKeySequence.StandardKey.Open)
        open_act.triggered.connect(self.open_file)
        file_menu.addAction(open_act)
        file_menu.addSeparator()
        self._export_act = QAction('&Export TIFF stack…', self)
        self._export_act.setShortcut(QKeySequence('Ctrl+E'))
        self._export_act.setEnabled(False)
        self._export_act.triggered.connect(self._export_tiff)
        file_menu.addAction(self._export_act)
        file_menu.addSeparator()
        set_white_act = QAction('Set &white field folder…', self)
        set_white_act.triggered.connect(self._select_white_folder)
        file_menu.addAction(set_white_act)
        set_dark_act = QAction('Set &dark field folder…', self)
        set_dark_act.triggered.connect(self._select_dark_folder)
        file_menu.addAction(set_dark_act)

        config_menu = self.menuBar().addMenu('&Config')
        export_settings_act = QAction('&Export settings…', self)
        export_settings_act.triggered.connect(self._open_export_settings)
        config_menu.addAction(export_settings_act)
        trigger_settings_act = QAction('&Trigger settings…', self)
        trigger_settings_act.triggered.connect(self._open_trigger_settings)
        config_menu.addAction(trigger_settings_act)

        # tools_menu = self.menuBar().addMenu('&Tools')
        # viewer_act = QAction('&Viewer', self)
        # viewer_act.setStatusTip('Launch Lucid Viewer (lucid_viewer.bat)')
        # viewer_act.triggered.connect(self._launch_viewer)
        # tools_menu.addAction(viewer_act)

        help_menu = self.menuBar().addMenu('&Help')
        manual_act = QAction('&Manual', self)
        manual_act.setShortcut(QKeySequence.StandardKey.HelpContents)
        manual_act.setStatusTip('Open HTML user manual in browser')
        manual_act.triggered.connect(self._show_manual)
        help_menu.addAction(manual_act)
        help_menu.addSeparator()
        about_act = QAction('&About…', self)
        about_act.triggered.connect(self._show_about)
        help_menu.addAction(about_act)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        self.setCentralWidget(splitter)

        # ── Sidebar ───────────────────────────────────────────────────────────
        sidebar = QWidget()
        sidebar.setFixedWidth(230)
        sb = QVBoxLayout(sidebar)
        sb.setContentsMargins(8, 8, 8, 8)
        sb.setSpacing(8)

        open_btn = QPushButton('Open image file…')
        open_btn.clicked.connect(self.open_file)
        sb.addWidget(open_btn)

        # Format group
        fmt_box  = QGroupBox('Frame format')
        fmt_grid = QGridLayout(fmt_box)
        fmt_grid.setSpacing(4)

        fmt_grid.addWidget(QLabel('Pixel format:'), 0, 0)
        self.fmt_combo = QComboBox()
        self.fmt_combo.addItems(PIXEL_FORMATS)
        fmt_grid.addWidget(self.fmt_combo, 0, 1)

        fmt_grid.addWidget(QLabel('Width (px):'), 1, 0)
        self.w_spin = QSpinBox()
        self.w_spin.setRange(1, 16384)
        self.w_spin.setValue(ATX245_W)
        fmt_grid.addWidget(self.w_spin, 1, 1)

        fmt_grid.addWidget(QLabel('Height (px):'), 2, 0)
        self.h_spin = QSpinBox()
        self.h_spin.setRange(1, 16384)
        self.h_spin.setValue(ATX245_H)
        fmt_grid.addWidget(self.h_spin, 2, 1)

        self.reload_btn = QPushButton('Re-parse')
        self.reload_btn.setEnabled(False)
        fmt_grid.addWidget(self.reload_btn, 3, 0, 1, 2)

        self.diag_label = QLabel('')
        self.diag_label.setWordWrap(True)
        self.diag_label.setStyleSheet('color: #888; font-size: 10px;')
        fmt_grid.addWidget(self.diag_label, 4, 0, 1, 2)
        sb.addWidget(fmt_box)

        # Metadata
        meta_box  = QGroupBox('File info')
        meta_grid = QGridLayout(meta_box)
        meta_grid.setSpacing(4)

        self._meta_vals = []
        for row, key in enumerate(['File:', 'Format:', 'Frames:', 'FPS:', 'Size:', 'Acquired:', 'Notes:']):
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
            'Threshold derived from pixel format bit depth'
        )
        disp_grid.addWidget(self.mask_chk, 2, 0, 1, 2)
        sb.addWidget(disp_box)

        # Correction
        corr_box  = QGroupBox('Correction')
        corr_grid = QGridLayout(corr_box)
        corr_grid.setSpacing(4)
        self.dark_chk  = QCheckBox('Dark field')
        self.dark_chk.setToolTip(
            'Finds closest dark_field_* folder by timestamp\n'
            'and averages all frames as the dark reference.'
        )
        self.white_chk = QCheckBox('White field')
        self.white_chk.setToolTip('Enable white-field correction using the selected technique.')
        self.white_combo = QComboBox()
        self.white_combo.addItems(['Flat field', 'PCA removal'])
        self.white_combo.setToolTip(
            'Flat field: divide by averaged white-field frames\n'
            'PCA removal: project each frame onto a PCA basis built from white-field\n'
            '  frames and subtract the estimated background\n\n'
            'Disabled options mean no compatible white_field folder was found.'
        )
        self._pca_n_lbl = QLabel('N components:')
        self._pca_n_lbl.setVisible(False)
        self.pca_n_spin = QSpinBox()
        self.pca_n_spin.setRange(1, 20)
        self.pca_n_spin.setValue(5)
        self.pca_n_spin.setToolTip('Number of PCA components to subtract (1–20)')
        self.pca_n_spin.setVisible(False)
        self.corr_status_lbl = QLabel('')
        self.corr_status_lbl.setWordWrap(True)
        self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')
        self.store_fields_btn = QPushButton('Store')
        self.store_fields_btn.setToolTip(
            'Save current dark/white field selection to this file\'s sidecar JSON.\n'
            'On next open the same fields will be loaded automatically.')
        self.store_fields_btn.setEnabled(False)
        corr_grid.addWidget(self.dark_chk,            0, 0, 1, 2)
        corr_grid.addWidget(self.white_chk,           1, 0)
        corr_grid.addWidget(self.white_combo,         1, 1)
        corr_grid.addWidget(self._pca_n_lbl,         2, 0)
        corr_grid.addWidget(self.pca_n_spin,         2, 1)
        corr_grid.addWidget(self.corr_status_lbl,    3, 0, 1, 2)
        corr_grid.addWidget(self.store_fields_btn,   4, 0, 1, 2)
        sb.addWidget(corr_box)

        sb.addStretch()
        splitter.addWidget(sidebar)

        # ── pyqtgraph canvas ──────────────────────────────────────────────────
        right = QWidget()
        rv = QVBoxLayout(right)
        rv.setContentsMargins(0, 0, 0, 0)
        rv.setSpacing(4)

        self.frame_title_label = QLabel('')
        self.frame_title_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        rv.addWidget(self.frame_title_label)

        self.imview = pg.ImageView()
        rv.addWidget(self.imview)

        self._mask_item = pg.ImageItem()
        self._mask_item.setZValue(10)
        self.imview.getView().addItem(self._mask_item)

        # Frame slider (for lazy loading; hides pyqtgraph's built-in timeline
        # when the full stack is not loaded)
        self.frame_slider = TriggerSlider(Qt.Orientation.Horizontal)
        self.frame_slider.setMinimum(0)
        self.frame_slider.setMaximum(0)
        self.frame_slider.setEnabled(False)
        rv.addWidget(self.frame_slider)

        ctrl_row = QHBoxLayout()
        self.play_btn = QPushButton('▶  Play')
        self.play_btn.setFixedWidth(80)
        self.play_btn.setEnabled(False)
        ctrl_row.addWidget(self.play_btn)
        self.frame_label = QLabel('— / —')
        self.frame_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        ctrl_row.addWidget(self.frame_label, stretch=1)
        rv.addLayout(ctrl_row)

        splitter.addWidget(right)
        splitter.setStretchFactor(1, 1)

        self.statusBar().showMessage(
            'Set Width × Height and pixel format, then open a .raw / .tiff / .mp4 file.'
        )

    def _connect(self):
        self.reload_btn.clicked.connect(self._reopen)
        self.cmap_combo.currentTextChanged.connect(self._apply_colormap)
        self.frame_slider.valueChanged.connect(self._show_frame)
        self.imview.ui.roiBtn.clicked.connect(self._on_roi_btn_clicked)
        # Disconnect pyqtgraph's default live-drag ROI handler; we re-route
        # below so we can guard against the expensive 3D downsampling path.
        self.imview.roi.sigRegionChanged.disconnect(self.imview.roiChanged)
        self.imview.roi.sigRegionChanged.connect(self._on_roi_dragged)
        self.imview.roi.sigRegionChangeFinished.connect(self._roi_changed)
        self.imview.roi.sigRegionChangeFinished.connect(self._on_roi_moved)
        self.mask_chk.toggled.connect(self._toggle_mask)
        self.dark_chk.toggled.connect(self._toggle_dark_field)
        self.white_chk.toggled.connect(self._on_white_chk_toggled)
        self.white_combo.currentIndexChanged.connect(self._on_white_combo_changed)
        self.pca_n_spin.valueChanged.connect(self._on_pca_n_changed)
        self.store_fields_btn.clicked.connect(self._store_fields_ref)

        self.play_btn.clicked.connect(self._toggle_play)

        QShortcut(QKeySequence(Qt.Key.Key_Left),  self, lambda: self._step(-1))
        QShortcut(QKeySequence(Qt.Key.Key_Right), self, lambda: self._step(+1))
        QShortcut(QKeySequence(Qt.Key.Key_Home),  self, lambda: self._go(0))
        QShortcut(QKeySequence(Qt.Key.Key_End),   self,
                  lambda: self._go(self._reader_len() - 1))
        QShortcut(QKeySequence(Qt.Key.Key_Space), self, self._toggle_play)

    # ── File loading ──────────────────────────────────────────────────────────
    def open_file(self):
        last = self._settings.value('last_path', '')
        start_dir = os.path.dirname(last) if last else ''
        path, _ = QFileDialog.getOpenFileName(
            self, 'Open image file', start_dir,
            'Image files (*.raw *.tiff *.tif *.mp4 *.avi);;All files (*)'
        )
        if not path:
            return
        self._path = path
        self._apply_hints(path)
        self._open_reader()

    def _apply_hints(self, path: str):
        """Apply metadata from .json sidecar first, fall back to filename heuristics."""
        self._stored_field_refs = {}
        sidecar, self._sidecar_status = _load_sidecar(path)

        w   = sidecar.get('width')
        h   = sidecar.get('height')
        # Accept both 'pixel_format' (new style) and 'pixelformat' (legacy)
        fmt = sidecar.get('pixel_format') or sidecar.get('pixelformat')
        self._sidecar_fps         = sidecar.get('fps')
        self._sidecar_acq_time    = sidecar.get('acquisition_time', '')
        self._sidecar_notes       = sidecar.get('notes', '')
        self._stored_field_refs   = sidecar.get('field_references', {})
        self._timestamps, self._ts_unit, self._gains, self._exposures, self._line_statuses = _load_timestamps(path)

        # Fall back to filename hint for pixel format if sidecar didn't provide it
        if not fmt:
            fmt = _guess_from_filename(os.path.basename(path))

        if w:
            self.w_spin.setValue(int(w))
        if h:
            self.h_spin.setValue(int(h))
        if fmt:
            # MatchFixedString without MatchCaseSensitive is case-insensitive
            idx = self.fmt_combo.findText(fmt, Qt.MatchFlag.MatchFixedString)
            if idx >= 0:
                self.fmt_combo.setCurrentIndex(idx)

    def _reopen(self):
        if self._path:
            self._cache.clear()
            self._open_reader()

    def _open_reader(self):
        if self._reader is not None:
            try:
                self._reader.close()
            except Exception:
                pass
            self._reader = None
            self._pixel_inspector.clear()

        path = self._path
        ext  = os.path.splitext(path)[1].lower()
        fmt  = self.fmt_combo.currentText()
        w    = self.w_spin.value()
        h    = self.h_spin.value()

        try:
            if ext == '.raw':
                reader = RawReader(path, fmt, w, h)
            elif ext in ('.tiff', '.tif'):
                reader = TiffReader(path)
                w, h = reader.w, reader.h
                self.w_spin.setValue(w)
                self.h_spin.setValue(h)
            elif ext in ('.mp4', '.avi'):
                reader = VideoReader(path)
                w, h = reader.w, reader.h
                self.w_spin.setValue(w)
                self.h_spin.setValue(h)
            else:
                QMessageBox.warning(self, 'Unsupported file',
                                    f'Extension {ext!r} not supported.')
                return
        except Exception as exc:
            self.statusBar().showMessage(f'Open error: {exc}')
            QMessageBox.warning(self, 'Open error', str(exc))
            return

        self._reader = reader
        self._cache  = {}

        # If the filename already encodes baked-in correction, disable controls —
        # (data-dark)/(white-dark) is non-commutative so re-applying would be wrong.
        stem_lower = os.path.splitext(os.path.basename(path))[0].lower()
        any_baked  = '_dark' in stem_lower or '_white' in stem_lower
        self.dark_chk.blockSignals(True)
        if any_baked:
            self.dark_chk.setChecked(False)
            self.dark_chk.setEnabled(False)
            self._dark_field      = None
            self._dark_field_gain = None
        else:
            self.dark_chk.setEnabled(True)
        self.dark_chk.blockSignals(False)
        self.white_chk.blockSignals(True)
        self.white_combo.blockSignals(True)
        if any_baked:
            self.white_chk.setChecked(False)
            self.white_chk.setEnabled(False)
            self.white_combo.setEnabled(False)
            self._white_field       = None
            self._white_field_gain  = None
            self._white_mode        = 'none'
            self._pca_mean          = None
            self._pca_components    = None
        else:
            self.white_chk.setEnabled(True)
            self.white_combo.setEnabled(True)
        self.white_chk.blockSignals(False)
        self.white_combo.blockSignals(False)

        self.mask_chk.blockSignals(True)
        if any_baked:
            self.mask_chk.setChecked(False)
            self.mask_chk.setEnabled(False)
            self.mask_chk.setToolTip(
                'Pixel mask unavailable — this file has field correction baked in.\n'
                'Saturation/dead-pixel thresholds are based on raw sensor counts.'
            )
        else:
            self.mask_chk.setEnabled(True)
            self.mask_chk.setToolTip(
                'Red = saturated  |  Yellow = dead (zero)\n'
                'Threshold derived from pixel format bit depth'
            )
        self.mask_chk.blockSignals(False)

        if not any_baked:
            self._check_white_options()
            self._apply_stored_field_refs()
        self._reload_fields()
        n = len(reader)

        # FPS: prefer sidecar, then video container metadata
        fps = self._sidecar_fps or getattr(reader, 'fps', None)
        fps_str = f'{fps:.3f}' if fps else '—'

        file_size_mb = os.path.getsize(path) / 1e6

        acq   = self._sidecar_acq_time or '—'
        notes = self._sidecar_notes    or '—'
        for lbl, val in zip(self._meta_vals, [
            os.path.basename(path),
            fmt if ext == '.raw' else ext.lstrip('.').upper(),
            str(n),
            fps_str,
            f'{file_size_mb:.1f} MB',
            acq,
            notes,
        ]):
            lbl.setText(val)

        if ext == '.raw':
            bpf = getattr(reader, 'bpf', None)
            dim = f'{w}×{h} px\n{bpf:,} B/frame' if bpf else f'{w}×{h} px'
        else:
            dim = f'{w}×{h} px'
        self.diag_label.setText(f'{dim}\n{self._sidecar_status}')

        self.reload_btn.setEnabled(True)
        self._export_act.setEnabled(True)

        self._stop_play()
        self.frame_slider.setMaximum(max(0, n - 1))
        self.frame_slider.setEnabled(n > 1)
        self.frame_slider.setValue(0)
        self.play_btn.setEnabled(n > 1)

        self._trigger_frames = np.empty(0, dtype=np.intp)
        self._trigger_t0     = None
        ls = self._line_statuses
        if ls is not None and len(ls) > 0:
            mask = 1 << self._trigger_bit
            trigger_idxs = np.where((ls & mask) != 0)[0]
            if len(trigger_idxs):
                self._trigger_frames = trigger_idxs.astype(np.intp)
                ts = self._timestamps
                if ts is not None and trigger_idxs[0] < len(ts):
                    self._trigger_t0 = float(ts[trigger_idxs[0]])
        self.frame_slider.set_triggers(self._trigger_frames)

        self._auto_levels_done = False
        self._show_frame(0)

        fps_part = f'  |  {fps:.3f} fps' if fps else ''
        self.statusBar().showMessage(
            f'{n} frames{fps_part}  |  {w}×{h} px  |  {os.path.basename(path)}'
        )

    # ── Frame display ─────────────────────────────────────────────────────────
    def _show_frame(self, idx: int):
        if self._reader is None:
            return
        idx = max(0, min(idx, self._reader_len() - 1))

        if idx not in self._cache:
            try:
                frame = self._reader[idx]
            except Exception as exc:
                self.statusBar().showMessage(f'Read error frame {idx}: {exc}')
                return
            if len(self._cache) >= self._cache_max:
                oldest = next(iter(self._cache))
                del self._cache[oldest]
            self._cache[idx] = frame

        frame = self._cache[idx]
        data_gain = (self._gains[idx] if self._gains is not None and idx < len(self._gains)
                     else None)
        display_frame = self._apply_field_correction(frame, data_gain=data_gain)
        auto = not self._auto_levels_done
        self._auto_levels_done = True
        self.imview.setImage(display_frame, autoLevels=auto, autoRange=False)
        self._pixel_inspector.update(display_frame)
        self._apply_colormap(self.cmap_combo.currentText())
        self._update_mask(frame)

        n = self._reader_len()

        ts = self._timestamps
        if ts is not None and idx < len(ts):
            if self._trigger_t0 is not None:
                dt = ts[idx] - self._trigger_t0
                label_text = f'Frame {idx + 1} / {n}   T{dt:+.3f} {self._ts_unit}'
            else:
                label_text = f'Frame {idx + 1} / {n}   +{ts[idx]:.3f} {self._ts_unit}'
        else:
            label_text = f'Frame {idx + 1} / {n}'
        self.frame_label.setText(label_text)

        title_parts = []
        gains = self._gains
        if gains is not None and idx < len(gains):
            title_parts.append(f'Gain: {gains[idx]:g} dB')
        exposures = self._exposures
        if exposures is not None and idx < len(exposures):
            title_parts.append(f'Exposure: {exposures[idx]:g} ms')
        self.frame_title_label.setText('   '.join(title_parts))

        self.frame_slider.blockSignals(True)
        self.frame_slider.setValue(idx)
        self.frame_slider.blockSignals(False)

        self.statusBar().showMessage(
            f'Frame {idx + 1} / {n}  |  '
            f'{self.w_spin.value()}×{self.h_spin.value()} px  |  '
            f'{os.path.basename(self._path)}'
        )

    def _reader_len(self) -> int:
        return len(self._reader) if self._reader else 0

    # ── Navigation ────────────────────────────────────────────────────────────
    def _step(self, delta: int):
        if self._reader is None:
            return
        self._go(self.frame_slider.value() + delta)

    def _go(self, idx: int):
        if self._reader is None:
            return
        idx = max(0, min(idx, self._reader_len() - 1))
        self._show_frame(idx)

    # ── Playback ──────────────────────────────────────────────────────────────
    def _toggle_play(self):
        if self._play_timer is not None and self._play_timer.isActive():
            self._stop_play()
        else:
            self._start_play()

    def _start_play(self):
        if self._reader is None:
            return
        n = self._reader_len()
        idx = self.frame_slider.value()
        if idx >= n - 1:
            idx = 0
            self._show_frame(0)
        self._play_start_wall   = time.monotonic()
        self._play_start_ts_idx = idx
        if self._play_timer is None:
            self._play_timer = QTimer(self)
            self._play_timer.timeout.connect(self._play_tick)
        self._play_timer.start(100)   # 10 FPS cap
        self.play_btn.setText('⏸  Pause')
        self.frame_slider.setEnabled(False)

    def _stop_play(self):
        if self._play_timer is not None:
            self._play_timer.stop()
        self.play_btn.setText('▶  Play')
        if self._reader is not None:
            self.frame_slider.setEnabled(self._reader_len() > 1)

    def _elapsed_in_ts_units(self, elapsed_s: float) -> float:
        if self._ts_unit == 'ms':
            return elapsed_s * 1000.0
        if self._ts_unit == 'min':
            return elapsed_s / 60.0
        return elapsed_s  # 's' or no unit

    def _play_tick(self):
        if self._reader is None:
            self._stop_play()
            return
        n   = self._reader_len()
        ts  = self._timestamps
        si  = self._play_start_ts_idx
        elapsed = time.monotonic() - self._play_start_wall

        if ts is not None and si < len(ts) and len(ts) >= 2:
            elapsed_disp = self._elapsed_in_ts_units(elapsed)
            target       = ts[si] + elapsed_disp
            ts_slice     = ts[si : min(n, len(ts))]
            pos  = int(np.searchsorted(ts_slice, target, side='right')) - 1
            idx  = si + max(0, pos)
            idx  = min(idx, n - 1)
        else:
            idx = min(si + int(elapsed * 10), n - 1)

        if idx != self.frame_slider.value():
            self._show_frame(idx)

        if idx >= n - 1:
            self._start_play()  # loop back to frame 0 while play is active

    def _on_roi_dragged(self):
        if self.imview.ui.roiBtn.isChecked():
            self.imview.roiChanged()

    # ── Pixel mask ────────────────────────────────────────────────────────────
    def _toggle_mask(self, checked: bool):
        self._mask_item.setVisible(checked)
        if checked and self._reader is not None:
            idx = self.frame_slider.value()
            if idx in self._cache:
                self._update_mask(self._cache[idx])

    def _pixel_sat_value(self) -> int:
        bits = _FORMAT_BITS.get(self.fmt_combo.currentText(), 16)
        return (1 << bits) - 1

    def _update_mask(self, frame: np.ndarray):
        if not self.mask_chk.isChecked():
            self._mask_item.setImage(None)
            return
        sat_val = self._pixel_sat_value()
        rgba = _build_pixel_mask(frame, sat_val)
        self._mask_item.setImage(rgba, autoLevels=False)

    # ── Field correction ──────────────────────────────────────────────────────
    def _field_folder_shape(self, folder: str):
        """Peek at first image file in folder; return (w, h) or None if unknown."""
        try:
            files = sorted(os.listdir(folder))
        except OSError:
            return None
        raw_files = [f for f in files if os.path.splitext(f)[1].lower() == '.raw']
        tif_files = [f for f in files
                     if os.path.splitext(f)[1].lower() in ('.tiff', '.tif')]
        first = (raw_files or tif_files or [None])[0]
        if first is None:
            return None
        fpath = os.path.join(folder, first)
        ext   = os.path.splitext(first)[1].lower()
        if ext == '.raw':
            sc, _ = _load_sidecar(fpath)
            w = sc.get('width') or sc.get('Width')
            h = sc.get('height') or sc.get('Height')
            if w and h:
                return (int(w), int(h))
            return None
        try:
            import tifffile
            with tifffile.TiffFile(fpath) as tf:
                s = tf.series[0].shape if tf.series else None
                if s is not None:
                    if len(s) == 2:
                        return (s[1], s[0])   # (w, h)
                    if len(s) >= 3:
                        return (s[-1], s[-2])
                page = tf.pages[0]
                return (page.shape[1], page.shape[0])
        except Exception:
            return None

    def _find_field_folder(self, keyword: str, min_frames: int = 1):
        """Return path to best shape-matching folder whose name contains keyword.

        Priority: (1) stored sidecar ref if path exists, shape matches, and has
                      enough frames,
                  (2) closest-timestamp match with matching shape and enough frames,
                  (3) closest-timestamp match with unknown shape and enough frames,
                  (4) None.

        min_frames: skip folders that contain fewer than this many frames (use 2
                    for PCA so pre-averaged single-frame folders are bypassed).
        """
        if not self._path:
            return None

        tw = self._reader.w if self._reader else self.w_spin.value()
        th = self._reader.h if self._reader else self.h_spin.value()

        # --- stored sidecar reference ---
        refs = self._stored_field_refs or {}
        stored = refs.get('dark_folder' if 'dark' in keyword else 'white_folder')
        if stored and os.path.isdir(stored):
            shape = self._field_folder_shape(stored)
            if shape is None or shape == (tw, th):
                if min_frames <= 1 or self._count_white_field_frames(stored) >= min_frames:
                    return stored
            else:
                print(f'[field] stored ref {stored!r} shape {shape} != target {(tw,th)}, ignoring')

        # --- timestamp-proximity search ---
        parent = os.path.dirname(self._path)
        exp_ts = _folder_timestamp(os.path.basename(parent))
        for search_dir in [parent, os.path.dirname(parent)]:
            if not os.path.isdir(search_dir):
                continue
            try:
                all_entries = os.listdir(search_dir)
            except OSError:
                continue
            raw_candidates = [
                d for d in all_entries
                if keyword in d and os.path.isdir(os.path.join(search_dir, d))
            ]
            if exp_ts:
                # Sort by absolute timestamp distance from experiment folder
                candidates = sorted(raw_candidates,
                                    key=lambda d: abs(_folder_timestamp(d) - exp_ts))
            else:
                # Fallback: sort by filesystem mtime proximity
                try:
                    exp_mtime = os.path.getmtime(parent)
                except OSError:
                    exp_mtime = 0.0
                candidates = sorted(
                    raw_candidates,
                    key=lambda d: abs(
                        (lambda p: os.path.getmtime(p) if os.path.exists(p) else 0.0)(
                            os.path.join(search_dir, d)) - exp_mtime))

            shape_match  = None
            unknown_shape = None
            for name in candidates:
                folder = os.path.join(search_dir, name)
                if min_frames > 1 and self._count_white_field_frames(folder) < min_frames:
                    print(f'[field] skipping {name!r}: fewer than {min_frames} frames')
                    continue
                shape  = self._field_folder_shape(folder)
                if shape is None:
                    if unknown_shape is None:
                        unknown_shape = folder
                elif shape == (tw, th):
                    shape_match = folder
                    break
                else:
                    print(f'[field] skipping {name!r}: shape {shape} != {(tw,th)}')
            result = shape_match or unknown_shape
            if result:
                return result
        return None

    def _load_field_frame(self, folder: str):
        """Average all raw/tiff frames in folder; return float32 (H, W) or None."""
        try:
            all_files = sorted(os.listdir(folder))
        except OSError as e:
            print(f'[field] listdir error on {folder!r}: {e}')
            return None

        raw_candidates  = [f for f in all_files if os.path.splitext(f)[1].lower() == '.raw']
        tiff_candidates = [f for f in all_files if os.path.splitext(f)[1].lower() in ('.tiff', '.tif')]

        # Prefer RAW when both are present — TIFFs are duplicates written for
        # legacy compatibility by the acquisition code.
        if raw_candidates:
            files = raw_candidates
            if tiff_candidates:
                print(f'[field] skipping {len(tiff_candidates)} TIFF(s) — RAW files take precedence')
        else:
            files = tiff_candidates

        print(f'[field] files to load from {folder!r}: {files}')
        if not files:
            print('[field] no matching files found')
            return None

        # Viewer-current settings used as fallback if a file has no sidecar.
        default_fmt = self.fmt_combo.currentText()
        default_w   = self.w_spin.value()
        default_h   = self.h_spin.value()

        def _fmt_from_sc(sc):
            return (sc.get('pixel_format') or sc.get('pixelformat')
                    or sc.get('PixelFormat') or sc.get('Pixel Format'))

        frames = []
        for fname in files:
            fpath = os.path.join(folder, fname)
            ext   = os.path.splitext(fname)[1].lower()
            try:
                if ext == '.raw':
                    # Read each file's own sidecar so format/dims are correct
                    # per-image (field captures may use a different format than
                    # the experiment).
                    sidecar, sidecar_status = _load_sidecar(fpath)
                    f_fmt = _fmt_from_sc(sidecar)
                    if f_fmt and f_fmt in PIXEL_FORMATS:
                        fmt = f_fmt
                    else:
                        fmt = default_fmt
                        if not sidecar:
                            print(f'[field] WARNING: no sidecar for {fname} '
                                  f'({sidecar_status}); falling back to viewer '
                                  f'format {fmt!r}. Decoding may be wrong.')
                        else:
                            print(f'[field] WARNING: sidecar for {fname} has no '
                                  f'pixel_format key (found: {list(sidecar.keys())}); '
                                  f'falling back to viewer format {fmt!r}.')
                    w = int(sidecar.get('width',  default_w))
                    h = int(sidecar.get('height', default_h))
                    print(f'[field]   {fname}: fmt={fmt!r}, {w}x{h}  [{sidecar_status}]')
                    r = RawReader(fpath, fmt, w, h)
                    for i in range(len(r)):
                        frames.append(r[i].astype(np.float32))
                    r.close()
                elif ext in ('.tiff', '.tif'):
                    import tifffile
                    data = tifffile.imread(fpath)
                    if data.ndim == 2:
                        frames.append(data.astype(np.float32))
                    else:
                        for i in range(data.shape[0]):
                            frames.append(data[i].astype(np.float32))
                print(f'[field]   loaded {fname}: {len(frames)} frames so far')
            except Exception as e:
                print(f'[field]   ERROR loading {fname}: {e}')
                continue

        if not frames:
            print('[field] no frames loaded after reading all files')
            return None
        print(f'[field] averaging {len(frames)} frames, shape {frames[0].shape}')
        return np.mean(frames, axis=0).astype(np.float32)

    def _read_average_gain(self, folder: str):
        """Return mean gain (dB) from frame_data.csv in folder, or None."""
        csv_path = os.path.join(folder, 'frame_data.csv')
        if not os.path.isfile(csv_path):
            return None
        try:
            import csv as _csv
            with open(csv_path, newline='', encoding='utf-8-sig') as fh:
                rows = [r for r in _csv.reader(fh) if any(c.strip() for c in r)]
            if len(rows) < 2:
                return None
            header = [c.strip().lower() for c in rows[0]]
            gain_col = next((i for i, h in enumerate(header) if 'gain' in h), None)
            if gain_col is None:
                return None
            vals = []
            for row in rows[1:]:
                if gain_col < len(row):
                    try:
                        vals.append(float(row[gain_col].strip()))
                    except ValueError:
                        pass
            return float(np.mean(vals)) if vals else None
        except Exception:
            return None

    def _toggle_dark_field(self, checked: bool):
        if not checked:
            self._dark_field       = None
            self._dark_field_gain  = None
            self._dark_field_error = None
            self.dark_chk.setToolTip(
                'Finds closest dark_field_* folder by timestamp\n'
                'and averages all frames as the dark reference.')
            self._refresh_corr_status()
            self._refresh_current_frame()
            self._auto_levels()
            return
        if not self._path:
            self.dark_chk.setChecked(False)
            return
        folder = self._find_field_folder('dark_field')
        if folder is None:
            self.corr_status_lbl.setText('dark_field folder not found')
            self.dark_chk.blockSignals(True)
            self.dark_chk.setChecked(False)
            self.dark_chk.blockSignals(False)
            return
        self._dark_field = self._load_field_frame(folder)
        if self._dark_field is None:
            self.corr_status_lbl.setText('Dark field: no frames loaded')
            self.dark_chk.blockSignals(True)
            self.dark_chk.setChecked(False)
            self.dark_chk.blockSignals(False)
            return
        self._dark_field_gain  = self._read_average_gain(folder)
        self._dark_field_error = self._check_field_shape(self._dark_field, 'dark')
        self.dark_chk.setToolTip(
            f'Using: {os.path.basename(folder)}\n'
            f'Path: {folder}\n\n'
            'Averages all frames as the dark reference.')
        self._refresh_corr_status()
        self._refresh_current_frame()
        self._auto_levels()

    def _on_white_chk_toggled(self, checked):
        if checked:
            self._on_white_mode_changed(self.white_combo.currentIndex() + 1)
        else:
            self._on_white_mode_changed(0)

    def _on_white_combo_changed(self, idx):
        if self.white_chk.isChecked():
            self._on_white_mode_changed(idx + 1)

    def _on_white_mode_changed(self, index):
        """Handle white field mode: 0=None, 1=Flat field, 2=PCA removal."""
        if index == 0:
            self._white_mode        = 'none'
            self._white_field       = None
            self._white_field_gain  = None
            self._white_field_error = None
            self._pca_mean          = None
            self._pca_components    = None
            self._pca_n_lbl.setVisible(False)
            self.pca_n_spin.setVisible(False)
            self._refresh_corr_status()
            self._refresh_current_frame()
            self._auto_levels()

        elif index == 1:
            self._pca_n_lbl.setVisible(False)
            self.pca_n_spin.setVisible(False)
            if not self._path:
                self.white_chk.blockSignals(True)
                self.white_chk.setChecked(False)
                self.white_chk.blockSignals(False)
                return
            folder = self._find_field_folder('white_field')
            if folder is None:
                self.corr_status_lbl.setText('white_field folder not found')
                self.white_chk.blockSignals(True)
                self.white_chk.setChecked(False)
                self.white_chk.blockSignals(False)
                return
            self._white_field = self._load_field_frame(folder)
            if self._white_field is None:
                self.corr_status_lbl.setText('White field: no frames loaded')
                self.white_chk.blockSignals(True)
                self.white_chk.setChecked(False)
                self.white_chk.blockSignals(False)
                return
            self._white_field_gain  = self._read_average_gain(folder)
            self._white_field_error = self._check_field_shape(self._white_field, 'white')
            self._white_mode        = 'flat'
            self._pca_mean          = None
            self._pca_components    = None
            self.white_combo.setToolTip(
                f'Using: {os.path.basename(folder)}\n'
                f'Path: {folder}\n\n'
                'Flat field: divides each frame by the averaged white-field frames.')
            self._refresh_corr_status()
            self._refresh_current_frame()
            self._auto_levels()

        elif index == 2:
            self._pca_n_lbl.setVisible(True)
            self.pca_n_spin.setVisible(True)
            if not self._path:
                self.white_chk.blockSignals(True)
                self.white_chk.setChecked(False)
                self.white_chk.blockSignals(False)
                return
            self._white_field       = None
            self._white_field_gain  = None
            self._white_field_error = None
            self._white_mode        = 'pca'
            self._compute_or_load_pca()

    def _on_pca_n_changed(self, value):
        self._pca_n_components = value
        if self._white_mode == 'pca' and self._pca_components is not None:
            self._refresh_current_frame()

    def _check_white_options(self):
        """Enable or disable combo items based on what white_field data is available."""
        flat_ok = pca_ok = False
        if self._reader is not None:
            if self._find_field_folder('white_field', min_frames=1):
                flat_ok = True
            if self._find_field_folder('white_field', min_frames=2):
                pca_ok = True

        model = self.white_combo.model()
        for i, enabled in enumerate([flat_ok, pca_ok]):
            item = model.item(i)
            if enabled:
                item.setFlags(Qt.ItemFlag.ItemIsSelectable | Qt.ItemFlag.ItemIsEnabled)
            else:
                item.setFlags(Qt.ItemFlag.ItemIsSelectable)

        if self.white_chk.isChecked():
            cur = self.white_combo.currentIndex()
            if (cur == 0 and not flat_ok) or (cur == 1 and not pca_ok):
                self.white_chk.blockSignals(True)
                self.white_chk.setChecked(False)
                self.white_chk.blockSignals(False)
                self._on_white_mode_changed(0)

    def _count_white_field_frames(self, folder):
        """Estimate total frame count in folder without loading pixel data."""
        try:
            all_files = sorted(os.listdir(folder))
        except OSError:
            return 0
        raw_files = [f for f in all_files if f.lower().endswith('.raw')]
        tif_files = [f for f in all_files if f.lower().endswith(('.tiff', '.tif'))]
        files = raw_files if raw_files else tif_files
        if not files:
            return 0
        default_fmt = self.fmt_combo.currentText()
        default_w   = self._reader.w if self._reader else self.w_spin.value()
        default_h   = self._reader.h if self._reader else self.h_spin.value()
        total = 0
        for fname in files:
            fpath = os.path.join(folder, fname)
            ext   = os.path.splitext(fname)[1].lower()
            if ext == '.raw':
                sidecar, _ = _load_sidecar(fpath)
                fmt = (sidecar.get('pixel_format') or sidecar.get('pixelformat')
                       or sidecar.get('PixelFormat') or sidecar.get('Pixel Format')
                       or default_fmt)
                w   = int(sidecar.get('width')  or sidecar.get('Width')  or default_w)
                h   = int(sidecar.get('height') or sidecar.get('Height') or default_h)
                fsize = os.path.getsize(fpath)
                # Use n_frames from sidecar when present (exported files carry this)
                n_sc = sidecar.get('n_frames')
                if n_sc is not None:
                    total += max(1, int(n_sc))
                    continue
                try:
                    bpf   = frame_byte_count(fmt, w, h)
                    total += max(1, int(fsize // bpf))
                except Exception:
                    # Unknown format — estimate conservatively at 2 bytes/pixel
                    bpf_est = w * h * 2
                    total += max(1, int(fsize // bpf_est)) if bpf_est else 1
            elif ext in ('.tiff', '.tif'):
                try:
                    import tifffile
                    with tifffile.TiffFile(fpath) as tf:
                        s = tf.series[0].shape if tf.series else None
                        if s is not None:
                            total += s[0] if len(s) > 2 else 1
                        else:
                            total += len(tf.pages)
                except Exception:
                    total += 1
        return total

    def _load_all_field_frames(self, folder):
        """Load every frame from folder as individual float32 arrays (for PCA)."""
        try:
            all_files = sorted(os.listdir(folder))
        except OSError:
            return []
        raw_files = [f for f in all_files if os.path.splitext(f)[1].lower() == '.raw']
        tif_files = [f for f in all_files
                     if os.path.splitext(f)[1].lower() in ('.tiff', '.tif')]
        files = raw_files if raw_files else tif_files
        if not files:
            return []

        default_fmt = self.fmt_combo.currentText()
        default_w   = self.w_spin.value()
        default_h   = self.h_spin.value()
        frames = []
        for fname in files:
            fpath = os.path.join(folder, fname)
            ext   = os.path.splitext(fname)[1].lower()
            try:
                if ext == '.raw':
                    sidecar, _ = _load_sidecar(fpath)
                    fmt = (sidecar.get('pixel_format')
                           or sidecar.get('pixelformat') or default_fmt)
                    w = int(sidecar.get('width',  default_w))
                    h = int(sidecar.get('height', default_h))
                    r = RawReader(fpath, fmt, w, h)
                    for i in range(len(r)):
                        frames.append(r[i].astype(np.float32))
                    r.close()
                elif ext in ('.tiff', '.tif'):
                    import tifffile
                    data = tifffile.imread(fpath)
                    if data.ndim == 2:
                        frames.append(data.astype(np.float32))
                    else:
                        for i in range(data.shape[0]):
                            frames.append(data[i].astype(np.float32))
            except Exception as e:
                print(f'[pca] error loading {fname}: {e}')
        return frames

    def _pca_cache_path(self, folder, h, w):
        return os.path.join(folder, f'pca_cache_{h}x{w}.npz')

    def _pca_cache_valid(self, cache_path, folder):
        """True if cache file exists and is newer than all source files in folder."""
        if not os.path.isfile(cache_path):
            return False
        cache_mtime = os.path.getmtime(cache_path)
        try:
            for fname in os.listdir(folder):
                if os.path.splitext(fname)[1].lower() in ('.raw', '.tiff', '.tif'):
                    if os.path.getmtime(os.path.join(folder, fname)) > cache_mtime:
                        return False
        except OSError:
            return False
        return True

    def _compute_or_load_pca(self):
        """Load cached PCA basis or compute from white_field frames (async)."""
        folder = self._find_field_folder('white_field', min_frames=2)
        if folder is None:
            self.corr_status_lbl.setText('white_field folder not found')
            self.white_chk.blockSignals(True)
            self.white_chk.setChecked(False)
            self.white_chk.blockSignals(False)
            return

        h = self._reader.h if self._reader else self.h_spin.value()
        w = self._reader.w if self._reader else self.w_spin.value()
        cache_path = self._pca_cache_path(folder, h, w)

        if self._pca_cache_valid(cache_path, folder):
            try:
                data = np.load(cache_path)
                mean = data['mean']
                comp = data['components']
                if mean.shape[0] == h * w:
                    self._pca_mean       = mean
                    self._pca_components = comp
                    self._pca_shape      = (h, w)
                    self._pca_folder     = folder
                    self._white_mode     = 'pca'
                    n_stored = comp.shape[0]
                    try:
                        ev_str = ', '.join(
                            f'{e*100:.1f}%' for e in data['explained'][:min(3, n_stored)])
                    except Exception:
                        ev_str = '?'
                    self.corr_status_lbl.setText(
                        f'PCA ✓  {n_stored} comp cached, top 3: {ev_str}')
                    self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')
                    self.white_chk.setToolTip(
                        f'Using: {os.path.basename(folder)}\n'
                        f'Path: {folder}\n\n'
                        'PCA removal: subtracts top N PCA components of the background.')
                    self._refresh_current_frame()
                    return
            except Exception as exc:
                print(f'[pca] cache load error: {exc}')

        # Load frames and compute PCA fully asynchronously
        fmt = self.fmt_combo.currentText()
        self.corr_status_lbl.setText('Loading white-field frames…')
        self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')
        if self._pca_worker is not None:
            self._pca_worker.pca_done.disconnect()
            self._pca_worker.pca_error.disconnect()
            self._pca_worker.pca_progress.disconnect()
            self._pca_worker.quit()
        self._pca_worker = PcaComputeWorker(folder, fmt, w, h, parent=self)
        self._pca_worker.pca_done.connect(
            lambda mu, comp, expl, _cp=cache_path, _f=folder:
                self._on_pca_computed(mu, comp, expl, _cp, _f)
        )
        self._pca_worker.pca_error.connect(self._on_pca_error)
        self._pca_worker.pca_progress.connect(self._on_pca_progress)
        self._pca_worker.start()

    def _on_pca_computed(self, mean, components, explained, cache_path, folder):
        if self._white_mode != 'pca':
            return
        self._pca_mean       = mean
        self._pca_components = components
        self._pca_shape      = (self._reader.h, self._reader.w) if self._reader else None
        self._pca_folder     = folder
        try:
            np.savez_compressed(cache_path,
                                mean=mean, components=components, explained=explained)
        except Exception as exc:
            print(f'[pca] cache save error: {exc}')
        n_stored = components.shape[0]
        ev_str = ', '.join(
            f'{e*100:.1f}%' for e in explained[:min(3, n_stored)])
        self.corr_status_lbl.setText(f'PCA ✓  {n_stored} comp, top 3: {ev_str}')
        self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')
        self.white_chk.setToolTip(
            f'Using: {os.path.basename(folder)}\n'
            f'Path: {folder}\n\n'
            'PCA removal: subtracts top N PCA components of the background.')
        self._refresh_current_frame()
        self._auto_levels()

    def _on_pca_progress(self, msg):
        self.corr_status_lbl.setText(msg)
        self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')

    def _on_pca_error(self, msg):
        self.corr_status_lbl.setText(f'PCA error: {msg}')
        self.corr_status_lbl.setStyleSheet('color: #ff6b6b; font-size: 10px;')
        self.white_chk.blockSignals(True)
        self.white_chk.setChecked(False)
        self.white_chk.blockSignals(False)
        self._white_mode = 'none'

    def _apply_stored_field_refs(self):
        """If the current file's sidecar has stored field refs, auto-activate them."""
        refs = self._stored_field_refs
        if not refs:
            return
        tw = self._reader.w if self._reader else self.w_spin.value()
        th = self._reader.h if self._reader else self.h_spin.value()

        dark_folder  = refs.get('dark_folder')
        white_folder = refs.get('white_folder')
        white_mode   = refs.get('white_mode', 'none')

        if dark_folder and os.path.isdir(dark_folder):
            shape = self._field_folder_shape(dark_folder)
            if shape is None or shape == (tw, th):
                self.dark_chk.blockSignals(True)
                self.dark_chk.setChecked(True)
                self.dark_chk.blockSignals(False)

        if white_folder and os.path.isdir(white_folder) and white_mode in ('flat', 'pca'):
            shape = self._field_folder_shape(white_folder)
            if shape is None or shape == (tw, th):
                combo_idx = 0 if white_mode == 'flat' else 1
                model = self.white_combo.model()
                item  = model.item(combo_idx)
                if item and (item.flags() & Qt.ItemFlag.ItemIsEnabled):
                    self.white_combo.blockSignals(True)
                    self.white_combo.setCurrentIndex(combo_idx)
                    self.white_combo.blockSignals(False)
                    self.white_chk.blockSignals(True)
                    self.white_chk.setChecked(True)
                    self.white_chk.blockSignals(False)
                    self._pca_n_lbl.setVisible(combo_idx == 1)
                    self.pca_n_spin.setVisible(combo_idx == 1)

    def _get_or_create_sidecar_path(self):
        """Return best sidecar path for writing.

        Prefers metadata.json (camera-written) in the same directory so field
        references stay in one file alongside capture metadata.  Falls back to
        an existing <stem>.json, then creates metadata.json if nothing exists.
        """
        dir_ = os.path.dirname(self._path)
        meta = os.path.join(dir_, 'metadata.json')
        if os.path.isfile(meta):
            return meta
        stem = os.path.splitext(self._path)[0]
        for candidate in [self._path + '.json', stem + '.json']:
            if os.path.isfile(candidate):
                return candidate
        return meta

    def _write_sidecar_field_refs(self, updates: dict):
        """Merge *updates* into the field_references section of the sidecar and save."""
        sidecar_path = self._get_or_create_sidecar_path()
        existing = {}
        if os.path.isfile(sidecar_path):
            for enc in ('utf-8-sig', 'utf-16', 'latin-1'):
                try:
                    with open(sidecar_path, 'r', encoding=enc) as fh:
                        existing = json.load(fh)
                    break
                except (UnicodeDecodeError, UnicodeError):
                    continue
        refs = existing.get('field_references', {})
        refs.update(updates)
        existing['field_references'] = refs
        with open(sidecar_path, 'w', encoding='utf-8') as fh:
            json.dump(existing, fh, indent=2)
        self._stored_field_refs = refs
        return sidecar_path

    def _store_fields_ref(self):
        """Store active dark and/or white field folders to this file's sidecar."""
        if not self._path:
            return
        updates = {}
        if self._dark_field is not None and not self._dark_field_error:
            folder = self._find_field_folder('dark_field')
            if folder:
                updates['dark_folder'] = folder
        if self._white_mode == 'flat' and self._white_field is not None and not self._white_field_error:
            folder = self._find_field_folder('white_field')
            if folder:
                updates['white_folder'] = folder
                updates['white_mode'] = 'flat'
        elif self._white_mode == 'pca' and self._pca_components is not None and self._pca_folder:
            updates['white_folder'] = self._pca_folder
            updates['white_mode'] = 'pca'
        if not updates:
            self.corr_status_lbl.setText('No active fields to store')
            self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')
            return
        try:
            p = self._write_sidecar_field_refs(updates)
            stored = ', '.join(k.replace('_folder', '').replace('_mode', '') for k in updates)
            self.corr_status_lbl.setText(f'Stored ({stored}) → {os.path.basename(p)}')
            self.corr_status_lbl.setStyleSheet('color: #6bcf7f; font-size: 10px;')
        except Exception as exc:
            self.corr_status_lbl.setText(f'Store error: {exc}')
            self.corr_status_lbl.setStyleSheet('color: #ff6b6b; font-size: 10px;')

    def _apply_pca_correction_frame(self, frame, dark=None):
        """Subtract PCA-estimated white-field background; return float32 transmission."""
        mu   = self._pca_mean
        comp = self._pca_components
        n    = min(self._pca_n_components, comp.shape[0])
        f = frame.astype(np.float32)
        if dark is not None:
            f = f - dark
        d_c    = f.ravel() - mu
        coeffs = comp[:n] @ d_c
        bg     = (mu + comp[:n].T @ coeffs).reshape(frame.shape)
        bg     = np.where(bg >= np.float32(1.0), bg, np.float32(1.0))
        return (f / bg).astype(np.float32)

    def _select_white_folder(self):
        """File menu: let user manually choose a white field folder."""
        start = os.path.dirname(self._path) if self._path else ''
        folder = QFileDialog.getExistingDirectory(self, 'Select white field folder', start)
        if not folder:
            return
        self._stored_field_refs['white_folder'] = folder
        self._check_white_options()
        if self.white_chk.isChecked():
            self._reload_fields()

    def _select_dark_folder(self):
        """File menu: let user manually choose a dark field folder."""
        start = os.path.dirname(self._path) if self._path else ''
        folder = QFileDialog.getExistingDirectory(self, 'Select dark field folder', start)
        if not folder:
            return
        self._stored_field_refs['dark_folder'] = folder
        if not self.dark_chk.isChecked():
            self.dark_chk.setChecked(True)  # triggers _toggle_dark_field → _reload_fields
        else:
            self._reload_fields()

    def _reload_fields(self):
        """Reload whichever field references are currently selected for the new file."""
        _dark_tip_default = ('Finds closest dark_field_* folder by timestamp\n'
                             'and averages all frames as the dark reference.')
        if self.dark_chk.isChecked():
            folder = self._find_field_folder('dark_field')
            self._dark_field       = self._load_field_frame(folder) if folder else None
            self._dark_field_gain  = self._read_average_gain(folder) if folder else None
            self._dark_field_error = self._check_field_shape(self._dark_field, 'dark')
            if folder and self._dark_field is not None:
                self.dark_chk.setToolTip(
                    f'Using: {os.path.basename(folder)}\n'
                    f'Path: {folder}\n\n'
                    'Averages all frames as the dark reference.')
            else:
                self.dark_chk.setToolTip(_dark_tip_default)

        mode_idx = (self.white_combo.currentIndex() + 1) if self.white_chk.isChecked() else 0
        if mode_idx == 1:
            folder = self._find_field_folder('white_field')
            self._white_field       = self._load_field_frame(folder) if folder else None
            self._white_field_gain  = self._read_average_gain(folder) if folder else None
            self._white_field_error = self._check_field_shape(self._white_field, 'white')
            self._white_mode        = 'flat'
            if folder and self._white_field is not None:
                self.white_chk.setToolTip(
                    f'Using: {os.path.basename(folder)}\n'
                    f'Path: {folder}\n\n'
                    'Flat field: divides each frame by the averaged white-field frames.')
        elif mode_idx == 2:
            self._compute_or_load_pca()  # tooltip updated inside _compute_or_load_pca/_on_pca_computed
        else:
            self._white_mode        = 'none'
            self._white_field       = None
            self._white_field_gain  = None
            self._white_field_error = None
            self._pca_mean          = None
            self._pca_components    = None
        self._refresh_corr_status()

    def _check_field_shape(self, field, name: str):
        """Return an error string if field shape doesn't match the open reader, else None."""
        if field is None or self._reader is None:
            return None
        rh, rw = self._reader.h, self._reader.w
        fh, fw = field.shape[0], field.shape[1]
        if fh != rh or fw != rw:
            return f'{name} field is {fw}×{fh} but data is {rw}×{rh}'
        return None

    _DARK_CHK_TIP = ('Finds most recent dark_field_* folder in parent directory\n'
                     'and averages all frames as the dark reference.')
    _WHITE_CHK_TIP = ('Finds most recent white_field_* folder in parent directory\n'
                      'and averages all frames as the white reference.')

    def _refresh_corr_status(self):
        parts = []
        has_error = False

        if self._dark_field is not None:
            if self._dark_field_error:
                parts.append('Dark ✗')
                self.dark_chk.setToolTip(f'{self._DARK_CHK_TIP}\n\n⚠ {self._dark_field_error}')
                has_error = True
            else:
                parts.append('Dark ✓')
                self.dark_chk.setToolTip(self._DARK_CHK_TIP)
        else:
            self.dark_chk.setToolTip(self._DARK_CHK_TIP)

        if self._white_mode == 'flat' and self._white_field is not None:
            if self._white_field_error:
                parts.append('White ✗')
                has_error = True
            else:
                parts.append('White ✓')
        elif self._white_mode == 'pca':
            if self._pca_components is not None:
                parts.append(f'PCA ✓ (n={self._pca_n_components})')
            else:
                parts.append('PCA …')

        if parts:
            self.corr_status_lbl.setText('  '.join(parts))
        color = '#ff6b6b' if has_error else '#888'
        self.corr_status_lbl.setStyleSheet(f'color: {color}; font-size: 10px;')

        has_file = self._path is not None
        dark_storable  = has_file and self._dark_field is not None and not self._dark_field_error
        white_storable = has_file and (
            (self._white_mode == 'flat' and self._white_field is not None
             and not self._white_field_error)
            or (self._white_mode == 'pca' and self._pca_components is not None)
        )
        self.store_fields_btn.setEnabled(dark_storable or white_storable)

    def _refresh_current_frame(self):
        if self._reader is not None:
            self._show_frame(self.frame_slider.value())

    def _apply_field_correction(self, frame: np.ndarray,
                                data_gain: float = None) -> np.ndarray:
        """
        Return the frame for on-screen display. When dark/white correction is
        active the result is the float32 x-ray transmission (so the user reads
        true transmission values, not the 12-bit-mapped codes that get written
        to disk on export). With no correction the raw frame is returned.
        """
        if self._white_mode == 'pca':
            if self._pca_components is None or self._pca_mean is None:
                return frame
            dark = None
            if self._dark_field is not None and not self._dark_field_error:
                dark = self._dark_field.astype(np.float32)
                if (data_gain is not None and self._dark_field_gain is not None
                        and data_gain != self._dark_field_gain):
                    dark = dark * np.float32(
                        10 ** ((data_gain - self._dark_field_gain) / 20.0))
            return self._apply_pca_correction_frame(frame, dark=dark)

        if self._dark_field is None and self._white_field is None:
            return frame
        if self._dark_field_error or self._white_field_error:
            return frame
        out = _field_correct_float(
            frame, self._dark_field, self._white_field,
            self._dark_field_gain, self._white_field_gain, data_gain,
        )
        return out if out is not None else frame

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
        self._trim_gradient_ticks(cm)

    def _trim_gradient_ticks(self, cm: pg.ColorMap):
        """Reduce the histogram gradient editor to exactly 3 handles (min, mid, max)."""
        try:
            ge = self.imview.getHistogramWidget().item.gradient
            # Sample the colormap at 3 positions
            positions = [0.0, 0.5, 1.0]
            colors = [cm.mapToQColor(p) for p in positions]
            # Remove all existing ticks
            for tick in list(ge.ticks.keys()):
                ge.removeTick(tick, finish=False)
            # Add 3 ticks
            for pos, color in zip(positions, colors):
                ge.addTick(pos, color, finish=False)
            ge.updateGradient()
        except Exception:
            pass

    def _auto_levels(self):
        """Set display levels to the 3rd / 97th percentile of the current display frame."""
        if self._reader is None:
            return
        idx = self.frame_slider.value()
        frame = self._cache.get(idx)
        if frame is None:
            return
        data_gain = (self._gains[idx] if self._gains is not None and idx < len(self._gains)
                     else None)
        display_frame = self._apply_field_correction(frame, data_gain=data_gain)
        lo = float(np.percentile(display_frame, 3))
        hi = float(np.percentile(display_frame, 97))
        if hi <= lo:
            hi = lo + 1
        self.imview.setLevels(lo, hi)

    # ── Config ────────────────────────────────────────────────────────────────
    def _open_export_settings(self):
        dlg = _ExportSettingsDialog(self._export_compression, self._export_imagej,
                                    parent=self, stylesheet=_DARK_STYLE)
        if dlg.exec() == QDialog.DialogCode.Accepted:
            self._export_compression = dlg.selected_compression()
            self._export_imagej      = dlg.selected_imagej()
            self._settings.setValue('export_compression', self._export_compression)
            self._settings.setValue('export_imagej',      self._export_imagej)

    def _open_trigger_settings(self):
        dlg = _TriggerConfigDialog(self._trigger_bit, parent=self, stylesheet=_DARK_STYLE)
        if dlg.exec() == QDialog.DialogCode.Accepted:
            self._trigger_bit = dlg.selected_bit()
            self._settings.setValue('trigger_bit', self._trigger_bit)
            # Re-compute triggers against the currently loaded file if one is open.
            if self._reader is not None:
                self._open_reader()

    # ── TIFF export ───────────────────────────────────────────────────────────
    def _export_tiff(self):
        if self._reader is None:
            return
        try:
            import tifffile  # noqa: F401 — check availability before dialog
        except ImportError:
            QMessageBox.warning(self, 'Missing dependency',
                                'tifffile is required for TIFF export.\n'
                                'Install it with:  pip install tifffile')
            return

        base = os.path.splitext(self._path)[0] if self._path else ''
        cmp  = self._export_compression
        if self._white_mode == 'pca' and self._pca_components is not None:
            if self._dark_field is not None:
                field_suffix = '_dark_pca'
            else:
                field_suffix = '_pca'
        elif self._dark_field is not None and self._white_field is not None:
            field_suffix = '_dark_white'
        elif self._dark_field is not None:
            field_suffix = '_dark'
        elif self._white_field is not None:
            field_suffix = '_white'
        else:
            field_suffix = ''
        cmp_suffix = f'_{cmp.lower()}' if cmp != 'None' else ''
        out_path, _ = QFileDialog.getSaveFileName(
            self, 'Export TIFF stack', base + field_suffix + cmp_suffix + '.tiff',
            'TIFF files (*.tiff *.tif);;All files (*)'
        )
        if not out_path:
            return
        # Ensure compression suffix is in the stem if compression is active
        if cmp != 'None':
            stem, ext = os.path.splitext(out_path)
            if not stem.endswith(cmp_suffix):
                out_path = stem + cmp_suffix + ext

        n = self._reader_len()
        w = self.w_spin.value()
        h = self.h_spin.value()

        meta = {
            'source_file':  os.path.basename(self._path) if self._path else '',
            'pixel_format': self.fmt_combo.currentText(),
            'width':        w,
            'height':       h,
            'n_frames':     n,
        }
        if self._sidecar_fps:
            meta['fps'] = self._sidecar_fps
        if self._sidecar_acq_time:
            meta['acquisition_time'] = self._sidecar_acq_time
        if self._sidecar_notes:
            meta['notes'] = self._sidecar_notes
        if self._timestamps is not None:
            meta['timestamps']     = self._timestamps.tolist()
            meta['timestamp_unit'] = self._ts_unit
        if self._gains is not None:
            meta['gains_dB']          = self._gains.tolist()
        if self._exposures is not None:
            meta['exposures_ms']      = self._exposures.tolist()
        if self._line_statuses is not None:
            meta['line_status_all']   = self._line_statuses.tolist()

        correction = {}
        if self._dark_field is not None:
            dark_folder = self._find_field_folder('dark_field')
            correction['dark_field'] = os.path.basename(dark_folder) if dark_folder else 'loaded'
            if self._dark_field_gain is not None:
                correction['dark_field_gain_dB'] = self._dark_field_gain
        if self._white_mode == 'pca' and self._pca_components is not None:
            correction['white_field_mode'] = 'pca'
            correction['pca_n_components'] = self._pca_n_components
            if self._pca_folder:
                correction['pca_folder'] = os.path.basename(self._pca_folder)
            correction['formula'] = 'transmission = data / pca_background'
        elif self._white_field is not None:
            white_folder = self._find_field_folder('white_field')
            correction['white_field'] = os.path.basename(white_folder) if white_folder else 'loaded'
            if self._white_field_gain is not None:
                correction['white_field_gain_dB'] = self._white_field_gain
        if correction and 'formula' not in correction:
            correction['formula'] = (
                'transmission = (data - dark) / (white - dark)'
                if self._dark_field is not None and self._white_field is not None
                else 'corrected = data - dark' if self._dark_field is not None
                else 'transmission = data / white'
            )
        if correction:
            meta['correction'] = correction

        desc        = json.dumps(meta, indent=2)
        data_bytes  = n * h * w * 2
        use_bigtiff = data_bytes > 4 * 1024 ** 3
        imagej      = self._export_imagej

        # ImageJ's own TIFF format is hard-capped at 4 GB and cannot be BigTIFF.
        # For larger stacks, fall back to a standard BigTIFF (which Fiji opens via
        # Bio-Formats) so the file is actually usable. In that case per-frame
        # factors are embedded per page in the TIFF, so no sidecar is written.
        if imagej and data_bytes > 3.9 * 1024 ** 3:
            imagej      = False
            use_bigtiff = True
            QMessageBox.information(
                self, 'ImageJ mode disabled for large file',
                f'This stack is ~{data_bytes / 1024 ** 3:.1f} GB, which exceeds the '
                '4 GB limit of the ImageJ TIFF format.\n\n'
                'Exporting as a standard BigTIFF instead — it opens in ImageJ/Fiji '
                'via Bio-Formats, and the per-frame transmission factors are embedded '
                'in each page of the TIFF (no sidecar needed).')

        pca_mean  = self._pca_mean       if self._white_mode == 'pca' else None
        pca_comp  = self._pca_components if self._white_mode == 'pca' else None
        worker = TiffExportWorker(
            out_path, self._reader, self._cache, n,
            self._dark_field,  self._dark_field_gain,
            self._white_field if self._white_mode == 'flat' else None,
            self._white_field_gain,
            self._gains, desc, use_bigtiff,
            compression=self._export_compression,
            pixel_bits=_FORMAT_BITS.get(self.fmt_combo.currentText(), 16),
            timestamps=self._timestamps,
            ts_unit=self._ts_unit,
            trigger_t0=self._trigger_t0,
            imagej=imagej,
            pca_mean=pca_mean,
            pca_components=pca_comp,
            pca_n_components=self._pca_n_components,
        )
        self._export_worker = worker

        dlg = _ExportProgressDialog(n, parent=self, stylesheet=_DARK_STYLE)
        dlg.cancel_requested.connect(worker.cancel)

        def on_progress(done, total):
            self.statusBar().showMessage(f'{dlg._phase} frame {done} / {total}…')
            dlg.update_progress(done, total)

        def on_finished(msg):
            self._export_act.setEnabled(True)
            self.statusBar().showMessage(msg)
            dlg.close()

        def on_error(msg):
            self._export_act.setEnabled(True)
            if msg != 'Export cancelled.':
                QMessageBox.warning(self, 'Export error', msg)
            self.statusBar().showMessage(msg)
            dlg.close()

        def on_worker_done():
            self._export_worker = None

        worker.progress.connect(on_progress)
        worker.phase.connect(dlg.set_phase)
        worker.export_done.connect(on_finished)
        worker.export_error.connect(on_error)
        worker.finished.connect(on_worker_done)    # clear ref only after thread exits
        worker.finished.connect(worker.deleteLater)

        self._export_act.setEnabled(False)
        self.statusBar().showMessage(f'Exporting {n} frames…')
        worker.start()
        dlg.show()

    # ── Tools ─────────────────────────────────────────────────────────────────
    # def _launch_viewer(self):
    #     """Launch lucid_viewer.bat via ShellExecute (non-blocking)."""
    #     bat = r'Z:\Price\Software\dcs-team\DataScripts\lucid_viewer.bat'
    #     try:
    #         import subprocess
    #         subprocess.Popen([bat], shell=True)
    #     except Exception as exc:
    #         QMessageBox.warning(self, 'Launch error',
    #                             f'Could not launch viewer:\n{exc}')

    # ── Help ──────────────────────────────────────────────────────────────────
    def _show_manual(self):
        """Open the HTML manual in the system default browser."""
        manual = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              'lucid_viewer_manual.html')
        if not os.path.isfile(manual):
            QMessageBox.warning(self, 'Manual not found',
                                f'Manual file not found:\n{manual}')
            return
        QDesktopServices.openUrl(QUrl.fromLocalFile(manual))

    def _show_about(self):
        """Show an About dialog."""
        QMessageBox.about(
            self,
            'About LucidLabs ATX245 Viewer',
            '<h3>LucidLabs ATX245 Viewer</h3>'
            '<p>A PyQt6 / pyqtgraph viewer for Lucid Vision Labs raw image data.</p>'
            '<p><b>Supported formats:</b> Mono8, Mono10, Mono10p, Mono10Packed, '
            'Mono12, Mono12p, Mono12Packed, Mono16</p>'
            '<p><b>File types:</b> .raw, .tiff/.tif, .mp4/.avi</p>'
            '<hr>'
            '<p style="color:#888;">DCS Team &nbsp;|&nbsp; Built with PyQt6 + pyqtgraph</p>'
        )

    # ── Settings ──────────────────────────────────────────────────────────────
    def _save_settings(self):
        s = self._settings
        s.setValue('geometry',      self.saveGeometry())
        s.setValue('windowState',   self.saveState())
        if self._path:
            s.setValue('last_path', self._path)
        s.setValue('frame_width',   self.w_spin.value())
        s.setValue('frame_height',  self.h_spin.value())
        s.setValue('pixel_format',  self.fmt_combo.currentText())
        s.setValue('colormap',      self.cmap_combo.currentText())
        s.setValue('mask_checked',        self.mask_chk.isChecked())
        s.setValue('dark_field_checked',  self.dark_chk.isChecked())
        s.setValue('white_chk_checked',   self.white_chk.isChecked())
        s.setValue('white_combo_index',   self.white_combo.currentIndex())
        s.setValue('pca_n_components',    self.pca_n_spin.value())
        s.setValue('export_compression',  self._export_compression)
        s.setValue('trigger_bit',         self._trigger_bit)

    def _restore_settings(self):
        s = self._settings
        geom = s.value('geometry')
        if geom:
            self.restoreGeometry(geom)
        state = s.value('windowState')
        if state:
            self.restoreState(state)
        self.w_spin.setValue(   s.value('frame_width',  ATX245_W, type=int))
        self.h_spin.setValue(   s.value('frame_height', ATX245_H, type=int))
        fmt = s.value('pixel_format', 'Mono12p')
        idx = self.fmt_combo.findText(fmt)
        if idx >= 0:
            self.fmt_combo.setCurrentIndex(idx)
        cmap = s.value('colormap', 'grey')
        idx  = self.cmap_combo.findText(cmap)
        if idx >= 0:
            self.cmap_combo.setCurrentIndex(idx)
        self.mask_chk.setChecked(s.value('mask_checked', True, type=bool))
        cmp = s.value('export_compression', 'None')
        if cmp in TIFF_COMPRESSIONS:
            self._export_compression = cmp
        self._trigger_bit    = s.value('trigger_bit',    2,     type=int)
        self._export_imagej  = s.value('export_imagej',  False, type=bool)
        # Restore correction checkboxes without triggering load (no file open yet)
        self.dark_chk.blockSignals(True)
        self.dark_chk.setChecked(s.value('dark_field_checked', False, type=bool))
        self.dark_chk.blockSignals(False)
        # Legacy: old setting stored 0/1/2 in white_combo_index (0=none,1=flat,2=pca)
        # New: white_chk_checked + white_combo_index (0=flat, 1=pca)
        raw_combo = s.value('white_combo_index', 0, type=int)
        if s.contains('white_chk_checked'):
            chk_checked = s.value('white_chk_checked', False, type=bool)
            combo_idx   = raw_combo
        else:
            chk_checked = raw_combo > 0
            combo_idx   = max(0, raw_combo - 1)
        self.white_chk.blockSignals(True)
        self.white_chk.setChecked(chk_checked)
        self.white_chk.blockSignals(False)
        self.white_combo.blockSignals(True)
        self.white_combo.setCurrentIndex(combo_idx)
        self.white_combo.blockSignals(False)
        self.pca_n_spin.setValue(s.value('pca_n_components', 5, type=int))
        pca_visible = chk_checked and (combo_idx == 1)
        self._pca_n_lbl.setVisible(pca_visible)
        self.pca_n_spin.setVisible(pca_visible)
        last = s.value('last_path', '')
        if last:
            self.statusBar().showMessage(f'Last file: {last}')

    def closeEvent(self, event):
        self._stop_play()
        self._save_settings()
        if self._pca_worker is not None:
            self._pca_worker.quit()
            self._pca_worker.wait(2000)
        if self._export_worker is not None:
            self._export_worker.cancel()
            self._export_worker.wait()
        if self._reader is not None:
            try:
                self._reader.close()
            except Exception:
                pass
        super().closeEvent(event)


_DARK_STYLE = _DARK_STYLE_BASE + """
QSlider::groove:horizontal { height: 4px; background: #444; border-radius: 2px; }
QSlider::handle:horizontal { background: #888; border: 1px solid #aaa;
    width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }
QSlider::handle:horizontal:hover { background: #aaa; }
"""

if __name__ == '__main__':
    # Tell Windows to use our AppUserModelID so the taskbar shows our icon
    # instead of the generic Python icon.
    try:
        import ctypes
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(
            'LucidViewer.ATX245.1'
        )
    except Exception:
        pass

    app = QApplication(sys.argv)
    _ico = os.path.join(os.path.dirname(__file__), 'assets', 'projector.ico')
    if os.path.isfile(_ico):
        app.setWindowIcon(QIcon(_ico))
    win = LucidViewer()
    win.show()
    sys.exit(app.exec())
