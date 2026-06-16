import ij.IJ;
import ij.ImagePlus;
import ij.VirtualStack;
import ij.gui.GenericDialog;
import ij.io.OpenDialog;
import ij.measure.Calibration;
import ij.plugin.PlugIn;
import ij.process.FloatProcessor;
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
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Lucid_Raw_Corrector — open Lucid ATX245 ".raw" files directly in ImageJ/Fiji
 * with optional dark-field / white-field correction, displayed as float
 * x-ray transmission, with lazy (virtual-stack) loading.
 *
 * On load the plugin:
 *   1. Reads geometry/format from the JSON sidecar (like Lucid_Raw_Reader).
 *   2. Finds dark_field_* and white_field_* capture folders the same way
 *      lucid_viewer.py does (siblings of the data file, then of the data
 *      folder), and reads each capture's average gain from its frame_data.csv.
 *   3. Shows one dialog that presents the found captures (with gains), lets you
 *      tick dark and/or white correction, and choose how gain is handled:
 *        - "Match gain"  : use the most-recent capture whose gain equals the
 *                          data gain (within 0.1 dB); no rescaling.
 *        - "Scale ..."   : use the most-recent capture and rescale it by
 *                          10^((data_gain - field_gain)/20), like lucid_viewer.py.
 *   4. Loads + averages the chosen references, shows them in their own windows,
 *      and opens the data as a lazily-corrected 32-bit float stack:
 *        dark+white -> (data - dark) / (white - dark)        (transmission)
 *        white only -> data / white
 *        dark only  -> data - dark
 *
 * Gain is assumed constant across all frames of the acquisition.
 *
 * Bit-unpacking mirrors lucid_viewer.py exactly. See README_Lucid_Raw_Reader.md.
 */
public class Lucid_Raw_Corrector implements PlugIn {

    static final String[] FORMATS = {
        "Mono8", "Mono10", "Mono10p", "Mono10Packed",
        "Mono12", "Mono12p", "Mono12Packed", "Mono16"
    };
    static final int    ATX245_W = 5328;
    static final int    ATX245_H = 4608;
    static final double GAIN_TOL = 0.1;          // dB tolerance for "matching" gain

    @Override
    public void run(String arg) {
        String path = arg;
        if (path == null || path.trim().isEmpty() || !new File(path).isFile()) {
            OpenDialog od = new OpenDialog("Select Lucid .raw file", null);
            if (od.getFileName() == null) return;
            path = od.getDirectory() + od.getFileName();
        }
        try {
            open(path);
        } catch (Exception e) {
            IJ.error("Lucid Raw Corrector", e.getMessage() == null ? e.toString() : e.getMessage());
        }
    }

    // ── Open ──────────────────────────────────────────────────────────────────
    void open(String path) throws IOException {
        File f = new File(path);

        Sidecar sc = loadSidecar(path);
        int    w   = sc.width;
        int    h   = sc.height;
        String fmt = sc.fmt != null ? sc.fmt : guessFromName(f.getName());
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
            w = (int) gd.getNextNumber();
            h = (int) gd.getNextNumber();
            fmt = gd.getNextChoice();
        }
        fmt = canonicalFormat(fmt);
        if (fmt == null) throw new IOException("Unknown pixel format.");
        if (w <= 0 || h <= 0) throw new IOException("Invalid frame dimensions: " + w + "x" + h);

        long bpf = bytesPerFrame(fmt, w, h);
        long len = f.length();
        long nL  = len / bpf;
        if (nL <= 0)
            throw new IOException("No complete frames fit in " + len + " bytes with "
                                  + w + "x" + h + " " + fmt + " (" + bpf + " B/frame).");
        int n = (int) Math.min(nL, Integer.MAX_VALUE);

        File dataDir = f.getParentFile();
        double dataGain = readAverageGain(dataDir);          // NaN if unknown
        Timing tm   = loadTiming(dataDir, n);
        long[] tsNs = tm != null ? tm.tsNs : null;
        long   t0Ns = (tm != null && tm.triggerIdx >= 0) ? tm.tsNs[tm.triggerIdx]
                      : (tsNs != null && tsNs.length > 0 ? tsNs[0] : 0L);

