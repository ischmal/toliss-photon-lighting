#!/usr/bin/env python3
"""
Resolve the RealWings wingtip patch OUT OF THE DSL — the bundle-time half.

RealWings bakes its own wingtip lights into MainNEO/SecondaryNEO/Main OBJs using
X-Plane's *sizeable* named lights with fixed colors and no profile switching.
Photon rewrites each one in place into a parametric light gated by the
ToLissPhoton datarefs. This module works out WHAT each one should become;
`installer/realwings.py` does the rewriting.

⚠ THE SPLIT, AND WHY
---------------------
This module used to do both halves, which meant the installer imported
`build_objs` — and therefore the whole DSL parser — at INSTALL time, and a release
had to bundle `payload/dsl/` for it. A C++ installer will not embed a DSL parser,
so the resolution moved to bundle time, exactly like the OBJs
(docs/installer_cpp_plan.md §2a):

    build time   this module + build_objs  ->  payload/realwings_patch.json
    install time installer/realwings.py    <-  payload/realwings_patch.json

⚠ THAT JSON IS A GENERATED ARTIFACT, NEVER HAND-EDITED — see `resolve_patch_data`
and the `_generated` header it writes. All lighting tuning still lives in the DSL;
this is the same "resolve once, apply many" shape `payload/objs/` already has, not
a hard-coded position table (which the tripwire forbids and nothing here creates).

Nothing about a light's appearance is duplicated here either. Class, color,
intensity, cone, direction, the category dataref, and the original-branch palette
are all resolved through `build_objs`' own resolvers, so editing
`lights.style.phdsl` / `lights.layout.phdsl` moves these lights too.

There is no CLI here; drive it through the builder, which also auto-locates the
installed mod folder:

    python build/build_objs.py build --wing realwings --write    # build + patch
    python build/build_objs.py patch-realwings --airframe a320 --dry-run
    python build/build_objs.py patch-realwings --airframe a320 --reverse

`--wing realwings` omits Photon's own lights_out wingtip nav/strobe, which
RealWings' geometry supersedes; those same omitted fixtures are what SOURCE below
reads its parameters from. The dev loop is unchanged by the split: `run()` resolves
the structure in memory and hands it straight to the applier, so a `.phdsl` edit
still lands with no bundle step in between.
"""
from __future__ import annotations

import datetime as _dt
import json
import sys
from pathlib import Path

import build_objs as B

REPO = Path(__file__).resolve().parent.parent
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from installer import realwings as RW           # noqa: E402
from installer.realwings import MARKER          # noqa: E402  (re-export)
from installer.realwings import RealWingsError  # noqa: E402  (re-export)

# Baked RealWings light class -> the Photon light it should render as, named as a
# path into the DSL: <fixture> <group> <light type> in lights.layout.phdsl.
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

# The two dimensions a resolved record is keyed by besides the airframe. RealWings
# ships CEO and NEO as separate mod OBJs (Main.obj vs MainNEO.obj) and they need
# different corrections, so the DSL offset is split by the free `wingtip` axis
# (@fence = CEO, @sharklet = NEO); the applier picks the branch from the OBJ
# filename. Both sides are always resolved because a strobe class's side is only
# recoverable at apply time, from the sign of the baked X.
WINGTIPS = ("fence", "sharklet")
SIDES = ("port", "starboard")

