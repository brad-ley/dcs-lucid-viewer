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
import tempfile
import re
import json
import time
import functools
import numpy as np

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QPushButton, QLabel, QFileDialog, QComboBox,
    QGroupBox, QGridLayout, QSpinBox, QFormLayout,
    QSplitter, QMessageBox, QSlider, QCheckBox, QSizePolicy,
    QDialog, QProgressBar, QDialogButtonBox, QFrame, QMenu,
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

# OrcaFireControl (the Hamamatsu ORCA-Fire app, a sibling project under
# HamamatsuOrcaFire/) writes its own sidecar JSON with lowercase pixel_format
# names ('mono8', 'mono16', 'mono12p') rather than this viewer's PIXEL_FORMATS
# spelling ('Mono8', 'Mono16', 'Mono12p'). frame_byte_count()/unpack_frame()
# key their dicts on the exact PIXEL_FORMATS spelling, so anything read from a
# sidecar or filename needs to go through this first.
def _canonicalize_pixel_format(fmt):
    if not fmt:
        return fmt
    for canonical in PIXEL_FORMATS:
        if canonical.lower() == str(fmt).lower():
            return canonical
    return fmt


def _sqrt_inverse_lut_from_sidecar(sidecar):
    """
    OrcaFireControl's "sqrt encoded" 12-bit Store-as mode packs 16-bit sensor
    counts through a square-root lookup table before writing, so the stored
    codes are NOT linear in counts -- see FrameRecorder.cpp's writeSidecar()
    in the OrcaFireControl repo. When a sidecar carries pack_transform ==
    'sqrt_lut', it also carries the exact inverse table (4096 entries, one
    per possible 12-bit code) needed to undo that, base64-encoded.

    Returns a (4096,) uint16 numpy array -- codes[i] is the reconstructed
    16-bit count for stored code i, ready for RawReader to apply with a
    single fancy-index lookup -- or None if this recording isn't sqrt-encoded
    (which is every recording from any other source, and every OrcaFireControl
    recording using one of its other three Store-as modes).
    """
    if not sidecar or sidecar.get('pack_transform') != 'sqrt_lut':
        return None
    b64 = sidecar.get('sqrt_inverse_lut_base64')
    if not b64:
        return None
    import base64
    return np.frombuffer(base64.b64decode(b64), dtype='<u2')


# ── Pixel-format unpacking ────────────────────────────────────────────────────

def frame_byte_count(fmt: str, w: int, h: int) -> float:
    """Return bytes per frame (may be fractional for packed formats)."""
    fmt = _canonicalize_pixel_format(fmt)
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
    fmt = _canonicalize_pixel_format(fmt)
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

    def __init__(self, path: str, fmt: str, w: int, h: int, inverse_lut=None):
        self.path  = path
        self.fmt   = _canonicalize_pixel_format(fmt)
        self.w     = w
        self.h     = h
        self._mmap = None
        # See _sqrt_inverse_lut_from_sidecar(): non-None only for an
        # OrcaFireControl recording using its sqrt-encoded 12-bit Store-as
        # mode, where the stored codes are not linear in sensor counts.
        self.inverse_lut = inverse_lut
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
        frame = unpack_frame(raw, self.fmt, self.w, self.h)
        if self.inverse_lut is not None:
            # Codes are 0..4095 by construction (12-bit packed), so this is a
            # single vectorized fancy-index lookup -- no per-pixel Python loop.
            frame = self.inverse_lut[frame]
        return frame

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
    Look for frame_data.csv (preferred), timestamps.csv, or an OrcaFireControl
    "<stem>_index.csv" (e.g. recording.raw -> recording_index.csv) in the same
    directory as *path*. Returns (timestamps, unit, gains, exposures,
    line_statuses) where timestamps is a 1-D float64 array of relative times
    in the best-fit unit, unit is a string ('µs', 'ms', 's', 'min'), gains and
    exposures are 1-D float64 arrays or None, and line_statuses is a 1-D
    int64 array or None. Returns (None, '', None, None, None) on failure.
    """
    dir_ = os.path.dirname(path)
    frame_data_path = os.path.join(dir_, 'frame_data.csv')
    index_path = os.path.splitext(path)[0] + '_index.csv'
    csv_path = os.path.join(dir_, 'timestamps.csv')
    if os.path.isfile(frame_data_path):
        csv_path = frame_data_path
    elif os.path.isfile(index_path):
        csv_path = index_path
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

        # OrcaFireControl's index CSV (file_index,framestamp,cam_sec,cam_usec,
        # host_ns) is a known, fixed schema -- handle it directly rather than
        # through the generic column-sniffing heuristics below. Those heuristics
        # pick a time column by testing whether the header CONTAINS any of a
        # list of substrings including the bare letter 't', and 'framestamp'
        # (the camera's frame counter, not a timestamp) matches that before
        # 'host_ns' is ever considered -- which would silently plot the frame
        # counter as if it were a time axis.
        if has_header and 'host_ns' in header_row and 'framestamp' in header_row:
            ns_idx = header_row.index('host_ns')

            # trigger_sent is OrcaFireControl's own trigger marker: a plain
            # 0/1 per frame, NOT a Lucid/Arena-style line_status_all bitmask
            # -- it is a SOFTWARE ESTIMATE (camera-clock elapsed time since
            # this run's first frame compared against the configured OUTPUT
            # TRIGGER DELAY), not a hardware confirmation that a pulse
            # actually reached the wire. See checkOutputTriggerAgainstFrame()
            # and the trigger label's own tooltip in the OrcaFireControl repo
            # for the exact caveat.
            #
            # Fed into line_statuses as 0xFFFF (all 16 bits set) rather than
            # bit 0 specifically: TriggerSlider/_trigger_frames test a single
            # user-configurable bit position (Config > Trigger settings...,
            # default 2) against line_status_all, a concept that only exists
            # for a real Lucid/Arena bitmask. There is no equivalent "which
            # bit" setting for OrcaFireControl's single flag, and this file's
            # loader has no access to self._trigger_bit to target it
            # specifically -- setting every bit means "triggered" reads as
            # true under WHATEVER bit position the user has configured,
            # Lucid recording or not, with no extra plumbing needed here.
            trig_idx = header_row.index('trigger_sent') if 'trigger_sent' in header_row else None

            ns_vals = []
            trig_vals = []   # raw 0/1 per frame, expanded to an edge-only mask below
            for row in rows[1:]:
                if ns_idx < len(row) and row[ns_idx].strip():
                    try:
                        ns_vals.append(float(row[ns_idx]))
                    except ValueError:
                        continue
                    else:
                        trig_val = 0
                        if trig_idx is not None and trig_idx < len(row) and row[trig_idx].strip():
                            try:
                                trig_val = 1 if float(row[trig_idx]) != 0 else 0
                            except ValueError:
                                trig_val = 0
                        trig_vals.append(trig_val)
            if len(ns_vals) >= 2:
                ts = (np.array(ns_vals, dtype=np.float64) - ns_vals[0]) * 1e-9  # -> seconds

                line_statuses = None
                if trig_idx is not None and trig_vals:
                    # trigger_sent is a level, not an event -- it reads 1 from
                    # the estimated trigger frame through the END of the
                    # recording (see frameTriggerSent() in the OrcaFireControl
                    # repo), not just at the moment it fires. Marking every
                    # one of those frames would paint a solid block of ticks
                    # instead of one marker at the trigger -- only the RISING
                    # EDGE (the first 0 -> 1 frame) is the actual event of
                    # interest, so that is all that goes into line_statuses.
                    raw = np.array(trig_vals, dtype=np.int64)
                    was_zero_before = np.concatenate(([True], raw[:-1] == 0))
                    rising_edge = (raw != 0) & was_zero_before
                    line_statuses = np.where(rising_edge, 0xFFFF, 0).astype(np.int64)

                span = ts[-1] - ts[0]
                if span < 0.5:
                    return ts * 1e3, 'ms', None, None, line_statuses
                if span < 120:
                    return ts, 's', None, None, line_statuses
                return ts / 60, 'min', None, None, line_statuses

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


def _auto_pca_n(explained: np.ndarray) -> int:
    """Scree-plot elbow: first i where explained[i] <= 1.5 * explained[i+1]; use i components."""
    for i in range(len(explained) - 1):
        if explained[i] <= 1.5 * explained[i + 1]:
            return max(1, i)
    return max(1, len(explained))


@functools.lru_cache(maxsize=16)
def _gauss_kernel_fft(H, W, sigma):
    """Gaussian blur kernel in rfft2 frequency space, cached by (H, W, sigma).

    The Fourier transform of a Gaussian with std sigma is a Gaussian with std
    1/(2π·sigma) in frequency units.  Building it directly in frequency space
    avoids constructing a spatial kernel altogether and makes the per-frame cost
    O(H·W·log(H·W)) rather than O(H·W·sigma) — ~35× faster for sigma=400."""
    fy = np.fft.fftfreq(H).astype(np.float32)[:, None]   # (H, 1)
    fx = np.fft.rfftfreq(W).astype(np.float32)[None, :]  # (1, W//2+1)
    return np.exp(np.float32(-2.0 * np.pi**2) * np.float32(sigma)**2
                  * (fy**2 + fx**2))                      # (H, W//2+1) float32


def _gaussian_blur_fft(f_2d, sigma):
    """Blur a 2-D float32 array with a Gaussian of the given sigma via FFT."""
    import scipy.fft as sfft
    H, W   = f_2d.shape
    kernel = _gauss_kernel_fft(H, W, float(sigma))
    F      = sfft.rfft2(f_2d.astype(np.float32))
    return sfft.irfft2(F * kernel, s=(H, W)).astype(np.float32)


def _blur_downsample_factor(blur_sigma):
    """Auto-compute block-average stride from blur sigma.
    Each sigma/50 pixels of blur → 1 bin pixel is sufficient to capture the mode.
    Capped at 16 to avoid rounding away real structure."""
    return min(16, max(1, int(blur_sigma // 50)))


def _blur_and_bin(f_2d, sigma, n_low):
    """Blur f_2d and block-average downsample to match n_low pixels.

    B is derived from the stored shape of mu_low so training and application
    always agree without passing the factor explicitly.
    Returns a float32 1-D vector of length n_low.
    """
    H, W = f_2d.shape
    f_blur = _gaussian_blur_fft(f_2d, sigma)
    if n_low == H * W:
        return f_blur.ravel().astype(np.float32)
    B = max(1, round(((H * W) / n_low) ** 0.5))
    H_low, W_low = H // B, W // B
    f_crop = f_blur[:H_low * B, :W_low * B]
    return f_crop.reshape(H_low, B, W_low, B).mean(axis=(1, 3)).ravel().astype(np.float32)


def _compute_pca_from_frames(frames, n_components=20, blur_sigma=0.0, progress_cb=None):
    """
    PCA on a list of (H, W) float32 frames using the frame-space covariance
    trick (K << N).  All heavy work is a small number of BLAS sgemm calls.
    Returns (mean, components, explained, mean_low, components_low).
      mean/components/explained: full-resolution PCA (H*W,) float32
      mean_low/components_low:   Gaussian-blurred PCA for dual-resolution
                                 projection (None when blur_sigma <= 0)
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

    mu_low = None
    V_low  = None

    # ── Blur path first: build X_low, project, free before building X ─────────
    # Downsample by B after blurring — modes of variation are purely macro-scale,
    # so N/B² pixels carry the same information at a fraction of the memory cost.
    # X_low and X never coexist, keeping peak memory at one (K, N_low) + one (K, N).
    if blur_sigma > 0.0:
        B      = _blur_downsample_factor(blur_sigma)
        H_low  = H // B
        W_low  = W // B
        N_low  = H_low * W_low
        X_low  = np.empty((K, N_low), dtype=np.float32)
        for i, f in enumerate(frames):
            _prog(f'PCA (blur): blurring frame {i + 1}/{K}…')
            f_blur = _gaussian_blur_fft(f, blur_sigma)
            if B > 1:
                f_crop = f_blur[:H_low * B, :W_low * B]
                X_low[i] = f_crop.reshape(H_low, B, W_low, B).mean(axis=(1, 3)).ravel()
            else:
                X_low[i] = f_blur.ravel()
        mu_low  = X_low.mean(axis=0)
        X_low  -= mu_low
        _prog('PCA (blur): frame covariance…')
        C_low = X_low.dot(X_low.T).astype(np.float64) / max(K - 1, 1)
        _prog('PCA (blur): eigendecomposition…')
        ev_low, evec_low = np.linalg.eigh(C_low)
        del C_low
        evec_low = evec_low[:, np.argsort(ev_low)[::-1]]
        _prog(f'PCA (blur): projecting {n_components} components…')
        E_low = np.ascontiguousarray(
            evec_low[:, :n_components].T.astype(np.float32))
        del evec_low
        V_low = E_low.dot(X_low)          # project onto blurred data
        del X_low, E_low
        norms_low = np.linalg.norm(V_low, axis=1, keepdims=True)
        np.maximum(norms_low, 1e-10, out=norms_low)
        V_low /= norms_low

    # ── Main (non-blur) PCA — X_low already freed ────────────────────────────
    _prog(f'PCA: stacking {K} frames…')
    X = np.empty((K, N), dtype=np.float32)
    for i, f in enumerate(frames):
        X[i] = f.ravel()

    _prog('PCA: centering…')
    mu32 = X.mean(axis=0)
    X   -= mu32

    _prog('PCA: computing frame covariance…')
    C = X.dot(X.T).astype(np.float64) / max(K - 1, 1)

    _prog('PCA: eigendecomposition…')
    eigenvalues, eigenvectors = np.linalg.eigh(C)
    del C
    order        = np.argsort(eigenvalues)[::-1]
    eigenvalues  = eigenvalues[order]
    eigenvectors = eigenvectors[:, order]

    _prog(f'PCA: projecting {n_components} components…')
    evecs = np.ascontiguousarray(
        eigenvectors[:, :n_components].T.astype(np.float32))
    del eigenvectors
    V = evecs.dot(X)
    del X, evecs
    _prog('PCA: normalizing components…')

    norms = np.linalg.norm(V, axis=1, keepdims=True)
    np.maximum(norms, 1e-10, out=norms)
    V /= norms
    components = V

    total_var = float(max(eigenvalues[eigenvalues > 0].sum(), 1e-12))
    explained = (eigenvalues[:n_components] / total_var).astype(np.float32)
    _prog('PCA done.')

    return mu32, components, explained, mu_low, V_low


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
                 pca_mean=None, pca_components=None, pca_n_components=5,
                 pca_mean_low=None, pca_components_low=None,
                 pca_blur_enabled=False, pca_blur_sigma=400,
                 pca_cell_gain=None):
        super().__init__()
        self._out_path            = out_path
        self._reader              = reader
        self._cache               = dict(cache)
        self._n                   = n
        self._dark_field          = dark_field
        self._dark_field_gain     = dark_field_gain
        self._white_field         = white_field
        self._white_field_gain    = white_field_gain
        self._gains               = gains
        self._desc                = desc
        self._bigtiff             = bigtiff
        self._compression         = compression
        self._pixel_bits          = pixel_bits
        self._timestamps          = timestamps
        self._ts_unit             = ts_unit
        self._trigger_t0          = trigger_t0
        self._imagej              = imagej
        self._pca_mean            = pca_mean
        self._pca_components      = pca_components
        self._pca_n_components    = pca_n_components
        self._pca_mean_low        = pca_mean_low
        self._pca_components_low  = pca_components_low
        self._pca_blur_enabled    = pca_blur_enabled
        self._pca_blur_sigma      = pca_blur_sigma
        self._pca_cell_gain       = pca_cell_gain
        self._cancelled           = False

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
            if (data_gain is not None and self._pca_cell_gain is not None
                    and data_gain != self._pca_cell_gain):
                gain_scale = np.float32(10 ** ((data_gain - self._pca_cell_gain) / 20.0))
                mu = mu * gain_scale
            else:
                gain_scale = None
            if (self._pca_blur_enabled
                    and self._pca_mean_low is not None
                    and self._pca_components_low is not None):
                n_low  = min(n, self._pca_components_low.shape[0])
                mu_low = (self._pca_mean_low * gain_scale
                          if gain_scale is not None else self._pca_mean_low)
                f_low  = _blur_and_bin(f, self._pca_blur_sigma, mu_low.shape[0])
                d_low  = f_low - mu_low
                coeffs = self._pca_components_low[:n_low] @ d_low
                bg     = (mu + comp[:n_low].T @ coeffs).reshape(frame.shape)
            else:
                d_c    = f.ravel() - mu
                coeffs = comp[:n] @ d_c
                bg     = (mu + comp[:n].T @ coeffs).reshape(frame.shape)
            bg = np.where(bg >= np.float32(1.0), bg, np.float32(1.0))
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


