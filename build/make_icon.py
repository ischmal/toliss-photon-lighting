"""Build the Windows executable icon for photon-installer from src/icons/*.png.

    python build/make_icon.py            # -> src/native/res/photon.ico
                                         #    src/native/ui/assets/icon.png

Two outputs, one generator, because they are the same artwork wearing two hats:

  res/photon.ico          compiled into the .exe by res/photon.rc. This is what
                          Explorer, the taskbar and Alt-Tab draw.
  ui/assets/icon.png      the Slint `Window.icon`, which is what a Linux window
                          manager draws. Windows does not need it (winit sets no
                          window icon, so the shell falls back to the resource
                          above) and macOS ignores it outright -- an unbundled
                          executable has no icon there at all, only an .app does.

⚠ THE SOURCE IS src/icons/, AND THIS SCRIPT IS NAME-AGNOSTIC. It reads every PNG
in that folder and keys them by the dimensions in their IHDR, so re-exporting the
set under different filenames changes nothing here. What it does need is square
images -- an icon directory entry has one byte for each side.

⚠ RE-RUN IT AFTER EDITING THE PNGs AND COMMIT THE RESULT. `photon.ico` is a
generated file that lives in `src/` rather than `dist/`, deliberately: CMake must
be able to build the installer on a runner with no Python, so the .ico is a build
INPUT there. `tests/test_icon.py` is what stops the two drifting -- it re-derives
the entries in memory and fails if the committed .ico no longer matches the PNGs.

Stdlib only (this repo has no Pillow and no pytest), which means a PNG decoder
lives here beside make_overlays.py's encoder. Only what the icon set actually
needs is supported: 8-bit non-interlaced RGBA/RGB/gray, with or without a palette.
"""
import pathlib
import struct
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "icons"
ICO_OUT = ROOT / "src" / "native" / "res" / "photon.ico"
PNG_OUT = ROOT / "src" / "native" / "ui" / "assets" / "icon.png"

# ⚠ THE ARTWORK SIZE THE SLINT WINDOW ICON IS TAKEN AT. 256 is what every window
# manager scales down from without help; going smaller shows on a HiDPI titlebar.
WINDOW_ICON = 256

# The sizes to emit even when nobody authored them: 16/32/48 are the documented
# minimum and 20/24/40/64/96 are their 125%-200% multiples. A missing size is not
# a failure -- the shell scales the nearest one -- it just looks softer than an
# authored one, and these cost a few KB each.
#
# ⚠ THIS IS A FLOOR, NOT THE LIST. The list is this set UNION whatever is in
# src/icons/, because a fixed list SILENTLY DISCARDS ARTWORK: an export at 60 and
# 72 (Windows' large-icon sizes at 125% and 150%) landed in the folder, matched no
# entry here, and never reached the .ico -- while the script's whole premise is
# that an authored size beats a resampled one. Nothing warned, because a dropped
# size looks exactly like a size the shell had to scale.
BASE_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)

# ⚠ THE CEILING IS A FORMAT LIMIT, NOT A CHOICE. An icon directory entry stores
# each side in ONE byte, where 0 means 256 -- so an authored 512 cannot be carried
# and must be DROPPED here rather than written out as a second entry claiming to
# be 256. It still earns its place in src/icons/ as the resampling source.
ICO_MAX = 256

# ⚠ SMALL ENTRIES ARE BMP, LARGE ONES ARE PNG. A PNG-compressed entry needs Vista
# or newer to decode; that is every machine this ships to, but the sizes below are
# also the ones every legacy shell path reads, and an uncompressed 48x48 costs
# 9 KB. Above it the trade flips hard -- 256x256 as BMP is 262 KB of the binary
# against 19 KB as PNG.
BMP_MAX = 48


# --- PNG ---------------------------------------------------------------------