# --- RealWings placement corrections (tuned in the DSL, resolved here) --------
# RealWings bakes its wingtip lights at the WRONG position (a bug in the mod, not
# in Photon). The correction is NOT hard-coded here — it lives in the DSL, on the
# wing_nav / wing_strobe fixtures, as the same `@realwings { offset: dx dy dz; }`
# knob the landing lights already use. docs/dsl.md calls offset "the knob for
# 'the mod's lights are in the wrong place'"; keeping this correction there is
# what puts ALL lighting tuning in one place. load_config(wing="realwings")
# resolves that offset onto the fixture, `_resolve_offset` walks it down to this
# (wingtip, side), and the applier ADDS it to RealWings' baked X/Y/Z — mirroring X
# for starboard just as Emitter.apply_offset does. A zero/absent offset leaves the
# baked position byte-for-byte unchanged.
#
# Frame note: the emitter applies a fixture offset as an aircraft-coordinate
# ANIM_trans, but RealWings' baked light sits deep inside the mod's own wing-flex
# animation chain, so there the offset is added to the light's mount-local
# position instead — which is what keeps the light glued to RealWings' wingtip
# through wing flex. Same DSL knob, frame-appropriate application.


def num(x):
    return B.fmt(x) if isinstance(x, float) else str(x)


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


def offset_vec(offset, side, wingtip):
    """The DSL fixture `offset` resolved for this (side, wingtip) and MIRRORED for
    starboard (same rule as Emitter.apply_offset), or None when there is no
    correction to make. None and an all-zero vector are the same thing to the
    applier — both mean "leave RealWings' bytes alone" — so both come back None,
    which is also what keeps the generated JSON free of no-op entries."""
    off = _resolve_offset(offset, wingtip, side)
    if not off:
        return None
    v = [B.rnd(a) for a in off]
    if side == "starboard":
        v = [-v[0], v[1], v[2]]
    return None if v == [0.0, 0.0, 0.0] else v


def apply_offset(x, y, z, offset, side, wingtip):
    """Resolve the DSL fixture `offset` for this side + wingtip and add it to a
    RealWings-baked position. Kept as one call because it is the unit the offset
    tests reason about; the two halves now live either side of the bundle-time
    split (`offset_vec` here, `shift_pos` in the applier)."""
    return RW.shift_pos(x, y, z, offset_vec(offset, side, wingtip))


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


def resolve_record(cfg, cls_size, airframe, wingtip, side) -> dict:
    """Everything the applier needs to render ONE baked light, fully resolved:
    no DSL vocabulary survives past this function.

    Mirrors what the old `photon_block` computed inline, in the same order, so a
    diff of the emitted OBJ lines is the check that the split is faithful."""
    category, merged, offset = resolve_source(cfg, cls_size, airframe)
    cls = merged["class"]
    swatch = B.swatch_for(cls)

    # The layout defines the port side; the starboard copy is the emitter's
    # mirror — negate the X component of the direction (same rule as
    # Emitter.light_line).
    d = [B.rnd(v) for v in merged["dir"]]
    if side == "starboard":
        d = [-d[0], d[1], d[2]]

    cname = B.pick_side(merged.get("color", "white"), side)
    # The original (dataref=0) branch's palette is the category's, exactly as the
    # OBJ emitter picks it — not a second copy of the categories table.
    original_palette = cfg["categories"][category]["original"]
    branches = []
    for branch, profile in (("original", original_palette), ("led", "led")):
        branches.append({
            "hide": "1 1" if branch == "original" else "0 0",
            "rgb": " ".join(B.fmt(v) for v in cfg["palettes"][profile][cname][swatch]),
            "intensity": int(round(B.resolve_field(merged.get("intensity", 0),
                                                   profile, side))),
            "cone": num(B.rnd(B.resolve_field(merged["cone"], profile, side))),
        })
    return {
        "category": category,
        "class": cls,
        "index": B.pick_side(merged.get("index", 0), side),
        "dir": " ".join(num(v) for v in d),
        "offset": offset_vec(offset, side, wingtip),
        "branches": branches,
    }


def realwings_airframes() -> list[str]:
    """Airframes worth resolving: those whose DSL actually supports a RealWings
    build. `SUPPORTED_WINGS` is the same gate `make_release.py` uses to decide
    which OBJs to emit, so the JSON can never carry a variant the bundle has no
    OBJ for (a339 has no wing mods at all)."""
    return [af for af, wings in B.SUPPORTED_WINGS.items() if "realwings" in wings]


