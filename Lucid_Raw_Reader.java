import ij.IJ;
import ij.ImagePlus;
import ij.VirtualStack;
import ij.gui.GenericDialog;
import ij.io.OpenDialog;
import ij.measure.Calibration;
import ij.plugin.PlugIn;
import ij.process.ImageProcessor;
import ij.process.ShortProcessor;

import java.awt.image.ColorModel;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Lucid_Raw_Reader — open Lucid Vision Labs ATX245 ".raw" files directly in
 * ImageJ / Fiji as a lazily-loaded 16-bit virtual stack.
 *
 * A .raw file is just concatenated frames with no header.  The frame geometry
 * and pixel format are read from a JSON sidecar next to the file (the same
 * sidecars the lucid_viewer.py tool writes):
 *
 *     {"width": 5328, "height": 4608, "pixel_format": "Mono12p", ...}
 *
 * The sidecar is searched for as  <file>.json , then <stem>.json , then
 * metadata.json in the same folder.  If none is found (or it lacks the needed
 * keys) a dialog asks for the geometry / format.
 *
 * Per-frame timestamps are read (in nanoseconds) from frame_data.csv (or
 * timestamps.csv) in the same folder and shown as slice labels.
 *
 * Supported pixel formats:
 *   Mono8, Mono10, Mono10p, Mono10Packed, Mono12, Mono12p, Mono12Packed, Mono16
 *
 * Bit-unpacking mirrors lucid_viewer.py exactly.
 *
 * Build/Install: see README_Lucid_Raw_Reader.md.  Quickest path is Fiji's
 * Script Editor (File > New > Script..., language Java) -> open this file -> Run.
 */
public class Lucid_Raw_Reader implements PlugIn {

    static final String[] FORMATS = {
        "Mono8", "Mono10", "Mono10p", "Mono10Packed",
        "Mono12", "Mono12p", "Mono12Packed", "Mono16"
    };
    static final int ATX245_W = 5328;
    static final int ATX245_H = 4608;

    @Override
    public void run(String arg) {
        String path = arg;
        if (path == null || path.trim().isEmpty() || !new File(path).isFile()) {
            OpenDialog od = new OpenDialog("Select Lucid .raw file", null);
            if (od.getFileName() == null) return;          // cancelled
            path = od.getDirectory() + od.getFileName();
        }
        try {
            open(path);
        } catch (Exception e) {
            IJ.error("Lucid Raw Reader", e.getMessage() == null ? e.toString() : e.getMessage());
        }
    }

    // -- Open ------------------------------------------------------------------
    void open(String path) throws IOException {
        File f = new File(path);

        Sidecar sc = loadSidecar(path);
        int    w   = sc.width;
        int    h   = sc.height;
        String fmt = sc.fmt != null ? sc.fmt : guessFromName(f.getName());

        // Ask the user for anything the sidecar/filename didn't supply.
        if (w <= 0 || h <= 0 || fmt == null) {
            GenericDialog gd = new GenericDialog("Lucid raw - frame format");
            gd.addMessage("No (complete) sidecar found for:\n" + f.getName()
                          + "\nEnter the frame geometry and pixel format.");
            gd.addNumericField("Width (px):",  w > 0 ? w : ATX245_W, 0);
            gd.addNumericField("Height (px):", h > 0 ? h : ATX245_H, 0);
            int sel = 0;
            if (fmt != null) for (int i = 0; i < FORMATS.length; i++)
                if (FORMATS[i].equalsIgnoreCase(fmt)) { sel = i; break; }
            gd.addChoice("Pixel format:", FORMATS, FORMATS[sel]);
            gd.showDialog();
            if (gd.wasCanceled()) return;
            w   = (int) gd.getNextNumber();
            h   = (int) gd.getNextNumber();
            fmt = gd.getNextChoice();
        }

        fmt = canonicalFormat(fmt);
        if (fmt == null)
            throw new IOException("Unknown pixel format.");
        if (w <= 0 || h <= 0)
            throw new IOException("Invalid frame dimensions: " + w + "x" + h);

        long bpf = bytesPerFrame(fmt, w, h);
        long len = f.length();
        long nL  = len / bpf;
        if (nL <= 0)
            throw new IOException("No complete frames fit in " + len + " bytes with "
                                  + w + "x" + h + " " + fmt + " (" + bpf + " B/frame).");
        int n = (int) Math.min(nL, Integer.MAX_VALUE);

        Timing tm    = loadTiming(f.getParentFile(), n);
        long[] tsNs  = tm != null ? tm.tsNs : null;
        long   t0Ns  = (tm != null && tm.triggerIdx >= 0) ? tm.tsNs[tm.triggerIdx]
                       : (tsNs != null && tsNs.length > 0 ? tsNs[0] : 0L);

        RandomAccessFile raf = new RandomAccessFile(f, "r");
        LucidVirtualStack stack = new LucidVirtualStack(w, h, fmt, bpf, n, raf, tsNs, t0Ns);

        ImagePlus imp = new ImagePlus(f.getName(), stack);
        if (tsNs != null && tsNs.length > 1) {
            // Median inter-frame interval (seconds) as a convenience for the
            // stack's t-axis; labels still carry the exact trigger-relative time.
            int m = tsNs.length;
            long[] d = new long[m - 1];
            for (int i = 1; i < m; i++) d[i - 1] = tsNs[i] - tsNs[i - 1];
            java.util.Arrays.sort(d);
            double medNs = d[d.length / 2];
            if (medNs > 0) {
                Calibration cal = imp.getCalibration();
                cal.frameInterval = medNs * 1e-9;   // seconds
                cal.setTimeUnit("sec");
                imp.setCalibration(cal);
            }
        }
        imp.setProperty("Info",
            "Lucid raw: " + w + "x" + h + " " + fmt + ", " + n + " frames\n"
            + "Source: " + f.getAbsolutePath());
        imp.show();
        IJ.showStatus("Opened " + n + " frames (" + w + "x" + h + " " + fmt + ")");
    }

