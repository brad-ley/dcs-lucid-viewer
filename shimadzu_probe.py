"""
Quick probe script — dumps the 14-byte headers of key sections in a
Shimadzu HPV-X .dat file so we can figure out where the section data
length and image dimensions are encoded.

Usage:
    python shimadzu_probe.py <file.dat>
"""
import sys, struct

TAG_REC_SPEED  = b'\x30\x30\x07\x30'
TAG_EXPOSURE   = b'\x30\x30\x08\x30'
TAG_TIMESTAMP  = b'\x40\x40\x0e\x40'
TAG_IMAGE_DATA = b'\xa0\xa0\x01\xa0'

TAGS = {
    'RecordingSpeed': TAG_REC_SPEED,
    'ExposureTime':   TAG_EXPOSURE,
    'Timestamp':      TAG_TIMESTAMP,
    'ImageData':      TAG_IMAGE_DATA,
}

path = sys.argv[1] if len(sys.argv) > 1 else None
if not path:
    print("Usage: shimadzu_probe.py <file.dat>")
    sys.exit(1)

with open(path, 'rb') as f:
    raw = f.read()

file_size = len(raw)
print(f"\nFile size: {file_size:,} bytes  ({file_size/1e6:.2f} MB)\n")

for name, tag in TAGS.items():
    off = raw.find(tag)
    if off < 0:
        print(f"{name}: TAG NOT FOUND")
        continue

    header = raw[off:off+14]
    hex_str = ' '.join(f'{b:02x}' for b in header)
    print(f"{name} @ byte {off:,}")
    print(f"  Raw 14-byte header: {hex_str}")

    for start in range(4, 11, 2):
        val_u32 = struct.unpack_from('<I', header, start)[0] if start+4 <= 14 else None
        val_u16 = struct.unpack_from('<H', header, start)[0] if start+2 <= 14 else None
        print(f"    bytes[{start}:{start+4}] as uint32-LE = {val_u32:,}  |  bytes[{start}:{start+2}] as uint16-LE = {val_u16}")
    print()

# ── Image data section analysis ───────────────────────────────────────────────
off = raw.find(TAG_IMAGE_DATA)
if off >= 0:
    data_start = off + 14
    bytes_remaining = file_size - data_start

    print(f"ImageData tag @ byte {off:,}")
    print(f"  Payload starts @ byte {data_start:,}")
    print(f"  Bytes from payload to EOF: {bytes_remaining:,}\n")

    # For candidate frame counts, compute implied pixel dimensions
    print("  Frame-count analysis (assuming 2 bytes/pixel, uint16):")
    print(f"  {'Frames':>8}  {'Bytes/frame':>12}  {'Pixels/frame':>13}  {'Possible W x H':>20}  {'Clean?':>6}")
    print(f"  {'-'*8}  {'-'*12}  {'-'*13}  {'-'*20}  {'-'*6}")
    for n in [128, 192, 256, 384, 512]:
        if bytes_remaining % (n * 2) == 0:
            pix = bytes_remaining // (n * 2)
            # find the cleanest factoring close to 4:2.5 (400x250) aspect ratio
            best = None
            for w in range(1, int(pix**0.5) + 1):
                if pix % w == 0:
                    h = pix // w
                    if best is None or abs(w/h - 400/250) < abs(best[0]/best[1] - 400/250):
                        best = (w, h)
            dim = f"{best[0]} x {best[1]}" if best else "—"
            print(f"  {n:>8}  {bytes_remaining//n:>12,}  {pix:>13,}  {dim:>20}  {'yes':>6}")
        else:
            rem = bytes_remaining % (n * 2)
            print(f"  {n:>8}  {'':>12}  {'':>13}  {'not divisible':>20}  rem={rem}")

    print()

    # Also check against the section-header candidate lengths
    header = raw[off:off+14]
    print("  Section-length candidates from header bytes:")
    for start in (4, 6, 8):
        if start + 4 <= 14:
            cand = struct.unpack_from('<I', header, start)[0]
            if 0 < cand < bytes_remaining:
                print(f"    bytes[{start}:{start+4}] = {cand:,}  (<-- plausible, used by viewer)")
                for n in [128, 192, 256, 384, 512]:
                    if cand % (n * 2) == 0:
                        pix = cand // (n * 2)
                        print(f"      {n} frames -> {pix:,} px/frame", end='')
                        # Factor search
                        factors = []
                        for w in range(100, min(2000, pix)):
                            if pix % w == 0:
                                h = pix // w
                                if 50 <= h <= 2000:
                                    factors.append((w, h))
                        if factors:
                            print(f"  e.g. {factors[0][0]}x{factors[0][1]}", end='')
                        print()
    print()

    # Print first 32 raw uint16 values (unsigned, no shift)
    sample = raw[data_start:data_start+64]
    vals_u16 = struct.unpack_from(f'<{len(sample)//2}H', sample)
    vals_i16 = struct.unpack_from(f'<{len(sample)//2}h', sample)
    print(f"First 32 pixels as uint16 (no shift): {list(vals_u16[:16])}")
    print(f"                                       {list(vals_u16[16:32])}")
    print(f"First 32 pixels as int16  (no shift): {list(vals_i16[:16])}")
    print(f"After >>6:                             {[v>>6 for v in vals_i16[:16]]}")

    # Print 32 pixels from middle of expected first frame (if 400x250)
    mid = data_start + (400 * 125 + 200) * 2
    if mid + 32 < file_size:
        mv = struct.unpack_from('<16H', raw, mid)
        print(f"\nMid-frame (400x250 layout, row 125 col 200) uint16: {list(mv)}")