def resolve_patch_data(cfg=None, airframes=None) -> dict:
    """Resolve the WHOLE patch table out of the DSL — every (airframe, baked
    class, wingtip, side) — as the structure `installer/realwings.py` applies.

    This is what `make_release.py` serializes into `payload/realwings_patch.json`
    and what the dev path hands straight to the applier in memory."""
    cfg = cfg if cfg is not None else B.load_config(wing="realwings")
    frames = airframes if airframes is not None else realwings_airframes()
    if SOURCE_AIRFRAME not in frames:
        frames = [SOURCE_AIRFRAME, *frames]
    data = {
        "_generated": (
            "GENERATED by build/patch_realwings.py from lights.style.phdsl + "
            "lights.layout.phdsl — DO NOT HAND-EDIT. Tune the lights in the DSL "
            "and rebuild the bundle; an edit here is overwritten and, unlike the "
            "DSL, reaches nothing else."),
        "schema": RW.SCHEMA,
        "builtAt": _dt.datetime.now().isoformat(timespec="seconds"),
        "marker": MARKER,
        "sourceAirframe": SOURCE_AIRFRAME,
        "classes": {
            cls: {"side": src["side"],
                  "path": f"{src['fixture']}/{src['group']}/{src['type']}"}
            for cls, src in SOURCE.items()
        },
        "airframes": {},
    }
    for af in frames:
        per_class = {}
        for cls_size in SOURCE:
            per_class[cls_size] = {
                wingtip: {side: resolve_record(cfg, cls_size, af, wingtip, side)
                          for side in SIDES}
                for wingtip in WINGTIPS
            }
        data["airframes"][af] = per_class
    return RW.validate(data, "resolve_patch_data()")


def write_patch_data(dest: Path, cfg=None) -> dict:
    """Serialize `resolve_patch_data()` to `dest` (the bundle's
    `realwings_patch.json`). Returns the data, for callers that want to report on
    it."""
    data = resolve_patch_data(cfg)
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return data


# ─── the dev path: resolve live, apply immediately ────────────────────────────
def patch_file(path: Path, cfg, airframe=None, dry=False):
    """Patch one mod OBJ against a freshly-resolved DSL config.

    The dev-loop entry point: `cfg` is a live `load_config(wing="realwings")`, so
    a `.phdsl` edit is picked up with no bundle step. The installer calls the
    applier directly with the bundled JSON instead."""
    return RW.patch_file(path, resolve_patch_data(cfg), airframe=airframe,
                         dry=dry, write=B.atomic_write_bytes)


def reverse_bak(bak: Path):
    """Restore the original .obj from its .obj.bak sibling."""
    return RW.reverse_bak(bak, write=B.atomic_write_bytes)


def realwings_objs(root: Path):
    """OBJs under root that contain a baked RealWings wingtip light."""
    return RW.realwings_objs(root, {"classes": {c: {} for c in SOURCE}})


def run(root: Path, airframe=None, dry=False, reverse=False):
    """Patch (or reverse) every RealWings OBJ under `root`. Entry point shared by
    build_objs' `patch-realwings` subcommand and its post-build --write hook.

    ⚠ `atomic_write_bytes` (not a plain write) is passed through to the applier:
    `watch.py --write` can re-patch this exact file moments after triggering an
    in-sim reload that reads it — see build_objs.py's crash-lesson comment. The
    installer's own path has no such race and uses the applier's plain atomic
    write."""
    root = Path(root)
    if not root.exists():
        raise SystemExit(f"not found: {root}")
    # Resolving the config is only worth it for a forward patch — a reverse just
    # restores .bak files and must keep working even if the DSL does not parse.
    data = {} if reverse else resolve_patch_data()
    try:
        return RW.run(root, data, airframe=airframe, dry=dry, reverse=reverse,
                      write=B.atomic_write_bytes)
    except RealWingsError as e:
        raise SystemExit(str(e)) from e