class GifExportWorker(QThread):
    progress     = pyqtSignal(int, int)   # frames_done, total
    export_done  = pyqtSignal(str)
    export_error = pyqtSignal(str)

    def __init__(self, out_path, reader, cache, n, step, scale, fps,
                 levels,
                 dark_field=None, dark_field_gain=None,
                 white_field=None, white_field_gain=None,
                 pca_mean=None, pca_components=None, pca_n_components=5,
                 pca_mean_low=None, pca_components_low=None,
                 pca_blur_enabled=False, pca_blur_sigma=400,
                 pca_cell_gain=None,
                 gains=None, parent=None):
        super().__init__(parent)
        self._out_path            = out_path
        self._reader              = reader
        self._cache               = dict(cache)
        self._n                   = n
        self._step                = step
        self._scale               = scale
        self._fps                 = fps
        self._levels              = levels
        self._dark_field          = dark_field
        self._dark_field_gain     = dark_field_gain
        self._white_field         = white_field
        self._white_field_gain    = white_field_gain
        self._pca_mean            = pca_mean
        self._pca_components      = pca_components
        self._pca_n_components    = pca_n_components
        self._pca_mean_low        = pca_mean_low
        self._pca_components_low  = pca_components_low
        self._pca_blur_enabled    = pca_blur_enabled
        self._pca_blur_sigma      = pca_blur_sigma
        self._pca_cell_gain       = pca_cell_gain
        self._gains               = gains
        self._cancelled           = False

    def cancel(self):
        self._cancelled = True

    def _corrected_float(self, frame, data_gain):
        if self._pca_mean is not None and self._pca_components is not None:
            mu   = self._pca_mean
            comp = self._pca_components
            n    = min(self._pca_n_components, comp.shape[0])
            f    = frame.astype(np.float32)
            dark = self._dark_field
            if dark is not None and data_gain is not None and self._dark_field_gain is not None:
                dark = dark * np.float32(10 ** ((data_gain - self._dark_field_gain) / 20.0))
            if dark is not None:
                f = f - dark
            if (data_gain is not None and self._pca_cell_gain is not None
                    and data_gain != self._pca_cell_gain):
                gain_scale = np.float32(10 ** ((data_gain - self._pca_cell_gain) / 20.0))
                mu = mu * gain_scale
            else:
                gain_scale = None
            if (self._pca_blur_enabled
                    and self._pca_mean_low is not None
                    and self._pca_components_low is not None):
                n_low  = min(n, self._pca_components_low.shape[0])
                mu_low = (self._pca_mean_low * gain_scale
                          if gain_scale is not None else self._pca_mean_low)
                f_low  = _blur_and_bin(f, self._pca_blur_sigma, mu_low.shape[0])
                d_low  = f_low - mu_low
                coeffs = self._pca_components_low[:n_low] @ d_low
                bg     = (mu + comp[:n_low].T @ coeffs).reshape(frame.shape)
            else:
                d_c    = f.ravel() - mu
                coeffs = comp[:n] @ d_c
                bg     = (mu + comp[:n].T @ coeffs).reshape(frame.shape)
            bg_floor = np.maximum(mu.reshape(frame.shape) * np.float32(0.5), np.float32(1.0))
            np.maximum(bg, bg_floor, out=bg)
            return np.clip(f / bg, np.float32(0.0), np.float32(10.0))
        return _field_correct_float(
            frame, self._dark_field, self._white_field,
            self._dark_field_gain, self._white_field_gain, data_gain,
        )

    def run(self):
        try:
            from PIL import Image
        except ImportError:
            self.export_error.emit('Pillow (PIL) not installed — cannot export GIF.\n'
                                   'Install with:  pip install pillow')
            return
        lo, hi = self._levels
        span   = max(float(hi - lo), 1e-6)
        indices = list(range(0, self._n, self._step))
        if not indices:
            self.export_error.emit('No frames to export.')
            return
        n_out = len(indices)
        frames_pil = []
        for done, idx in enumerate(indices):
            if self._cancelled:
                self.export_error.emit('GIF export cancelled.')
                return
            frame = self._cache[idx] if idx in self._cache else self._reader[idx]
            data_gain = (float(self._gains[idx])
                         if self._gains is not None and idx < len(self._gains) else None)
            corr = self._corrected_float(frame, data_gain)
            display = corr if corr is not None else frame.astype(np.float32)
            u8 = np.clip((display - lo) / span * 255.0, 0.0, 255.0).astype(np.uint8)
            img = Image.fromarray(u8)
            if abs(self._scale - 1.0) > 0.001:
                new_w = max(1, int(u8.shape[1] * self._scale))
                new_h = max(1, int(u8.shape[0] * self._scale))
                img = img.resize((new_w, new_h), Image.LANCZOS)
            frames_pil.append(img.convert('P', dither=0, palette=Image.Palette.ADAPTIVE))
            self.progress.emit(done + 1, n_out)
        if not frames_pil:
            self.export_error.emit('No frames could be loaded.')
            return
        try:
            duration_ms = max(1, int(1000.0 / max(self._fps, 0.01)))
            frames_pil[0].save(
                self._out_path,
                save_all=True, append_images=frames_pil[1:],
                loop=0, duration=duration_ms, optimize=False,
            )
            self.export_done.emit(
                f'GIF exported ({len(frames_pil)} frames) → {os.path.basename(self._out_path)}')
        except Exception as exc:
            self.export_error.emit(str(exc))


