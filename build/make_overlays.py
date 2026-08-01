"""Generate the starter overlay images for the Panel FX compositor.

The compositor lets a layer be an IMAGE rather than a flat colour. An empty drop
folder is a bad first experience, so four worked examples ship with the plugin --
generated rather than committed as binaries, because the interesting part is the
recipe and a 4-line change here beats re-exporting a PNG from a paint program.

    python build/make_overlays.py            # -> src/native/overlays/

⚠ ALPHA IS COVERAGE, and it is what these images are really made of. The plugin
premultiplies on upload and every blend mode treats a zero-alpha texel as "leave
this pixel alone", so an overlay's shape lives in its alpha channel and its RGB
is usually just white -- the LAYER COLOUR does the tinting. That is why a
darkening vignette is opaque at the EDGES: the layer supplies the dark colour and
Multiply, and the image only says where it lands.

Written with zlib + struct rather than Pillow: this repo's tooling is stdlib-only
(see the pytest note in CLAUDE.md) and an RGBA PNG writer is fifteen lines.
"""
import math
import pathlib
import struct
import zlib

OUT = pathlib.Path(__file__).resolve().parents[1] / "src" / "native" / "overlays"


def write_png(path, w, h, rgba):
    """Minimal RGBA8 non-interlaced PNG."""
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def build(w, h, alpha_fn):
    """White pixels whose ALPHA comes from alpha_fn(u, v), both in 0..1."""
    px = bytearray(w * h * 4)
    for y in range(h):
        v = (y + 0.5) / h
        for x in range(w):
            u = (x + 0.5) / w
            a = alpha_fn(u, v)
            i = (y * w + x) * 4
            px[i:i + 4] = bytes((255, 255, 255, max(0, min(255, int(a * 255 + 0.5)))))
    return px


def smoothstep(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


# v runs 0 at the TOP of the image. X-Plane's panel coordinates run the other
# way, and the quad is emitted with v0 at the bottom, so "top" here reaches the
# cockpit as the top of the target. Confirm with the ? identify button before
# trusting that on a target you have not looked at.
IMAGES = {
    # Bright at the top, gone by halfway. Add or Screen, warm colour: the soft
    # wash a display's own bezel throws onto the glass.
    "gradient_top.png":  (256, 256, lambda u, v: 1.0 - smoothstep(v * 2.0)),

    # Opaque at the EDGES, clear in the middle -- the shape of what a Multiply
    # with a dark colour should darken. The reverse (opaque centre) would tint
    # exactly the part of a readout you want left alone.
    "vignette_edges.png": (256, 256, lambda u, v: smoothstep(
        (math.hypot(u - 0.5, v - 0.5) / 0.7071 - 0.55) / 0.45)),

    # One dark line per two rows. Tiny on purpose: tile it up in the editor
    # rather than storing a big image, so the line pitch stays adjustable.
    "scanlines.png":     (4, 4, lambda u, v: 1.0 if v < 0.5 else 0.0),

    # A soft diagonal band. Screen or Add, near-white, low opacity: the
    # reflection off a curved glass face.
    "sheen_diagonal.png": (256, 256, lambda u, v: 0.85 * (
        1.0 - smoothstep(abs((u + v) * 0.5 - 0.42) / 0.16))),
}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, (w, h, fn) in sorted(IMAGES.items()):
        path = OUT / name
        write_png(path, w, h, build(w, h, fn))
        print("%-22s %4dx%-4d %7d bytes" % (name, w, h, path.stat().st_size))
    print("-> %s" % OUT)


if __name__ == "__main__":
    main()
