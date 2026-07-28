#!/usr/bin/env python3
"""
ToLiss Photon Lighting — OBJ generator (Phase II).

Reads the declarative light config — the CSS-like DSL pair src/lights/lights.style.phdsl
(appearance) + src/lights/lights.layout.phdsl (placement), parsed by photon_dsl.py; the
legacy reference/legacy/lights.poc.jsonc is used only if the DSL files are absent
— and emits the X-Plane lights_out3xx_XP12.obj files into dist/. dist/ is OBJs
only; the plugin is now the native .xpl (src/native/), built and installed
separately (see src/native/README.md), not staged here.

Also verifies a generated airframe against the frozen hand-authored golden in
reference/photon/ with a NORMALIZED (not byte-exact) compare: both sides are
parsed into a multiset of (gate-path, light params) records with all floats
rounded to 3 decimals, so hand-authored formatting quirks and the intentional
mirror-symmetry cleanup don't register as differences.

Usage:
    python build/build_objs.py build [--airframe a320] [--wing stock] [--out DIR] [--write]
    python build/build_objs.py check [--airframe a320] [--category nav]
    python build/build_objs.py patch-realwings [--airframe a320] [--dry-run] [--reverse]

`build` writes to dist/ by default; `--out DIR` redirects elsewhere. `--write`
additionally installs each built OBJ straight into the live X-Plane install, at
<X-Plane 12>/Aircraft/ToLissA3XX*/objects/, which is auto-located. This replaces
the old Link-Objects.ps1 symlink workflow (removed): `build --write`, or
`watch.py --write` to do it on every save. Any leftover symlink from that
workflow is replaced with a real file. This installs OBJs only — the native
plugin (src/native/) is built and installed on its own (src/native/README.md).

`--wing realwings` with `--write` also patches the installed RealWings mod's
baked wingtip lights in place (patch_realwings.py) — one command, no separate
step. The `patch-realwings` subcommand runs that same patch on its own (with
--dry-run / --reverse) for in-sim tuning without a rebuild. Each airframe's
RealWings folder is auto-located too (…/objects/RealWings3XX/); override the
X-Plane root with the XPLANE_ROOT env var if it isn't auto-detected, or pass
--root.

`check` reference source: reference/photon/<obj> — the frozen hand-authored OBJs.
These are deliberately NOT the build output: comparing dist/ against itself would
pass trivially and silently destroy the safety net.

See docs/design.md for the data model and docs/dsl.md for the config syntax.
"""
from __future__ import annotations

import argparse
import contextlib
import json
import os
import shutil
import sys
import time
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CONFIG = REPO / "reference" / "legacy" / "lights.poc.jsonc"  # legacy fallback
CONFIG_STYLE = REPO / "src" / "lights" / "lights.style.phdsl"
CONFIG_LAYOUT = REPO / "src" / "lights" / "lights.layout.phdsl"

DIST = REPO / "dist"                       # generated installable tree (OBJs only)
GOLDEN = REPO / "reference" / "photon"     # frozen hand-authored `check` reference

# airframe key -> aircraft folder in the X-Plane tree
OBJ_FOLDER = {"a319": "A319", "a320": "A320", "a321": "A321", "a339": "A339"}

# airframe key -> {target: obj filename}. An airframe with no entry for a target
# does not build that target at all — this is how the A339 is kept out of the
# interior mod (docs/interior_plan.md decision #14), the same shape as
# SUPPORTED_WINGS gating its absent wing mods. Do not "fix" the missing a339
# "interior" key: ToLiss's A339 cockpit is a different model with different
# rheostat indices, and Gus's mod does not cover it.
OBJ_TARGETS = {
    "a319": {"exterior": "lights_out319_XP12.obj", "interior": "lights_inn.obj"},
    "a320": {"exterior": "lights_out320_XP12.obj", "interior": "lights_inn.obj"},
    "a321": {"exterior": "lights_out321_XP12.obj", "interior": "lights_inn.obj"},
    "a339": {"exterior": "ExternalLights_XP12.obj"},
}

TARGETS = ("exterior", "interior")

# airframe key -> supported wing-mod variants for that airframe. Source of truth
# for which --wing values may be built/installed: resolve_mount()'s fallback to
# an arbitrary mount variant when the requested one is missing would otherwise
# silently ship stock content for an airframe with no such wing mod (e.g. a339
# has no Durantula/RealWings variants at all) instead of erroring.
SUPPORTED_WINGS = {
    "a319": ("stock", "durantula", "realwings"),
    "a320": ("stock", "durantula", "realwings"),
    "a321": ("stock", "durantula", "realwings"),
    "a339": ("stock",),
}

HEADER = "I\n800\nOBJ\n\nGLOBAL_cockpit_lit\nPOINT_COUNTS\t0\t0\t0\t0\n"

# Per-target OBJ header. The interior header is NOT the exterior one: stock
# lights_inn.obj declares no GLOBAL_cockpit_lit, writes POINT_COUNTS as
# "0 0 1 0" (spaces, and a 1) rather than "0\t0\t0\t0", and carries a TEXTURE
# line with a trailing tab and an EMPTY value. Both stock and Gus's files say
# "0 0 1 0" while carrying 13 and 23 lights respectively, so the count is
# plainly ignored — matched anyway rather than introducing an untested
# difference. Verified byte-for-byte against the installed stock file.
#
# NB the trailing tab on "TEXTURE\t" is load-bearing and is why emit() keeps the
# header OUT of reindent(), which strips every line.
HEADERS = {
    "exterior": HEADER,
    "interior": "I\n800\nOBJ\n\nTEXTURE\t\nPOINT_COUNTS\t0 0 1 0\n",
}