class PcaComputeWorker(QThread):
    pca_done     = pyqtSignal(object, object, object, object, object)  # mean, comp, expl, mu_low, comp_low
    pca_error    = pyqtSignal(str)
    pca_progress = pyqtSignal(str)                     # status message for UI

    def __init__(self, folder, fmt, w, h, n_components=20, blur_sigma=0, parent=None):
        super().__init__(parent)
        self._folder       = folder
        self._fmt          = fmt
        self._w            = w
        self._h            = h
        self._n_components = n_components
        self._blur_sigma   = blur_sigma

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
            mu, comp, expl, mu_low, comp_low = _compute_pca_from_frames(
                frames, n_store, blur_sigma=self._blur_sigma,
                progress_cb=lambda msg: self.pca_progress.emit(msg))
            self.pca_done.emit(mu, comp, expl, mu_low, comp_low)
        except Exception as exc:
            self.pca_error.emit(str(exc))

    def _load_frames(self, max_frames=150):
        """Load up to max_frames evenly-sampled frames from folder as float32 arrays."""
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
        # Memory cap: keep stacked (K, H*W) float32 array under 1.5 GB.
        # blur path creates a second identical array, so 1.5 GB each → ~3 GB peak.
        MAX_STACK_BYTES = int(1.5 * 1024 ** 3)
        bytes_per_frame = max(1, self._w * self._h * 4)
        mem_cap = max(4, MAX_STACK_BYTES // bytes_per_frame)
        max_frames = min(max_frames, mem_cap)
        frames = []
        n = len(files)
        for i, fname in enumerate(files):
            if len(frames) >= max_frames:
                break
            fpath = os.path.join(self._folder, fname)
            ext   = os.path.splitext(fname)[1].lower()
            try:
                if ext == '.raw':
                    sidecar, _ = _load_sidecar(fpath)
                    fmt = (sidecar.get('pixel_format') or sidecar.get('pixelformat')
                           or self._fmt)
                    w = int(sidecar.get('width',  self._w))
                    h = int(sidecar.get('height', self._h))
                    r = RawReader(fpath, fmt, w, h,
                                  inverse_lut=_sqrt_inverse_lut_from_sidecar(sidecar))
                    n_file = len(r)
                    budget = max_frames - len(frames)
                    if n_file <= budget:
                        indices = range(n_file)
                    else:
                        indices = [int(k * n_file / budget) for k in range(budget)]
                    self.pca_progress.emit(
                        f'Loading white-field frames ({i + 1}/{n})'
                        + (f' [{len(indices)}/{n_file}]' if len(indices) < n_file else '') + '…')
                    for j in indices:
                        frames.append(r[j].astype(np.float32))
                    r.close()
                elif ext in ('.tiff', '.tif'):
                    import tifffile
                    data = tifffile.imread(fpath)
                    if data.ndim == 2:
                        frames.append(data.astype(np.float32))
                    else:
                        budget = max_frames - len(frames)
                        n_file = data.shape[0]
                        if n_file <= budget:
                            indices = range(n_file)
                        else:
                            indices = [int(k * n_file / budget) for k in range(budget)]
                        self.pca_progress.emit(
                            f'Loading white-field frames ({i + 1}/{n})'
                            + (f' [{len(indices)}/{n_file}]' if len(indices) < n_file else '') + '…')
                        for j in indices:
                            frames.append(data[j].astype(np.float32))
            except Exception as e:
                print(f'[pca] error loading {fname}: {e}')
        return frames


class CellAdaptWorker(QThread):
    """Adapt master PCA eigenvectors for a new sample cell without rerunning SVD.

    Algorithm:
      T_cell        = cell_mean / master_mean              (native-res scaling)
      E_high_scaled = E_high * T_cell                      (scaled, non-orthogonal)
      U, _, Vt      = svd(E_high_scaled)                   (thin SVD)
      E_high_cell   = Vt                                   (orthonormal rows; U is the component rotation)
      T_cell_low    = blur(cell_mean) / master_mean_low    (blurred-space scaling)
      E_low_scaled  = E_low * T_cell_low
      E_low_aligned = U.T @ E_low_scaled                   (same rotation → preserves correspondence)
      Q_low         = E_low_aligned / row_norms            (unit-norm low-res analysis basis)
    """

    adapt_done     = pyqtSignal(object, object, object, object)  # cell_mean, E_high_cell, mu_low_cell, Q_low
    adapt_progress = pyqtSignal(str)

    def __init__(self, folder, fmt, w, h,
                 master_mean, master_components,
                 master_mean_low, master_components_low,
                 blur_sigma=0, parent=None):
        super().__init__(parent)
        self._folder                 = folder
        self._fmt                    = fmt
        self._w                      = w
        self._h                      = h
        self._master_mean            = master_mean
        self._master_components      = master_components
        self._master_mean_low        = master_mean_low
        self._master_components_low  = master_components_low
        self._blur_sigma             = blur_sigma

    def run(self):
        try:
            # Reuse PcaComputeWorker's loader by creating a minimal instance just to call _load_frames
            loader = PcaComputeWorker(self._folder, self._fmt, self._w, self._h)
            loader.pca_progress = self.adapt_progress  # forward progress messages
            self.adapt_progress.emit('Cell adaptation: loading frames…')
            frames = loader._load_frames(max_frames=50)
            if not frames:
                self.adapt_done.emit(None, None, None, None)
                return

            self.adapt_progress.emit(f'Cell adaptation: averaging {len(frames)} frames…')
            stack = np.stack([f.astype(np.float32) for f in frames], axis=0)
            cell_mean_2d = stack.mean(axis=0)                           # (H, W)
            cell_mean    = cell_mean_2d.ravel()                         # (H*W,)

            mu_master = np.maximum(self._master_mean, np.float32(1e-3))
            T_cell    = cell_mean / mu_master                           # (H*W,)

            E_high      = self._master_components                       # (n, H*W)
            E_high_scaled = (E_high * T_cell).astype(np.float32)        # (n, H*W)

            # Re-orthogonalize via thin SVD so the projection is a proper orthogonal
            # projection rather than an oblique one.  Vt rows are orthonormal in
            # pixel space; U encodes the rotation applied to the component ordering
            # and is reused below to keep the low-res basis in correspondence.
            self.adapt_progress.emit('Cell adaptation: re-orthogonalizing components…')
            U, _, E_high_cell_vt = np.linalg.svd(E_high_scaled, full_matrices=False)
            E_high_cell = np.ascontiguousarray(E_high_cell_vt.astype(np.float32))  # (n, H*W)

            if (self._blur_sigma > 0
                    and self._master_mean_low is not None
                    and self._master_components_low is not None):
                self.adapt_progress.emit('Cell adaptation: blurring cell mean…')
                # mu_low_cell must be in the same downsampled space as master_mean_low
                n_low       = self._master_mean_low.shape[0]
                mu_low_cell = _blur_and_bin(cell_mean_2d, self._blur_sigma, n_low)
                mu_master_low = np.maximum(self._master_mean_low, np.float32(1e-3))
                T_cell_low    = mu_low_cell / mu_master_low             # (N_low,)
                E_low_scaled  = self._master_components_low * T_cell_low  # (n, N_low)

                # Apply the same component-space rotation as E_high_cell so that
                # Q_low[i] and E_high_cell[i] still represent the same beam mode.
                self.adapt_progress.emit('Cell adaptation: aligning low-res components…')
                E_low_aligned = (U.T @ E_low_scaled).astype(np.float32)  # (n, N_low)
                norms_low = np.linalg.norm(E_low_aligned, axis=1, keepdims=True)
                np.maximum(norms_low, np.float32(1e-10), out=norms_low)
                Q_low = np.ascontiguousarray((E_low_aligned / norms_low).astype(np.float32))  # (n, N_low)

                self.adapt_done.emit(cell_mean, E_high_cell, mu_low_cell, Q_low)
            else:
                self.adapt_done.emit(cell_mean, E_high_cell, None, None)
        except Exception as exc:
            self.adapt_progress.emit(f'Cell adaptation error: {exc}')
            self.adapt_done.emit(None, None, None, None)


class FieldLoadWorker(QThread):
    """Load a single field correction frame from a folder in a background thread.
    Prefers files with 'mean' in the name (precomputed by acquisition software)."""
    field_done = pyqtSignal(object, str, str)   # (ndarray|None, folder, error)

    def __init__(self, folder, fmt, w, h, parent=None):
        super().__init__(parent)
        self._folder = folder
        self._fmt    = fmt
        self._w      = w
        self._h      = h

    def run(self):
        try:
            frame = self._load_frame()
            if frame is None:
                self.field_done.emit(None, self._folder, 'no file found')
            else:
                self.field_done.emit(frame, self._folder, '')
        except Exception as exc:
            self.field_done.emit(None, self._folder, str(exc))

    def _load_frame(self):
        try:
            all_files = sorted(os.listdir(self._folder))
        except OSError:
            return None
        raw_files  = [f for f in all_files if os.path.splitext(f)[1].lower() == '.raw']
        tiff_files = [f for f in all_files
                      if os.path.splitext(f)[1].lower() in ('.tiff', '.tif')]
        files = raw_files if raw_files else tiff_files
        if not files:
            return None
        mean_files = [f for f in files if 'mean' in f.lower()]
        target = mean_files[0] if mean_files else files[0]
        fpath  = os.path.join(self._folder, target)
        ext    = os.path.splitext(target)[1].lower()
        try:
            if ext == '.raw':
                sidecar, _ = _load_sidecar(fpath)
                fmt = (sidecar.get('pixel_format') or sidecar.get('pixelformat')
                       or sidecar.get('PixelFormat') or self._fmt)
                w = int(sidecar.get('width',  self._w))
                h = int(sidecar.get('height', self._h))
                r = RawReader(fpath, fmt, w, h,
                              inverse_lut=_sqrt_inverse_lut_from_sidecar(sidecar))
                frame = r[0].astype(np.float32)
                r.close()
                return frame
            elif ext in ('.tiff', '.tif'):
                import tifffile
                data = tifffile.imread(fpath)
                return data.astype(np.float32) if data.ndim == 2 else data[0].astype(np.float32)
        except Exception as e:
            print(f'[field] error loading {target}: {e}')
            return None


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


class _GifExportDialog(QDialog):
    """Settings dialog for GIF export: step, scale, fps."""

    def __init__(self, n_frames, w, h, parent=None, stylesheet=''):
        super().__init__(parent)
        self.setWindowTitle('Export GIF…')
        self.setMinimumWidth(320)
        if stylesheet:
            self.setStyleSheet(stylesheet)
        layout = QVBoxLayout(self)
        layout.setSpacing(8)
        layout.setContentsMargins(12, 12, 12, 12)

        form = QFormLayout()
        form.setLabelAlignment(Qt.AlignmentFlag.AlignRight)

        self._step_spin = QSpinBox()
        self._step_spin.setRange(1, max(1, n_frames))
        self._step_spin.setValue(1)
        self._step_spin.setSuffix('  frame(s)')
        form.addRow('Export every:', self._step_spin)

        self._scale_spin = QSpinBox()
        self._scale_spin.setRange(5, 100)
        self._scale_spin.setSingleStep(5)
        self._scale_spin.setValue(50)
        self._scale_spin.setSuffix(' %')
        form.addRow('Spatial scale:', self._scale_spin)

        self._fps_spin = QSpinBox()
        self._fps_spin.setRange(1, 60)
        self._fps_spin.setValue(10)
        self._fps_spin.setSuffix(' fps')
        form.addRow('Playback FPS:', self._fps_spin)

        layout.addLayout(form)

        self._est_lbl = QLabel()
        self._est_lbl.setStyleSheet('color: #888; font-size: 10px;')
        layout.addWidget(self._est_lbl)

        self._n_frames = n_frames
        self._w        = w
        self._h        = h
        self._step_spin.valueChanged.connect(self._update_estimate)
        self._scale_spin.valueChanged.connect(self._update_estimate)
        self._update_estimate()

        btns = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok |
                                QDialogButtonBox.StandardButton.Cancel)
        btns.accepted.connect(self.accept)
        btns.rejected.connect(self.reject)
        layout.addWidget(btns)
        self.adjustSize()

    def _update_estimate(self):
        step  = self._step_spin.value()
        scale = self._scale_spin.value() / 100.0
        n_out = max(1, (self._n_frames + step - 1) // step)
        ow    = max(1, int(self._w * scale))
        oh    = max(1, int(self._h * scale))
        est_mb = n_out * ow * oh * 0.25 / 1024 / 1024
        self._est_lbl.setText(
            f'≈ {n_out} frames  ·  {ow}×{oh} px  ·  ~{est_mb:.1f} MB uncompressed')

    @property
    def step(self):
        return self._step_spin.value()

    @property
    def scale(self):
        return self._scale_spin.value() / 100.0

    @property
    def fps(self):
        return self._fps_spin.value()


class _PcaSettingsDialog(QDialog):
    """PCA-specific settings: Gaussian-blur dual-resolution projection and universal mode."""

    def __init__(self, blur_enabled, blur_sigma,
                 universal_enabled=False, master_folder='', master_folder_mode='',
                 find_fn=None, start_dir='',
                 parent=None, stylesheet=''):
        super().__init__(parent)
        self.setWindowTitle('PCA Settings')
        self.setMinimumWidth(380)
        if stylesheet:
            self.setStyleSheet(stylesheet)
        self._master_folder      = master_folder
        self._master_folder_mode = master_folder_mode
        self._find_fn            = find_fn
        self._start_dir          = start_dir

        layout = QVBoxLayout(self)
        layout.setSpacing(8)
        layout.setContentsMargins(12, 12, 12, 12)

        # ── Gaussian blur ──────────────────────────────────────────────────────
        self._blur_chk = QCheckBox('Enable Gaussian blur for temporal weight projection')
        self._blur_chk.setChecked(blur_enabled)
        layout.addWidget(self._blur_chk)

        blur_note = QLabel(
            'Blurs each sample frame before projecting onto the low-resolution PCA\n'
            'basis to estimate beam weights without sample contamination.\n'
            'Requires a second PCA pass during white-field computation.')
        blur_note.setWordWrap(True)
        blur_note.setStyleSheet('color: #888; font-size: 10px;')
        layout.addWidget(blur_note)

        form = QFormLayout()
        form.setLabelAlignment(Qt.AlignmentFlag.AlignRight)
        self._sigma_spin = QSpinBox()
        self._sigma_spin.setRange(10, 2000)
        self._sigma_spin.setSingleStep(10)
        self._sigma_spin.setSuffix(' px')
        self._sigma_spin.setValue(blur_sigma)
        self._sigma_spin.setEnabled(blur_enabled)
        form.addRow('Blur sigma:', self._sigma_spin)
        layout.addLayout(form)

        self._blur_chk.toggled.connect(self._sigma_spin.setEnabled)

        # ── Separator ─────────────────────────────────────────────────────────
        sep = QFrame()
        sep.setFrameShape(QFrame.Shape.HLine)
        sep.setFrameShadow(QFrame.Shadow.Sunken)
        layout.addWidget(sep)

        # ── Universal mode ────────────────────────────────────────────────────
        self._universal_chk = QCheckBox('Universal mode (master no-cell basis)')
        self._universal_chk.setChecked(universal_enabled)
        layout.addWidget(self._universal_chk)

        univ_note = QLabel(
            'Uses a pre-computed master PCA from a cell-free white field.\n'
            'Eigenvectors are adapted per-experiment via cell-mean scaling + re-orthogonalization\n'
            '— no SVD recomputation needed for each new cell.')
        univ_note.setWordWrap(True)
        univ_note.setStyleSheet('color: #888; font-size: 10px;')
        layout.addWidget(univ_note)

        self._master_lbl = QLabel()
        self._master_lbl.setStyleSheet('color: #aaa; font-size: 10px;')
        layout.addWidget(self._master_lbl)

        btn_row = QHBoxLayout()
        btn_row.setSpacing(6)
        self._choose_btn = QPushButton('Choose Folder…')
        self._choose_btn.setFixedHeight(24)
        self._choose_btn.clicked.connect(self._choose_folder)
        self._find_btn = QPushButton('Find')
        self._find_btn.setFixedHeight(24)
        self._find_btn.setCheckable(True)
        self._find_btn.clicked.connect(self._do_find)
        btn_row.addWidget(self._choose_btn)
        btn_row.addWidget(self._find_btn)
        btn_row.addStretch()
        layout.addLayout(btn_row)

        self._universal_chk.toggled.connect(self._on_universal_toggled)
        self._on_universal_toggled(universal_enabled)
        self._refresh_master_label()

        btns = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok |
                                QDialogButtonBox.StandardButton.Cancel)
        btns.accepted.connect(self.accept)
        btns.rejected.connect(self.reject)
        layout.addWidget(btns)
        self.adjustSize()

    def _on_universal_toggled(self, enabled):
        self._choose_btn.setEnabled(enabled)
        self._find_btn.setEnabled(enabled)

    def _refresh_master_label(self):
        folder = self._master_folder
        mode   = self._master_folder_mode
        if folder:
            name   = os.path.basename(folder)
            suffix = ' (manually chosen)' if mode == 'manual' else ' (auto-found)' if mode == 'auto' else ''
            self._master_lbl.setText(f'Master: {name}{suffix}')
            self._master_lbl.setToolTip(folder)
        else:
            self._master_lbl.setText('Master: (not set)')
            self._master_lbl.setToolTip('No master folder set')
        self._find_btn.setChecked(self._master_folder_mode == 'auto')
        choose_text = '✓ Choose Folder…' if self._master_folder_mode == 'manual' else 'Choose Folder…'
        self._choose_btn.setText(choose_text)

    def _choose_folder(self):
        folder = QFileDialog.getExistingDirectory(
            self, 'Select master white field folder (clean, no cell)', self._start_dir)
        if not folder:
            return
        self._master_folder      = folder
        self._master_folder_mode = 'manual'
        self._refresh_master_label()

    def _do_find(self):
        if self._find_fn is None:
            self._find_btn.setChecked(False)
            return
        folder = self._find_fn()
        if not folder:
            self._find_btn.setChecked(False)
            QMessageBox.warning(self, 'Master PCA',
                                'No *_master* folder found near the current file.')
            return
        self._master_folder      = folder
        self._master_folder_mode = 'auto'
        self._refresh_master_label()

    @property
    def blur_enabled(self):
        return self._blur_chk.isChecked()

    @property
    def blur_sigma(self):
        return self._sigma_spin.value()

    @property
    def universal_enabled(self):
        return self._universal_chk.isChecked()

    @property
    def master_folder(self):
        return self._master_folder

    @property
    def master_folder_mode(self):
        return self._master_folder_mode


