# Lucid Raw Reader / Corrector — ImageJ / Fiji plugins

Open Lucid Vision Labs ATX245 **`.raw`** files directly in ImageJ/Fiji, with no
TIFF conversion step. Frames load **lazily as a virtual stack**, so a 7 GB raw
file opens instantly and uses almost no RAM — only the frame you're viewing is
decoded.

There are two plugins:

- **`Lucid_Raw_Reader`** — opens the raw data as a 16-bit stack (sensor counts).
- **`Lucid_Raw_Corrector`** — same lazy loading, but with optional dark-field /
  white-field correction, displayed as **32-bit float x-ray transmission**. See
  "Field correction" below.

Both read the frame geometry and pixel format from the JSON sidecar that
`lucid_viewer.py` already writes, unpack every Lucid pixel format, and label each
slice with its timestamp in **milliseconds**, measured **relative to the event
trigger** (from `frame_data.csv`; see "Timestamps" below).

## What it does

- Reads `width`, `height`, and `pixel_format` from a sidecar next to the file
  (searched as `<file>.json`, then `<stem>.json`, then `metadata.json`). If none
  is found, it falls back to the format hinted by the filename, and finally to a
  dialog where you enter the geometry/format yourself.
- Infers the frame count from the file size, the way `lucid_viewer.py` does.
- Unpacks `Mono8, Mono10, Mono10p, Mono10Packed, Mono12, Mono12p, Mono12Packed,
  Mono16` to a 16-bit (`GRAY16`) virtual stack. The bit math is a direct,
  verified port of `lucid_viewer.py`'s `unpack_frame`.
- Reads per-frame timestamps from `frame_data.csv` (or `timestamps.csv`) in the
  same folder and shows them as slice labels in **milliseconds, relative to the
  event trigger** (`frame N   t = <ms> ms`). See "Timestamps" below. The stack's
  frame interval is also set from the median spacing.

## Field correction (`Lucid_Raw_Corrector`)

When you open a file with the **Corrector** plugin it looks for dark/white
reference captures and offers to apply them, displaying the result as float
transmission.

How references are found (mirrors `lucid_viewer.py`): it scans for folders whose
names contain `dark_field` / `white_field`, first among the siblings of the data
file, then among the siblings of the data folder, taking the **most recent** by
name. Each capture's average gain is read from its own `frame_data.csv`.

A single dialog opens that **presents the found captures** (folder names + gains)
and lets you choose:

- **Apply dark-field correction** / **Apply white-field correction** (checkboxes).
- **Gain handling:**
  - **Match gain** — use the most-recent capture whose gain equals the data gain
    (within 0.1 dB), with no rescaling. If none matches, it falls back to the
    most-recent capture and rescales it (and logs that it did so).
  - **Scale most-recent to data gain** — always use the most-recent capture and
    rescale it by `10^((data_gain − field_gain) / 20)`, exactly like
    `lucid_viewer.py`.

Gain is assumed **constant across all frames** of the acquisition.

After you confirm, the chosen dark/white references are averaged, shown in their
own windows for inspection, and the data opens as a lazily-corrected 32-bit float
stack:

```
dark + white -> (data - dark) / (white - dark)     (x-ray transmission)
white only   -> data / white
dark only    -> data - dark
```

Pixels where `white - dark < 1` are set to 0 (same guard as `lucid_viewer.py`).
The math is a verified port: it reproduces `lucid_viewer.py`'s float transmission
bit-for-bit, including gain scaling and the edge-case guards.

The plain **`Lucid_Raw_Reader`** applies no correction — it just shows raw counts.

## Install

You need **Fiji** (recommended) or ImageJ 1.x. Fiji bundles a Java compiler, so
no separate JDK is required. Install **both** `Lucid_Raw_Reader.java` and
`Lucid_Raw_Corrector.java` the same way (each becomes its own Plugins menu item).

### Option A — Compile and Run (simplest, persistent)