# Credit banner emitted into the generated interior OBJ (interior_plan.md
# decision #6 — Gus gets heavy credit on every user-facing surface).
INTERIOR_CREDIT = """# ToLiss Photon Lighting — interior (cockpit) lights
# GENERATED from src/lights/lights.{style,layout}.phdsl — do not hand-edit.
#
# Interior light design, placement, intensities and the three colour ramps are
# the work of GUS ("Gus Mod"), used with permission and vendored in
# reference/gus/. Photon's contribution is only to fold his three separate
# variant files into one runtime-switchable OBJ.
#
# Old halogen = 0, new halogen = 1, LED = 2, per category dataref:
#   ToLissPhoton/interior/{dome,map,mainpnl,pedestal,console}"""


def _rel(p: Path):
    """Repo-relative path for display; absolute if --out points outside."""
    try:
        return p.relative_to(REPO)
    except ValueError:
        return p


# ─── JSONC loading ────────────────────────────────────────────────────────────
def strip_jsonc(text: str) -> str:
    """Remove // line and /* */ block comments outside of strings."""
    out = []
    i, n = 0, len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def load_config(path: Path = CONFIG, wing: str = "stock") -> dict:
    """Load the light config: the DSL pair (lights.style.phdsl + lights.layout.phdsl)
    when both exist, else the legacy JSONC at `path`. Both decode to the same dict.

    `wing` resolves the DSL's @stock/@durantula/@realwings conditions, so the
    result is specific to that variant and must be paired with an Emitter built
    for the same one (the Emitter checks). The legacy JSONC has no wing
    conditions, so it ignores this."""
    if CONFIG_STYLE.exists() and CONFIG_LAYOUT.exists():
        import photon_dsl
        return photon_dsl.load_dsl(CONFIG_STYLE, CONFIG_LAYOUT, wing)
    return json.loads(strip_jsonc(path.read_text(encoding="utf-8")))


# ─── merge / inheritance ──────────────────────────────────────────────────────
def deep_merge(base, override):
    """Deep-merge override onto base. Dicts merge by key; lists merge by index
    (an empty/{} element inherits that index; null override detaches)."""
    if override is None:
        return None
    if isinstance(base, dict) and isinstance(override, dict):
        out = dict(base)
        for k, v in override.items():
            out[k] = deep_merge(base.get(k), v) if k in base else v
        return out
    if isinstance(base, list) and isinstance(override, list):
        out = []
        for i in range(max(len(base), len(override))):
            b = base[i] if i < len(base) else None
            if i < len(override):
                out.append(deep_merge(b, override[i]))
            else:
                out.append(b)
        return out
    return override


def resolve_airframe(cfg: dict, name: str) -> dict:
    af = cfg["airframes"][name]
    if "extends" in af:
        base = resolve_airframe(cfg, af["extends"])
        fixtures = deep_merge(base["fixtures"], af.get("fixtures", {}))
    else:
        fixtures = af.get("fixtures", {})
    return {"fixtures": fixtures, "extends": af.get("extends")}


def resolve_mount(cfg: dict, airframe: str, name: str, variant: str = "stock") -> str:
    """Look up a mount by name in the airframe (then up the extends chain) and
    load its verbatim snippet file. The snippet holds a {{lights}} slot."""
    seen = airframe
    while seen:
        mounts = cfg.get("mounts", {}).get(seen, {})
        if name in mounts:
            m = mounts[name]
            path = m.get(variant) or next(iter(m.values()))
            return (REPO / path).read_text(encoding="utf-8").rstrip("\n")
        seen = cfg["airframes"].get(seen, {}).get("extends")
    raise KeyError(f"mount {name!r} not found for airframe {airframe!r}")


# ─── number & value resolution ────────────────────────────────────────────────
def fmt(x) -> str:
    """Format a number rounded to 3 decimals, trimming trailing zeros; -0 -> 0."""
    r = round(float(x), 3)
    if r == 0:
        r = 0.0
    s = f"{r:.3f}".rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def rnd(x) -> float:
    r = round(float(x), 3)
    return 0.0 if r == 0 else r


def pick_side(val, side):
    """If val is a {port, starboard} map, select the side; else return as-is."""
    if isinstance(val, dict) and ("port" in val or "starboard" in val):
        return val.get(side, val.get("port"))
    return val


PROFILES = ("halogen", "xenon", "led")


def resolve_field(value, profile: str, side: str | None, profiles=PROFILES):
    """A field (intensity, cone, ...) may be scalar or nested with profile keys
    and/or port/starboard (either order). Walk to a scalar.

    `profiles` is the set of keys that count as a profile dimension. It defaults
    to the exterior axis (halogen/xenon/led) for the legacy JSONC path, but the
    Emitter passes the union of every declared profile axis so the interior's
    own `axis intprofile: int_old int_new int_led` resolves the same way."""
    v = value
    for _ in range(4):
        if isinstance(v, dict):
            if any(p in v for p in profiles):
                v = (v[profile] if profile in v
                     else next(v[p] for p in profiles if p in v))
                continue
            if "port" in v or "starboard" in v:
                v = pick_side(v, side or "port")
                continue
        break
    return v


def swatch_for(cls: str) -> str:
    return "bb" if cls.endswith("_bb") else "sp"


PROFILE_LABEL = {"led": "LED", "halogen": "Halogen", "xenon": "Xenon",
                 "int_old": "Old Halogen", "int_new": "New Halogen",
                 "int_led": "LED"}
SIDE_LABEL = {"port": "Left", "starboard": "Right"}


def subgate_label(g: dict):
    """Human label for a group's secondary gate (for descriptive comments)."""
    gate = g.get("gate")
    if gate:
        dref, (lo, hi) = gate["dref"], gate["hide"]
        if "SHARK" in dref:
            return "Sharklet" if [lo, hi] == [0, 0] else "Wingtip Fence"
    return g.get("comment")


def reindent(text: str) -> str:
    """Re-indent by ANIM nesting: any *_begin line opens a level (like `{`),
    any *_end closes one (like `}`). One tab per level."""
    out, depth = [], 0
    for line in text.split("\n"):
        s = line.strip()
        if not s:
            out.append("")
            continue
        head = s.split()[0]
        if head.endswith("_end"):
            depth = max(0, depth - 1)
        out.append("\t" * depth + s)
        if head.endswith("_begin"):
            depth += 1
    return "\n".join(out)


