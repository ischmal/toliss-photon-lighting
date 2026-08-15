"""Derive display-unit UV rects from a ToLiss cockpit OBJ.

The premise, confirmed in-sim 2026-07-31: `cockpit_INN.obj` carries no TEXTURE
line and declares GLOBAL_cockpit_lit, so every ATTR_cockpit triangle samples the
aircraft's panel texture. UV 0..1 therefore maps to the whole 4096 panel, and a
display face's UV bounding box IS its pixel rect in the panel FBO.

Two things make this non-obvious, and both cost a day each when missed:

  * A single TRIS batch covers MANY faces, so measuring per-batch smears all six
    display units into one rect. Triangles are grouped into islands by shared
    VERTEX INDEX instead — an OBJ splits vertices wherever UVs differ, so a UV
    island is exactly one face and a screen does not get welded to its bezel.
  * Every DU face exists TWICE, gated by AirbusFBW/DisplayResolutionFallback:
    a ~748 px variant at 0 and a ~499 px one at 1, anchored at the same origin.
    Both are reported; `--gating` shows the ANIM block that selects each.

Output is ready to paste into kPanelRectsHi / kPanelRectsLo in plugin.cpp.

    python build/derive_panel_rects.py <path-to-cockpit_INN.obj> [--gating]

See docs/fcu_tint_plan.md §2 for what the numbers are used for.
"""
import argparse
import math
import sys

ATLAS = 4096.0


def load(path):
    """Return (verts, idx, tris). tris = (line_no, offset, count, in_cockpit)."""
    verts, idx, tris = [], [], []
    in_cockpit = False
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for ln, line in enumerate(fh):
            f = line.split()
            if not f:
                continue
            k = f[0]
            if k == "VT":
                verts.append(tuple(float(x) for x in f[1:9]))
            elif k == "IDX":
                idx.append(int(f[1]))
            elif k == "IDX10":
                idx.extend(int(x) for x in f[1:11])
            elif k in ("ATTR_cockpit", "ATTR_cockpit_region"):
                in_cockpit = True
            elif k == "ATTR_no_cockpit":
                in_cockpit = False
            elif k == "TRIS":
                tris.append((ln, int(f[1]), int(f[2]), in_cockpit))
    return verts, idx, tris


class DSU:
    def __init__(self):
        self.p = {}

    def find(self, a):
        self.p.setdefault(a, a)
        while self.p[a] != a:
            self.p[a] = self.p[self.p[a]]
            a = self.p[a]
        return a

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.p[ra] = rb


def islands(verts, idx, tris):
    dsu, faces = DSU(), []
    for ln, off, cnt, cockpit in tris:
        if not cockpit:
            continue
        for t in range(0, cnt - 2, 3):
            a, b, c = idx[off + t], idx[off + t + 1], idx[off + t + 2]
            dsu.union(a, b)
            dsu.union(b, c)
            faces.append((a, b, c, ln))

    groups = {}
    for tri in faces:
        groups.setdefault(dsu.find(tri[0]), []).append(tri)

    out = []
    for tl in groups.values():
        vi = {i for tri in tl for i in tri[:3]}
        vs = [verts[i] for i in vi]
        xs = [v[0] for v in vs]; ys = [v[1] for v in vs]; zs = [v[2] for v in vs]
        us = [v[6] for v in vs]; vv = [v[7] for v in vs]

        nx = sum(v[3] for v in vs) / len(vs)
        ny = sum(v[4] for v in vs) / len(vs)
        nz = sum(v[5] for v in vs) / len(vs)
        nl = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        n = (nx / nl, ny / nl, nz / nl)

        w, h = in_plane_size(vs, n)
        out.append(dict(
            ntri=len(tl), lines=sorted({t[3] + 1 for t in tl}),
            w=w, h=h,
            cx=(max(xs) + min(xs)) / 2, cy=(max(ys) + min(ys)) / 2,
            cz=(max(zs) + min(zs)) / 2,
            n=n,
            u0=min(us) * ATLAS, u1=max(us) * ATLAS,
            v0=min(vv) * ATLAS, v1=max(vv) * ATLAS,
        ))
    return out


def in_plane_size(vs, n):
    """(width, height) measured IN THE FACE'S OWN PLANE, meters.

    ⚠ This used to be the axis-aligned x and y extents, and that is wrong for
    any face that is not vertical. It cost the MCDUs a whole pass: they lie
    almost flat on the pedestal (normal 0.00 0.99 0.14, i.e. 8 degrees off
    horizontal), so a 9.4 x 11.4 cm screen measured 11.4 cm wide by **1.4 cm
    tall** and read as a label strip. Anything on the pedestal, the glareshield
    top or a console lands in the same trap.
    """
    ex = (0.0, 1.0, 0.0) if abs(n[1]) < 0.9 else (1.0, 0.0, 0.0)
    ax = (ex[1] * n[2] - ex[2] * n[1],
          ex[2] * n[0] - ex[0] * n[2],
          ex[0] * n[1] - ex[1] * n[0])
    al = math.sqrt(sum(c * c for c in ax)) or 1.0
    ax = tuple(c / al for c in ax)
    ay = (n[1] * ax[2] - n[2] * ax[1],
          n[2] * ax[0] - n[0] * ax[2],
          n[0] * ax[1] - n[1] * ax[0])

    pu = [v[0] * ax[0] + v[1] * ax[1] + v[2] * ax[2] for v in vs]
    pv = [v[0] * ay[0] + v[1] * ay[1] + v[2] * ay[2] for v in vs]
    return max(pu) - min(pu), max(pv) - min(pv)


