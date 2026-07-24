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

NOTE: every emitted parameter comes from the DSL. Positions are RealWings' own
and are preserved verbatim, not rounded — except for the DSL `@realwings offset`
on the wing_nav / wing_strobe fixtures, which corrects RealWings' known wingtip
misplacement (see the "RealWings placement corrections" block below).
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

# --- RealWings placement corrections (tuned in the DSL, applied here) --------
# RealWings bakes its wingtip lights at the WRONG position (a bug in the mod, not
# in Photon). The correction is NOT hard-coded here — it lives in the DSL, on the
# wing_nav / wing_strobe fixtures, as the same `@realwings { offset: dx dy dz; }`
# knob the landing lights already use. docs/dsl.md calls offset "the knob for
# 'the mod's lights are in the wrong place'"; keeping this correction there is
# what puts ALL lighting tuning in one place. load_config(wing="realwings")
# resolves that offset onto the fixture, this patcher reads it (see photon_block)
# and ADDS it to RealWings' baked X/Y/Z, mirroring X for the starboard side just
# as Emitter.apply_offset does. A zero/absent offset leaves the baked position
# byte-for-byte unchanged.
#
# Frame note: the emitter applies a fixture offset as an aircraft-coordinate
# ANIM_trans, but RealWings' baked light sits deep inside the mod's own wing-flex
# animation chain, so here the offset is added to the light's mount-local
# position instead — which is what keeps the light glued to RealWings' wingtip
# through wing flex. Same DSL knob, frame-appropriate application.
#
# CEO and NEO ship as separate mod OBJs (Main.obj vs MainNEO.obj) and need
# different corrections, so the offset is further split by the free `wingtip`
# axis (@fence = CEO, @sharklet = NEO): the patcher picks the branch from the
# OBJ filename (…NEO.obj -> sharklet, else fence) — see patch_file — and the
# airframe threaded in by the caller selects any a319/a321 `extends` override.
# Both dimensions resolve in _resolve_offset; keep tuning in the DSL, never in
# a lookup table here. Tuning loop: edit the DSL and just re-patch (or save
# under watch.py --write) — patch_file REFRESHES an already-patched OBJ from
# its pristine .bak, so new values land without a manual --reverse.


def num(x):
    return B.fmt(x) if isinstance(x, float) else str(x)


def _fmt_pos(v) -> str:
    """Format a corrected coordinate: trim trailing zeros but keep RealWings'
    precision (unlike B.fmt, which rounds to 3 dp — too coarse for positions)."""
    s = f"{float(v):.7f}".rstrip("0").rstrip(".")
    return s or "0"


def _resolve_offset(off, wingtip, side):
    """Walk an offset value — a scalar vec3, or dicts keyed by wingtip
    (fence/sharklet) and/or side (port/starboard) in any nesting order — down to
    the vec3 for this (wingtip, side). A wingtip map that lacks this variant's
    key means no correction was declared for it (config missing the usual
    unconditioned base), so return None -> the baked position stays verbatim.
    Side maps fall back to port, matching pick_side/the emitter."""
    v = off
    for _ in range(3):
        if isinstance(v, dict):
            if "fence" in v or "sharklet" in v:
                v = v.get(wingtip)
                continue
            if "port" in v or "starboard" in v:
                v = v.get(side, v.get("port"))
                continue
        break
    return v


def apply_offset(x, y, z, offset, side, wingtip):
    """Add the DSL fixture `offset` to a RealWings-baked position, resolving it for
    this side + wingtip (fence=CEO / sharklet=NEO) and mirroring X for starboard
    (same rule as Emitter.apply_offset). Returns x/y/z as strings; a zero/absent
    offset passes them through UNCHANGED, so an untuned fixture keeps RealWings'
    exact bytes. Only a moved axis is reformatted."""
    off = _resolve_offset(offset, wingtip, side)
    if not off:
        return x, y, z
    v = [B.rnd(a) for a in off]
    if side == "starboard":
        v = [-v[0], v[1], v[2]]
    if v == [0.0, 0.0, 0.0]:
        return x, y, z
    return tuple(orig if d == 0 else _fmt_pos(float(orig) + d)
                 for orig, d in zip((x, y, z), v))