# ─── emission ─────────────────────────────────────────────────────────────────
class Emitter:
    def __init__(self, cfg: dict, airframe: str, variant: str = "stock",
                 annotate: bool = False, target: str = "exterior"):
        cfg_wing = cfg.get("wing")
        if cfg_wing is not None and cfg_wing != variant:
            raise SystemExit(
                f"config was loaded for wing variant {cfg_wing!r} but the Emitter "
                f"was built for {variant!r} — the DSL resolves @<variant> "
                f"conditions at load time, so pass load_config(wing={variant!r})")
        if target not in TARGETS:
            raise SystemExit(f"unknown target {target!r} (expected one of {TARGETS})")
        self.cfg = cfg
        self.airframe = airframe
        self.variant = variant  # wing-mod variant: stock | durantula | realwings
        self.target = target    # exterior | interior — selects fixtures + header
        self.annotate = annotate  # emit "# @fixture|gi|li|side" tags (bootstrap)
        self.palettes = cfg["palettes"]
        self.categories = cfg["categories"]
        self.types = cfg["lightTypes"]
        # every declared profile-axis value, so resolve_field walks the interior
        # axis (int_old/int_new/int_led) as readily as the exterior one
        axes = cfg.get("axes") or {}
        keys = list(PROFILES)
        for ax in ("profile", "intprofile"):
            for v in axes.get(ax, ()):
                if v not in keys:
                    keys.append(v)
        self.profile_keys = tuple(keys)

    def profiles_of(self, category):
        """The ordered profile list for a category: branch *i* renders profile
        *i* and is gated on the category dataref reading *i*. The legacy JSONC
        config has no explicit list, so a 2-way [original, led] is assumed."""
        catdef = self.categories[category]
        return catdef.get("profiles") or [catdef["original"], "led"]

    def field(self, value, profile, side):
        """resolve_field bound to this build's full profile-key set."""
        return resolve_field(value, profile, side, self.profile_keys)

    def dref(self, category):
        return f"ToLissPhoton/{self.target}/{category}"

    def light_line(self, light, *, profile, side, group, fixture, mirror):
        # appearance from the light type; placement (position/dir) from the light
        tpl = self.types[light["type"]] if "type" in light else {}
        merged = {**tpl, **{k: v for k, v in light.items() if k != "type"}}
        # LIGHT_SPILL_CUSTOM carries no light class; everything else must name
        # one. The empty default still resolves the `sp` palette swatch, which
        # is right — a spill light is not a billboard.
        cls = merged.get("class", "")
        if not cls and "spill_dref" not in merged:
            raise SystemExit(
                f"light {light.get('type', '?')!r} has no 'class:' (only a "
                f"'spill_dref:' light may omit it)")

        # position: light > group > fixture
        pos = merged.get("position") or (group or {}).get("position") or fixture.get("position")
        pos = [rnd(p) for p in pos]
        d = [rnd(v) for v in merged["dir"]]
        cone = rnd(self.field(merged["cone"], profile, side))
        if mirror and side == "starboard":
            pos = [-pos[0], pos[1], pos[2]]
            d = [-d[0], d[1], d[2]]

        # color is an explicit property of the light type (per-side where it
        # differs); swatch (bb/sp) comes from the class. Resolved through
        # field() rather than a bare pick_side because the interior conditions
        # colour on the PROFILE too (ramp P3 goes warm only under @int_led);
        # for the exterior, whose colours are only ever side-conditioned, this
        # walks the identical path and yields the identical name.
        cname = self.field(merged.get("color", "white"), profile, side)
        rgb = [rnd(v) for v in self.palettes[profile][cname][swatch_for(cls)]]

        index = pick_side(merged.get("index", 0), side or "port")

        # Slot 8 is class-dependent. Exterior billboard/spill classes read it as
        # a photometric intensity and X-Plane wants the "cd" suffix; the
        # interior's airplane_panel_sp / airplane_inst_sp read the same slot as
        # a bare fractional SIZE (1.414, 0.471, ...). A light type declares
        # `size:` or `intensity:` to pick which — never both.
        if "size" in merged:
            slot = fmt(self.field(merged["size"], profile, side))
        else:
            inten = self.field(merged.get("intensity", 0), profile, side)
            slot = f"{int(round(inten))}cd"

        # A light type carrying `spill_dref` emits LIGHT_SPILL_CUSTOM instead of
        # LIGHT_PARAM. Used for exactly one light — the map spot, whose
        # brightness source is a ToLiss dataref rather than a rheostat INDEX, so
        # airplane_panel_sp cannot express it (docs/interior_plan.md §3.3).
        #   LIGHT_SPILL_CUSTOM x y z  r g b a  s  dx dy dz  semi  dref
        # The baked params are the INPUT: X-Plane runs them through the dataref,
        # which may modify them, and draws them UNMODIFIED if the dataref is not
        # found. So with the plugin absent this still draws at its baked alpha —
        # which is why alpha is baked conservatively (dim, not glaring).
        if "spill_dref" in merged:
            alpha = self.field(merged.get("alpha", 1), profile, side)
            nums = pos + rgb + [alpha, slot] + d + [cone]
            fields = [fmt(x) for x in nums]
            return ("\tLIGHT_SPILL_CUSTOM\t" + " ".join(fields)
                    + f" {merged['spill_dref']}")

        nums = pos + rgb + [index, slot] + d + [cone]
        fields = [fmt(x) if isinstance(x, (int, float)) else str(x) for x in nums]
        return f"\tLIGHT_PARAM\t{cls} " + " ".join(fields)

    def groups_of(self, fixture):
        if "groups" in fixture:
            return fixture["groups"]
        return [{"position": fixture.get("position"), "lights": fixture["lights"]}]

    def describe(self, fixture, g, side, profile, name):
        """Descriptive section comment, e.g. 'Wing Nav -> Red/Left -> Wingtip
        Fence -> LED' or 'Wing Strobe -> Left -> Sharklet -> Xenon'."""
        parts = [fixture.get("comment") or name]
        lights = g.get("lights") or []
        tcolor = (self.types[lights[0]["type"]].get("color", "white")
                  if lights and "type" in lights[0] else "white")
        cname = self.field(tcolor, profile, side)
        # Only a real tint (red/green) is worth naming in the comment; the
        # several shades of white are not — the profile suffix already says
        # which era, and "-> Fluoro -> Old Halogen" reads as a contradiction.
        colorlbl = (None if cname in ("white", "warm", "fluoro")
                    else cname.capitalize())
        sidelbl = SIDE_LABEL.get(side)
        if sidelbl and colorlbl:
            parts.append(f"{colorlbl}/{sidelbl}")
        elif sidelbl or colorlbl:
            parts.append(sidelbl or colorlbl)
        sub = subgate_label(g)
        if sub:
            parts.append(sub)
        parts.append(PROFILE_LABEL.get(profile, profile.capitalize()))
        return "# " + " -> ".join(parts)

    def branch_gate(self, category, branch):
        """The ANIM line(s) selecting branch `branch` of `category`.

        ALWAYS ANIM_hide, never ANIM_show. An OBJ8 animation block starts
        VISIBLE, and a show/hide command only takes effect while its range
        MATCHES the dataref. So a block whose sole conditional is
        `ANIM_show -0.5 0.5 dref` is visible at dref 0 (matched) *and* at
        dref 2 (nothing matched, default visible) — every branch draws at
        once and switching the dataref changes nothing. Confirmed in-sim on
        the interior, 2026-07-28. `ANIM_hide` has no such hole, because
        "no match" is exactly the state we want. The corpus agrees: the
        FF777's lights_inn.obj gates 218 blocks and uses hide for all 218.

        TWO-value categories keep the original single-line encoding verbatim —
        "hide when the dataref reads the OTHER branch's value". Every exterior
        category is two-valued, so exterior output stays byte-identical and the
        frozen reference/photon goldens keep passing `check`.

        THREE-or-more-value categories hide the two OPEN-ENDED ranges either
        side of their own value — Not-below AND Not-above. Interior branch 1
        of {0,1,2} emits both lines; branches 0 and 2 need only the one, having
        nothing on one side. Multiple ANIM_hide in a single block AND together
        (204 OBJs in a stock install rely on this; Laminar's own 737 stacks
        three), so no nesting and no per-profile boolean datarefs are needed —
        the ternary dataref already carries everything."""
        profiles = self.profiles_of(category)
        dref = self.dref(category)
        if len(profiles) == 2:
            return [f"ANIM_hide {'1 1' if branch == 0 else '0 0'} {dref}"]
        last = len(profiles) - 1
        lines = []
        if branch > 0:
            lines.append(f"ANIM_hide {fmt(-0.5)} {fmt(branch - 0.5)} {dref}")
        if branch < last:
            lines.append(f"ANIM_hide {fmt(branch + 0.5)} {fmt(last + 0.5)} {dref}")
        return lines

    def emit_branch(self, fixture, category, *, branch, side, mirror, indent="", name=""):
        profile = self.profiles_of(category)[branch]
        lines = [f"{indent}ANIM_begin"]
        lines += [f"{indent}\t" + g for g in self.branch_gate(category, branch)]
        for gi_idx, g in enumerate(self.groups_of(fixture)):
            gate = g.get("gate")
            gi = indent + "\t"
            body = [f"{gi}\t" + self.describe(fixture, g, side, profile, name)]
            for li_idx, light in enumerate(g["lights"]):
                if self.annotate:
                    body.append(f"{gi}\t# @{name}|{gi_idx}|{li_idx}|{side or 'c'}")
                body.append(gi + "\t" + self.light_line(
                    light, profile=profile, side=side,
                    group=g, fixture=fixture, mirror=mirror).lstrip("\t"))
            if gate:
                lo, hi = gate["hide"]
                lines.append(f"{gi}ANIM_begin")
                lines.append(f"{gi}\tANIM_hide {fmt(lo)} {fmt(hi)} {gate['dref']}")
                lines.extend(body)
                lines.append(f"{gi}ANIM_end")
            else:
                lines.extend(body)
        lines.append(f"{indent}ANIM_end")
        return "\n".join(lines)

    def emit_side(self, fixture, category, *, side, mirror, name=""):
        blocks = [self.emit_branch(fixture, category, branch=b, side=side, mirror=mirror,
                                   name=name)
                  for b in range(len(self.profiles_of(category)))]
        body = "\n".join(blocks)

        mount = fixture["gating"].get("mount")
        if isinstance(mount, dict):
            mount = mount.get(side)
        if mount:
            text = resolve_mount(self.cfg, self.airframe, mount, self.variant)
            body = text.replace("{{lights}}", body)
        return self.apply_offset(fixture, body, side, mirror)

    def apply_offset(self, fixture, body, side, mirror):
        """A fixture `offset` translates the whole assembly — mount rigging and
        all — in aircraft coordinates, as a static ANIM_trans wrapping it. The
        mount's own ANIM_trans is first in its chain and so shares this frame:
        the two add linearly, making an offset numerically identical to editing
        the mount snippet's placement by hand. Lights stay in mount-local space.

        This is deliberately translation-only. Rotations and the deploy
        keyframes are frozen rigging lifted from ToLiss and are not what varies
        between wing mods; a mod that needs different *rotation* is what the
        per-variant mount snippet slot is still there for."""
        off = pick_side(fixture.get("offset"), side or "port")
        if not off:
            return body
        if isinstance(off, dict):
            # a @fence/@sharklet (wingtip) offset is patch_realwings-only — it
            # keys the RealWings mod-OBJ correction and has no meaning on a
            # fixture the emitter actually emits (which OBJ would it pick?).
            raise SystemExit(
                "offset: @fence/@sharklet (wingtip) conditions are only valid "
                "on the RealWings-omitted wingtip fixtures (wing_nav / "
                f"wing_strobe under @realwings); an emitted fixture resolved {off!r}")
        v = [rnd(x) for x in off]
        if mirror and side == "starboard":
            v = [-v[0], v[1], v[2]]
        if v == [0.0, 0.0, 0.0]:
            return body  # a zero offset emits nothing, so stock output is unchanged
        t = " ".join(fmt(x) for x in v + v)  # static trans: both keys identical
        return "\n".join(["ANIM_begin", f"\tANIM_trans {t}", body, "ANIM_end"])

    def emit_fixture(self, name, fixture):
        out = []
        # raw passthrough: decorative/opaque blocks copied verbatim (e.g. the
        # landing spill-blob). Kept verbatim like mounts; not rounded.
        if "raw" in fixture:
            if fixture.get("comment"):
                out.append(f"# {fixture['comment']}")
            out.append((REPO / fixture["raw"]).read_text(encoding="utf-8").rstrip("\n"))
            return "\n".join(out)
        # non-raw fixtures get descriptive per-section comments instead of a
        # single header (the section comments already name the fixture).
        gating = fixture["gating"]
        category = gating["category"]
        if gating.get("mirror"):
            out.append(self.emit_side(fixture, category, side="port", mirror=True, name=name))
            out.append(self.emit_side(fixture, category, side="starboard", mirror=True, name=name))
        else:
            out.append(self.emit_side(fixture, category, side=None, mirror=False, name=name))
        return "\n".join(out)

    def emit(self):
        af = resolve_airframe(self.cfg, self.airframe)
        parts = []
        if self.target == "interior":
            parts.append(INTERIOR_CREDIT)
            parts.append("")
        emitted = 0   # counted separately: the credit banner above is not a fixture
        for name, fixture in af["fixtures"].items():
            if not fixture or ("gating" not in fixture and "raw" not in fixture):
                continue
            if fixture.get("target", "exterior") != self.target:
                continue  # this OBJ is not where that fixture belongs
            if self.variant in fixture.get("gating", {}).get("omit_for", []):
                continue  # e.g. wingtip nav/strobe under RealWings (mod owns them)
            parts.append(self.emit_fixture(name, fixture))
            parts.append("")
            emitted += 1
        if not emitted:
            raise SystemExit(
                f"{self.airframe}: no {self.target} fixtures — nothing to emit. "
                f"(An airframe with no {self.target} lights should be left out of "
                f"OBJ_TARGETS entirely rather than building an empty OBJ.)")
        # The header is deliberately NOT run through reindent(): reindent strips
        # every line, which would eat the load-bearing trailing tab on the
        # interior header's "TEXTURE\t". Splitting it out leaves exterior output
        # byte-identical (its header has no leading/trailing whitespace, and
        # contributes no ANIM nesting, so depth still starts at 0).
        return (HEADERS[self.target] + "\n"
                + reindent("\n".join(parts))).replace("\n", "\r\n")