    // -- Virtual stack ---------------------------------------------------------
    static class LucidVirtualStack extends VirtualStack {
        final int    width, height, n;
        final String fmt;
        final long   bpf;
        final RandomAccessFile raf;
        final long[] tsNs;          // absolute ns, may be null
        final long   t0;

        LucidVirtualStack(int w, int h, String fmt, long bpf, int n,
                          RandomAccessFile raf, long[] tsNs, long t0Ns) {
            super(w, h, (ColorModel) null, "");
            this.width = w; this.height = h; this.fmt = fmt;
            this.bpf = bpf; this.n = n; this.raf = raf; this.tsNs = tsNs;
            this.t0 = t0Ns;
        }

        @Override public int getSize()      { return n; }
        @Override public int getWidth()     { return width; }
        @Override public int getHeight()    { return height; }
        @Override public int getBitDepth()  { return 16; }
        @Override public boolean isVirtual(){ return true; }

        @Override
        public String getSliceLabel(int idx) {
            if (tsNs == null || idx < 1 || idx > tsNs.length) return null;
            double relMs = (tsNs[idx - 1] - t0) * 1e-6;     // ns -> ms, trigger-relative
            return "frame " + idx + "   t = " + String.format("%.4f", relMs) + " ms";
        }

        @Override
        public ImageProcessor getProcessor(int idx) {
            byte[] buf = new byte[(int) bpf];
            try {
                synchronized (raf) {
                    raf.seek((long) (idx - 1) * bpf);
                    raf.readFully(buf);
                }
            } catch (IOException e) {
                IJ.log("Lucid: read error on frame " + idx + ": " + e);
                return new ShortProcessor(width, height);
            }
            short[] pix = unpack(buf, fmt, width, height);
            return new ShortProcessor(width, height, pix, null);
        }
    }

    // -- Pixel-format geometry -------------------------------------------------
    static long bytesPerFrame(String fmt, int w, int h) throws IOException {
        long n = (long) w * h;
        switch (fmt) {
            case "Mono8":  return n;
            case "Mono10": case "Mono12": case "Mono16": return n * 2;
            case "Mono10p":
                if ((n * 10) % 8 != 0)
                    throw new IOException("Mono10p needs width*height divisible by 4 (got " + n + ").");
                return n * 10 / 8;                  // 4 px / 5 bytes
            case "Mono10Packed":
            case "Mono12p":
            case "Mono12Packed":
                if ((n * 3) % 2 != 0)
                    throw new IOException(fmt + " needs an even width*height (got " + n + ").");
                return n * 3 / 2;                   // 2 px / 3 bytes
            default:
                throw new IOException("Unknown pixel format: " + fmt);
        }
    }