        List<Field> darks  = findCandidates(dataDir, "dark_field");
        List<Field> whites = findCandidates(dataDir, "white_field");

        boolean useDark = false, useWhite = false;
        boolean matchMode = true;
        if (!darks.isEmpty() || !whites.isEmpty()) {
            GenericDialog gd = new GenericDialog("Lucid field correction");
            gd.addMessage("Data: " + n + " frames, " + w + "x" + h + " " + fmt
                          + "\nData gain: " + gainStr(dataGain));
            gd.addMessage("Dark field:\n" + summarize(darks, dataGain));
            boolean hasDark = !darks.isEmpty();
            if (hasDark) gd.addCheckbox("Apply dark-field correction", true);
            gd.addMessage("White field:\n" + summarize(whites, dataGain));
            boolean hasWhite = !whites.isEmpty();
            if (hasWhite) gd.addCheckbox("Apply white-field correction", true);
            gd.addChoice("Gain handling:",
                new String[]{"Match gain (closest capture)", "Scale most-recent to data gain"},
                "Match gain (closest capture)");
            gd.showDialog();
            if (gd.wasCanceled()) return;
            if (hasDark)  useDark  = gd.getNextBoolean();
            if (hasWhite) useWhite = gd.getNextBoolean();
            matchMode = gd.getNextChoiceIndex() == 0;
        }

        float[] dark = null, white = null;
        if (useDark)  dark  = resolveField(darks,  dataGain, matchMode, w, h, fmt, "dark");
        if (useWhite) white = resolveField(whites, dataGain, matchMode, w, h, fmt, "white");
        // resolveField returns null (and warns) if it could not be loaded
        useDark  = dark  != null;
        useWhite = white != null;

        RandomAccessFile raf = new RandomAccessFile(f, "r");
        LucidStack stack = new LucidStack(w, h, fmt, bpf, n, raf, tsNs, t0Ns, dark, white);

