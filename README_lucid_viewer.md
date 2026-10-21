# lucid_viewer.py — LucidLabs ATX245 Frame Viewer

A PyQt6 desktop viewer for LucidLabs ATX245 mono-camera acquisitions. Opens
raw camera files, applies dark-field / white-field corrections (flat field or
PCA-based background removal), and exports corrected stacks as TIFF or GIF.

---

## Getting Started

### 1. Install prerequisites

You only need to do this once.

**Git** — for downloading and updating the software:
- Download from https://git-scm.com/download/win
- During install, accept the default options. Make sure "Git from the command line" is selected so `git` is on your PATH.

**Miniconda** — for the Python environment:
- Download the Windows installer from https://docs.conda.io/en/latest/miniconda.html
- During install, select **"Just Me"** and accept the default install path (`%USERPROFILE%\AppData\Local\miniconda3`).
- You do **not** need to add conda to PATH or make it the default Python.

### 2. Clone the repository

Open **Git Bash** or **Command Prompt** and run:

```
git clone https://github.com/brad-ley/dcs-lucid-viewer.git
cd dcs-lucid-viewer
```

Or use GitHub Desktop / any Git GUI to clone `https://github.com/brad-ley/dcs-lucid-viewer.git`.

### 3. Run the viewer

Double-click **`launch.bat`** in the cloned folder.

On first run it will:
1. Pull the latest code from GitHub.
2. Create a `lucid-viewer` conda environment with all dependencies (takes a few minutes).
3. Open the viewer.

On every subsequent run it pulls updates and refreshes the environment automatically, then opens the viewer.

> **Offline use:** if you have no internet connection, `launch.bat` will warn that git pull failed and continue using the local copy.

---

---

## Contents