    // -- Bit-unpacking (mirrors lucid_viewer.py unpack_frame) ------------------
    static short[] unpack(byte[] b, String fmt, int w, int h) {
        int n = w * h;
        short[] out = new short[n];
        switch (fmt) {
            case "Mono8": {
                for (int i = 0; i < n; i++) out[i] = (short) (b[i] & 0xFF);
                return out;
            }
            case "Mono10": case "Mono12": case "Mono16": {
                // little-endian 16-bit word; value already right-aligned
                for (int i = 0; i < n; i++) {
                    int lo = b[2 * i]     & 0xFF;
                    int hi = b[2 * i + 1] & 0xFF;
                    out[i] = (short) ((hi << 8) | lo);
                }
                return out;
            }
            case "Mono12Packed": {
                // GigE Vision MSB-aligned: 2 px in 3 bytes
                for (int j = 0, p = 0; p + 1 < n; j += 3, p += 2) {
                    int b0 = b[j] & 0xFF, b1 = b[j + 1] & 0xFF, b2 = b[j + 2] & 0xFF;
                    out[p]     = (short) ((b0 << 4) | (b1 & 0x0F));
                    out[p + 1] = (short) ((b2 << 4) | ((b1 & 0xF0) >> 4));
                }
                return out;
            }
            case "Mono12p": {
                // USB3 Vision / PFNC LSB-aligned: 2 px in 3 bytes
                for (int j = 0, p = 0; p + 1 < n; j += 3, p += 2) {
                    int b0 = b[j] & 0xFF, b1 = b[j + 1] & 0xFF, b2 = b[j + 2] & 0xFF;
                    out[p]     = (short) (b0 | ((b1 & 0x0F) << 8));
                    out[p + 1] = (short) (((b1 & 0xF0) >> 4) | (b2 << 4));
                }
                return out;
            }
            case "Mono10Packed": {
                // Lucid Vision native: 2 px in 3 bytes
                for (int j = 0, p = 0; p + 1 < n; j += 3, p += 2) {
                    int b0 = b[j] & 0xFF, b1 = b[j + 1] & 0xFF, b2 = b[j + 2] & 0xFF;
                    out[p]     = (short) ((b0 << 2) | (b1 & 0x03));
                    out[p + 1] = (short) ((b2 << 2) | (b1 >> 6));
                }
                return out;
            }
            case "Mono10p": {
                // GenICam LSB-aligned: 4 px in 5 bytes
                for (int j = 0, p = 0; p + 3 < n; j += 5, p += 4) {
                    int b0 = b[j] & 0xFF, b1 = b[j + 1] & 0xFF, b2 = b[j + 2] & 0xFF,
                        b3 = b[j + 3] & 0xFF, b4 = b[j + 4] & 0xFF;
                    out[p]     = (short) ( b0        | ((b1 & 0x03) << 8));
                    out[p + 1] = (short) ((b1 >> 2)  | ((b2 & 0x0F) << 6));
                    out[p + 2] = (short) ((b2 >> 4)  | ((b3 & 0x3F) << 4));
                    out[p + 3] = (short) ((b3 >> 6)  |  (b4 << 2));
                }
                return out;
            }
            default:
                return out;     // unreachable; geometry check already rejected it
        }
    }

    // -- Sidecar JSON ----------------------------------------------------------
    static class Sidecar { int width = -1, height = -1; String fmt = null; }

    static Sidecar loadSidecar(String path) {
        Sidecar sc = new Sidecar();
        String stem = path;
        int dot = path.lastIndexOf('.');
        if (dot > path.lastIndexOf(File.separatorChar)) stem = path.substring(0, dot);
        File dir = new File(path).getParentFile();

        List<File> candidates = new ArrayList<>();
        candidates.add(new File(path + ".json"));
        candidates.add(new File(stem + ".json"));
        if (dir != null) candidates.add(new File(dir, "metadata.json"));

        for (File c : candidates) {
            if (!c.isFile()) continue;
            String json;
            try {
                json = new String(Files.readAllBytes(c.toPath()), StandardCharsets.UTF_8);
            } catch (IOException e) {
                continue;
            }
            Integer w = getInt(json, "width");
            Integer h = getInt(json, "height");
            String  f = getStr(json, "pixel_format", "pixelformat",
                                      "PixelFormat", "Pixel Format");
            if (w != null) sc.width  = w;
            if (h != null) sc.height = h;
            if (f != null) sc.fmt    = f;
            if (sc.width > 0 && sc.height > 0 && sc.fmt != null) break;
        }
        return sc;
    }

    static Integer getInt(String json, String key) {
        Matcher m = Pattern.compile("\"" + Pattern.quote(key) + "\"\\s*:\\s*(\\d+)",
                                    Pattern.CASE_INSENSITIVE).matcher(json);
        return m.find() ? Integer.valueOf(m.group(1)) : null;
    }

    static String getStr(String json, String... keys) {
        for (String key : keys) {
            Matcher m = Pattern.compile("\"" + Pattern.quote(key) + "\"\\s*:\\s*\"([^\"]+)\"",
                                        Pattern.CASE_INSENSITIVE).matcher(json);
            if (m.find()) return m.group(1);
        }
        return null;
    }