def resolve_source(cfg, cls_size, airframe=None):
    """Resolve a baked RealWings class to (category, merged-light-properties,
    fixture-offset) by walking SOURCE's <fixture> <group> <type> path into the
    DSL. The props are the light type's declarations overlaid with the layout's
    per-light overrides — the same merge Emitter.light_line does. The offset is
    the fixture's `@realwings`-resolved placement correction (see apply_offset).

    `airframe` selects which airframe's fixtures to read so per-airframe offset
    overrides (a319/a321 `extends` a320) apply; it falls back to SOURCE_AIRFRAME
    when unknown. Appearance is unaffected by the choice — a319/a321 inherit the
    A320 fence-group params unchanged — so only the offset actually varies."""
    src = SOURCE[cls_size]
    af_name = airframe if airframe in cfg["airframes"] else SOURCE_AIRFRAME
    af = B.resolve_airframe(cfg, af_name)
    try:
        fixture = af["fixtures"][src["fixture"]]
    except KeyError:
        raise SystemExit(f"patch-realwings: fixture {src['fixture']!r} not found in "
                         f"airframe {af_name} — SOURCE is out of sync with "
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
    return fixture["gating"]["category"], merged, fixture.get("offset")


def photon_block(cfg, cls_size, x, y, z, indent, airframe=None, wingtip="fence"):
    """Render one baked light as an original/LED pair of Photon parametric lights
    at RealWings' exact position."""
    src = SOURCE[cls_size]
    category, merged, offset = resolve_source(cfg, cls_size, airframe)
    # RealWings bakes one strobe class for both wingtips; the port/starboard
    # distinction is only recoverable from the position it baked in.
    side = src["side"] or ("port" if float(x) < 0 else "starboard")
    # Position is RealWings' own, corrected only by the DSL @realwings offset
    # (resolved per side + wingtip inside apply_offset, X mirrored for starboard);
    # a zero offset is a verbatim passthrough.
    x, y, z = apply_offset(x, y, z, offset, side, wingtip)
    cls = merged["class"]
    swatch = B.swatch_for(cls)

    # The layout defines the port side; the starboard copy is the emitter's
    # mirror — negate the X component of the direction (same rule as
    # Emitter.light_line).
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


def patch_file(path: Path, cfg, airframe=None, dry=False):
    raw = path.read_text(encoding="utf-8", errors="replace")
    # An already-patched file is REFRESHED, not skipped: re-derive from the
    # pristine .bak so DSL changes (offset tuning, palette edits, a new Photon
    # version) reach a mod that was patched under older values. Without this,
    # re-running the patch — dev builds, watch.py, an installer upgrade — left
    # the stale patch in place. The .bak is the refresh source and is NOT
    # re-copied (that would clobber the true original with patched content).
    refreshing = MARKER in raw
    if refreshing:
        bak = path.with_suffix(path.suffix + ".bak")
        if not bak.exists():
            print(f"  = {path.name}: patched but no .bak to refresh from, skipping")
            return 0
        raw = bak.read_text(encoding="utf-8", errors="replace")
        if MARKER in raw:
            print(f"  = {path.name}: .bak itself contains a patch (?), skipping")
            return 0
    nl = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.replace("\r\n", "\n").split("\n")
    # RealWings ships CEO and NEO as separate OBJs (Main.obj vs MainNEO.obj); the
    # baked lights carry no marker, so the wingtip variant (which keys the DSL
    # @realwings offset: fence=CEO / sharklet=NEO) is read from the filename.
    wingtip = "sharklet" if "NEO" in path.stem.upper() else "fence"
    out, n = [], 0
    for line in lines:
        s = line.strip()
        tok = s.split()
        if len(tok) >= 5 and tok[0] == "LIGHT_PARAM" and tok[1] in SOURCE:
            indent = line[: len(line) - len(line.lstrip())]
            # baked X/Y/Z pass straight to photon_block, which applies the DSL
            # @realwings offset (if any) per side + wingtip.
            out.append(f"{indent}#{s}")  # keep original, commented
            out.extend(photon_block(cfg, tok[1], tok[2], tok[3], tok[4], indent,
                                    airframe, wingtip))
            n += 1
        else:
            out.append(line)
    if n and not dry:
        if not refreshing:  # refresh: .bak already holds the true original
            shutil.copy2(path, path.with_suffix(path.suffix + ".bak"))
        # atomic_write_bytes, not a plain write: watch.py --write can re-patch
        # this exact file moments after triggering an in-sim reload that reads
        # it — see build_objs.py's crash-lesson comment on atomic_write_bytes.
        B.atomic_write_bytes(path, nl.join(out).encode("utf-8"))
    verb = (("would refresh" if dry else "refreshed (from .bak)") if refreshing
            else ("would patch" if dry else "patched"))
    print(f"  {'~' if n else '.'} {path.name}: {verb} {n} wingtip light(s)")
    return n


def reverse_bak(bak: Path):
    """Restore the original .obj from its .obj.bak sibling."""
    orig = bak.with_suffix("")  # strip the trailing .bak -> foo.obj
    # atomic (see the crash-lesson comment on atomic_write_bytes): --reverse can
    # also run with the sim up.
    B.atomic_write_bytes(orig, bak.read_bytes())
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


def run(root: Path, airframe=None, dry=False, reverse=False):
    """Patch (or reverse) every RealWings OBJ under `root`. Entry point shared by
    build_objs' `patch-realwings` subcommand and its post-build --write hook.
    Placement corrections come from the DSL @realwings offset (see apply_offset);
    `airframe` selects per-airframe offset overrides (falls back to A320)."""
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
    total = sum(patch_file(p, cfg, airframe=airframe, dry=dry) for p in objs)
    print(f"{'would patch' if dry else 'patched'} {total} light(s) across "
          f"{len(objs)} file(s)")
    return total
