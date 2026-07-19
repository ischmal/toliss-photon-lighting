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
RealWings' exact position and animation, but gains Photon's original/LED color
switching. The original line is preserved (commented) for reversibility.

There is no CLI here; drive it through the builder, which also auto-locates the
installed mod folder:

    python build/build_objs.py build --wing realwings --write    # build + patch
    python build/build_objs.py patch-realwings --airframe a320 --dry-run
    python build/build_objs.py patch-realwings --airframe a320 --reverse

`--wing realwings` omits Photon's own lights_out wingtip nav/strobe, which
RealWings' geometry supersedes; those same omitted fixtures are what SOURCE
below reads its parameters from.

NOTE: every emitted parameter except position comes from the DSL (see SOURCE).
Positions are RealWings' own and are preserved verbatim, not rounded.
"""
from __future__ import annotations

import shutil
from pathlib import Path

import build_objs as B

MARKER = "# >>> Photon RealWings"

# Baked RealWings light class -> the Photon light it should render as, named as a
# path into the DSL: <fixture> <group> <light type> in lights.layout.phdsl.
#
# Nothing about the light's appearance is duplicated here. Class, color,
# intensity, cone, direction, the category dataref, and the original-branch
# palette are all resolved out of the config through build_objs' own resolvers,
# so editing lights.style.phdsl / lights.layout.phdsl moves these lights too.
#
# The source fixtures (wing_nav / wing_strobe) are the ones `--wing realwings`
# omits from lights_out — RealWings supersedes their geometry, so reusing their
# parameters is what keeps the patched lights consistent with stock Photon.
#
#   side: which side of the mirrored source fixture to resolve as; None = infer
#         from the sign of the baked light's own X (RealWings bakes one light
#         class for both wingtips and distinguishes them only by position).
SOURCE = {
    "airplane_nav_left_size":  dict(fixture="wing_nav", group="fence",
                                    type="nav_beam", side="port"),
    "airplane_nav_right_size": dict(fixture="wing_nav", group="fence",
                                    type="nav_beam", side="starboard"),
    "airplane_strobe_size":    dict(fixture="wing_strobe", group="fence",
                                    type="strobe_beam", side=None),
    "airplane_strobe_sp":      dict(fixture="wing_strobe", group="fence",
                                    type="strobe_spill", side=None),
}

# The airframe whose wingtip parameters seed the patch. RealWings' baked lights
# carry no airframe marker, and the A319/A321 wingtip fixtures are inherited from
# the A320 unchanged (only the sharklet nav_spill position is overridden), so one
# airframe covers all three.
SOURCE_AIRFRAME = "a320"


def num(x):
    return B.fmt(x) if isinstance(x, float) else str(x)


def resolve_source(cfg, cls_size):
    """Resolve a baked RealWings class to (category, merged-light-properties) by
    walking SOURCE's <fixture> <group> <type> path into the DSL. The returned
    props are the light type's declarations overlaid with the layout's per-light
    overrides — the same merge Emitter.light_line does."""
    src = SOURCE[cls_size]
    af = B.resolve_airframe(cfg, SOURCE_AIRFRAME)
    try:
        fixture = af["fixtures"][src["fixture"]]
    except KeyError:
        raise SystemExit(f"patch-realwings: fixture {src['fixture']!r} not found in "
                         f"airframe {SOURCE_AIRFRAME} — SOURCE is out of sync with "
                         f"lights.layout.phdsl")
    groups = [g for g in fixture.get("groups", []) if g.get("name") == src["group"]]
    if len(groups) != 1:
        raise SystemExit(f"patch-realwings: expected exactly one group named "
                         f"{src['group']!r} in {src['fixture']}, found {len(groups)}")
    lights = [l for l in groups[0]["lights"] if l.get("type") == src["type"]]
    if len(lights) != 1:
        raise SystemExit(f"patch-realwings: expected exactly one {src['type']!r} in "
                         f"{src['fixture']}/{src['group']}, found {len(lights)}")
    light = lights[0]
    merged = {**cfg["lightTypes"][light["type"]],
              **{k: v for k, v in light.items() if k != "type"}}
    return fixture["gating"]["category"], merged


def photon_block(cfg, cls_size, x, y, z, indent):
    """Render one baked light as an original/LED pair of Photon parametric lights
    at RealWings' exact position."""
    src = SOURCE[cls_size]
    category, merged = resolve_source(cfg, cls_size)
    # RealWings bakes one strobe class for both wingtips; the port/starboard
    # distinction is only recoverable from the position it baked in.
    side = src["side"] or ("port" if float(x) < 0 else "starboard")
    cls = merged["class"]
    swatch = B.swatch_for(cls)

    # The layout defines the port side; the starboard copy is the emitter's
    # mirror — negate the X component of the direction (same rule as
    # Emitter.light_line). Position is RealWings' own, so it is never mirrored.
    d = [B.rnd(v) for v in merged["dir"]]
    if side == "starboard":
        d = [-d[0], d[1], d[2]]
    dirs = " ".join(num(v) for v in d)
    index = B.pick_side(merged.get("index", 0), side)
    cname = B.pick_side(merged.get("color", "white"), side)

    lines = [f"{indent}{MARKER} — {cls_size} -> "
             f"{src['fixture']}/{src['group']}/{src['type']} ({side}); "
             f"edit it in lights.style.phdsl / lights.layout.phdsl"]
    # The original (dataref=0) branch's palette is the category's, exactly as the
    # OBJ emitter picks it — not a second copy of the categories table.
    original_palette = cfg["categories"][category]["original"]
    for branch, profile in (("original", original_palette), ("led", "led")):
        rgb = " ".join(B.fmt(v) for v in cfg["palettes"][profile][cname][swatch])
        inten = B.resolve_field(merged.get("intensity", 0), profile, side)
        cone = B.rnd(B.resolve_field(merged["cone"], profile, side))
        hide = "1 1" if branch == "original" else "0 0"
        lp = (f"{indent}\tLIGHT_PARAM {cls} {x} {y} {z} {rgb} {index} "
              f"{int(round(inten))}cd {dirs} {num(cone)}")
        lines += [f"{indent}ANIM_begin",
                  f"{indent}\tANIM_hide {hide} ToLissPhoton/exterior/{category}",
                  lp,
                  f"{indent}ANIM_end"]
    return lines