        String title = f.getName()
                     + (useDark && useWhite ? "  [transmission]"
                        : useDark ? "  [dark-subtracted]"
                        : useWhite ? "  [white-divided]" : "");
        ImagePlus imp = new ImagePlus(title, stack);
        applyTimeCalibration(imp, tsNs, n);
        imp.setProperty("Info",
            "Lucid raw: " + w + "x" + h + " " + fmt + ", " + n + " frames\n"
            + "Correction: " + (useDark ? "dark " : "") + (useWhite ? "white " : "")
            + (!useDark && !useWhite ? "none" : "") + "\nSource: " + f.getAbsolutePath());
        imp.show();
        IJ.showStatus("Opened " + n + " frames (" + w + "x" + h + " " + fmt + ")");
    }

    // ── Field selection / loading ─────────────────────────────────────────────
    static class Field { File folder; double gain; Field(File f, double g){folder=f;gain=g;} }

    /** Folders containing keyword, most-recent first, taken from the first of
     *  [dataDir, dataDir/..] that has any match — mirrors lucid_viewer.py. */
    static List<Field> findCandidates(File dataDir, String keyword) {
        List<Field> out = new ArrayList<>();
        File[] searchDirs = { dataDir, dataDir != null ? dataDir.getParentFile() : null };
        for (File sd : searchDirs) {
            if (sd == null || !sd.isDirectory()) continue;
            File[] kids = sd.listFiles();
            if (kids == null) continue;
            List<File> matches = new ArrayList<>();
            for (File k : kids)
                if (k.isDirectory() && k.getName().contains(keyword)) matches.add(k);
            if (!matches.isEmpty()) {
                matches.sort(Comparator.comparing(File::getName).reversed());  // most-recent first
                for (File m : matches) out.add(new Field(m, readAverageGain(m)));
                break;     // stop at the first search dir that yielded matches
            }
        }
        return out;
    }

    static Field firstMatch(List<Field> cands, double dataGain) {
        if (Double.isNaN(dataGain)) return null;
        for (Field c : cands)
            if (!Double.isNaN(c.gain) && Math.abs(c.gain - dataGain) <= GAIN_TOL) return c;
        return null;
    }

    static String summarize(List<Field> cands, double dataGain) {
        if (cands.isEmpty()) return "  (none found)";
        StringBuilder sb = new StringBuilder();
        Field recent = cands.get(0);
        sb.append("  most-recent: ").append(recent.folder.getName())
          .append("  (").append(gainStr(recent.gain)).append(")");
        Field m = firstMatch(cands, dataGain);
        if (m != null)
            sb.append("\n  match-gain:  ").append(m.folder.getName())
              .append("  (").append(gainStr(m.gain)).append(")");
        else if (!Double.isNaN(dataGain))
            sb.append("\n  match-gain:  (no capture within ").append(GAIN_TOL).append(" dB)");
        return sb.toString();
    }

    /** Pick folder per mode, load+average, scale, show it, return float[w*h]. */
    float[] resolveField(List<Field> cands, double dataGain, boolean matchMode,
                         int w, int h, String dataFmt, String label) {
        if (cands.isEmpty()) return null;
        Field chosen;
        double scale;
        Field match = firstMatch(cands, dataGain);
        if (matchMode) {
            if (match != null) { chosen = match; scale = 1.0; }
            else {
                chosen = cands.get(0);   // fall back to most-recent + rescale
                scale  = gainScale(dataGain, chosen.gain);
                IJ.log("Lucid: no " + label + " capture within " + GAIN_TOL
                       + " dB of data gain " + gainStr(dataGain)
                       + "; using most-recent (" + gainStr(chosen.gain) + ") rescaled x"
                       + String.format("%.4f", scale) + ".");
            }
        } else {
            chosen = cands.get(0);
            scale  = gainScale(dataGain, chosen.gain);
        }

        float[] avg;
        try {
            avg = loadFieldAverage(chosen.folder, w, h, dataFmt);
        } catch (IOException e) {
            IJ.error("Lucid Raw Corrector", label + " field could not be loaded:\n" + e.getMessage());
            return null;
        }
        if (avg == null) {
            IJ.error("Lucid Raw Corrector", "No frames found in " + label
                     + " folder:\n" + chosen.folder.getName());
            return null;
        }
        if (scale != 1.0) for (int i = 0; i < avg.length; i++) avg[i] *= (float) scale;

        String gainTag = "gain " + gainStr(chosen.gain) + (scale != 1.0
                         ? String.format(", x%.4f", scale) : "");
        new ImagePlus(label + " field [" + chosen.folder.getName() + ", " + gainTag + "]",
                      new FloatProcessor(w, h, avg)).show();
        return avg;
    }

    static double gainScale(double dataGain, double fieldGain) {
        if (Double.isNaN(dataGain) || Double.isNaN(fieldGain)) return 1.0;
        return Math.pow(10.0, (dataGain - fieldGain) / 20.0);
    }

    /** Average all .raw (preferred) or .tif frames in a folder into float[w*h]. */
    float[] loadFieldAverage(File folder, int w, int h, String dataFmt) throws IOException {
        File[] all = folder.listFiles();
        if (all == null) return null;
        List<File> raws = new ArrayList<>(), tifs = new ArrayList<>();
        for (File c : all) {
            if (!c.isFile()) continue;
            String low = c.getName().toLowerCase();
            if (low.endsWith(".raw")) raws.add(c);
            else if (low.endsWith(".tif") || low.endsWith(".tiff")) tifs.add(c);
        }
        raws.sort(Comparator.comparing(File::getName));
        tifs.sort(Comparator.comparing(File::getName));
        boolean useRaw = !raws.isEmpty();
        List<File> files = useRaw ? raws : tifs;
        if (files.isEmpty()) return null;

        int n = w * h;
        double[] sum = new double[n];
        long count = 0;

        for (File file : files) {
            if (useRaw) {
                Sidecar fsc = loadSidecar(file.getAbsolutePath());
                String ffmt = fsc.fmt != null ? canonicalFormat(fsc.fmt) : canonicalFormat(dataFmt);
                int fw = fsc.width  > 0 ? fsc.width  : w;
                int fh = fsc.height > 0 ? fsc.height : h;
                if (ffmt == null) throw new IOException("unknown pixel format for " + file.getName());
                if (fw != w || fh != h)
                    throw new IOException(file.getName() + " is " + fw + "x" + fh
                                          + " but data is " + w + "x" + h);
                long fbpf = bytesPerFrame(ffmt, fw, fh);
                long frames = file.length() / fbpf;
                try (RandomAccessFile r = new RandomAccessFile(file, "r")) {
                    byte[] buf = new byte[(int) fbpf];
                    for (long k = 0; k < frames; k++) {
                        r.seek(k * fbpf);
                        r.readFully(buf);
                        short[] px = unpack(buf, ffmt, fw, fh);
                        for (int i = 0; i < n; i++) sum[i] += (px[i] & 0xFFFF);
                        count++;
                    }
                }
            } else {
                ImagePlus tip = IJ.openImage(file.getAbsolutePath());
                if (tip == null) continue;
                if (tip.getWidth() != w || tip.getHeight() != h)
                    throw new IOException(file.getName() + " is " + tip.getWidth() + "x"
                                          + tip.getHeight() + " but data is " + w + "x" + h);
                ij.ImageStack st = tip.getStack();
                for (int s = 1; s <= st.getSize(); s++) {
                    ImageProcessor ip = st.getProcessor(s).convertToFloat();
                    float[] px = (float[]) ip.getPixels();
                    for (int i = 0; i < n; i++) sum[i] += px[i];
                    count++;
                }
                tip.close();
            }
        }
        if (count == 0) return null;
        float[] avg = new float[n];
        for (int i = 0; i < n; i++) avg[i] = (float) (sum[i] / count);
        return avg;
    }

    /** Mean of the 'gain' column in folder/frame_data.csv, or NaN. */
    static double readAverageGain(File dir) {
        if (dir == null) return Double.NaN;
        File csv = new File(dir, "frame_data.csv");
        if (!csv.isFile()) return Double.NaN;
        List<String[]> rows = readCsv(csv);
        if (rows.size() < 2) return Double.NaN;
        String[] hd = rows.get(0);
        int gcol = -1;
        for (int i = 0; i < hd.length; i++)
            if (hd[i].trim().toLowerCase().contains("gain")) { gcol = i; break; }
        if (gcol < 0) return Double.NaN;
        double s = 0; int c = 0;
        for (int r = 1; r < rows.size(); r++) {
            String[] row = rows.get(r);
            if (gcol >= row.length) continue;
            try { s += Double.parseDouble(row[gcol].trim()); c++; } catch (NumberFormatException ignore) {}
        }
        return c > 0 ? s / c : Double.NaN;
    }

    static String gainStr(double g) { return Double.isNaN(g) ? "unknown" : String.format("%.3g dB", g); }

    // ── Stack (16-bit raw OR 32-bit corrected float) ──────────────────────────
    static class LucidStack extends VirtualStack {
        final int width, height, n;
        final String fmt;
        final long bpf;
        final RandomAccessFile raf;
        final long[] tsNs; final long t0;
        final float[] dark, white;          // null when not applied
        final boolean corrected;

        LucidStack(int w, int h, String fmt, long bpf, int n, RandomAccessFile raf,
                   long[] tsNs, long t0Ns, float[] dark, float[] white) {
            super(w, h, (ColorModel) null, "");
            this.width = w; this.height = h; this.fmt = fmt; this.bpf = bpf; this.n = n;
            this.raf = raf; this.tsNs = tsNs;
            this.t0 = t0Ns;
            this.dark = dark; this.white = white;
            this.corrected = dark != null || white != null;
        }

        @Override public int getSize()       { return n; }
        @Override public int getWidth()      { return width; }
        @Override public int getHeight()     { return height; }
        @Override public int getBitDepth()   { return corrected ? 32 : 16; }
        @Override public boolean isVirtual() { return true; }

        @Override public String getSliceLabel(int idx) {
            if (tsNs == null || idx < 1 || idx > tsNs.length) return null;
            double relMs = (tsNs[idx - 1] - t0) * 1e-6;     // ns -> ms, trigger-relative
            return "frame " + idx + "   t = " + String.format("%.4f", relMs) + " ms";
        }

        @Override public ImageProcessor getProcessor(int idx) {
            byte[] buf = new byte[(int) bpf];
            try {
                synchronized (raf) {
                    raf.seek((long) (idx - 1) * bpf);
                    raf.readFully(buf);
                }
            } catch (IOException e) {
                IJ.log("Lucid: read error on frame " + idx + ": " + e);
                return corrected ? new FloatProcessor(width, height)
                                 : new ShortProcessor(width, height);
            }
            short[] px = unpack(buf, fmt, width, height);
            if (!corrected) return new ShortProcessor(width, height, px, null);

            int nn = width * height;
            float[] out = new float[nn];
            if (dark != null && white != null) {
                for (int i = 0; i < nn; i++) {
                    float denom = white[i] - dark[i];
                    out[i] = denom >= 1f ? ((px[i] & 0xFFFF) - dark[i]) / denom : 0f;
                }
            } else if (dark != null) {
                for (int i = 0; i < nn; i++) out[i] = (px[i] & 0xFFFF) - dark[i];
            } else {
                for (int i = 0; i < nn; i++) {
                    float wv = white[i] >= 1f ? white[i] : 1f;
                    out[i] = (px[i] & 0xFFFF) / wv;
                }
            }
            return new FloatProcessor(width, height, out);
        }
    }

    static void applyTimeCalibration(ImagePlus imp, long[] tsNs, int n) {
        if (tsNs == null || tsNs.length <= 1) return;
        int m = tsNs.length;
        long[] d = new long[m - 1];
        for (int i = 1; i < m; i++) d[i - 1] = tsNs[i] - tsNs[i - 1];
        Arrays.sort(d);
        double medNs = d[d.length / 2];
        if (medNs > 0) {
            Calibration cal = imp.getCalibration();
            cal.frameInterval = medNs * 1e-9;
            cal.setTimeUnit("sec");
            imp.setCalibration(cal);
        }
    }

    // ── Pixel-format geometry + unpacking (mirrors lucid_viewer.py) ────────────
    static long bytesPerFrame(String fmt, int w, int h) throws IOException {
        long n = (long) w * h;
        switch (fmt) {
            case "Mono8":  return n;
            case "Mono10": case "Mono12": case "Mono16": return n * 2;
            case "Mono10p":
                if ((n * 10) % 8 != 0)
                    throw new IOException("Mono10p needs width*height divisible by 4 (got " + n + ").");
                return n * 10 / 8;
            case "Mono10Packed": case "Mono12p": case "Mono12Packed":
                if ((n * 3) % 2 != 0)
                    throw new IOException(fmt + " needs an even width*height (got " + n + ").");
                return n * 3 / 2;
            default:
                throw new IOException("Unknown pixel format: " + fmt);
        }
    }

    static short[] unpack(byte[] b, String fmt, int w, int h) {
        int n = w * h;
        short[] out = new short[n];
        switch (fmt) {
            case "Mono8":
                for (int i = 0; i < n; i++) out[i] = (short) (b[i] & 0xFF);
                return out;
            case "Mono10": case "Mono12": case "Mono16":
                for (int i = 0; i < n; i++) {
                    int lo = b[2 * i] & 0xFF, hi = b[2 * i + 1] & 0xFF;
                    out[i] = (short) ((hi << 8) | lo);
                }
                return out;
            case "Mono12Packed":
                for (int j = 0, p = 0; p + 1 < n; j += 3, p += 2) {
                    int b0 = b[j] & 0xFF, b1 = b[j + 1] & 0xFF, b2 = b[j + 2] & 0xFF;
                    out[p]     = (short) ((b0 << 4) | (b1 & 0x0F));
                    out[p + 1] = (short) ((b2 << 4) | ((b1 & 0xF0) >> 4));
                }
                return out;
            case "Mono12p":
                for (int j = 0, p = 0; p + 1 < n; j += 3, p += 2) {
                    int b0 = b[j] & 0xFF, b1 = b[j + 1] & 0xFF, b2 = b[j + 2] & 0xFF;
                    out[p]     = (short) (b0 | ((b1 & 0x0F) << 8));
                    out[p + 1] = (short) (((b1 & 0xF0) >> 4) | (b2 << 4));
                }
                return out;
            case "Mono10Packed":
                for (int j = 0, p = 0; p + 1 < n; j += 3, p += 2) {
                    int b0 = b[j] & 0xFF, b1 = b[j + 1] & 0xFF, b2 = b[j + 2] & 0xFF;
                    out[p]     = (short) ((b0 << 2) | (b1 & 0x03));
                    out[p + 1] = (short) ((b2 << 2) | (b1 >> 6));
                }
                return out;
            case "Mono10p":
                for (int j = 0, p = 0; p + 3 < n; j += 5, p += 4) {
                    int b0 = b[j] & 0xFF, b1 = b[j + 1] & 0xFF, b2 = b[j + 2] & 0xFF,
                        b3 = b[j + 3] & 0xFF, b4 = b[j + 4] & 0xFF;
                    out[p]     = (short) ( b0       | ((b1 & 0x03) << 8));
                    out[p + 1] = (short) ((b1 >> 2) | ((b2 & 0x0F) << 6));
                    out[p + 2] = (short) ((b2 >> 4) | ((b3 & 0x3F) << 4));
                    out[p + 3] = (short) ((b3 >> 6) |  (b4 << 2));
                }
                return out;
            default:
                return out;
        }
    }

    // ── Sidecar JSON + CSV helpers ────────────────────────────────────────────
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
            try { json = new String(Files.readAllBytes(c.toPath()), StandardCharsets.UTF_8); }
            catch (IOException e) { continue; }
            Integer wv = getInt(json, "width");
            Integer hv = getInt(json, "height");
            String  fv = getStr(json, "pixel_format", "pixelformat", "PixelFormat", "Pixel Format");
            if (wv != null) sc.width  = wv;
            if (hv != null) sc.height = hv;
            if (fv != null) sc.fmt    = fv;
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

    static List<String[]> readCsv(File csv) {
        List<String[]> rows = new ArrayList<>();
        try (BufferedReader br = new BufferedReader(new FileReader(csv))) {
            String line;
            while ((line = br.readLine()) != null) {
                if (line.trim().isEmpty()) continue;
                rows.add(line.split(",", -1));
            }
        } catch (IOException e) { /* return what we have */ }
        return rows;
    }

    // ── frame_data.csv → per-frame timestamps (ns) + event-trigger frame ──────
    // The event trigger is bit 2 (3rd from the right) of the line_status_all
    // column; the first frame where it is high becomes t = 0.
    static final int TRIGGER_BIT = 2;

    static class Timing { long[] tsNs; int triggerIdx = -1; }

    static Timing loadTiming(File dir, int nframes) {
        if (dir == null) return null;
        File csv = new File(dir, "frame_data.csv");
        if (!csv.isFile()) csv = new File(dir, "timestamps.csv");
        if (!csv.isFile()) return null;
        List<String[]> rows = readCsv(csv);
        if (rows.isEmpty()) return null;

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
            for (int i = 0; i < hd.length; i++) {
                String name = hd[i].trim().toLowerCase();
                if (name.contains("ns") && containsAny(name, tokens)) { tsCol = i; scaleToNs = 1.0; break; }
            }
            if (tsCol < 0) for (int i = 0; i < hd.length; i++) {
                String name = hd[i].trim().toLowerCase();
                if (containsAny(name, tokens)) {
                    tsCol = i;
                    if      (name.contains("_ns") || name.endsWith("ns")) scaleToNs = 1.0;
                    else if (name.contains("_us") || name.endsWith("us")) scaleToNs = 1e3;
                    else if (name.contains("_ms") || name.endsWith("ms")) scaleToNs = 1e6;
                    else if (name.contains("_s")  || name.endsWith("_s")) scaleToNs = 1e9;
                    else scaleToNs = 1.0;
                    break;
                }
            }
        }
        if (tsCol < 0) tsCol = 0;
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
