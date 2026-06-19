# Lucid Camera Acquisition Tool

Desktop application for controlling Lucid Vision Labs cameras and acquiring high-speed image streams. Built on Qt 6 with the Arena SDK.

---

## Features

- **Live Preview** — real-time camera stream at ~30 fps with zoom/pan and auto-contrast display
- **Batch Acquisition** — capture frames continuously until stopped; automatic session folder naming (`acq_YYYYMMDD_HHmmss`), shown as placeholder text so you always know where data will land
- **Auto-Stop** — configure a timed acquisition duration (µs / ms / s); the Stop button counts down live ("Stop (auto 5s)") and stops cleanly when the timer expires
- **Pinned Parameters** — star any GenICam node in the Advanced dialog to pin it to the main panel; list persists in the registry
- **Advanced Parameter Browser** — full GenICam node tree with Beginner / Expert / Guru / Favorites filtering
- **Config Menu** — per-preference dialogs for Auto Contrast percentile thresholds and Auto-Stop duration; all settings saved to the Windows registry
- **Metadata Logging** — camera settings + per-frame gain saved as `metadata.json` and `frame_data.csv`; full save path printed to the log on completion
- **Save Formats** — Raw sequence, TIFF sequence, or single Raw video file (preference saved to registry)
- **Acquisition Notes** — free-form text saved alongside each acquisition session

---

## Quick Start