def patch_file(path: Path, cfg, dry=False):
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
        if len(tok) >= 5 and tok[0] == "LIGHT_PARAM" and tok[1] in SOURCE:
            indent = line[: len(line) - len(line.lstrip())]
            x, y, z = tok[2], tok[3], tok[4]
            out.append(f"{indent}#{s}")  # keep original, commented
            out.extend(photon_block(cfg, tok[1], x, y, z, indent))
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
        if any(c in head for c in SOURCE):
            yield p


def run(root: Path, dry=False, reverse=False):
    """Patch (or reverse) every RealWings OBJ under `root`. Entry point shared by
    build_objs' `patch-realwings` subcommand and its post-build --write hook."""
    if not root.exists():
        raise SystemExit(f"not found: {root}")

    if reverse:
        baks = sorted(root.rglob("*.obj.bak"))
        if dry:
            for b in baks:
                print(f"  < {b.with_suffix('').name}: would restore from .bak")
            print(f"would restore {len(baks)} file(s)")
            return len(baks)
        total = sum(reverse_bak(b) for b in baks)
        print(f"restored {total} file(s)")
        return total
    cfg = B.load_config(wing="realwings")  # patching the mod == the realwings build
    objs = list(realwings_objs(root))
    if not objs:
        raise SystemExit("no RealWings OBJs with baked wingtip lights found under root")
    total = sum(patch_file(p, cfg, dry=dry) for p in objs)
    print(f"{'would patch' if dry else 'patched'} {total} light(s) across "
          f"{len(objs)} file(s)")
    return total