def is_display(f):
    """Aft-facing, hand-sized, forward in the cockpit, big slab of the atlas.

    Deliberately geometric — no reliance on names, which ToLiss does not provide
    for these faces anyway.

    ⚠ Tuned for the six DISPLAY UNITS and nothing else. The >200 px floor in both
    axes excludes every other readout in the cockpit, including all three faces
    of the FCU row (the strip is 66 px tall; the baro windows are 127 px wide).
    Use --all to survey for those; see docs/fcu_tint_plan.md §6a.
    """
    return (f["n"][2] > 0.25
            and f["cz"] < -4.2
            and 0.10 < f["w"] < 0.40 and 0.06 < f["h"] < 0.40
            and (f["u1"] - f["u0"]) > 200 and (f["v1"] - f["v0"]) > 200
            and f["ntri"] <= 64)


def is_readout(f):
    """Any face the pilot looks at, big enough in the atlas to be worth tinting.

    The survey filter for --all: same premise as is_display with the display-unit
    size floor dropped. Catches the FCU row and is the starting point for mapping
    the rest of the cockpit's readouts.

    ⚠ "Aft-facing" is NOT enough, and assuming it was is what hid the MCDUs.
    They lie on the pedestal with a normal of 0.00 0.99 0.14 — n[2] is 0.14, so
    an `n[2] > 0.2` test drops them, and with them every other console and
    glareshield-top face. UPWARD counts as facing the pilot too.
    """
    return ((f["n"][2] > 0.2 or f["n"][1] > 0.5)
            and f["cz"] < -4.0
            and 0.01 < f["w"] < 0.60 and 0.005 < f["h"] < 0.60
            and (f["u1"] - f["u0"]) > 24 and (f["v1"] - f["v0"]) > 12
            and f["ntri"] <= 64)


def anim_depth_at(path, line_no):
    """Open ANIM_begin blocks at a 1-based line — 0 means ungated.

    Counted over the WHOLE file. show_gating only looks back 40 lines, which can
    show a block it finds but cannot prove there is none.
    """
    depth = 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for ln, line in enumerate(fh, start=1):
            if ln >= line_no:
                break
            s = line.strip()
            if s.startswith("ANIM_begin"):
                depth += 1
            elif s.startswith("ANIM_end") and depth:
                depth -= 1
    return depth


def show_gating(path, cands):
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    seen = set()
    for f in cands:
        for ln in f["lines"]:
            if ln in seen:
                continue
            seen.add(ln)
            start, depth = ln - 1, 0
            for j in range(ln - 1, max(0, ln - 40), -1):
                s = lines[j].strip()
                if s.startswith("ANIM_end"):
                    depth += 1
                if s.startswith("ANIM_begin"):
                    if depth == 0:
                        start = j
                        break
                    depth -= 1
            print(f"\n  --- TRIS line {ln}, block from {start + 1} ---")
            for j in range(start, min(ln, len(lines))):
                s = lines[j].strip()
                if s:
                    print("     ", s)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("obj", help="path to the aircraft's cockpit_INN.obj")
    ap.add_argument("--gating", action="store_true",
                    help="print the ANIM block selecting each candidate")
    ap.add_argument("--all", action="store_true",
                    help="survey mode: every aft-facing readout-sized island, "
                         "not just the six display units, with an ungated check")
    args = ap.parse_args(argv)

    verts, idx, tris = load(args.obj)
    isl = islands(verts, idx, tris)
    keep = is_readout if args.all else is_display
    cands = sorted((f for f in isl if keep(f)),
                   key=lambda f: (-f["cy"], f["cx"]))

    print(f"VT={len(verts)}  IDX={len(idx)}  ATTR_cockpit islands={len(isl)}")
    print(f"{len(cands)} candidate {'readout' if args.all else 'display'} faces\n")
    print("   tri  cm(w x h)    pos x,y,z             "
          "u0..u1            v0..v1           px" + ("   anim" if args.all else ""))
    print("  " + "-" * (99 if args.all else 92))
    for f in cands:
        row = ("  %4d  %5.1fx%5.1f  %6.3f %6.3f %6.3f  "
               "%7.1f..%7.1f %7.1f..%7.1f  %4.0fx%4.0f" % (
                   f["ntri"], f["w"] * 100, f["h"] * 100,
                   f["cx"], f["cy"], f["cz"],
                   f["u0"], f["u1"], f["v0"], f["v1"],
                   f["u1"] - f["u0"], f["v1"] - f["v0"]))
        if args.all:
            # A gated face needs its ANIM condition understood before it is
            # tinted, and an ungated one can go straight into the rect table.
            d = max(anim_depth_at(args.obj, ln) for ln in f["lines"])
            row += "   %s" % ("-" if d == 0 else "GATED(%d)" % d)
        print(row)

    print("\n--- as plugin.cpp rows ---")
    for f in cands:
        print("    { \"?\",  %8.1ff, %8.1ff, %8.1ff, %8.1ff, -1 },  "
              "// %.1f x %.1f cm  x=%+.3f y=%+.3f z=%+.3f" % (
                  f["u0"], f["u1"], f["v0"], f["v1"],
                  f["w"] * 100, f["h"] * 100, f["cx"], f["cy"], f["cz"]))

    if args.gating:
        show_gating(args.obj, cands)
    return 0


if __name__ == "__main__":
    sys.exit(main())