def read_png(path):
    """Decode a PNG to (width, height, RGBA bytes). 8-bit, non-interlaced."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path.name}: not a PNG")

    idat = bytearray()
    palette = None
    trns = None
    pos = 8
    while pos < len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            w, h, depth, color, comp, filt, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8:
                raise ValueError(f"{path.name}: {depth}-bit PNG, need 8")
            if interlace:
                raise ValueError(f"{path.name}: interlaced PNGs are not supported")
            if comp or filt:
                raise ValueError(f"{path.name}: unsupported compression/filter method")
        elif tag == b"PLTE":
            palette = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color)
    if channels is None:
        raise ValueError(f"{path.name}: unsupported color type {color}")

    raw = zlib.decompress(bytes(idat))
    rgba = _unfilter(raw, w, h, channels)
    if color == 6:
        return w, h, rgba
    return w, h, _to_rgba(rgba, color, channels, palette, trns)


def _unfilter(raw, w, h, channels):
    """Undo the per-scanline PNG filters, returning packed samples."""
    stride = w * channels
    out = bytearray(stride * h)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if ftype == 1:      # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ftype == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:    # Average
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:    # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif ftype != 0:
            raise ValueError(f"unknown PNG filter {ftype} on row {y}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def _to_rgba(samples, color, channels, palette, trns):
    """Expand a non-RGBA sample buffer to RGBA."""
    n = len(samples) // channels
    out = bytearray(n * 4)
    for i in range(n):
        s = i * channels
        d = i * 4
        if color == 0:          # gray
            g = samples[s]
            out[d:d + 4] = bytes((g, g, g, 255))
        elif color == 2:        # RGB
            out[d:d + 3] = samples[s:s + 3]
            out[d + 3] = 255
        elif color == 3:        # palette
            idx = samples[s]
            out[d:d + 3] = palette[idx * 3:idx * 3 + 3]
            out[d + 3] = trns[idx] if trns and idx < len(trns) else 255
        elif color == 4:        # gray + alpha
            g = samples[s]
            out[d:d + 4] = bytes((g, g, g, samples[s + 1]))
    return out


def encode_png(w, h, rgba):
    """Minimal RGBA8 non-interlaced PNG (the twin of make_overlays.write_png)."""
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


# --- resampling --------------------------------------------------------------

def _spans(src_n, dst_n):
    """For each destination index, the source cells it covers and by how much."""
    out = []
    for i in range(dst_n):
        lo = i * src_n / dst_n
        hi = (i + 1) * src_n / dst_n
        cells = []
        j = int(lo)
        while j < hi and j < src_n:
            weight = min(hi, j + 1) - max(lo, j)
            if weight > 0:
                cells.append((j, weight))
            j += 1
        out.append(cells)
    return out


def resample(rgba, sw, sh, size):
    """Box-filter down to size x size.

    ⚠ AVERAGED IN PREMULTIPLIED ALPHA. Averaging straight RGB pulls the color of
    fully transparent texels into the edge -- on artwork drawn over transparent
    black that is a dark halo around every shape, and it is exactly the sizes
    users see smallest that show it worst.
    """
    if size > sw or size > sh:
        raise ValueError(f"cannot upscale {sw}x{sh} to {size}x{size}")
    cols = _spans(sw, size)
    rows = _spans(sh, size)
    out = bytearray(size * size * 4)
    for dy, ycells in enumerate(rows):
        for dx, xcells in enumerate(cols):
            r = g = b = a = total = 0.0
            for sy, wy in ycells:
                base = sy * sw * 4
                for sx, wx in xcells:
                    w = wx * wy
                    s = base + sx * 4
                    sa = rgba[s + 3] * w
                    r += rgba[s] * sa
                    g += rgba[s + 1] * sa
                    b += rgba[s + 2] * sa
                    a += sa
                    total += w
            d = (dy * size + dx) * 4
            if a > 0:
                out[d] = min(255, int(r / a + 0.5))
                out[d + 1] = min(255, int(g / a + 0.5))
                out[d + 2] = min(255, int(b / a + 0.5))
                out[d + 3] = min(255, int(a / total + 0.5))
    return out


# --- ICO ---------------------------------------------------------------------

def bmp_entry(size, rgba):
    """A 32-bpp BITMAPINFOHEADER icon image: BGRA bottom-up, then the AND mask."""
    header = struct.pack("<IiiHHIIiiII",
                         40,            # biSize
                         size,          # biWidth
                         size * 2,      # ⚠ biHeight IS DOUBLED -- it counts the
                                        # AND mask rows too, even though a 32-bpp
                                        # entry carries its alpha in the pixels.
                                        # Halve it and the shell reads the mask as
                                        # image data and draws garbage.
                         1, 32, 0, 0, 0, 0, 0, 0)

    pixels = bytearray()
    for y in range(size - 1, -1, -1):   # bottom-up
        row = y * size * 4
        for x in range(size):
            s = row + x * 4
            pixels += bytes((rgba[s + 2], rgba[s + 1], rgba[s], rgba[s + 3]))

    # The 1-bpp AND mask: set where the pixel is fully transparent. Modern paths
    # ignore it in favor of the alpha channel; the legacy ones do not, and a
    # zero-filled mask makes those draw the transparent border as opaque black.
    mask_stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        row = bytearray(mask_stride)
        for x in range(size):
            if rgba[(y * size + x) * 4 + 3] == 0:
                row[x >> 3] |= 0x80 >> (x & 7)
        mask += row

    return header + bytes(pixels) + bytes(mask)


def build_ico(images):
    """Pack {size: payload bytes} into an .ico, smallest entry first."""
    sizes = sorted(images)
    out = struct.pack("<HHH", 0, 1, len(sizes))          # reserved, type=icon, count
    offset = len(out) + 16 * len(sizes)
    directory = b""
    body = b""
    for size in sizes:
        payload = images[size]
        directory += struct.pack("<BBBBHHII",
                                 # ⚠ 0 MEANS 256. The field is one byte, which is
                                 # also why 512 cannot be in an .ico at all.
                                 0 if size >= 256 else size,
                                 0 if size >= 256 else size,
                                 0,     # bColorCount: 0 for anything truecolor
                                 0,     # bReserved
                                 1,     # wPlanes
                                 32,    # wBitCount
                                 len(payload), offset)
        body += payload
        offset += len(payload)
    return out + directory + body


# --- driver ------------------------------------------------------------------

def load_sources():
    """{side length: (path, rgba)} for every square PNG in src/icons/."""
    found = {}
    for path in sorted(SRC.glob("*.png")):
        w, h, rgba = read_png(path)
        if w != h:
            raise SystemExit(f"{path.name} is {w}x{h}; icon sources must be square")
        if w in found:
            raise SystemExit(f"{path.name} and {found[w][0].name} are both {w}x{w}")
        found[w] = (path, rgba)
    if not found:
        raise SystemExit(f"no PNGs in {SRC}")
    return found


def icon_sizes(sources):
    """Every size the .ico will carry: the floor, plus everything authored.

    ⚠ AUTHORED SIZES ARE NEVER DROPPED (except above `ICO_MAX`, which the format
    cannot express). Re-exporting the set at a size this script has never heard of
    is the normal way to work, and it must not need an edit here to take effect.
    """
    return tuple(sorted({s for s in (*BASE_SIZES, *sources) if s <= ICO_MAX}))


def render(sources):
    """{size: payload} for the .ico, taking authored sizes over resampled ones."""
    largest = max(sources)
    images = {}
    for size in icon_sizes(sources):
        if size in sources:
            path, rgba = sources[size]
            # An authored size embeds VERBATIM when it is a PNG entry -- nothing
            # here can beat the file the artist exported, and re-encoding it would
            # make this script's output depend on the local zlib.
            images[size] = (path.read_bytes() if size > BMP_MAX
                            else bmp_entry(size, rgba))
            continue
        rgba = resample(sources[largest][1], largest, largest, size)
        # ⚠ RESAMPLED FROM THE LARGEST SOURCE, never from the next size up. A box
        # filter at 21:1 beats one at 1.33:1, which lands every output pixel on a
        # ragged fraction of four inputs and reads as blur.
        images[size] = (bmp_entry(size, rgba) if size <= BMP_MAX
                        else encode_png(size, size, rgba))
    return images


def main():
    sources = load_sources()
    print(f"{SRC.relative_to(ROOT)}: {', '.join(f'{s}px' for s in sorted(sources))}")

    images = render(sources)
    ICO_OUT.parent.mkdir(parents=True, exist_ok=True)
    ICO_OUT.write_bytes(build_ico(images))
    sizes = icon_sizes(sources)
    authored = [s for s in sizes if s in sources]
    derived = [s for s in sizes if s not in sources]
    print(f"{ICO_OUT.relative_to(ROOT)}: {len(images)} entries, "
          f"{ICO_OUT.stat().st_size / 1024:.1f} KB "
          f"(authored {authored}, resampled {derived})")
    # An authored size the format cannot carry is worth SAYING, not swallowing:
    # it is the one case where a file in src/icons/ is deliberately ignored.
    if too_big := sorted(s for s in sources if s > ICO_MAX):
        print(f"  note: {', '.join(f'{s}px' for s in too_big)} not in the .ico "
              f"(an entry maxes out at {ICO_MAX}px) - used as a resampling source")

    if WINDOW_ICON in sources:
        PNG_OUT.write_bytes(sources[WINDOW_ICON][0].read_bytes())
    else:
        largest = max(sources)
        PNG_OUT.write_bytes(encode_png(
            WINDOW_ICON, WINDOW_ICON,
            resample(sources[largest][1], largest, largest, WINDOW_ICON)))
    print(f"{PNG_OUT.relative_to(ROOT)}: {WINDOW_ICON}px window icon")
    return 0


if __name__ == "__main__":
    sys.exit(main())