1. [Supported file formats](#file-formats)
2. [Pixel formats](#pixel-formats)
3. [Opening a file](#opening-a-file)
4. [Frame navigation](#frame-navigation)
5. [Display controls](#display-controls)
6. [Field correction](#field-correction)
   - [Dark field](#dark-field)
   - [Flat white field](#flat-white-field)
   - [PCA background removal](#pca-background-removal)
   - [Gaussian blur dual-resolution PCA](#gaussian-blur-dual-resolution-pca)
   - [Master / Universal PCA](#master--universal-pca)
7. [Storing field references](#storing-field-references)
8. [Export](#export)
   - [TIFF stack export](#tiff-stack-export)
   - [GIF export](#gif-export)
9. [Timestamps and trigger](#timestamps-and-trigger)
10. [File info panel](#file-info-panel)
11. [Keyboard shortcuts](#keyboard-shortcuts)
12. [Directory naming conventions](#directory-naming-conventions)
13. [Settings persistence](#settings-persistence)
14. [Dependencies](#dependencies)

---

## File formats

| Extension | How it is read |
|-----------|---------------|
| `.raw` | Binary blob; frame geometry and pixel format read from JSON sidecar or entered manually |
| `.tiff` / `.tif` | Multi-frame TIFF via `tifffile` |
| `.mp4` / `.avi` | Video via `opencv-python` |

### JSON sidecar

When opening a `.raw` file the viewer searches for metadata in this order:

1. `<file>.json` (e.g. `capture_Mono12p.raw.json`)
2. `<stem>.json` (e.g. `capture_Mono12p.json`)
3. `metadata.json` in the same folder

Keys read: `width`, `height`, `pixel_format` (also `pixelformat` / `PixelFormat`
for legacy sidecars), `fps`, `acquisition_time`, `notes`, `field_references`.

If no sidecar is found the viewer attempts to infer the pixel format from the
filename (e.g. a file containing `Mono12p` in its name is parsed as Mono12p).
Width/height fall back to the ATX245 full-sensor default (5328 × 4608).

---

## Pixel formats

All Lucid mono pixel formats are supported and unpacked to 16-bit internally:

| Format | Bits | Bytes / pixel |
|--------|------|--------------|
| Mono8 | 8 | 1 |
| Mono10 | 10 | 2 (little-endian uint16) |
| Mono10p | 10 | 1.25 (PFNC 4 px / 5 bytes) |
| Mono10Packed | 10 | 1.5 (Lucid: 2 px / 3 bytes) |
| Mono12 | 12 | 2 (little-endian uint16) |
| Mono12p | 12 | 1.5 (USB3 Vision/PFNC, LSB-aligned) |
| Mono12Packed | 12 | 1.5 (GigE Vision, MSB-aligned) |
| Mono16 | 16 | 2 |

---

## Opening a file

**File → Open file…** (Ctrl+O) or click **Open image file…** in the sidebar.

The **Frame format** group lets you manually set pixel format, width, and height
before opening. After changing any of these, click **Re-parse** to re-read the
file with the new parameters without opening a dialog.

The status bar and **File info** panel show the resolved format, frame count,
inferred FPS, and acquisition timestamp once a file is loaded.

---

## Frame navigation

- **Frame slider** — scrub through frames; thumb shows current position.
- **← / →** — step one frame backward / forward.
- **Home / End** — jump to the first / last frame.
- **Space or ▶ Play button** — start/stop playback at the file's native FPS.

---

## Display controls

**Colormap** — choose any pyqtgraph or matplotlib colormap from the drop-down.

**Auto levels** — sets the display range to the 3rd–97th percentile of the
current frame (applied automatically when a new file is opened).

**Pixel mask** — overlays colour indicators on the image:
- **Red** — saturated pixels (at or above the bit-depth maximum)
- **Yellow** — dead pixels (value = 0)

---

## Field correction

Corrections are applied in the **Correction** panel in the sidebar. Dark-only
correction gives dark-subtracted counts; flat/PCA correction gives float32
transmission; Median Filter gives float32 relative local intensity around 1.
Raw counts are shown when correction is off.

### Dark field

Check **Dark field** to subtract a dark reference before any white-field step.

**Auto-detection:** on file open the viewer scans for a folder whose name
contains `dark_field` among the siblings of the data folder (and one level up).
If multiple candidates exist, the one whose folder-name timestamp is closest to
the experiment folder's timestamp is chosen. Folders are shape-filtered: only
those whose frames match the current width × height are accepted.

Gain scaling is applied automatically when the dark reference was captured at a
different gain:

```
dark_scaled = dark × 10^((data_gain − dark_gain) / 20)
```

### Flat white field

Choose **Flat field** in the white-field combo and check **White field**.

A flat white-field correction divides the dark-subtracted data frame by the
averaged white-field mean:

```
transmission = (data − dark) / (white − dark)   [dark + white]
transmission = data / white                       [white only]
corrected    = data − dark                        [dark only]
```

**Auto-detection:** same timestamp-proximity logic as dark field, searching for
folders whose names contain `white_field`.

### Synthetic white field: Median Filter

Choose **Median Filter** in the white-field combo and check **White field**.
No reference folder is needed. A **Patch size** selector appears, defaulting to
**100 pixels**; it is hidden for the other modes. Patch size and mode persist
in application settings, and **Store** can save them in the recording sidecar.

Each frame is first dark-subtracted with the usual gain matching, if a valid
dark reference is active. A fast approximate median background is estimated
from an area-reduced float image (about 15 working samples across a large
patch), then interpolated back to full size. The original dark-corrected pixels
are divided by that background. Only the estimated white field is reduced;
image detail is not downsampled or quantized to 8-bit values.

This removes overall attenuation and broad structures, retaining localized
variations. It is **relative contrast, not absolute transmission**. The Density
Retrieval tool receives this relative image and labels it accordingly; it
cannot recover the removed attenuation. Nonpositive or negligible background
pixels are masked to zero, with their count reported; signed input variations
are retained where the background is valid.

Filtering runs in a Qt worker with one latest pending frame, obsolete-result
protection, a two-frame result cache, and debounced patch-size changes.
Playback allows each calculation to finish before following the playback
clock, so it can skip frames without continually cancelling every result.
Larger jobs use CUDA when available, with bounded patch scratch memory and a
CPU/SciPy fallback. Initial CUDA loading also happens in the worker.

Current-frame, TIFF-stack, average-frame and GIF exports use the same correction.
Average exports normalize **each frame before averaging**. TIFF metadata
(including the ImageJ mapping sidecar) records the approximation, requested patch,
working grid and `absolute_transmission: false`. Suggested TIFF names include
`_median_white` or `_dark_median_white`.

Run `python -m unittest test_median_white_field -v` to check the median math,
CPU/CUDA edge agreement, GUI worker behavior, cancellation and export semantics.

### PCA background removal

Choose **PCA removal** in the white-field combo and check **White field**.

PCA removal builds a basis from multiple white-field frames, then for each data
frame it estimates the beam background using that basis and divides:

```
bg   = mean + V.T @ (V @ (data − mean))
trans = clip(data / max(bg, 0.5 × mean), 0, 10)
```

where `V` is the `(n_components, H×W)` matrix of eigenvectors computed from the
white-field frames (via SVD / randomized PCA).

**N components** spin box — number of PCA components to subtract (1–20,
default 5). More components capture more beam variation but risk subtracting
real sample signal.

**PCA settings…** — opens the [PCA Settings dialog](#pca-settings-dialog).

The PCA basis is computed from the white-field folder in a background thread the
first time it is needed. Progress is shown in the correction status label.

#### PCA Settings dialog

**Config → Set White Field → PCA…** or click **PCA settings…**.

##### Gaussian blur dual-resolution PCA

Enable **Gaussian blur for temporal weight projection** and set a **Blur sigma**
(pixels, default 400 px).

Instead of projecting the raw data frame onto the PCA basis, the frame is first
blurred with a Gaussian (FFT-accelerated, O(H·W·log(H·W))) and block-average
downsampled. The projection is done in this low-resolution space to obtain beam
coefficients that are insensitive to sample features. The background is then
reconstructed using the full-resolution eigenvectors:

```
d_low  = blur_and_bin(data, σ) − mean_low
coeffs = V_low @ d_low            # projection in blurred space
bg     = mean + V_high.T @ coeffs # reconstruction at full resolution
```

This mode requires two PCA passes: one on the raw white frames (for `V_high`
and `mean`) and one on the blurred white frames (for `V_low` and `mean_low`).
Both passes run automatically when the white-field folder is loaded.

### Master / Universal PCA

Enable **Universal mode (master no-cell basis)** in the PCA Settings dialog.

In a typical experiment the x-ray cell changes between acquisitions, shifting the
mean white-field image. Universal PCA avoids recomputing the full SVD for every
cell by:

1. Computing the PCA once from a **master** (cell-free) white-field capture.
2. For each experiment, loading the experiment's own white-field frames to
   compute a per-cell mean `W_cell`.
3. Adapting the master eigenvectors via element-wise scaling:

```
T_cell = W_cell / W_master        # per-pixel gain map
E_cell[i] = T_cell × E_master[i] # adapted i-th eigenvector
E_cell[i] /= |E_cell[i]|         # row-normalize
```

This produces cell-adapted components without an SVD recomputation.

**Setting the master folder — Config → Set White Field → Master PCA…**

The viewer also **auto-detects** the master folder when Universal mode is first
activated: it walks up the directory tree (up to 4 levels) looking for a sibling
folder whose name contains `_master`. If multiple candidates are found, the one
whose name timestamp is closest to the experiment timestamp is chosen. The
auto-detected path is saved to settings and shown in the status label.

**Cell white-field auto-detection in Universal mode:** the viewer prefers a
`white_field` folder that does *not* contain `_master` in its name (to avoid
picking the master capture as the cell reference). If no non-master white-field
folder is found it falls back to any matching folder.

**Tooltip on ✓ White** — when Universal PCA is active and cell adaptation
succeeds, hovering the ✓ White label shows both the master folder path and the
cell white-field folder path.

---

## Storing field references

Once the desired dark/white correction is active, click **Store** to write the
folder paths into the file's sidecar JSON under `field_references`. On the next
open of the same `.raw` file, the same correction is applied automatically.

Stored keys: `dark_folder`, `white_folder`, `white_pca_folder`,
`white_pca_master_folder`, `pca_universal`, `white_mode`, `median_patch_size_px`.

---

## Export

### TIFF stack export

**File → Export TIFF stack…** (Ctrl+E).

Exports all frames as a multi-page TIFF with correction applied (if active).
Frames are written as **float32** when correction is active, **uint16** when not.

The output filename is auto-suggested with a suffix reflecting the active
correction (`_pca`, `_dark_pca`, `_dark_white`, etc.) and optionally a
compression tag.

**Export settings (Config → Export settings…)**:

- **Compression** — None / LZW / ZSTD. ZSTD typically gives best ratio for
  float32 data; LZW for uint16. None gives maximum read speed.
- **ImageJ mode** — writes an ImageJ-compatible TIFF with per-frame metadata in
  the ImageDescription tag. Automatically falls back to standard BigTIFF for
  stacks > 3.9 GB (ImageJ's hard limit).

A JSON sidecar is written alongside the TIFF containing: source file, pixel
format, frame count, FPS, timestamps, gains, exposures, trigger data, and
correction parameters.

For stacks larger than 4 GB the viewer automatically uses BigTIFF format (Fiji
opens these via Bio-Formats).

### GIF export

**File → Export GIF…** (Ctrl+G). Requires `Pillow`.

Presents a dialog to select:
- **Frame step** — export every Nth frame (1 = all frames).
- **Scale** — resize factor (e.g. 0.25 for quarter resolution).
- **FPS** — playback speed of the output GIF.

The current display levels (brightness/contrast) are applied when rendering each
frame, so what you see is what you get in the GIF.

---

## Timestamps and trigger

Per-frame timing and trigger data are read from `frame_data.csv` (or
`timestamps.csv`) in the same folder as the data file.

The timestamp column is auto-detected: a column header containing
`timestamp / time / ts / ticks / device / system` is preferred; a `_ns` suffix
means nanoseconds, `_us` microseconds, `_ms` milliseconds, `_s` seconds.

**Trigger detection** — the viewer reads `line_status_all`, a binary string
(e.g. `00000101`). The configurable **trigger bit** (default: bit 2, the 3rd bit
from the right) marks the first frame where that bit goes high as **t = 0**.
Frames before the trigger get negative timestamps; frames after get positive
ones. If no trigger is found, times are relative to the first frame.

**Config → Trigger settings…** — change which bit is used as the trigger.

The frame slider is annotated with a tick at the trigger frame. The frame label
shows the current frame's timestamp in milliseconds relative to the trigger.

---

## File info panel

The **File info** group in the sidebar shows, once a file is open:

| Field | Content |
|-------|---------|
| Folder | Parent directory name |
| File | Filename |
| Format | Resolved pixel format |
| Frames | Total frame count |
| FPS | From sidecar or inferred from timestamps |
| Size | Width × Height in pixels |
| Acquired | Acquisition timestamp from sidecar |
| Notes | Free-text notes from sidecar |

---

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| Ctrl+O | Open file |
| Ctrl+E | Export TIFF stack |
| Ctrl+G | Export GIF |
| ← / → | Step one frame backward / forward |
| Home | Jump to first frame |
| End | Jump to last frame |
| Space | Toggle playback |
| F1 | Open HTML manual in browser |

---

## Directory naming conventions

The auto-detection logic looks for keyword matches in folder names.

| Folder name must contain | Used as |
|--------------------------|---------|
| `dark_field` | Dark field reference |
| `white_field` | White field reference (flat or PCA) |
| `white_field` + not `_master` | Cell white field in Universal PCA mode |
| `_master` | Master (no-cell) white field for Universal PCA |

Folder search order: siblings of the data file's folder first, then siblings of
that folder (one level up). The closest-timestamp match is preferred.

### Example layout

```
experiment_root/
  20250501_120000_master_white_field/   <- master (cell-free) white field
  20250501_133000_white_field/          <- dark field for session
  20250501_133000_dark_field/           <- dark field for session
  20250501_140000_cell_A/
    capture_Mono12p.raw
    metadata.json
    frame_data.csv
    white_field_20250501_140000/        <- cell A white field
  20250501_153000_cell_B/
    capture_Mono12p.raw
    metadata.json
    frame_data.csv
```

In this layout, when cell A is open with Universal PCA, the viewer finds
`20250501_120000_master_white_field` as the master folder and
`white_field_20250501_140000` as the cell white field.

---


## Spatial calibration and measurements

Choose **Tools → Set Scale…** to open a nonmodal calibration window. Enter
**Distance in pixels** and **Known distance (µm)**, or click **Draw Calibration
Line** and drag across a known feature in the main image. The amber line has
draggable endpoints; its pixel length updates live in the window. Editing the
pixel distance also resizes the line. Escape or right-click cancels drawing.

**Apply Scale** immediately uses `known distance / pixel distance` in µm/px.
A name is optional when applying. **Save Calibration** requires a nonblank,
unique name (case insensitive), stores the two distances, and applies the scale.
Saved calibrations appear in the top drop-down: selecting one loads its fields
for review; click **Apply Scale** to use it. To save an edited calibration,
enter a new unique name. **Delete Stored** removes the selected preset without
changing the scale currently in use.

The measurement bar above the image shows the active scale. A manual scale
persists across recordings and viewer restarts; **Use Recording Scale** returns
to the sensor-pitch/objective calibration in each recording's metadata. Unknown
recording scale uses pixels. The calibration assumes square image pixels and
is the sample-plane pitch shared by measurements, newly opened FFT windows,
and density retrieval. Applying a scale also updates an open density-retrieval
window; existing FFT windows retain the scale with which they were opened.

Click **Measure** and drag a line on the image. Each click arms one new line;
drag either circular endpoint or the line body to refine it. Numbered blue
labels show length in µm, or pixels before calibration. Lines stay at their
source-pixel coordinates through frame changes and opening other images,
including different image sizes. Applying a new scale relabels all of them.
**Clear Measurements** removes all measurement lines. Measurements last until
cleared or the viewer closes; saved calibrations persist separately. Overlays
are for inspection and are not burned into exported image data.


## Density Retrieval

Choose **Tools → Density Retrieval…** with a frame open. The nonmodal window
uses its own copy of the viewer's current corrected image, before colormap,
histogram contrast, pixel masks or integer export encoding. Frame navigation
and dark/white/PCA correction changes update that copy. No background or
flat-field correction is repeated inside the tool. Input should be normalized
transmission (vacuum = 1); opening the tool does not normalize raw counts.

The analysis matches the reference `pci_retrieval_gui.py`: select **Paganin
Fourier filter** or **Nonlinear Fresnel with TV (TikhonovTV)** from its Phase
Retrieval menu. Energy, formula, RR1 (source to sample), RR2 (sample to detector),
pixel pitch and reference material density are editable. Pixel pitch uses the
reference's sample-plane convention and starts at sensor pitch / microscope
objective magnification when that calibration is available, or 0.274 µm
otherwise. A manual pitch edit is retained while stepping through a recording.

The iterative solver exposes TV and Tikhonov regularization, maximum iterations,
relative tolerance, and CUDA/CPU selection; CUDA falls back to CPU when unavailable.
The **Output → Plot** selector defaults to **Areal density (mg/cm²)** and also
provides **Thickness (µm)**, **Exit phase (rad)**, and **Object-plane transmission
(I/I₀)**. Switching outputs reuses the completed retrieval and updates the plot
title and cursor units; it does not rerun either solver or change the ROI.
Both images have linked spatial axes, independent histograms, equal/stretch
aspect options, and cursor readouts.

Exit phase is the unwrapped projected phase shift relative to vacuum,
`φ = -2π δ t / λ`; positive equivalent thickness gives negative phase.
Object-plane transmission is the intensity ratio immediately after the object,
before propagation to the detector: `T = I_exit/I_incident = exp(-μ t)`, where
`μ = 4π β/λ`. Vacuum has phase zero and transmission one. This is intensity,
not field amplitude. Both outputs use the same selected material/energy model
and retrieval regularization; they are not independent phase and absorption
reconstructions. Synthetic median white fields still provide relative contrast
only and cannot recover the removed overall attenuation or phase offset.

Enable **Processing ROI** to process a subset of the corrected image. Click
**Draw Box** and drag on the full input image, then move/resize the rectangle or
edit its zero-based **X**, **Y**, **Width**, and **Height** in pixels. Escape or
clicking Draw Box again cancels an unfinished selection. **Full Image** clears
the selection and restores processing of the entire frame.

The selected array is cropped before either solver runs, including before FFTs
and CUDA transfers; the input display stays uncropped. The group shows the
selected dimensions and percentage of full-frame pixels. Retrieved pixels and
cursor readouts retain their original image coordinates. A selection is retained
between frames of the same recording and resets for a different source or image
size. Auto retrieval waits until a box drag finishes. Include a margin around
features: cropping changes the solvers' Fourier boundaries and can affect values
near crop edges.

Auto retrieval follows image and parameter changes. Uncheck **Auto Retrieve
Phase** to calculate only when **Retrieve Phase** is pressed. One Qt worker
runs at a time and retains only the latest pending request; obsolete results
are discarded. During continuous playback, retrieval catches up to the latest
frame once a calculation can finish without another change.

Closing a busy retrieval window hides it immediately and lets its numerical
call finish before releasing the worker. Closing the main viewer also waits
asynchronously for that worker, so its GUI stays responsive. HoToPy's internal
iteration loop is not forcibly terminated.

New environments include xraylib, PyTorch and HoToPy. Update an existing
viewer environment from this repository before using the new tool:

```powershell
conda env update -n lucid-viewer -f environment.yml
```

Optional solver imports happen inside the worker; a missing dependency produces
an installation message in the tool's status bar. Paganin needs xraylib;
nonlinear retrieval additionally needs torch and hotopy (validated with 0.25).
These homogeneous-material, single-distance methods retain the reference's
geometry, material model, Fourier boundaries and logarithm floor.

The **?** button in **Iterative Solver** opens a nonmodal guide to **Alpha TV**
and **Tikhonov Alpha**, with noisy toy reconstructions at low, intermediate
and high alpha values, plus practical tuning advice. It
explains edge/noise tradeoffs, density bias, and the scalar Tikhonov weighting
used by this viewer. The guide can stay open while adjusting parameters.

Run the focused regression tests with `python -m unittest test_density_retrieval -v`.
Qt tests use isolated settings and an offscreen window. Nonlinear tests skip
when HoToPy is not installed. CUDA tests additionally require a CUDA-enabled
PyTorch build and an available GPU; they check vacuum, CPU/GPU agreement and
responsive Qt event handling during real GPU retrieval.

---

## Settings persistence

All user preferences are saved via `QSettings` (Windows Registry /
platform-native store) and restored on next launch:

- Last opened file path
- Pixel format, width, height
- Colormap
- PCA blur enabled, blur sigma
- PCA universal mode enabled
- Master folder path
- Export compression, ImageJ mode
- Trigger bit

---

## Dependencies

```
pip install PyQt6 pyqtgraph numpy scipy tifffile opencv-python pillow xraylib torch hotopy==0.25
```

| Package | Required for |
|---------|-------------|
| `PyQt6` | UI framework |
| `pyqtgraph` | Image display and histogram |
| `numpy` | All array math |
| `scipy` | FFT-accelerated Gaussian blur and retrieval constants |
| `tifffile` | TIFF reading and TIFF stack export |
| `opencv-python` | MP4/AVI video reading |
| `pillow` | GIF export |
| `xraylib` | Density Retrieval material optical constants |
| `torch`, `hotopy` | Nonlinear Fresnel density retrieval |

### Windows Qt startup

The viewer calls `viewer_runtime.prepare_qt_runtime()` before loading NumPy or Qt.
This loads `icuuc.dll` from Windows System32. Conda also supplies an `icuuc.dll`,
but its exports differ from the Windows ICU exports that the pip Qt runtime
needs; loading conda's copy can produce
`DLL load failed ... The specified procedure could not be found` when importing
QtCore or QtWidgets. The startup helper resolves this within the viewer process.

Run `python -m unittest test_viewer_runtime -v` to verify a fresh import and
window launch in the selected environment. The tests use temporary settings.

### Windows nonlinear-solver runtime

The conda environment pins NumPy's BLAS/LAPACK backend to OpenBLAS. The MKL
backend can initialize a different OpenMP runtime before PyTorch is loaded;
then selecting Nonlinear Fresnel + TV may report WinError 127 while loading
`torch/lib/shm.dll`. OpenBLAS avoids that runtime collision while retaining
multithreaded linear algebra.

To repair an older `lucid-viewer` environment:

```powershell
conda install -n lucid-viewer -c conda-forge "libblas=*=*openblas" "liblapack=*=*openblas"
```

Restart the viewer after an environment repair so it loads the new native
libraries. The regression
`python -m unittest test_viewer_runtime.ViewerRuntimeTests.test_pca_libraries_then_nonlinear_tv_in_fresh_process -v`
checks the actual order: Qt startup, NumPy SVD/least squares, then nonlinear TV
retrieval in a fresh process.

### GPU environment

`lucid-viewer-gpu` is an independent copy of the working `lucid-viewer`
environment, with PyTorch replaced by the matching CUDA 13.0 build. It retains
the existing Qt, HoToPy, NumPy/OpenBLAS and other dependency versions.

To reproduce it from the working CPU environment:

```powershell
conda create -n lucid-viewer-gpu --clone lucid-viewer --copy -y
conda run -n lucid-viewer-gpu python -m pip install --upgrade --no-deps "torch==2.14.1+cu130" --index-url https://download.pytorch.org/whl/cu130
```

Launch the viewer using the GPU environment:

```powershell
conda activate lucid-viewer-gpu
python lucid_viewer.py
```

In Density Retrieval, select **Nonlinear Fresnel with TV** and solver device
**cuda**. The CUDA wheel includes its runtime libraries; an NVIDIA driver
compatible with CUDA 13.0 is required. Check the selected environment with:

```powershell
python -c "import torch; print(torch.__version__, torch.version.cuda); print(torch.cuda.is_available()); print(torch.cuda.get_device_name(0))"
python -m pip check
python -m unittest test_viewer_runtime test_density_retrieval -v
```

Both CUDA tests must run rather than skip to verify GPU retrieval and responsive
Qt event handling. Keep the OpenBLAS pins when updating this environment.