    static String guessFromName(String name) {
        String lower = name.toLowerCase();
        // longer names first so "Mono12p"/"Mono12Packed" win over "Mono12"
        String[] order = {"Mono12Packed", "Mono10Packed", "Mono12p", "Mono10p",
                          "Mono16", "Mono12", "Mono10", "Mono8"};
        for (String f : order) if (lower.contains(f.toLowerCase())) return f;
        return null;
    }

    static String canonicalFormat(String fmt) {
        if (fmt == null) return null;
        for (String f : FORMATS) if (f.equalsIgnoreCase(fmt.trim())) return f;
        return null;
    }

    // -- frame_data.csv -> per-frame timestamps (ns) + event-trigger frame -----
    // The event trigger is bit 2 (3rd from the right) of the line_status_all
    // column; the first frame where it is high becomes t = 0.
    static final int TRIGGER_BIT = 2;

    static class Timing { long[] tsNs; int triggerIdx = -1; }

    static Timing loadTiming(File dir, int nframes) {
        if (dir == null) return null;
        File csv = new File(dir, "frame_data.csv");
        if (!csv.isFile()) csv = new File(dir, "timestamps.csv");
        if (!csv.isFile()) return null;

        List<String[]> rows = new ArrayList<>();
        try (BufferedReader br = new BufferedReader(new FileReader(csv))) {
            String line;
            while ((line = br.readLine()) != null) {
                if (line.trim().isEmpty()) continue;
                rows.add(line.split(",", -1));
            }
        } catch (IOException e) {
            return null;
        }
        if (rows.isEmpty()) return null;

        // Header present if the first row has any non-numeric cell.
        boolean header = false;
        for (String cell : rows.get(0)) {
            String s = cell.trim();
            if (s.isEmpty()) continue;
            try { Double.parseDouble(s); } catch (NumberFormatException ex) { header = true; break; }
        }

        int tsCol = -1, lsCol = -1;
        double scaleToNs = 1.0;
        if (header) {
            String[] hd = rows.get(0);
            String[] tokens = {"timestamp", "time", "ts", "ticks", "device", "system"};
            for (int i = 0; i < hd.length; i++)
                if (hd[i].trim().toLowerCase().contains("line_status")) { lsCol = i; break; }
            // First pass: prefer a column that is explicitly in nanoseconds.
            for (int i = 0; i < hd.length; i++) {
                String name = hd[i].trim().toLowerCase();
                if (name.contains("ns") && containsAny(name, tokens)) { tsCol = i; scaleToNs = 1.0; break; }
            }
            // Second pass: any time-like column, scaled by its unit suffix.
            if (tsCol < 0) {
                for (int i = 0; i < hd.length; i++) {
                    String name = hd[i].trim().toLowerCase();
                    if (containsAny(name, tokens)) {
                        tsCol = i;
                        if      (name.contains("_ns") || name.endsWith("ns")) scaleToNs = 1.0;
                        else if (name.contains("_us") || name.endsWith("us")) scaleToNs = 1e3;
                        else if (name.contains("_ms") || name.endsWith("ms")) scaleToNs = 1e6;
                        else if (name.contains("_s")  || name.endsWith("_s")) scaleToNs = 1e9;
                        else scaleToNs = 1.0;       // assume already ns
                        break;
                    }
                }
            }
        }
        if (tsCol < 0) tsCol = 0;                    // no header / no match -> first column

        int start = header ? 1 : 0;
        int count = Math.min(rows.size() - start, nframes);
        Timing tm = new Timing();
        tm.tsNs = new long[count];
        int written = 0;
        for (int r = start; r < rows.size() && written < count; r++) {
            String[] row = rows.get(r);
            if (tsCol < row.length) {
                try { tm.tsNs[written] = Math.round(Double.parseDouble(row[tsCol].trim()) * scaleToNs); }
                catch (NumberFormatException ex) { tm.tsNs[written] = written > 0 ? tm.tsNs[written - 1] : 0L; }
            } else tm.tsNs[written] = written > 0 ? tm.tsNs[written - 1] : 0L;

            if (tm.triggerIdx < 0 && lsCol >= 0 && lsCol < row.length) {
                long ls = parseLineStatus(row[lsCol].trim());
                if (ls >= 0 && (ls & (1L << TRIGGER_BIT)) != 0) tm.triggerIdx = written;
            }
            written++;
        }
        return written > 0 ? tm : null;
    }

    /** line_status_all is a binary string (e.g. "00000101"); fall back to decimal. */
    static long parseLineStatus(String s) {
        if (s.isEmpty()) return -1;
        try { return Long.parseLong(s, 2); }
        catch (NumberFormatException e) {
            try { return Long.parseLong(s); } catch (NumberFormatException e2) { return -1; }
        }
    }

    static boolean containsAny(String s, String[] tokens) {
        for (String t : tokens) if (s.contains(t)) return true;
        return false;
    }
}