# ─── normalized parsing (for check) ───────────────────────────────────────────
def parse_records(text: str):
    """Parse an OBJ into a multiset of (gate_path, class, pos, rgb, index,
    intensity, dir, cone) records, floats rounded to 3 dp. Only ANIM_hide/show
    conditions form the gate path; transform nodes are ignored."""
    records = Counter()
    stack = []  # list of frames; each frame is a list of gate-conditions
    for raw in text.replace("\r\n", "\n").split("\n"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        tok = line.split()
        head = tok[0]
        if head == "ANIM_begin":
            stack.append([])
        elif head == "ANIM_end":
            if stack:
                stack.pop()
        elif head in ("ANIM_hide", "ANIM_show"):
            lo, hi, dref = rnd(tok[1]), rnd(tok[2]), tok[3]
            if stack:
                stack[-1].append((head, lo, hi, dref))
        elif head in ("LIGHT_PARAM", "LIGHT_SPILL_CUSTOM"):
            # strip trailing inline comment
            body = line.split("#", 1)[0].split()
            # LIGHT_PARAM names a light class then 12 numbers; LIGHT_SPILL_CUSTOM
            # has no class (12 numbers then a dataref). Recording the directive
            # itself as the "class" keeps one comparable record shape, and slot 7
            # holds an INDEX for the former and an ALPHA for the latter — both
            # sides of any compare parse the same way, so it stays sound.
            if head == "LIGHT_PARAM":
                cls, vals = body[1], body[2:]
            else:
                cls, vals = head, body[1:]
            nums = []
            for v in vals:
                v = v.replace("cd", "")
                try:
                    nums.append(float(v))
                except ValueError:
                    pass
            if len(nums) < 12:
                continue
            x, y, z, r, g, b, index, inten, dx, dy, dz, cone = nums[:12]
            gate_path = tuple(c for frame in stack for c in frame)
            # slot 8 is rounded, not truncated to int: the interior reads it as
            # a fractional SIZE (0.4 vs 0.471 vs 0.5), which int() would collapse
            # to 0 and hide from `check`. Exterior intensities are whole numbers,
            # so they compare identically either way.
            # slot 7 rounded rather than int()-ed for the same reason as slot 8:
            # it is a whole-number rheostat INDEX on a LIGHT_PARAM but a
            # fractional ALPHA on a LIGHT_SPILL_CUSTOM. Whole numbers compare
            # identically either way, so exterior records are unaffected.
            rec = (gate_path, cls,
                   (rnd(x), rnd(y), rnd(z)), (rnd(r), rnd(g), rnd(b)),
                   rnd(index), rnd(inten),
                   (rnd(dx), rnd(dy), rnd(dz)), rnd(cone))
            records[rec] += 1
    return records


def project_positions(records):
    """Collapse parse_records() output to a SET of (class, pos, index, dir),
    dropping gate_path/rgb/intensity/cone and multiplicity. For `check --against
    --positions-only`: eyeballing "does every original light have a Photon
    counterpart at the same place", independent of gating structure (Photon wraps
    every fixture in an extra ToLissPhoton/exterior/<category> ANIM_hide the
    original file never had, which would show up as an all-gates-differ false
    positive under the normal gate-path-sensitive compare) or appearance tuning
    (each Photon light also emits twice — once per halogen/xenon and led branch —
    which a set naturally collapses to one)."""
    return {(cls, pos, index, d)
            for (gate, cls, pos, rgb, index, inten, d, cone) in records}


def get_reference(airframe: str, target: str = "exterior") -> str:
    """The canonical Photon reference is the frozen hand-authored OBJ in
    reference/photon/ — a checked-in fixture, deliberately NOT the build output.

    (Historically this read `git show HEAD:<aircraft>/objects/<obj>`, which
    worked only while the committed OBJs were hand-authored. Now that the build
    generates that tree into dist/, reading it back from git would compare
    generated output against generated output and pass unconditionally.)

    Per-target filenames differ (lights_out3xx_XP12.obj vs lights_inn.obj), so
    one flat golden folder still addresses both. The interior golden is shared
    by all three A3xx airframes because the OBJ is identical for all of them."""
    try:
        fname = OBJ_TARGETS[airframe][target]
    except KeyError:
        raise SystemExit(
            f"{airframe} has no {target!r} target (it does not ship that OBJ)")
    p = GOLDEN / fname
    if not p.exists():
        raise SystemExit(
            f"missing golden reference {p.relative_to(REPO)} — `check` needs the "
            f"frozen hand-authored OBJ to compare against.")
    text = p.read_text(encoding="utf-8", errors="replace")
    if "ToLissPhoton" not in text:
        raise SystemExit(
            f"{p.relative_to(REPO)} is not Photon-converted — it looks like a "
            f"stock or truncated file, not a usable golden reference.")
    return text, str(p.relative_to(REPO))


# ─── CLI ──────────────────────────────────────────────────────────────────────
# airframe -> aircraft-folder glob under <X-Plane 12>/Aircraft/
AIRCRAFT_GLOB = {
    "a319": "ToLissA319*",
    "a320": "ToLissA320*",
    "a321": "ToLissA321*",
    "a339": "ToLissA339*",
}
# airframe -> RealWings mod subfolder under the aircraft's objects/. No entry for
# an airframe means it has no wing mods at all (e.g. a339) — _find_realwings_dir
# reports that as a normal "skip", not a crash.
REALWINGS_DIR = {
    "a319": "RealWings319",
    "a320": "RealWings320",
    "a321": "RealWings321",
}


def _xplane_root():
    """Locate the X-Plane 12 folder: $XPLANE_ROOT env override -> the repo's
    Log.txt symlink target -> Steam default."""
    env = os.environ.get("XPLANE_ROOT")
    if env and Path(env).is_dir():
        return Path(env)
    log = REPO / "Log.txt"
    try:
        if log.exists():
            root = log.resolve().parent  # symlink -> <X-Plane 12>/Log.txt
            if root.is_dir():
                return root
    except OSError:
        pass
    fallback = Path(r"C:/Program Files (x86)/Steam/steamapps/common/X-Plane 12")
    return fallback if fallback.is_dir() else None


def _find_aircraft_dir(airframe):
    """Auto-locate an airframe's installed objects/ folder in X-Plane.
    Returns (path, None) on success or (None, reason) if it can't be found."""
    root = _xplane_root()
    if root is None:
        return None, "X-Plane 12 folder not found (set the XPLANE_ROOT env var)"
    glob_pat = AIRCRAFT_GLOB[airframe]
    matches = sorted((root / "Aircraft").glob(glob_pat))
    if not matches:
        return None, f"no aircraft folder matching {glob_pat} under {root / 'Aircraft'}"
    objects = matches[0] / "objects"
    if not objects.is_dir():
        return None, f"{matches[0].name} has no objects/ folder"
    return objects, None


def _find_realwings_dir(airframe):
    """Auto-locate an airframe's installed RealWings mod folder in X-Plane.
    Returns (path, None) on success or (None, reason) if it can't be found."""
    rw_folder = REALWINGS_DIR.get(airframe)
    if rw_folder is None:
        return None, f"{airframe} has no wing mods (no RealWings variant exists for it)"
    objects, reason = _find_aircraft_dir(airframe)
    if objects is None:
        return None, reason
    rw = objects / rw_folder
    if not rw.is_dir():
        return None, (f"{rw_folder}/ not installed in {objects.parent.name} "
                      f"(RealWings not set up for this airframe)")
    return rw, None


# CRASH LESSON (2026-07-20): watch.py --write auto-reloads the aircraft (via
# PI_PhotonDevReload) right after installing a rebuild, and that reload's
# sim/operation/reload_aircraft call runs SYNCHRONOUSLY, reading this exact OBJ
# (and, for --wing realwings, the RealWings mod's Main.obj) off disk while it
# runs. If the NEXT edit is saved fast enough, watch.py's next cycle can start
# rewriting that same file WHILE X-Plane is still mid-read of the previous one.
# A plain write (the old `shutil.copyfile` / `path.write_text` here) can then
# hand X-Plane a torn/half-written file — not a parse error, a native crash in
# X-Plane's OBJ8 loader (reported: worked N times, crashed on the next save;
# N varied because it depends on save-cadence vs. the reload's variable
# duration). atomic_write_bytes below removes the precondition entirely: a
# concurrent reader always gets the complete old file or the complete new one,
# never a mix, regardless of timing. (The plugin's own state guard — Trigger()
# ignoring a retrigger while state != ST_IDLE — prevents a second REENTRANT
# reload_aircraft call, but does nothing to protect the file bytes themselves;
# that's this function's job.)
def atomic_write_bytes(dest: Path, data: bytes, attempts: int = 20,
                       retry_delay: float = 0.15) -> None:
    """Write `data` to `dest` via a same-directory temp file + os.replace, so it
    can never be observed half-written. Windows can transiently deny a rename
    over a file another process still has open for reading (no
    FILE_SHARE_DELETE) — that's the expected, self-resolving case here (X-Plane's
    read finishes well under a second), so failures are retried briefly rather
    than surfaced as an error; the analogous transient WinError 5 on rmtree is
    the same story (see make_release.py's OneDrive/AV note)."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(f"{dest.name}.{os.getpid()}.tmp")
    tmp.write_bytes(data)
    err = None
    for _ in range(attempts):
        try:
            os.replace(tmp, dest)
            return
        except OSError as e:
            err = e
            time.sleep(retry_delay)
    with contextlib.suppress(OSError):
        tmp.unlink()
    raise err


def _write_live(airframe, src: Path):
    """Copy a built OBJ into the airframe's installed objects/ folder, so a build
    lands in the sim without any linking step. Returns True if it was written."""
    objects, reason = _find_aircraft_dir(airframe)
    if objects is None:
        print(f"  ! {airframe}: not written — {reason}")
        return False
    dest = objects / src.name
    # A leftover symlink from the retired Link-Objects.ps1 workflow points back
    # at the repo (and, since the dist/ restructure, usually points at a path
    # that no longer exists); atomic_write_bytes's os.replace overwrites the
    # symlink's own directory entry (never follows it), so no separate unlink
    # is needed — just note it for the log.
    if dest.is_symlink():
        print(f"  - {airframe}: replacing stale symlink at {dest.name}")
    atomic_write_bytes(dest, src.read_bytes())
    print(f"  -> {dest}")
    return True


def _patch_realwings_after_build(targets, deploying):
    """After a `--wing realwings` build, patch each built airframe's installed
    RealWings mod OBJs in place so their baked wingtip lights obey the Photon
    datarefs. The mod folder is auto-discovered per airframe (no path needed)."""
    if not deploying:
        print("note: staging build (no --write) — RealWings mod folders NOT patched; "
              "re-run with --write to patch the live install.")
        return
    import patch_realwings as P  # lazy: patch_realwings imports build_objs
    for af in targets:
        rw, reason = _find_realwings_dir(af)
        if rw is None:
            print(f"\n# {af}: RealWings patch skipped — {reason}")
            continue
        print(f"\n# {af}: patching RealWings wingtip lights under {rw}")
        P.run(rw, airframe=af)


def cmd_build(args):
    cfg = load_config(wing=args.wing)
    if args.airframe:
        if args.wing not in SUPPORTED_WINGS[args.airframe]:
            raise SystemExit(
                f"{args.airframe} has no {args.wing!r} wing-mod variant "
                f"(supported: {', '.join(SUPPORTED_WINGS[args.airframe])})")
        targets = [args.airframe]
    else:
        targets = [af for af in OBJ_TARGETS if args.wing in SUPPORTED_WINGS[af]]
    # getattr, not args.target: cmd_build is also called programmatically (tests,
    # tooling) with a minimal args object that predates this flag.
    target = getattr(args, "target", "both")
    want = TARGETS if target == "both" else (target,)
    root = (REPO / args.out) if args.out else DIST
    for af in targets:
        folder = OBJ_FOLDER[af]
        for tgt in want:
            fname = OBJ_TARGETS[af].get(tgt)
            if fname is None:
                # a339 has no interior OBJ (decision #14) — a normal skip, and
                # the reason an explicit --target interior on it is an error.
                if target != "both":
                    raise SystemExit(f"{af} has no {tgt!r} target")
                continue
            text = Emitter(cfg, af, args.wing, target=tgt).emit()
            # mirror the X-Plane tree: <root>/A320/objects/lights_out320_XP12.obj
            dest = root / folder / "objects" / fname if not args.flat else root / fname
            atomic_write_bytes(dest, text.encode("utf-8"))
            print(f"wrote {_rel(dest)} ({len(text)} bytes)")
            if args.write:
                _write_live(af, dest)
    if not args.flat:
        print("note: dist/ holds OBJs only — the plugin is now the native .xpl "
              "(src/native/), built + installed separately (see src/native/README.md).")
    if args.wing == "realwings":
        _patch_realwings_after_build(targets, deploying=bool(args.write))


def cmd_patch_realwings(args):
    """Patch/preview/revert the installed RealWings mod's baked wingtip lights.
    Same operation `build --wing realwings --write` runs, exposed on its own for
    tuning and reversal without a rebuild."""
    import patch_realwings as P  # lazy: patch_realwings imports build_objs
    if args.root:
        roots = [(args.airframe or "?", Path(args.root))]
    else:
        targets = [args.airframe] if args.airframe else list(OBJ_TARGETS)
        roots = []
        for af in targets:
            rw, reason = _find_realwings_dir(af)
            if rw is None:
                print(f"# {af}: skipped — {reason}")
                continue
            roots.append((af, rw))
        if not roots:
            raise SystemExit("no installed RealWings folder found — pass --root to "
                             "point at one explicitly, or set XPLANE_ROOT.")
    for af, root in roots:
        print(f"\n# {af}: {root}")
        P.run(root, airframe=af, dry=args.dry_run, reverse=args.reverse)
    return 0


def _fmt_rec(rec):
    gate, cls, pos, rgb, idx, inten, d, cone = rec
    g = " / ".join(f"{k[0]}[{k[3]}]{k[1]},{k[2]}" for k in gate) or "(root)"
    return f"    {cls} pos{pos} rgb{rgb} i{idx} {inten}cd dir{d} c{cone}\n      gate: {g}"


def cmd_check(args):
    cfg = load_config(wing=args.wing)
    af = args.airframe or "a320"
    gen_text = Emitter(cfg, af, args.wing, target=getattr(args, "target", "exterior")).emit()
    gen = parse_records(gen_text)

    if args.against:
        ref_path = Path(args.against)
        if not ref_path.is_file():
            raise SystemExit(f"--against {ref_path}: not a file")
        ref_text = ref_path.read_text(encoding="utf-8", errors="replace")
        ref_src = str(_rel(ref_path))
    else:
        if args.positions_only:
            raise SystemExit("--positions-only only makes sense together with --against")
        ref_text, ref_src = get_reference(af, getattr(args, "target", "exterior"))
    ref = parse_records(ref_text)

    if args.positions_only:
        proj_gen, proj_ref = project_positions(gen), project_positions(ref)
        only_gen, only_ref = proj_gen - proj_ref, proj_ref - proj_gen
        print(f"airframe {af} — positions-only compare against: {ref_src}")
        print(f"  reference lights : {len(proj_ref)}")
        print(f"  generated lights : {len(proj_gen)}")
        print(f"  matched          : {len(proj_gen & proj_ref)}")
        print(f"  only in generated (BUGS?)                : {len(only_gen)}")
        print(f"  only in reference (missing from Photon?) : {len(only_ref)}")
        if only_gen:
            print("\n  --- only in GENERATED ---")
            for cls, pos, index, d in sorted(only_gen):
                print(f"    {cls} pos{pos} i{index} dir{d}")
        if only_ref:
            print("\n  --- only in REFERENCE (not yet in Photon?) ---")
            for cls, pos, index, d in sorted(only_ref):
                print(f"    {cls} pos{pos} i{index} dir{d}")
        return 0 if not only_gen else 1

    if args.category:
        def keep(rec):
            return any(args.category in c[3] for c in rec[0])
        gen = Counter({k: v for k, v in gen.items() if keep(k)})
        ref = Counter({k: v for k, v in ref.items() if keep(k)})

    matched = gen & ref
    only_gen = gen - ref
    only_ref = ref - gen
    print(f"airframe {af} — reference: {ref_src}"
          + (f"  (category filter: {args.category})" if args.category else ""))
    print(f"  reference light records : {sum(ref.values())}")
    print(f"  generated light records : {sum(gen.values())}")
    print(f"  matched                 : {sum(matched.values())}")
    print(f"  only in generated (BUGS?) : {sum(only_gen.values())}")
    print(f"  only in reference (unmigrated / mirror-normalized): {sum(only_ref.values())}")
    if only_gen:
        print("\n  --- only in GENERATED (investigate) ---")
        for rec in only_gen:
            print(_fmt_rec(rec))
    if only_ref and args.verbose:
        print("\n  --- only in REFERENCE (not yet migrated, or intentional cleanup) ---")
        for rec in only_ref:
            print(_fmt_rec(rec))
    return 0 if not only_gen else 1


def main():
    ap = argparse.ArgumentParser(description="ToLiss Photon OBJ generator")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="generate the installable tree into dist/")
    b.add_argument("--airframe", choices=list(OBJ_TARGETS))
    b.add_argument("--wing", choices=["stock", "durantula", "realwings"], default="stock",
                   help="wing-mod variant (selects wing mount transforms)")
    b.add_argument("--target", choices=[*TARGETS, "both"], default="both",
                   help="which OBJ(s) to build: exterior (lights_out3xx), "
                        "interior (lights_inn, the cockpit 'Gus Mod' lights), "
                        "or both (default)")
    b.add_argument("--out", help="output dir (default: dist/)")
    b.add_argument("--flat", action="store_true",
                   help="write bare OBJs side by side instead of the X-Plane tree "
                        "(for diffing/inspection; skips staging the plugin)")
    b.add_argument("--write", action="store_true",
                   help="this build goes live: also write each OBJ into the "
                        "installed ToLiss aircraft's objects/ folder in X-Plane "
                        "(auto-located), and with --wing realwings patch the mod")
    b.set_defaults(func=cmd_build)
    c = sub.add_parser("check", help="normalized round-trip check vs reference OBJ")
    c.add_argument("--airframe", choices=list(OBJ_TARGETS))
    c.add_argument("--wing", choices=["stock", "durantula", "realwings"], default="stock",
                   help="wing-mod variant")
    c.add_argument("--target", choices=list(TARGETS), default="exterior",
                   help="which OBJ to check (default: exterior). NOTE there is "
                        "no interior golden until an in-sim pass is accepted and "
                        "frozen, so --target interior errors by design until then "
                        "— the same situation as --airframe a339.")
    c.add_argument("--category", help="only compare lights under this category dataref")
    c.add_argument("-v", "--verbose", action="store_true", help="list only-in-reference too")
    c.add_argument("--against", help="compare against an arbitrary OBJ file instead of the "
                                     "frozen reference/photon golden (e.g. a .scratch/ "
                                     "original) — for authoring an airframe that has no "
                                     "golden yet")
    c.add_argument("--positions-only", action="store_true",
                   help="with --against, ignore class/rgb/intensity/cone and only compare "
                        "(class, pos, index, dir) — for 'did I transcribe every light at "
                        "the right place', independent of Photon's own appearance tuning")
    c.set_defaults(func=cmd_check)
    p = sub.add_parser("patch-realwings",
                       help="patch the installed RealWings mod's baked wingtip "
                            "lights in place (also run by build --wing realwings "
                            "--write)")
    p.add_argument("--airframe", choices=list(OBJ_TARGETS),
                   help="airframe whose RealWings folder to patch (default: all "
                        "installed)")
    p.add_argument("--root", help="RealWings folder (default: auto-locate in the "
                                  "X-Plane install)")
    p.add_argument("--dry-run", action="store_true", help="preview only")
    p.add_argument("--reverse", action="store_true",
                   help="restore originals from .bak")
    p.set_defaults(func=cmd_patch_realwings)
    args = ap.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()