1. Launch the application
2. Click **Refresh** to scan for cameras
3. Select a camera from the dropdown and click **Connect**
4. Adjust parameters on the main panel (or open **Advanced...** for full control)
5. Choose an output folder with **Browse**
6. Click **Start Acquisition** (or **Start Streaming** if TriggerMode is On — see [Acquisition Modes](#acquisition-modes))

---

## Camera Connection

- Only one camera can be connected at a time — click **Disconnect** before switching
- Always disconnect or close the application cleanly; force-killing the process may require a system restart to reconnect the camera

---

## Camera Parameters Panel

The pinned parameters panel shows editable controls for any GenICam nodes you have starred. To add or remove parameters:

1. Click **Advanced...** to open the full parameter browser
2. Navigate to any parameter using the Category and Feature dropdowns
3. Click **★** to pin it to the main panel — click again to unpin
4. Use the **Favorites** filter in the Advanced dialog to see and manage your pinned set
5. Click **Edit list...** to reorder or remove entries from the pinned list directly

**Widget types by node type:**

| Type | Control | Example |
|------|---------|---------|
| Enumeration | Combo box | `PixelFormat`, `TriggerMode` |
| Float | Decimal spinner | `ExposureTime` (µs), `Gain` (dB) |
| Integer | Integer spinner | `Width`, `Height`, `OffsetX` |
| Boolean | Combo box (false/true) | `GainAuto` |

Click **Apply Pinned Parameters** to write all widget values to the camera.

---

## Advanced Parameter Browser

Accessible via the **Advanced...** button. Provides access to every GenICam node the camera exposes.

- **Visibility filter** — All Levels / Favorites / Beginner / Expert / Guru
- **Category dropdown** — top-level GenICam categories (Acquisition Control, Image Format Control, etc.)
- **Feature dropdown** — leaf nodes in the selected category; the **★** button (inline, to its right) toggles the current feature's pinned status on the main panel
- **Apply** — writes the current widget value to the camera
- **Refresh** — re-scans the node tree (useful after a parameter change makes others appear/disappear)

The dialog reopens to whichever parameter was selected when it was last closed.

> **Live read-back:** each time you click a feature in the Feature dropdown, the dialog re-reads that node's current value directly from the camera — it does not use a cached value. This means dependent nodes (e.g. `TriggerSelector` after changing `TriggerSource`) always reflect the camera's actual current state.

---

## Config Menu

The **Config** menu (in the main window menu bar) controls acquisition preferences persisted to the Windows registry:

| Item | What it controls |
|------|-----------------|
| **Auto-Stop Acquisition** (checkable) | Toggles timed auto-stop on/off directly — check state persists across launches |
| **Auto-Stop Settings...** | Opens the settings dialog to configure the duration (µs / ms / s) |

Auto-contrast settings have moved to the **Preview window** — see [Live Preview](#live-preview).

---

## Acquisition

### Setup

1. Select a **Save format** (preference persists across launches):
   - **Raw Sequence** — one `.raw` file per frame (fastest)
   - **TIFF Sequence** — one `.tiff` per frame (readable in most image viewers)
   - **Raw Video** — single file with all frames concatenated
2. Enter an optional **Acquisition name** (folder name); leave blank to auto-generate `acq_YYYYMMDD_HHmmss` — the placeholder text shows exactly what that name would be right now
3. Click **Notes...** to add free-form notes saved in `metadata.json`
4. Choose an **Output folder** with **Browse**
5. Optionally configure a timed stop via **Config → Auto-Stop Acquisition**
6. Click **Start Acquisition** — if `TriggerMode` is `On`, the button reads **Start Streaming** instead (the camera waits for a hardware trigger to deliver frames after arming)

### Acquisition Modes

`AcquisitionMode` controls how many frames the camera delivers per acquisition. Set it via **Advanced... → Acquisition Control → AcquisitionMode** before clicking Start.

When `TriggerMode` is `On`, the Start button label changes to **Start Streaming** to signal that the camera will wait for a hardware trigger to begin delivering frames — software-initiated capture is not used. The Stop button and auto-stop timer behave the same regardless of trigger mode.

| Mode | Behaviour |
|------|-----------|
| **Continuous** | Streams indefinitely until you click Stop or the auto-stop timer fires |
| **SingleFrame** | Camera delivers exactly 1 frame, then stops automatically |
| **MultiFrame** | Camera delivers the number of frames set in `AcquisitionFrameCount`, then stops automatically |

In SingleFrame and MultiFrame modes the acquisition loop exits as soon as all expected frames have been received — no Stop click is needed. If frames are lost on the network before the expected count is reached, acquisition reports a timeout error.

### Auto-Stop

Open **Config → Auto-Stop Acquisition** to set a duration (supports µs, ms, s). Auto-Stop is only active in **Continuous** mode — in SingleFrame and MultiFrame modes the camera itself controls the frame count, so the timer is not armed.

The Stop button reflects the auto-stop configuration at application launch — if auto-stop was enabled in the previous session, the button already shows **Stop (auto Ns)** before a camera is connected, so you can see at a glance that the feature is armed.

When auto-stop is enabled and the camera is in Continuous mode:

- The Stop button changes to **Stop (auto Ns)** and counts down each second
- Hovering the Stop button shows a tooltip with the remaining time
- The acquisition stops automatically when the timer expires — no manual click needed
- A manual click on Stop also cancels the countdown cleanly

When it finishes, the log shows the full path to the saved session folder. Preview is automatically disabled during acquisition and re-enabled when it completes.

### Output Files

Each acquisition creates a timestamped subfolder containing:

| File | Contents |
|------|----------|
| `frame_0000.raw` / `.tiff` | Individual frame files |
| `metadata.json` | Acquisition timestamp, pinned camera settings, and notes |
| `frame_data.csv` | Per-frame index, timestamp (ns), and gain (dB) |
| `camera_settings.json` | Full snapshot of every readable camera node at acquisition start (same content as **Save Custom** in the Advanced dialog) |

**metadata.json example:**
```json
{
  "timestamp": "2025-06-03T14:30:45Z",
  "num_frames": 100,
  "camera_settings": {
    "ExposureTime": 500.0,
    "Gain": 6.5,
    "PixelFormat": "Mono16"
  },
  "notes": "Outdoor test, bright sunlight"
}
```

**frame_data.csv example:**
```
frame_index,timestamp_ns,gain_db
0,1234567890,6.5000
1,2468135780,6.5000
```

Timestamps are nanoseconds since acquisition start. Divide by `1,000,000,000` to get seconds.

---

## Live Preview

Click **Preview** in the Acquisition section to open the preview window. It is modeless — the main window stays fully interactive while preview is open.

### Controls

| Control | Location | Description |
|---------|----------|-------------|
| **Start Preview** / **Stop Preview** | Bottom bar | Arms and disarms the preview stream |
| **Auto Contrast** button | Bottom bar (turns blue when on) | Toggles percentile-based contrast stretch on the display |
| **Draw ROI** button | Bottom bar (turns orange when on) | Click and drag on the image to set an ROI rectangle; right-click the button to reset ROI to full frame |
| **Auto-contrast** menu | Menu bar | "Enable Auto Contrast" (checkable, mirrors the button) and "Auto Contrast Settings…" (percentile thresholds) |
| **ROI** menu | Menu bar | "Draw ROI Bounds" (checkable, mirrors the button) and "Reset ROI to Full Frame" |
| **Tools** menu | Menu bar | "Open Focus Diagnostic" — launches the focus diagnostic tool |

### Display features

- **Histogram** — live pixel intensity histogram shown to the right of the image with draggable black/white-point handles
- **Zoom / pan** — scroll wheel to zoom, drag to pan; double-click to fit the image to the window
- **Saturation overlay** — pixels at maximum value (255 in 8-bit display) are tinted red
- **Dead-pixel overlay** — pixels at zero are tinted yellow
- **Pixel readback** — left-click any pixel to pin a readback overlay showing its raw sensor count
- **Line status** — GPIO line states (from ChunkLineStatusAll) displayed in the status bar when chunk data is enabled

### FPS counters

The status bar shows two FPS values:
- **Display FPS** — how fast the preview window is rendering (throttled to ~30 fps)
- **Camera FPS** — true acquisition rate reported by the preview worker

### Notes

- Auto-contrast and ROI button states are mirrored by the menu bar — use whichever is more convenient
- Preview is automatically paused when acquisition starts and resumes when it finishes
- Closing the preview window stops streaming automatically

---

## Lucid Viewer — White Field Division

The companion Python viewer (`lucid_viewer.py`) corrects beam inhomogeneity in three
modes. Set the active mode from **Config → Set White Field** and the sidebar
**Correction** group.

### Folder naming conventions

Capture reference frames at the same resolution and pixel format as your experiment
and store them in folders alongside your data:

| Folder name pattern | Purpose |
|---------------------|---------|
| `dark_field_*` | Camera dark current (no beam, lens cap on) |
| `white_field_*` | Full-beam flat-field or PCA reference |
| `*_master*` | Universal / Master PCA no-cell reference |

The viewer auto-discovers the closest-timestamp folder when you open a file.
A file with `mean` in its name inside the folder is preferred for flat-field mode.

### Mode 1 — Flat Field

**Config → Set White Field → Flat Field…**

Divides each data frame by an averaged white-field image, yielding transmission in
the range 0–1. When a dark field is also loaded both dark-current terms are removed:

| Fields loaded | Formula |
|---------------|---------|
| White only | `T = data / white` |
| Dark + White | `T = (data − dark) / (white − dark)` |
| Dark only | `corrected = data − dark` |

If the data and field captures used different camera gains the viewer automatically
scale-corrects before dividing:
`field_scaled = field × 10^((data_gain − field_gain) / 20)`

### Mode 2 — PCA Removal

**Config → Set White Field → PCA…**

Models the x-ray beam background with PCA built from the white-field folder and
removes it per-frame (`T = data / bg`).

1. All frames in the selected white-field folder are stacked.
2. PCA is computed (up to N+2 components capped at 20; N is the sidebar spinner).
   The number of components used for each frame is auto-selected by explained
   variance.
3. Each data frame is projected onto the basis to reconstruct the smooth beam
   background `bg`, then `T = data / bg`.

**PCA Settings dialog** (`Config → PCA Settings…` or the sidebar button):

| Option | Effect |
|--------|--------|
| N components (1–20) | Number of PCA components used for background estimation |
| Gaussian blur σ | Blur frames before projection to isolate macro-scale beam variation |
| Universal mode | Adapt Master PCA eigenvectors per-experiment instead of recomputing |

A cache file `pca_cache_{H}x{W}.npz` (or `…_blur{sigma}.npz` when blur is on) is
written to the white-field folder after the first computation and reused until any
source frame is newer. Delete the cache file to force recomputation.

### Mode 3 — Universal / Master PCA

**Config → Set White Field → Master PCA…** *(enable Universal mode in PCA Settings)*

Builds one PCA basis from a "no-cell" master folder and adapts it per-experiment
without re-running SVD:

1. Master PCA is loaded from cache or computed from the master folder.
2. Per-cell mean is computed from the current white-field folder.
3. Each eigenvector is rescaled: `E_cell = E_master × (cell_mean / master_mean)`.
4. The scaled basis is re-orthogonalized via QR decomposition.

The master folder is located automatically by searching parent directories for any
folder with `_master` in its name, sorted by timestamp proximity.

### Storing field references

Click **Store** in the sidebar Correction group to save the currently loaded field
folder paths into the experiment's `.json` sidecar (`field_references` key). On the
next open the viewer reloads them automatically.

### TIFF export with correction

**File → Export TIFF…** applies the active correction to every frame and writes a
float32 TIFF stack. Each page stores `transmission_min`/`transmission_max` in TIFF
metadata. With ImageJ-compatible export these values go to a `_transmission.json`
sidecar instead.

---

## Troubleshooting

**Camera not detected**
- Verify power and Ethernet connection (GigE cameras)
- Check Windows Device Manager for the network adapter
- For 10GigE cameras, confirm your NIC supports jumbo frames (8192 bytes)
- Restart the application

**Preview shows blank or corrupted image**
- Check exposure and gain — they may be too low for the lighting
- Stop and restart preview
- Disconnect and reconnect the camera

**Acquisition stops early**
- Check the log for error messages
- Verify the output drive has sufficient free space and is not slow/unreliable
- For GigE cameras, check network stability (avoid Wi-Fi)

**Parameters fail to apply**
- Some nodes are read-only on certain camera models
- Check that the value is within the valid min/max range shown in the Advanced dialog
- Ensure `ExposureAuto` is `Off` before manually setting `ExposureTime` (same for `GainAuto` / `Gain`)

**High CPU or low preview FPS**
- Close other resource-heavy applications
- Set Windows power plan to **High Performance**
- High-resolution cameras require more scaling work in the preview — some FPS reduction is expected

---

## Requirements

- Windows 10/11 (64-bit)
- [Lucid Vision Labs Arena SDK](https://thinklucid.com/downloads-hub/) installed
- Qt 6.11+ (MSVC 2022 64-bit)
- GigE Vision camera connected via Ethernet

## Building

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Qt path and Arena SDK path are configured in `CMakeLists.txt`.