1. Copy **`Lucid_Raw_Reader.java`** into your `Fiji.app/plugins/` folder (a
   subfolder like `plugins/Lucid/` is fine).
2. In Fiji: **Plugins ▸ Compile and Run…** and select that `.java` file. It
   compiles to a `.class` beside it and runs once.
3. **Restart Fiji** (or **Help ▸ Refresh Menus**). The command now appears at
   **Plugins ▸ Lucid Raw Reader** (a plugin class with an underscore in its name
   is auto-added to the Plugins menu).

### Option B — Script Editor (quickest one-off)

1. **File ▸ New ▸ Script…**, set **Language ▸ Java**.
2. Open `Lucid_Raw_Reader.java` in the editor and click **Run**. It compiles and
   runs immediately (no install). Good for trying it out.

### Option C — Build a .jar (for distribution)

From a shell with a JDK, using Fiji's bundled ImageJ jar on the classpath:

```bash
# adjust the ij path to the one in your Fiji.app/jars/
javac -cp "Fiji.app/jars/ij-1.54*.jar" Lucid_Raw_Reader.java
jar cf Lucid_Raw_Reader.jar Lucid_Raw_Reader*.class plugins.config
```

Drop `Lucid_Raw_Reader.jar` into `Fiji.app/plugins/` and restart. The included
`plugins.config` registers the menu entry **Plugins ▸ Lucid ▸ Open Raw (Lucid
ATX245)…**.

## Use

**Plugins ▸ Lucid Raw Reader** (or the menu entry above) → pick a `.raw` file.
The stack opens; scroll with the slider, and each slice's title shows its ns
timestamp. Standard ImageJ tools (B&C, measure, profiles, Image ▸ Stacks) all
work on the virtual stack.

## File layout it expects

```
my_capture/
  capture_Mono12p.raw          <- the data
  capture_Mono12p.raw.json     <- or capture_Mono12p.json, or metadata.json
  frame_data.csv               <- optional; per-frame timestamps/gain/exposure
```

Sidecar keys used: `width`, `height`, `pixel_format` (legacy `pixelformat` /
`PixelFormat` also accepted). Extra keys are ignored.

### Timestamps

`frame_data.csv`'s timestamp column is auto-detected: a header containing one of
`timestamp/time/ts/ticks/device/system` is preferred, and a column whose name
contains `ns` is read as nanoseconds; `_us`/`_ms`/`_s` suffixes are scaled.

Times are displayed in **milliseconds relative to the event trigger**. The
trigger is read from the `line_status_all` column, which holds a binary string
(e.g. `00000101`). The plugin watches **bit 2 (the 3rd bit from the right)**: the
first frame where that bit is high is taken as **t = 0 ms**, so frames before it
are negative and frames after it are positive. If no `line_status_all` column or
trigger is present, times are shown relative to the first frame instead.

Example `frame_data.csv`:

```
frame_index,timestamp_ns,gain_db,exposure_us,line_status_all
0,3337917961128,0.0000,101.88,00000001
1,3337927961128,0.0000,101.88,00000001
2,3337937961128,0.0000,101.88,00000101   <- trigger -> t = 0 ms here
3,3337947961128,0.0000,101.88,00000101
```

## Notes & limits

- 16-bit output: values are the raw sensor counts (0–1023 for 10-bit, 0–4095 for
  12-bit, etc.). Use **Image ▸ Adjust ▸ Brightness/Contrast** to set levels.
- The plugin keeps one file handle open for the stack's lifetime; closing the
  window releases it.
- Packed formats require the frame geometry to yield a whole number of bytes
  (e.g. `Mono12p`/`Mono10Packed` need an even `width×height`; `Mono10p` needs it
  divisible by 4). A clear error is shown otherwise.
- Want drag-and-drop / `File ▸ Open` to handle `.raw` automatically? That needs
  ImageJ's `HandleExtraFileTypes` hook — ask and I can add a variant that
  registers there.
```
