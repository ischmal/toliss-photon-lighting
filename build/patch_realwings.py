#!/usr/bin/env python3
"""
Make the RealWings wing-mod's baked wingtip lights Photon-aware (R2, in place).

RealWings replaces the ToLiss wing objects and bakes its own wingtip lights into
MainNEO/SecondaryNEO/Main OBJs, using X-Plane's *sizeable* named lights
(airplane_nav_left_size / _right_size, airplane_strobe_size / _sp) with fixed
colors and no profile switching. Those lights sit inside deep, wing-specific
animation chains (multi-level anim/winglex, even anim/slat*), so they can't be
safely re-homed into lights_out.

This patcher instead rewrites each baked light *in place* into a ToLiss
parametric light gated by the ToLissPhoton datarefs — same X/Y/Z, so it keeps
RealWings' exact position and animation, but gains Photon's original/LED colour
switching. The original line is preserved (commented) for reversibility.

Pair this with:  build_objs.py build --wing realwings   (omits Photon's own
lights_out wingtip nav/strobe, which RealWings' geometry supersedes).

Usage:
    python build/patch_realwings.py --root "<...>/RealWings320"   # patch in place
    python build/patch_realwings.py --root <dir> --dry-run        # preview only
    python build/patch_realwings.py --root <dir> --reverse        # restore .bak

NOTE: intensity / direction / cone are seeded from Photon's stock A320 wingtip
values and are the parameters most likely to want in-sim tuning; colours come
straight from the Photon palette. Positions are preserved exactly (not rounded).
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import build_objs as B

MARKER = "# >>> Photon RealWings"

# Baked RealWings class -> how to render it as a Photon parametric light.
#   cat    : ToLissPhoton category dataref
#   cls    : ToLiss parametric light class (bb billboard / pm spill)
#   color  : Photon palette colour name
#   side   : 'left'/'right'/None (None = infer from x sign, for strobes)
#   inten  : (original, led) candela
#   cone   : dot-product cutoff
CONVERT = {
    "airplane_nav_left_size":  dict(cat="nav",    cls="airplane_nav_bb", color="red",
                                     side="left",  inten=(5000, 6000), cone=0.2, index=0),
    "airplane_nav_right_size": dict(cat="nav",    cls="airplane_nav_bb", color="green",
                                     side="right", inten=(4000, 5000), cone=0.2, index=0),
    "airplane_strobe_size":    dict(cat="strobe", cls="airplane_strobe_bb", color="white",
                                     side=None,    inten=(30000, 30000), cone=0.0, index=0),
    "airplane_strobe_sp":      dict(cat="strobe", cls="airplane_strobe_pm", color="white",
                                     side=None,    inten=(8000, 8000), cone=0.091, index=0),
}
# Outward beam directions borrowed from Photon's stock A320 wingtip lights.
DIRS = {
    ("airplane_nav_bb", "left"):     [-0.574, 0, -0.819],
    ("airplane_nav_bb", "right"):    [0.574, 0, -0.819],
    ("airplane_strobe_bb", "left"):  [-0.707, 0, -0.707],
    ("airplane_strobe_bb", "right"): [0.707, 0, -0.707],
    ("airplane_strobe_pm", "left"):  [-0.895, -0.094, -0.436],
    ("airplane_strobe_pm", "right"): [0.895, -0.094, -0.436],
}


def num(x):
    return B.fmt(x) if isinstance(x, float) else str(x)


def photon_block(cls_size, x, y, z, indent, palettes):
    c = CONVERT[cls_size]
    side = c["side"] or ("left" if float(x) < 0 else "right")
    cls = c["cls"]
    d = DIRS[(cls, side)]
    sw = B.swatch_for(cls)
    pos = f"{x} {y} {z}"
    dirs = " ".join(num(v) for v in d)
    lines = [f"{indent}{MARKER} — was {cls_size} (edit intensity/dir/cone to taste)"]
    for branch, palette_name in (("original", "xenon" if c["cat"] in ("strobe", "beacon") else "halogen"),
                                 ("led", "led")):
        rgb = " ".join(B.fmt(v) for v in palettes[palette_name][c["color"]][sw])
        inten = c["inten"][0 if branch == "original" else 1]
        hide = "1 1" if branch == "original" else "0 0"
        lp = (f"{indent}\tLIGHT_PARAM {cls} {pos} {rgb} {c['index']} "
              f"{inten}cd {dirs} {num(c['cone'])}")
        lines += [f"{indent}ANIM_begin",
                  f"{indent}\tANIM_hide {hide} ToLissPhoton/exterior/{c['cat']}",
                  lp,
                  f"{indent}ANIM_end"]
    return lines


def patch_file(path: Path, palettes, dry=False):
    raw = path.read_text(encoding="utf-8", errors="replace")
    nl = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.replace("\r\n", "\n").split("\n")
    if any(MARKER in l for l in lines):
        print(f"  = {path.name}: already patched, skipping")
        return 0
    out, n = [], 0
    for line in lines:
        s = line.strip()
        tok = s.split()
        if len(tok) >= 5 and tok[0] == "LIGHT_PARAM" and tok[1] in CONVERT:
            indent = line[: len(line) - len(line.lstrip())]
            x, y, z = tok[2], tok[3], tok[4]
            out.append(f"{indent}#{s}")  # keep original, commented
            out.extend(photon_block(tok[1], x, y, z, indent, palettes))
            n += 1
        else:
            out.append(line)
    if n and not dry:
        shutil.copy2(path, path.with_suffix(path.suffix + ".bak"))
        path.write_text(nl.join(out), encoding="utf-8")
    verb = "would patch" if dry else "patched"
    print(f"  {'~' if n else '.'} {path.name}: {verb} {n} wingtip light(s)")
    return n


def reverse_bak(bak: Path):
    """Restore the original .obj from its .obj.bak sibling."""
    orig = bak.with_suffix("")  # strip the trailing .bak -> foo.obj
    shutil.copy2(bak, orig)
    bak.unlink()
    print(f"  < {orig.name}: restored from .bak")
    return 1


def realwings_objs(root: Path):
    """OBJs under root that contain a baked RealWings wingtip light."""
    for p in sorted(root.rglob("*.obj")):
        try:
            head = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if any(c in head for c in CONVERT):
            yield p


def run(root: Path, dry=False, reverse=False):
    """Patch (or reverse) every RealWings OBJ under `root`. Reusable entry point
    shared by this script's CLI and build_realwings.py's one-shot wrapper."""
    if not root.exists():
        sys.exit(f"not found: {root}")

    if reverse:
        total = sum(reverse_bak(b) for b in sorted(root.rglob("*.obj.bak")))
        print(f"restored {total} file(s)")
        return
    palettes = B.load_config()["palettes"]
    objs = list(realwings_objs(root))
    if not objs:
        sys.exit("no RealWings OBJs with baked wingtip lights found under root")
    total = sum(patch_file(p, palettes, dry=dry) for p in objs)
    print(f"{'would patch' if dry else 'patched'} {total} light(s) across "
          f"{len(objs)} file(s)")


def main():
    ap = argparse.ArgumentParser(description="Photon-ify RealWings wingtip lights")
    ap.add_argument("--root", required=True, help="folder to scan for RealWings OBJs")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--reverse", action="store_true", help="restore originals from .bak")
    args = ap.parse_args()
    run(Path(args.root), dry=args.dry_run, reverse=args.reverse)


if __name__ == "__main__":
    main()