# ── PCA eigenvector viewer ────────────────────────────────────────────────────

class _PcaTemporalWeightWorker(QThread):
    """Compute per-frame projection coefficients for ALL PCA components in one pass."""
    progress = pyqtSignal(int)
    done     = pyqtSignal(object)   # np.ndarray shape (n_components, n_frames)
    error    = pyqtSignal(str)

    def __init__(self, components, mean, reader, parent=None):
        super().__init__(parent)
        self._comps  = components.astype(np.float32)   # (n_comp, H*W)
        self._mean   = mean.astype(np.float32)          # (H*W,)
        self._reader = reader

    def run(self):
        try:
            n_frames = len(self._reader)
            n_comp   = self._comps.shape[0]
            coeffs   = np.empty((n_comp, n_frames), dtype=np.float32)
            report_every = max(1, n_frames // 100)
            for i in range(n_frames):
                frame      = self._reader[i].astype(np.float32).ravel()
                coeffs[:, i] = self._comps @ (frame - self._mean)
                if i % report_every == 0:
                    self.progress.emit(int(100 * i / n_frames))
            self.progress.emit(100)
            self.done.emit(coeffs)
        except Exception as exc:
            self.error.emit(str(exc))


class _PcaEigenvectorDialog(QDialog):
    """Non-modal dialog: browse PCA eigenvector images and plot temporal weights."""

    def __init__(self, components, mean, shape, explained,
                 reader, timestamps, ts_unit,
                 parent=None, stylesheet=''):
        super().__init__(parent)
        self.setWindowTitle('PCA Eigenvector Viewer')
        self.setMinimumSize(720, 560)
        if stylesheet:
            self.setStyleSheet(stylesheet)

        self._components    = components   # (n, H*W) float32
        self._mean          = mean         # (H*W,)  float32
        self._shape         = shape        # (H, W)
        self._explained     = explained    # (n,) float32 or None
        self._reader        = reader
        self._timestamps    = timestamps   # (n_frames,) float64 or None
        self._ts_unit       = ts_unit or ''
        self._weight_worker = None
        self._all_coeffs    = None   # (n_components, n_frames) once computed
        self._current_idx   = 0
        n = components.shape[0]

        layout = QVBoxLayout(self)
        layout.setSpacing(6)
        layout.setContentsMargins(10, 10, 10, 10)

        # ── eigenvector image ─────────────────────────────────────────────────
        self._img_view = pg.ImageView()
        self._img_view.ui.roiBtn.hide()
        self._img_view.ui.menuBtn.hide()
        self._img_view.setMinimumHeight(280)
        layout.addWidget(self._img_view)

        # ── component slider ──────────────────────────────────────────────────
        slider_row = QHBoxLayout()
        slider_row.addWidget(QLabel('Component:'))
        self._slider = QSlider(Qt.Orientation.Horizontal)
        self._slider.setRange(0, n - 1)
        self._slider.setValue(0)
        slider_row.addWidget(self._slider, 1)
        self._comp_lbl = QLabel()
        self._comp_lbl.setMinimumWidth(130)
        slider_row.addWidget(self._comp_lbl)
        layout.addLayout(slider_row)

        # ── buttons ───────────────────────────────────────────────────────────
        btn_row = QHBoxLayout()
        self._mean_btn = QPushButton('Show mean image')
        self._mean_btn.setCheckable(True)
        self._weight_btn  = QPushButton('Show temporal weights')
        self._weight_prog = QProgressBar()
        self._weight_prog.setVisible(False)
        self._weight_prog.setMaximumWidth(120)
        self._weight_prog.setTextVisible(False)
        btn_row.addWidget(self._mean_btn)
        btn_row.addStretch()
        btn_row.addWidget(self._weight_prog)
        btn_row.addWidget(self._weight_btn)
        layout.addLayout(btn_row)

        # ── temporal weights plot (hidden until computed) ─────────────────────
        self._plot = pg.PlotWidget()
        self._plot.setLabel('left', 'Projection coefficient')
        self._plot.setLabel('bottom', 'Frame')
        self._plot.setMaximumHeight(170)
        self._plot.setVisible(False)
        layout.addWidget(self._plot)

        # ── close ─────────────────────────────────────────────────────────────
        close_box = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        close_box.rejected.connect(self.reject)
        layout.addWidget(close_box)

        self._slider.valueChanged.connect(self._show_component)
        self._mean_btn.toggled.connect(self._toggle_mean)
        self._weight_btn.clicked.connect(self._start_weight_computation)

        self._show_component(0)

    # ── component display ─────────────────────────────────────────────────────

    @staticmethod
    def _pct_levels(img, lo=3, hi=97):
        """Return (vmin, vmax) at the given percentiles for display."""
        flat = img.ravel()
        return (float(np.percentile(flat, lo)), float(np.percentile(flat, hi)))

    def _show_component(self, idx):
        self._current_idx = idx
        n = self._components.shape[0]
        img = self._components[idx].reshape(self._shape)
        levels = self._pct_levels(img)
        self._img_view.setImage(img, levels=levels, autoHistogramRange=False)

        lbl = f'{idx + 1} / {n}'
        if self._explained is not None and idx < len(self._explained):
            lbl += f'  ({self._explained[idx] * 100:.1f}% var)'
        self._comp_lbl.setText(lbl)

        self._mean_btn.blockSignals(True)
        self._mean_btn.setChecked(False)
        self._mean_btn.blockSignals(False)

        # If weights are already cached, update the plot immediately
        if self._all_coeffs is not None:
            self._update_weight_plot(idx)

    def _toggle_mean(self, checked):
        if checked:
            img = self._mean.reshape(self._shape)
            levels = self._pct_levels(img)
            self._img_view.setImage(img, levels=levels, autoHistogramRange=False)
        else:
            self._show_component(self._current_idx)

    # ── temporal weights ──────────────────────────────────────────────────────

    def _start_weight_computation(self):
        if self._reader is None or self._weight_worker is not None:
            return
        # If already cached, just update the plot for the current component
        if self._all_coeffs is not None:
            self._update_weight_plot(self._current_idx)
            return
        self._weight_btn.setEnabled(False)
        self._weight_prog.setVisible(True)
        self._weight_prog.setValue(0)
        # Pass ALL components so we compute everything in one data pass
        self._weight_worker = _PcaTemporalWeightWorker(
            self._components, self._mean, self._reader, parent=self)
        self._weight_worker.progress.connect(self._weight_prog.setValue)
        self._weight_worker.done.connect(self._on_weights_done)
        self._weight_worker.error.connect(self._on_weights_error)
        self._weight_worker.start()

    def _on_weights_done(self, all_coeffs):
        # all_coeffs: (n_components, n_frames)
        self._weight_worker = None
        self._all_coeffs    = all_coeffs
        self._weight_btn.setEnabled(True)
        self._weight_btn.setText('Recompute temporal weights')
        self._weight_prog.setVisible(False)
        self._update_weight_plot(self._current_idx)

    def _update_weight_plot(self, idx):
        coeffs = self._all_coeffs[idx]
        n      = self._components.shape[0]

        if (self._timestamps is not None
                and len(self._timestamps) == len(coeffs)):
            x     = self._timestamps
            x_lbl = f'Time ({self._ts_unit})' if self._ts_unit else 'Time'
        else:
            x     = np.arange(len(coeffs), dtype=np.float32)
            x_lbl = 'Frame'

        self._plot.setLabel('bottom', x_lbl)
        self._plot.clear()
        self._plot.plot(x, coeffs, pen=pg.mkPen('#4fc3f7', width=1))
        title = f'Component {idx + 1} / {n}'
        if self._explained is not None and idx < len(self._explained):
            title += f'  ({self._explained[idx] * 100:.1f}% var)'
        self._plot.setTitle(title)
        self._plot.setVisible(True)

    def _on_weights_error(self, msg):
        self._weight_worker = None
        self._weight_btn.setEnabled(True)
        self._weight_prog.setVisible(False)
        QMessageBox.warning(self, 'Temporal weights error', msg)


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
        self._sidecar           = {}   # Full parsed sidecar dict -- see _apply_hints()
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
        self._gif_worker         = None   # GifExportWorker while a GIF export is running
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
        self._pca_mean_low          = None  # (H*W,) float32 low-res PCA mean
        self._pca_components_low    = None  # (n_stored, H*W) float32 low-res eigenvectors
        self._pca_blur_enabled      = False # loaded from QSettings
        self._pca_blur_sigma        = 400   # Gaussian blur sigma for low-res pass (px)
        self._pca_universal             = False  # use master basis + cell-mean adaptation
        self._pca_master_folder         = ''     # path to master (no-cell) white field folder
        self._pca_master_folder_mode    = ''     # 'manual' | 'auto' | ''
        self._pca_master_mean           = None   # (H*W,) float32 master no-cell mean
        self._pca_master_components     = None   # (n, H*W) float32 master eigenvectors
        self._pca_master_mean_low       = None   # (H*W,) float32 master blurred mean
        self._pca_master_components_low = None   # (n, H*W) float32 master blurred eigenvectors
        self._pca_cell_gain             = None   # mean gain (dB) of the cell white field
        self._cell_adapt_worker         = None   # CellAdaptWorker while adapting
        self._dark_field_worker         = None   # FieldLoadWorker for dark field
        self._white_flat_worker         = None   # FieldLoadWorker for flat white field
        self._stored_field_refs = {}     # field_references loaded from sidecar
        self._play_timer        = None   # QTimer driving playback
        self._play_start_wall   = 0.0    # time.monotonic() when play started
        self._play_start_ts_idx = 0      # frame index at play start
        self._auto_levels_done  = False  # True after first frame of each file is shown

        self.setWindowTitle('Single-frame PCI Viewer')
        self.resize(1300, 860)
        _ico = os.path.join(os.path.dirname(__file__), 'assets', 'tv_png.ico')
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
        self._export_gif_act = QAction('Export &GIF…', self)
        self._export_gif_act.setShortcut(QKeySequence('Ctrl+G'))
        self._export_gif_act.setEnabled(False)
        self._export_gif_act.triggered.connect(self._export_gif)
        file_menu.addAction(self._export_gif_act)
        config_menu = self.menuBar().addMenu('&Config')
        white_menu = config_menu.addMenu('Set &White Field')
        flat_act = QAction('&Flat Field…', self)
        flat_act.triggered.connect(self._select_white_flat_folder)
        white_menu.addAction(flat_act)
        pca_act = QAction('&PCA…', self)
        pca_act.triggered.connect(self._select_white_pca_folder)
        white_menu.addAction(pca_act)
        master_menu = white_menu.addMenu('&Master PCA')
        self._master_choose_act = QAction('Choose Folder…', self)
        self._master_choose_act.triggered.connect(self._select_master_folder)
        master_menu.addAction(self._master_choose_act)
        self._master_find_act = QAction('Find', self)
        self._master_find_act.setCheckable(True)
        self._master_find_act.triggered.connect(self._find_master_folder_action)
        master_menu.addAction(self._master_find_act)
        dark_cap_act = QAction('Set &Dark Field…', self)
        dark_cap_act.triggered.connect(self._select_dark_folder)
        config_menu.addAction(dark_cap_act)
        config_menu.addSeparator()
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
        sidebar.setMinimumWidth(180)
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
        fmt_grid.setColumnStretch(0, 0)
        fmt_grid.setColumnStretch(1, 1)

        _lbl = QLabel('Pixel format:')
        _lbl.setSizePolicy(QSizePolicy.Policy.Maximum, QSizePolicy.Policy.Preferred)
        fmt_grid.addWidget(_lbl, 0, 0)
        self.fmt_combo = QComboBox()
        self.fmt_combo.addItems(PIXEL_FORMATS)
        fmt_grid.addWidget(self.fmt_combo, 0, 1)

        _lbl = QLabel('Width (px):')
        _lbl.setSizePolicy(QSizePolicy.Policy.Maximum, QSizePolicy.Policy.Preferred)
        fmt_grid.addWidget(_lbl, 1, 0)
        self.w_spin = QSpinBox()
        self.w_spin.setRange(1, 16384)
        self.w_spin.setValue(ATX245_W)
        fmt_grid.addWidget(self.w_spin, 1, 1)

        _lbl = QLabel('Height (px):')
        _lbl.setSizePolicy(QSizePolicy.Policy.Maximum, QSizePolicy.Policy.Preferred)
        fmt_grid.addWidget(_lbl, 2, 0)
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
        meta_grid.setColumnStretch(0, 0)
        meta_grid.setColumnStretch(1, 1)
        meta_grid.setColumnMinimumWidth(0, 80)

        self._meta_vals = []
        for row, key in enumerate(['Folder:', 'File:', 'Format:', 'Frames:', 'FPS:', 'Size:', 'Acquired:', 'Notes:']):
            lbl = QLabel(key)
            lbl.setStyleSheet('font-weight: bold;')
            lbl.setSizePolicy(QSizePolicy.Policy.Maximum, QSizePolicy.Policy.Preferred)
            meta_grid.addWidget(lbl, row, 0)
            v = QLabel('—')
            self._meta_vals.append(v)
            meta_grid.addWidget(v, row, 1)
        sb.addWidget(meta_box)

        # Display
        disp_box  = QGroupBox('Display')
        disp_grid = QGridLayout(disp_box)
        disp_grid.setSpacing(4)
        disp_grid.setColumnStretch(0, 0)
        disp_grid.setColumnStretch(1, 1)

        _lbl = QLabel('Colormap:')
        _lbl.setSizePolicy(QSizePolicy.Policy.Maximum, QSizePolicy.Policy.Preferred)
        disp_grid.addWidget(_lbl, 0, 0)
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
        corr_grid.setColumnStretch(0, 0)
        corr_grid.setColumnStretch(1, 1)
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
        self._pca_n_lbl.setSizePolicy(QSizePolicy.Policy.Maximum, QSizePolicy.Policy.Preferred)
        self._pca_n_lbl.setVisible(False)
        self.pca_n_spin = QSpinBox()
        self.pca_n_spin.setRange(1, 20)
        self.pca_n_spin.setValue(5)
        self.pca_n_spin.setToolTip('Number of PCA components to subtract (1–20)')
        self.pca_n_spin.setVisible(False)
        self._pca_settings_btn = QPushButton('PCA settings…')
        self._pca_settings_btn.setToolTip(
            'Configure Gaussian-blur dual-resolution projection settings.')
        self._pca_settings_btn.setVisible(False)
        self.corr_status_lbl = QLabel('')
        self.corr_status_lbl.setWordWrap(True)
        self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')
        self.store_fields_btn = QPushButton('Store')
        self.store_fields_btn.setToolTip(
            'Save current dark/white field selection to this file\'s sidecar JSON.\n'
            'On next open the same fields will be loaded automatically.')
        self.store_fields_btn.setEnabled(False)
        self._dark_field_info_lbl = QLabel('—')
        self._dark_field_info_lbl.setSizePolicy(QSizePolicy.Policy.Maximum, QSizePolicy.Policy.Preferred)
        self._dark_field_info_lbl.setStyleSheet('color: #888; font-size: 10px;')
        self._white_field_info_lbl = QLabel('—')
        self._white_field_info_lbl.setStyleSheet('color: #888; font-size: 10px;')
        corr_grid.addWidget(self.dark_chk,               0, 0, 1, 2)
        corr_grid.addWidget(self.white_chk,              1, 0)
        corr_grid.addWidget(self.white_combo,            1, 1)
        corr_grid.addWidget(self._dark_field_info_lbl,   2, 0)
        corr_grid.addWidget(self._white_field_info_lbl,  2, 1)
        corr_grid.addWidget(self._pca_n_lbl,             3, 0)
        corr_grid.addWidget(self.pca_n_spin,             3, 1)
        corr_grid.addWidget(self._pca_settings_btn,      4, 0, 1, 2)
        corr_grid.addWidget(self.corr_status_lbl,        5, 0, 1, 2)
        corr_grid.addWidget(self.store_fields_btn,       6, 0, 1, 2)
        sb.addWidget(corr_box)

        sb.addStretch()
        splitter.addWidget(sidebar)

        # ── pyqtgraph canvas ──────────────────────────────────────────────────
        right = QWidget()
        right.setMinimumWidth(300)
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
        self._pca_settings_btn.clicked.connect(self._open_pca_settings)
        self._pca_settings_btn.setContextMenuPolicy(
            Qt.ContextMenuPolicy.CustomContextMenu)
        self._pca_settings_btn.customContextMenuRequested.connect(
            self._show_pca_eigenvector_menu)
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
        self._sidecar = sidecar

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
                # Only apply the sidecar's inverse LUT if the format in effect
                # still matches what the sidecar describes -- if the user has
                # hand-overridden the format combo, its assumptions (including
                # the LUT) no longer apply.
                sidecar_fmt = _canonicalize_pixel_format(
                    self._sidecar.get('pixel_format') or self._sidecar.get('pixelformat'))
                lut = (_sqrt_inverse_lut_from_sidecar(self._sidecar)
                       if _canonicalize_pixel_format(fmt) == sidecar_fmt else None)
                reader = RawReader(path, fmt, w, h, inverse_lut=lut)
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
            self._white_mode            = 'none'
            self._pca_mean              = None
            self._pca_components        = None
            self._pca_mean_low          = None
            self._pca_components_low    = None
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
        _meta_keys = ['Folder', 'File', 'Format', 'Frames', 'FPS', 'Size', 'Acquired', 'Notes']
        _meta_data = [
            os.path.basename(os.path.dirname(path)),
            os.path.basename(path),
            fmt if ext == '.raw' else ext.lstrip('.').upper(),
            str(n),
            fps_str,
            f'{file_size_mb:.1f} MB',
            acq,
            notes,
        ]
        for lbl, key, val in zip(self._meta_vals, _meta_keys, _meta_data):
            lbl.setText(val)
            lbl.setToolTip(f'{key}: {val}')

        if ext == '.raw':
            bpf = getattr(reader, 'bpf', None)
            dim = f'{w}×{h} px\n{bpf:,} B/frame' if bpf else f'{w}×{h} px'
        else:
            dim = f'{w}×{h} px'
        self.diag_label.setText(f'{dim}\n{self._sidecar_status}')

        self.reload_btn.setEnabled(True)
        self._export_act.setEnabled(True)
        self._export_gif_act.setEnabled(True)

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

    def _find_field_folder(self, keyword: str, min_frames: int = 1,
                           sidecar_key: str = None, exclude: str = None):
        """Return path to best shape-matching folder whose name contains keyword.

        Priority: (1) stored sidecar ref if path exists, shape matches, and has
                      enough frames,
                  (2) closest-timestamp match with matching shape and enough frames,
                  (3) closest-timestamp match with unknown shape and enough frames,
                  (4) None.

        min_frames: skip folders that contain fewer than this many frames (use 2
                    for PCA so pre-averaged single-frame folders are bypassed).
        exclude: if set, skip auto-search candidates whose name contains this string
                 (case-insensitive).  Stored sidecar refs are always honoured.
        """
        if not self._path:
            return None

        tw = self._reader.w if self._reader else self.w_spin.value()
        th = self._reader.h if self._reader else self.h_spin.value()

        # --- stored sidecar reference ---
        refs = self._stored_field_refs or {}
        stored = refs.get(sidecar_key if sidecar_key else ('dark_folder' if 'dark' in keyword else 'white_folder'))
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
                and (exclude is None or exclude not in d.lower())
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

        MAX_FIELD_FRAMES = 100  # cap per-folder to avoid loading multi-GB master sequences
        frames = []
        for fname in files:
            if len(frames) >= MAX_FIELD_FRAMES:
                print(f'[field] hit {MAX_FIELD_FRAMES}-frame cap; skipping remaining files')
                break
            fpath = os.path.join(folder, fname)
            ext   = os.path.splitext(fname)[1].lower()
            try:
                if ext == '.raw':
                    # Read each file's own sidecar so format/dims are correct
                    # per-image (field captures may use a different format than
                    # the experiment).
                    sidecar, sidecar_status = _load_sidecar(fpath)
                    f_fmt = _canonicalize_pixel_format(_fmt_from_sc(sidecar))
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
                    r = RawReader(fpath, fmt, w, h,
                                  inverse_lut=_sqrt_inverse_lut_from_sidecar(sidecar))
                    n_file = len(r)
                    budget = MAX_FIELD_FRAMES - len(frames)
                    if n_file <= budget:
                        indices = range(n_file)
                    else:
                        indices = [int(k * n_file / budget) for k in range(budget)]
                        print(f'[field]   {fname}: subsampling {n_file} → {len(indices)} frames')
                    for i in indices:
                        frames.append(r[i].astype(np.float32))
                    r.close()
                elif ext in ('.tiff', '.tif'):
                    import tifffile
                    data = tifffile.imread(fpath)
                    if data.ndim == 2:
                        frames.append(data.astype(np.float32))
                    else:
                        budget = MAX_FIELD_FRAMES - len(frames)
                        n_file = data.shape[0]
                        if n_file <= budget:
                            indices = range(n_file)
                        else:
                            indices = [int(k * n_file / budget) for k in range(budget)]
                            print(f'[field]   {fname}: subsampling {n_file} → {len(indices)} frames')
                        for i in indices:
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
            self._set_dark_field_info()
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
        self._dark_field       = None
        self._dark_field_gain  = None
        self._dark_field_error = None
        self._start_dark_field_worker(folder)

    def _set_dark_field_info(self, folder=None, error=''):
        lbl = self._dark_field_info_lbl
        if folder is None:
            lbl.setText('—')
            lbl.setStyleSheet('color: #888; font-size: 10px;')
            lbl.setToolTip('')
            return
        basename = os.path.basename(folder)
        if error:
            lbl.setText('⚠ Dark')
            lbl.setStyleSheet('color: #cc8800; font-size: 10px;')
        else:
            lbl.setText('✓ Dark')
            lbl.setStyleSheet('color: #66cc66; font-size: 10px;')
        tip = f'{basename}\nPath: {folder}'
        if error:
            tip += f'\n\n⚠ {error}'
        lbl.setToolTip(tip)

    def _set_white_field_info(self, folder=None, extra=''):
        lbl = self._white_field_info_lbl
        if folder is None:
            lbl.setText('—')
            lbl.setStyleSheet('color: #888; font-size: 10px;')
            lbl.setToolTip('')
            return
        basename = os.path.basename(folder)
        is_error = extra and not extra.startswith('master:')
        if is_error:
            lbl.setText('⚠ White')
            lbl.setStyleSheet('color: #cc8800; font-size: 10px;')
        else:
            lbl.setText('✓ White')
            lbl.setStyleSheet('color: #66cc66; font-size: 10px;')
        if extra.startswith('master:'):
            tip = extra
        else:
            tip = f'{basename}\nPath: {folder}'
            if extra:
                tip += f'\n\n{extra}'
        lbl.setToolTip(tip)

    # ── async field loaders ───────────────────────────────────────────────────

    def _start_dark_field_worker(self, folder):
        if self._dark_field_worker is not None:
            try:
                self._dark_field_worker.field_done.disconnect()
            except TypeError:
                pass
        fmt    = self.fmt_combo.currentText()
        w, h   = self.w_spin.value(), self.h_spin.value()
        worker = FieldLoadWorker(folder, fmt, w, h)
        worker.field_done.connect(self._on_dark_field_loaded)
        self._dark_field_worker = worker
        self.corr_status_lbl.setText('Loading dark field…')
        worker.start()

    def _start_white_flat_worker(self, folder):
        if self._white_flat_worker is not None:
            try:
                self._white_flat_worker.field_done.disconnect()
            except TypeError:
                pass
        fmt    = self.fmt_combo.currentText()
        w, h   = self.w_spin.value(), self.h_spin.value()
        worker = FieldLoadWorker(folder, fmt, w, h)
        worker.field_done.connect(self._on_white_flat_loaded)
        self._white_flat_worker = worker
        self.corr_status_lbl.setText('Loading white field…')
        worker.start()

    def _on_dark_field_loaded(self, data, folder, error):
        if data is None:
            self.corr_status_lbl.setText(f'Dark field: {error}')
            self.dark_chk.blockSignals(True)
            self.dark_chk.setChecked(False)
            self.dark_chk.blockSignals(False)
            self._set_dark_field_info()
            return
        self._dark_field       = data
        self._dark_field_gain  = self._read_average_gain(folder)
        self._dark_field_error = self._check_field_shape(data, 'dark')
        self._set_dark_field_info(folder, self._dark_field_error or '')
        self._refresh_corr_status()
        self._refresh_current_frame()
        self._auto_levels()

    def _on_white_flat_loaded(self, data, folder, error):
        if data is None:
            self.corr_status_lbl.setText(f'White field: {error}')
            self.white_chk.blockSignals(True)
            self.white_chk.setChecked(False)
            self.white_chk.blockSignals(False)
            self._set_white_field_info()
            return
        self._white_field       = data
        self._white_field_gain  = self._read_average_gain(folder)
        self._white_field_error = self._check_field_shape(data, 'white')
        self._white_mode         = 'flat'
        self._pca_mean           = None
        self._pca_components     = None
        self._pca_mean_low       = None
        self._pca_components_low = None
        self._set_white_field_info(folder, self._white_field_error or '')
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
            self._pca_mean              = None
            self._pca_components        = None
            self._pca_mean_low          = None
            self._pca_components_low    = None
            self._pca_n_lbl.setVisible(False)
            self.pca_n_spin.setVisible(False)
            self._pca_settings_btn.setVisible(False)
            self._set_white_field_info()
            self._refresh_corr_status()
            self._refresh_current_frame()
            self._auto_levels()

        elif index == 1:
            self._pca_n_lbl.setVisible(False)
            self.pca_n_spin.setVisible(False)
            self._pca_settings_btn.setVisible(False)
            if not self._path:
                self.white_chk.blockSignals(True)
                self.white_chk.setChecked(False)
                self.white_chk.blockSignals(False)
                return
            folder = self._find_field_folder('white_field', sidecar_key='white_folder')
            if folder is None:
                self.corr_status_lbl.setText('white_field folder not found')
                self.white_chk.blockSignals(True)
                self.white_chk.setChecked(False)
                self.white_chk.blockSignals(False)
                return
            self._white_field       = None
            self._white_field_gain  = None
            self._white_field_error = None
            self._white_mode        = 'flat'
            self._pca_mean          = None
            self._pca_components    = None
            self._pca_mean_low      = None
            self._pca_components_low = None
            self._start_white_flat_worker(folder)

        elif index == 2:
            self._pca_n_lbl.setVisible(True)
            self.pca_n_spin.setVisible(True)
            self._pca_settings_btn.setVisible(True)
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
                    r = RawReader(fpath, fmt, w, h,
                                  inverse_lut=_sqrt_inverse_lut_from_sidecar(sidecar))
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
        if self._pca_blur_enabled:
            return os.path.join(folder, f'pca_cache_{h}x{w}_blur{self._pca_blur_sigma}.npz')
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
        if self._pca_universal:
            self._compute_universal_pca()
            return

        folder = self._find_field_folder('white_field', min_frames=2, sidecar_key='white_pca_folder')
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
                    if self._pca_blur_enabled:
                        if 'mean_low' not in data or 'components_low' not in data:
                            raise KeyError('cache lacks low-res arrays — recomputing with blur')
                        self._pca_mean_low       = data['mean_low']
                        self._pca_components_low = data['components_low']
                    else:
                        self._pca_mean_low       = None
                        self._pca_components_low = None
                    self._pca_mean       = mean
                    self._pca_components = comp
                    self._pca_shape      = (h, w)
                    self._pca_folder     = folder
                    self._white_mode     = 'pca'
                    n_stored = comp.shape[0]
                    try:
                        explained = data['explained']
                        n_auto = _auto_pca_n(explained)
                        self.pca_n_spin.blockSignals(True)
                        self.pca_n_spin.setValue(n_auto)
                        self.pca_n_spin.blockSignals(False)
                        self._pca_n_components = n_auto
                        ev_str = ', '.join(
                            f'{e*100:.1f}%' for e in explained[:min(3, n_stored)])
                    except Exception:
                        ev_str = '?'
                    blur_tag = f'  [blur σ={self._pca_blur_sigma}px]' if self._pca_blur_enabled else ''
                    self.corr_status_lbl.setText(
                        f'PCA ✓  {n_stored} comp cached, top 3: {ev_str}{blur_tag}')
                    self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')
                    self._set_white_field_info(folder)
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
        # Train enough components to cover the spinner max + 2 headroom so the
        # user can adjust n interactively without recomputing, but avoid paying
        # for 20 projections when 3 are ever used.
        n_train = min(20, self._pca_n_components + 2)
        self._pca_worker = PcaComputeWorker(
            folder, fmt, w, h, n_components=n_train,
            blur_sigma=self._pca_blur_sigma if self._pca_blur_enabled else 0,
            parent=self)
        self._pca_worker.pca_done.connect(
            lambda mu, comp, expl, mu_low, comp_low, _cp=cache_path, _f=folder:
                self._on_pca_computed(mu, comp, expl, mu_low, comp_low, _cp, _f)
        )
        self._pca_worker.pca_error.connect(self._on_pca_error)
        self._pca_worker.pca_progress.connect(self._on_pca_progress)
        self._pca_worker.start()

    def _on_pca_computed(self, mean, components, explained, mean_low, components_low,
                         cache_path, folder):
        if self._white_mode != 'pca':
            return
        self._pca_mean              = mean
        self._pca_components        = components
        self._pca_mean_low          = mean_low
        self._pca_components_low    = components_low
        self._pca_shape             = (self._reader.h, self._reader.w) if self._reader else None
        self._pca_folder            = folder
        try:
            save_kw = dict(mean=mean, components=components, explained=explained)
            if mean_low is not None and components_low is not None:
                save_kw['mean_low']       = mean_low
                save_kw['components_low'] = components_low
            np.savez_compressed(cache_path, **save_kw)
        except Exception as exc:
            print(f'[pca] cache save error: {exc}')
        n_stored = components.shape[0]
        n_auto = _auto_pca_n(explained)
        self.pca_n_spin.blockSignals(True)
        self.pca_n_spin.setValue(n_auto)
        self.pca_n_spin.blockSignals(False)
        self._pca_n_components = n_auto
        ev_str = ', '.join(
            f'{e*100:.1f}%' for e in explained[:min(3, n_stored)])
        blur_tag = f'  [blur σ={self._pca_blur_sigma}px]' if self._pca_blur_enabled else ''
        self.corr_status_lbl.setText(f'PCA ✓  {n_stored} comp, top 3: {ev_str}{blur_tag}')
        self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')
        self._set_white_field_info(folder)
        self._refresh_current_frame()
        self._auto_levels()

    def _on_pca_progress(self, msg):
        self.corr_status_lbl.setText(msg)
        self.corr_status_lbl.setStyleSheet('color: #aaa; font-size: 10px;')

    # ------------------------------------------------------------------ #
    # Universal PCA (cell-transform) methods                               #
    # ------------------------------------------------------------------ #

    def _find_master_folder(self):
        """Walk up the directory tree from the current file looking for a *_master* folder.

        Checks sibling directories at each ancestor level (up to 4 levels up).
        Returns the first match whose shape is compatible, or None.
        """
        if not self._path:
            return None
        tw = self._reader.w if self._reader else self.w_spin.value()
        th = self._reader.h if self._reader else self.h_spin.value()
        search_dir = os.path.dirname(self._path)
        exp_ts = _folder_timestamp(os.path.basename(search_dir))
        for _ in range(4):
            parent = os.path.dirname(search_dir)
            if parent == search_dir:
                break
            try:
                entries = os.listdir(parent)
            except OSError:
                break
            candidates = [
                d for d in entries
                if '_master' in d.lower() and os.path.isdir(os.path.join(parent, d))
            ]
            if exp_ts:
                candidates.sort(key=lambda d: abs(_folder_timestamp(d) - exp_ts))
            for name in candidates:
                folder = os.path.join(parent, name)
                shape = self._field_folder_shape(folder)
                if shape is None or shape == (tw, th):
                    return folder
            search_dir = parent
        return None

    def _compute_universal_pca(self):
        """Load or compute master PCA, then adapt it to the current cell folder."""
        master = self._pca_master_folder
        if not master or not os.path.isdir(master):
            master = self._find_master_folder()
            if master:
                self._pca_master_folder      = master
                self._pca_master_folder_mode = 'auto'
                self._settings.setValue('pca/master_folder', master)
                self._update_master_menu_actions()
                self.corr_status_lbl.setText(
                    f'Universal PCA: auto-detected master → {os.path.basename(master)}')
                self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')
        if not master or not os.path.isdir(master):
            self.corr_status_lbl.setText('Universal PCA: master folder not set — '
                                         'use Config → Set White Field → Master PCA…')
            self.corr_status_lbl.setStyleSheet('color: #c88; font-size: 10px;')
            return

        h   = self._reader.h if self._reader else self.h_spin.value()
        w   = self._reader.w if self._reader else self.w_spin.value()
        cache_path = self._pca_cache_path(master, h, w)

        # Try to use already-loaded master arrays (same session).
        if (self._pca_master_mean is not None
                and self._pca_master_components is not None):
            self._launch_cell_adapt_worker(
                self._pca_master_mean, self._pca_master_components,
                self._pca_master_mean_low, self._pca_master_components_low)
            return

        # Try disk cache.
        if self._pca_cache_valid(cache_path, master):
            try:
                data = np.load(cache_path)
                mu   = data['mean']
                comp = data['components']
                if mu.shape[0] == h * w:
                    mu_low   = data.get('mean_low',       None)
                    comp_low = data.get('components_low', None)
                    self._pca_master_mean            = mu
                    self._pca_master_components      = comp
                    self._pca_master_mean_low        = mu_low
                    self._pca_master_components_low  = comp_low
                    self._launch_cell_adapt_worker(mu, comp, mu_low, comp_low)
                    return
            except Exception as exc:
                print(f'[pca] master cache load error: {exc}')

        # Compute master PCA from scratch.
        fmt = self.fmt_combo.currentText()
        self.corr_status_lbl.setText('Computing master PCA…')
        self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')
        blur_sigma = self._pca_blur_sigma if self._pca_blur_enabled else 0
        if self._pca_worker is not None:
            self._pca_worker.pca_done.disconnect()
            self._pca_worker.pca_error.disconnect()
            self._pca_worker.pca_progress.disconnect()
            self._pca_worker.quit()
        n_train = min(20, self._pca_n_components + 2)
        self._pca_worker = PcaComputeWorker(
            master, fmt, w, h, n_components=n_train, blur_sigma=blur_sigma, parent=self)
        self._pca_worker.pca_done.connect(
            lambda mu, comp, expl, mu_low, comp_low, _cp=cache_path:
                self._on_master_pca_computed(mu, comp, expl, mu_low, comp_low, _cp)
        )
        self._pca_worker.pca_error.connect(self._on_pca_error)
        self._pca_worker.pca_progress.connect(self._on_pca_progress)
        self._pca_worker.start()

    def _on_master_pca_computed(self, mean, components, explained,
                                mean_low, components_low, cache_path):
        """Store master arrays, cache to disk, then kick off cell adaptation."""
        try:
            save_kw = dict(mean=mean, components=components, explained=explained)
            if mean_low is not None and components_low is not None:
                save_kw['mean_low']       = mean_low
                save_kw['components_low'] = components_low
            np.savez_compressed(cache_path, **save_kw)
        except Exception as exc:
            print(f'[pca] master cache save error: {exc}')
        self._pca_master_mean            = mean
        self._pca_master_components      = components
        self._pca_master_mean_low        = mean_low
        self._pca_master_components_low  = components_low
        self._launch_cell_adapt_worker(mean, components, mean_low, components_low)

    def _launch_cell_adapt_worker(self, master_mean, master_comp,
                                  master_mean_low, master_comp_low):
        """Find the cell PCA folder and start CellAdaptWorker.

        Uses the same white_field folder as flat-field mode (min_frames=1 so a
        single pre-averaged mean file is accepted).  Set via Config → Set White
        Field → Flat Field… without switching the correction mode."""
        folder = self._find_field_folder('white_field', min_frames=1,
                                         sidecar_key='white_folder', exclude='_master')
        if folder is None:
            folder = self._find_field_folder('white_field', min_frames=1,
                                             sidecar_key='white_folder')
        if folder is None:
            self.corr_status_lbl.setText(
                'Universal PCA: cell white field not found — '
                'use Config → Set White Field → Flat Field… to point to the cell mean folder')
            self.corr_status_lbl.setStyleSheet('color: #c88; font-size: 10px;')
            return

        self._pca_cell_gain = self._read_average_gain(folder)

        h   = self._reader.h if self._reader else self.h_spin.value()
        w   = self._reader.w if self._reader else self.w_spin.value()
        fmt = self.fmt_combo.currentText()
        blur_sigma = self._pca_blur_sigma if self._pca_blur_enabled else 0

        self.corr_status_lbl.setText('Cell adaptation: loading frames…')
        self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')

        if self._cell_adapt_worker is not None:
            self._cell_adapt_worker.adapt_done.disconnect()
            self._cell_adapt_worker.adapt_progress.disconnect()
            self._cell_adapt_worker.quit()

        self._cell_adapt_worker = CellAdaptWorker(
            folder, fmt, w, h,
            master_mean, master_comp,
            master_mean_low, master_comp_low,
            blur_sigma=blur_sigma, parent=self)
        self._cell_adapt_worker.adapt_done.connect(
            lambda cm, eh, ml, ql, _f=folder: self._on_cell_adapted(cm, eh, ml, ql, _f))
        self._cell_adapt_worker.adapt_progress.connect(self._on_pca_progress)
        self._cell_adapt_worker.start()

    def _on_cell_adapted(self, cell_mean, e_high_cell, mu_low_cell, q_low, folder):
        """Slot adapted eigenvectors into the projection pipeline."""
        if cell_mean is None:
            self.corr_status_lbl.setText('Cell adaptation failed — check console')
            self.corr_status_lbl.setStyleSheet('color: #c88; font-size: 10px;')
            return
        self._pca_mean       = cell_mean
        self._pca_components = e_high_cell
        if mu_low_cell is not None and q_low is not None:
            self._pca_mean_low       = mu_low_cell
            self._pca_components_low = q_low
        else:
            self._pca_mean_low       = None
            self._pca_components_low = None
        h = self._reader.h if self._reader else self.h_spin.value()
        w = self._reader.w if self._reader else self.w_spin.value()
        self._pca_shape  = (h, w)
        self._pca_folder = folder
        self._white_mode = 'pca'
        n = e_high_cell.shape[0]
        blur_tag = f'  [blur σ={self._pca_blur_sigma}px]' if self._pca_blur_enabled else ''
        self.corr_status_lbl.setText(
            f'PCA ✓  universal {n} comp (cell-adapted){blur_tag}')
        self.corr_status_lbl.setStyleSheet('color: #888; font-size: 10px;')
        master_folder = self._pca_master_folder or ''
        master_name   = os.path.basename(master_folder) if master_folder else ''
        if master_name:
            extra_tip = (f'master: {master_name}\n  {master_folder}\n'
                         f'cell:   {os.path.basename(folder)}\n  {folder}')
        else:
            extra_tip = ''
        self._set_white_field_info(folder, extra_tip)
        self._refresh_current_frame()
        self._auto_levels()

    def _open_pca_settings(self):
        dlg = _PcaSettingsDialog(
            self._pca_blur_enabled, self._pca_blur_sigma,
            universal_enabled=self._pca_universal,
            master_folder=self._pca_master_folder,
            master_folder_mode=self._pca_master_folder_mode,
            find_fn=self._find_master_folder,
            start_dir=os.path.dirname(self._path) if self._path else '',
            parent=self, stylesheet=self.styleSheet())
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        new_enabled   = dlg.blur_enabled
        new_sigma     = dlg.blur_sigma
        new_universal = dlg.universal_enabled
        new_master    = dlg.master_folder
        new_master_mode = dlg.master_folder_mode
        blur_changed      = (new_enabled != self._pca_blur_enabled or
                             (new_enabled and new_sigma != self._pca_blur_sigma))
        universal_changed = new_universal != self._pca_universal
        master_changed    = new_master != self._pca_master_folder
        self._pca_blur_enabled = new_enabled
        self._pca_blur_sigma   = new_sigma
        self._pca_universal    = new_universal
        self._settings.setValue('pca/blur_enabled', self._pca_blur_enabled)
        self._settings.setValue('pca/blur_sigma',   self._pca_blur_sigma)
        self._settings.setValue('pca/universal',    self._pca_universal)
        if master_changed and new_master:
            self._pca_master_folder      = new_master
            self._pca_master_folder_mode = new_master_mode
            self._pca_master_mean            = None
            self._pca_master_components      = None
            self._pca_master_mean_low        = None
            self._pca_master_components_low  = None
            self._settings.setValue('pca/master_folder', new_master)
            self._update_master_menu_actions()
        if (blur_changed or universal_changed or master_changed) and self._white_mode == 'pca':
            self._pca_mean_low          = None
            self._pca_components_low    = None
            if not master_changed:
                self._pca_master_mean       = None
                self._pca_master_components = None
            self._compute_or_load_pca()

    def _show_pca_eigenvector_menu(self, pos):
        has_data = self._pca_components is not None
        if not has_data and self._pca_folder:
            h = self._reader.h if self._reader else self.h_spin.value()
            w = self._reader.w if self._reader else self.w_spin.value()
            cache_path = self._pca_cache_path(self._pca_folder, h, w)
            has_data = self._pca_cache_valid(cache_path, self._pca_folder)
        menu = QMenu(self)
        act  = menu.addAction('View eigenvectors…')
        act.setEnabled(has_data)
        if menu.exec(self._pca_settings_btn.mapToGlobal(pos)) is act:
            self._open_pca_eigenvector_viewer()

    def _open_pca_eigenvector_viewer(self):
        components = self._pca_components
        mean       = self._pca_mean
        shape      = self._pca_shape
        explained  = None

        if components is None and self._pca_folder:
            h = self._reader.h if self._reader else self.h_spin.value()
            w = self._reader.w if self._reader else self.w_spin.value()
            cache_path = self._pca_cache_path(self._pca_folder, h, w)
            try:
                data       = np.load(cache_path)
                mean       = data['mean']
                components = data['components']
                explained  = data['explained'] if 'explained' in data else None
                shape      = (h, w)
            except Exception as exc:
                QMessageBox.warning(self, 'PCA Viewer', f'Could not load PCA cache:\n{exc}')
                return

        if components is None or mean is None:
            return

        if explained is None and self._pca_folder:
            h = self._reader.h if self._reader else self.h_spin.value()
            w = self._reader.w if self._reader else self.w_spin.value()
            try:
                data      = np.load(self._pca_cache_path(self._pca_folder, h, w))
                explained = data['explained'] if 'explained' in data else None
            except Exception:
                pass

        dlg = _PcaEigenvectorDialog(
            components, mean, shape, explained,
            reader=self._reader,
            timestamps=self._timestamps,
            ts_unit=self._ts_unit,
            parent=self,
            stylesheet=self.styleSheet())
        dlg.exec()

    def _on_pca_error(self, msg):
        self.corr_status_lbl.setText(f'PCA error: {msg}')
        self.corr_status_lbl.setStyleSheet('color: #ff6b6b; font-size: 10px;')
        self.statusBar().clearMessage()
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
        white_mode   = refs.get('white_mode', 'none')
        if white_mode == 'pca':
            white_folder = refs.get('white_pca_folder') or refs.get('white_folder')
        else:
            white_folder = refs.get('white_folder')

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
                    self._pca_settings_btn.setVisible(combo_idx == 1)

        if white_mode == 'pca' and refs.get('pca_universal'):
            master = refs.get('white_pca_master_folder', '')
            if master and os.path.isdir(master):
                self._pca_universal          = True
                self._pca_master_folder      = master
                self._pca_master_folder_mode = 'manual'
                self._update_master_menu_actions()

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
            updates['white_pca_folder'] = self._pca_folder
            updates['white_mode'] = 'pca'
            if self._pca_universal and self._pca_master_folder:
                updates['white_pca_master_folder'] = self._pca_master_folder
                updates['pca_universal'] = True
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

    def _apply_pca_correction_frame(self, frame, dark=None, data_gain=None):
        """Subtract PCA-estimated white-field background; return float32 transmission."""
        mu   = self._pca_mean
        comp = self._pca_components
        n    = min(self._pca_n_components, comp.shape[0])
        f = frame.astype(np.float32)
        if dark is not None:
            f = f - dark
        if (data_gain is not None and self._pca_cell_gain is not None
                and data_gain != self._pca_cell_gain):
            gain_scale = np.float32(10 ** ((data_gain - self._pca_cell_gain) / 20.0))
            mu = mu * gain_scale
        else:
            gain_scale = None
        if (self._pca_blur_enabled
                and self._pca_mean_low is not None
                and self._pca_components_low is not None):
            n_low  = min(n, self._pca_components_low.shape[0])
            mu_low = (self._pca_mean_low * gain_scale
                      if gain_scale is not None else self._pca_mean_low)
            f_low  = _blur_and_bin(f, self._pca_blur_sigma, mu_low.shape[0])
            d_low  = f_low - mu_low
            coeffs = self._pca_components_low[:n_low] @ d_low
            bg     = (mu + comp[:n_low].T @ coeffs).reshape(frame.shape)
        else:
            d_c    = f.ravel() - mu
            coeffs = comp[:n] @ d_c
            bg     = (mu + comp[:n].T @ coeffs).reshape(frame.shape)
        # Floor bad/near-zero background pixels at half the per-pixel mean so that
        # pixels the PCA model reconstructs poorly don't cause extreme ratios.
        bg_floor = np.maximum(mu.reshape(frame.shape) * np.float32(0.5), np.float32(1.0))
        np.maximum(bg, bg_floor, out=bg)
        return np.clip(f / bg, np.float32(0.0), np.float32(10.0))

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

    def _select_white_flat_folder(self):
        """Config → Set White Field → Flat Field…

        In Universal PCA mode the selected folder is used as the cell-present white
        field source (W_cell) without switching the correction mode.  In all other
        modes this works as before: switch to flat-field and load the mean frame."""
        start = os.path.dirname(self._path) if self._path else ''
        folder = QFileDialog.getExistingDirectory(
            self, 'Select flat white field folder (mean)', start)
        if not folder:
            return
        self._stored_field_refs['white_folder'] = folder
        self._check_white_options()

        if self._pca_universal and self._white_mode == 'pca':
            # Re-run cell adaptation against the new cell white field folder.
            if self._pca_master_mean is not None:
                self._launch_cell_adapt_worker(
                    self._pca_master_mean, self._pca_master_components,
                    self._pca_master_mean_low, self._pca_master_components_low)
            return

        # Default: switch combo to flat if possible
        if self.white_combo.count() > 0:
            self.white_combo.blockSignals(True)
            self.white_combo.setCurrentIndex(0)
            self.white_combo.blockSignals(False)
        if self.white_chk.isChecked():
            self._reload_fields()

    def _select_white_pca_folder(self):
        """Config → Set White Field → PCA…"""
        start = os.path.dirname(self._path) if self._path else ''
        folder = QFileDialog.getExistingDirectory(
            self, 'Select multi-frame white field folder (PCA)', start)
        if not folder:
            return
        self._stored_field_refs['white_pca_folder'] = folder
        self._stored_field_refs['white_folder']     = folder
        self._check_white_options()
        # Switch combo to PCA if possible
        if self.white_combo.count() > 1:
            self.white_combo.blockSignals(True)
            self.white_combo.setCurrentIndex(1)
            self.white_combo.blockSignals(False)
        if self.white_chk.isChecked():
            self._reload_fields()

    def _update_master_menu_actions(self):
        """Sync Master PCA submenu text, checked state, and tooltips to current folder/mode."""
        folder = self._pca_master_folder
        tip = folder if folder else 'No master folder set'
        mode = self._pca_master_folder_mode
        self._master_choose_act.setText('✓ Choose Folder…' if mode == 'manual' else 'Choose Folder…')
        self._master_choose_act.setToolTip(tip)
        self._master_find_act.setChecked(mode == 'auto')
        self._master_find_act.setToolTip(tip)

    def _find_master_folder_action(self):
        """Config → Set White Field → Master PCA → Find"""
        folder = self._find_master_folder()
        if not folder:
            self._master_find_act.setChecked(False)
            from PyQt6.QtWidgets import QMessageBox
            QMessageBox.warning(self, 'Master PCA',
                                'No *_master* folder found near the current file.')
            return
        self._pca_master_folder      = folder
        self._pca_master_folder_mode = 'auto'
        self._pca_master_mean            = None
        self._pca_master_components      = None
        self._pca_master_mean_low        = None
        self._pca_master_components_low  = None
        self._settings.setValue('pca/master_folder', folder)
        self._update_master_menu_actions()
        if self._pca_universal and self._white_mode == 'pca':
            self._compute_or_load_pca()

    def _select_master_folder(self):
        """Config → Set White Field → Master PCA → Choose Folder…"""
        start = os.path.dirname(self._path) if self._path else ''
        folder = QFileDialog.getExistingDirectory(
            self, 'Select master white field folder (clean, no cell)', start)
        if not folder:
            return
        self._pca_master_folder      = folder
        self._pca_master_folder_mode = 'manual'
        # Clear cached master arrays so they're reloaded from the new folder
        self._pca_master_mean            = None
        self._pca_master_components      = None
        self._pca_master_mean_low        = None
        self._pca_master_components_low  = None
        self._settings.setValue('pca/master_folder', folder)
        self._update_master_menu_actions()
        if self._pca_universal and self._white_mode == 'pca':
            self._compute_or_load_pca()

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
        if self.dark_chk.isChecked():
            folder = self._find_field_folder('dark_field')
            if folder:
                self._dark_field       = None
                self._dark_field_gain  = None
                self._dark_field_error = None
                self._start_dark_field_worker(folder)
            else:
                self._dark_field       = None
                self._dark_field_gain  = None
                self._dark_field_error = None
                self._set_dark_field_info()

        mode_idx = (self.white_combo.currentIndex() + 1) if self.white_chk.isChecked() else 0
        if mode_idx == 1:
            folder = self._find_field_folder('white_field')
            if folder:
                self._white_field       = None
                self._white_field_gain  = None
                self._white_field_error = None
                self._white_mode        = 'flat'
                self._start_white_flat_worker(folder)
            else:
                self._white_field       = None
                self._white_field_error = None
                self._set_white_field_info()
        elif mode_idx == 2:
            self._compute_or_load_pca()
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

    def _refresh_corr_status(self):
        parts = []
        has_error = False

        if self._dark_field is not None and self._dark_field_error:
            parts.append('Dark ✗')
            has_error = True

        if self._white_mode == 'flat' and self._white_field is not None:
            if self._white_field_error:
                parts.append('White ✗')
                has_error = True
        elif self._white_mode == 'pca':
            if self._pca_components is not None:
                parts.append(f'PCA  n={self._pca_n_components}')
            else:
                parts.append('PCA …')

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
            return self._apply_pca_correction_frame(frame, dark=dark, data_gain=data_gain)

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
        pca_mean_low  = self._pca_mean_low       if self._white_mode == 'pca' else None
        pca_comp_low  = self._pca_components_low if self._white_mode == 'pca' else None
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
            pca_mean_low=pca_mean_low,
            pca_components_low=pca_comp_low,
            pca_blur_enabled=self._pca_blur_enabled,
            pca_blur_sigma=self._pca_blur_sigma,
            pca_cell_gain=self._pca_cell_gain,
        )
        self._export_worker = worker

        dlg = _ExportProgressDialog(n, parent=self, stylesheet=_DARK_STYLE)
        dlg.cancel_requested.connect(worker.cancel)

        def on_progress(done, total):
            self.statusBar().showMessage(f'{dlg._phase} frame {done} / {total}…')
            dlg.update_progress(done, total)

        def on_finished(msg):
            self._export_act.setEnabled(True)
            self._export_gif_act.setEnabled(True)
            self.statusBar().showMessage(msg)
            dlg.close()

        def on_error(msg):
            self._export_act.setEnabled(True)
            self._export_gif_act.setEnabled(True)
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
        self._export_gif_act.setEnabled(False)
        self.statusBar().showMessage(f'Exporting {n} frames…')
        worker.start()
        dlg.show()

    def _export_gif(self):
        if self._reader is None:
            return
        try:
            from PIL import Image  # noqa: F401
        except ImportError:
            QMessageBox.warning(self, 'Missing dependency',
                                'Pillow is required for GIF export.\n'
                                'Install it with:  pip install pillow')
            return

        n = self._reader_len()
        w = self._reader.w
        h = self._reader.h

        dlg_settings = _GifExportDialog(n, w, h, parent=self, stylesheet=_DARK_STYLE)
        if dlg_settings.exec() != QDialog.DialogCode.Accepted:
            return

        step  = dlg_settings.step
        scale = dlg_settings.scale
        fps   = dlg_settings.fps

        base     = os.path.splitext(self._path)[0] if self._path else ''
        out_path, _ = QFileDialog.getSaveFileName(
            self, 'Export GIF', base + '.gif', 'GIF files (*.gif);;All files (*)')
        if not out_path:
            return

        levels = self.imview.getLevels()

        pca_mean     = self._pca_mean            if self._white_mode == 'pca' else None
        pca_comp     = self._pca_components      if self._white_mode == 'pca' else None
        pca_mean_low = self._pca_mean_low        if self._white_mode == 'pca' else None
        pca_comp_low = self._pca_components_low  if self._white_mode == 'pca' else None
        worker = GifExportWorker(
            out_path, self._reader, self._cache, n, step, scale, fps,
            levels,
            dark_field=self._dark_field,
            dark_field_gain=self._dark_field_gain,
            white_field=self._white_field if self._white_mode == 'flat' else None,
            white_field_gain=self._white_field_gain,
            pca_mean=pca_mean,
            pca_components=pca_comp,
            pca_n_components=self._pca_n_components,
            pca_mean_low=pca_mean_low,
            pca_components_low=pca_comp_low,
            pca_blur_enabled=self._pca_blur_enabled,
            pca_blur_sigma=self._pca_blur_sigma,
            pca_cell_gain=self._pca_cell_gain,
            gains=self._gains,
            parent=self,
        )
        self._gif_worker = worker

        prog_dlg = _ExportProgressDialog(
            max(1, (n + step - 1) // step), parent=self, stylesheet=_DARK_STYLE)
        prog_dlg.setWindowTitle('Exporting GIF…')
        prog_dlg.set_phase('Exporting')
        prog_dlg.cancel_requested.connect(worker.cancel)

        def on_progress(done, total):
            self.statusBar().showMessage(f'Exporting GIF frame {done} / {total}…')
            prog_dlg.update_progress(done, total)

        def on_finished(msg):
            self._export_act.setEnabled(True)
            self._export_gif_act.setEnabled(True)
            self.statusBar().showMessage(msg)
            prog_dlg.close()

        def on_error(msg):
            self._export_act.setEnabled(True)
            self._export_gif_act.setEnabled(True)
            if msg != 'GIF export cancelled.':
                QMessageBox.warning(self, 'GIF export error', msg)
            self.statusBar().showMessage(msg)
            prog_dlg.close()

        def on_worker_done():
            self._gif_worker = None

        worker.progress.connect(on_progress)
        worker.export_done.connect(on_finished)
        worker.export_error.connect(on_error)
        worker.finished.connect(on_worker_done)
        worker.finished.connect(worker.deleteLater)

        self._export_act.setEnabled(False)
        self._export_gif_act.setEnabled(False)
        self.statusBar().showMessage(f'Exporting GIF ({(n + step - 1) // step} frames)…')
        worker.start()
        prog_dlg.show()

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
        s.setValue('pca/blur_enabled',    self._pca_blur_enabled)
        s.setValue('pca/blur_sigma',      self._pca_blur_sigma)
        s.setValue('pca/universal',       self._pca_universal)
        s.setValue('pca/master_folder',   self._pca_master_folder)
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
        self._pca_blur_enabled  = s.value('pca/blur_enabled',  False, type=bool)
        self._pca_blur_sigma    = s.value('pca/blur_sigma',    400,   type=int)
        self._pca_universal     = s.value('pca/universal',     False, type=bool)
        self._pca_master_folder = s.value('pca/master_folder', '',    type=str)
        if self._pca_master_folder:
            self._pca_master_folder_mode = 'manual'
            self._update_master_menu_actions()
        pca_visible = chk_checked and (combo_idx == 1)
        self._pca_n_lbl.setVisible(pca_visible)
        self.pca_n_spin.setVisible(pca_visible)
        self._pca_settings_btn.setVisible(pca_visible)
        last = s.value('last_path', '')
        if last:
            self.statusBar().showMessage(f'Last file: {last}')

    def closeEvent(self, event):
        self._stop_play()
        self._save_settings()
        if self._pca_worker is not None:
            self._pca_worker.quit()
            self._pca_worker.wait(2000)
        if self._cell_adapt_worker is not None:
            self._cell_adapt_worker.quit()
            self._cell_adapt_worker.wait(2000)
        if self._export_worker is not None:
            self._export_worker.cancel()
            self._export_worker.wait()
        if self._reader is not None:
            try:
                self._reader.close()
            except Exception:
                pass
        super().closeEvent(event)


def _write_arrow_svgs():
    """Write light-colored SVG arrows to a temp dir for QSS; return path dict."""
    d = os.path.join(tempfile.gettempdir(), 'lucidvision_qss_arrows')
    os.makedirs(d, exist_ok=True)
    defs = {
        'up':     '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 5"><polygon points="4,0 8,5 0,5" fill="#aaaaaa"/></svg>',
        'up_hov': '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 5"><polygon points="4,0 8,5 0,5" fill="#dddddd"/></svg>',
        'dn':     '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 5"><polygon points="0,0 8,0 4,5" fill="#aaaaaa"/></svg>',
        'dn_hov': '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 5"><polygon points="0,0 8,0 4,5" fill="#dddddd"/></svg>',
    }
    paths = {}
    for name, svg in defs.items():
        p = os.path.join(d, f'arrow_{name}.svg')
        try:
            with open(p, 'w', encoding='utf-8') as fh:
                fh.write(svg)
            paths[name] = p.replace('\\', '/')
        except OSError:
            paths[name] = ''
    return paths


def _spinbox_qss(sel, p):
    return (
        f"{sel} {{ background: #3c3f41; color: #dddddd;"
        f" border: 1px solid #555; border-radius: 3px; padding: 2px 4px; }}\n"
        f"{sel}::up-button, {sel}::down-button {{"
        f" background: #4c5052; border: none; width: 16px; }}\n"
        f"{sel}::up-button:hover, {sel}::down-button:hover {{ background: #5c6062; }}\n"
        f"{sel}::up-arrow   {{ image: url({p['up']});     width: 8px; height: 5px; }}\n"
        f"{sel}::down-arrow {{ image: url({p['dn']});     width: 8px; height: 5px; }}\n"
        f"{sel}::up-arrow:hover   {{ image: url({p['up_hov']}); }}\n"
        f"{sel}::down-arrow:hover {{ image: url({p['dn_hov']}); }}\n"
    )


_arrow_svg_paths = _write_arrow_svgs()

_DARK_STYLE = (_DARK_STYLE_BASE + """
QSlider::groove:horizontal { height: 4px; background: #444; border-radius: 2px; }
QSlider::handle:horizontal { background: #888; border: 1px solid #aaa;
    width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }
QSlider::handle:horizontal:hover { background: #aaa; }
""" + _spinbox_qss('QSpinBox', _arrow_svg_paths)
    + _spinbox_qss('QDoubleSpinBox', _arrow_svg_paths))

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
    _ico = os.path.join(os.path.dirname(__file__), 'assets', 'tv_png.ico')
    if os.path.isfile(_ico):
        app.setWindowIcon(QIcon(_ico))
    win = LucidViewer()
    win.show()
    sys.exit(app.exec())
