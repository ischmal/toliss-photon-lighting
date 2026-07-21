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

# airframe key -> (aircraft folder in the X-Plane tree, obj filename)
OBJ_PATH = {
    "a319": ("A319", "lights_out319_XP12.obj"),
    "a320": ("A320", "lights_out320_XP12.obj"),
    "a321": ("A321", "lights_out321_XP12.obj"),
}

HEADER = "I\n800\nOBJ\n\nGLOBAL_cockpit_lit\nPOINT_COUNTS\t0\t0\t0\t0\n"


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


def resolve_field(value, profile: str, side: str | None):
    """A field (intensity, cone, ...) may be scalar or nested with profile keys
    (halogen/xenon/led) and/or port/starboard (either order). Walk to a scalar."""
    v = value
    for _ in range(4):
        if isinstance(v, dict):
            if any(p in v for p in PROFILES):
                v = v[profile] if profile in v else next(v[p] for p in PROFILES if p in v)
                continue
            if "port" in v or "starboard" in v:
                v = pick_side(v, side or "port")
                continue
        break
    return v


def swatch_for(cls: str) -> str:
    return "bb" if cls.endswith("_bb") else "sp"


PROFILE_LABEL = {"led": "LED", "halogen": "Halogen", "xenon": "Xenon"}
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
                 annotate: bool = False):
        cfg_wing = cfg.get("wing")
        if cfg_wing is not None and cfg_wing != variant:
            raise SystemExit(
                f"config was loaded for wing variant {cfg_wing!r} but the Emitter "
                f"was built for {variant!r} — the DSL resolves @<variant> "
                f"conditions at load time, so pass load_config(wing={variant!r})")
        self.cfg = cfg
        self.airframe = airframe
        self.variant = variant  # wing-mod variant: stock | durantula | realwings
        self.annotate = annotate  # emit "# @fixture|gi|li|side" tags (bootstrap)
        self.palettes = cfg["palettes"]
        self.categories = cfg["categories"]
        self.types = cfg["lightTypes"]

    def light_line(self, light, *, profile, side, group, fixture, mirror):
        # appearance from the light type; placement (position/dir) from the light
        tpl = self.types[light["type"]] if "type" in light else {}
        merged = {**tpl, **{k: v for k, v in light.items() if k != "type"}}
        cls = merged["class"]

        # position: light > group > fixture
        pos = merged.get("position") or (group or {}).get("position") or fixture.get("position")
        pos = [rnd(p) for p in pos]
        d = [rnd(v) for v in merged["dir"]]
        cone = rnd(resolve_field(merged["cone"], profile, side))
        if mirror and side == "starboard":
            pos = [-pos[0], pos[1], pos[2]]
            d = [-d[0], d[1], d[2]]

        # color is an explicit property of the light type (per-side where it
        # differs); swatch (bb/sp) comes from the class
        cname = pick_side(merged.get("color", "white"), side or "port")
        rgb = [rnd(v) for v in self.palettes[profile][cname][swatch_for(cls)]]

        index = pick_side(merged.get("index", 0), side or "port")
        inten = resolve_field(merged.get("intensity", 0), profile, side)

        nums = pos + rgb + [index, f"{int(round(inten))}cd"] + d + [cone]
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
        cname = pick_side(tcolor, side or "port")
        colorlbl = None if cname == "white" else cname.capitalize()
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

    def emit_branch(self, fixture, category, *, branch, side, mirror, indent="", name=""):
        catdef = self.categories[category]
        profile = catdef["original"] if branch == "original" else "led"
        hide = "1 1" if branch == "original" else "0 0"
        lines = [f"{indent}ANIM_begin",
                 f"{indent}\tANIM_hide {hide} ToLissPhoton/exterior/{category}"]
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
                  for b in ("original", "led")]
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
        parts = [HEADER]
        for name, fixture in af["fixtures"].items():
            if not fixture or ("gating" not in fixture and "raw" not in fixture):
                continue
            if self.variant in fixture.get("gating", {}).get("omit_for", []):
                continue  # e.g. wingtip nav/strobe under RealWings (mod owns them)
            parts.append(self.emit_fixture(name, fixture))
            parts.append("")
        return reindent("\n".join(parts)).replace("\n", "\r\n")


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
        elif head == "LIGHT_PARAM":
            # strip trailing inline comment
            body = line.split("#", 1)[0].split()
            cls = body[1]
            vals = body[2:]
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
            rec = (gate_path, cls,
                   (rnd(x), rnd(y), rnd(z)), (rnd(r), rnd(g), rnd(b)),
                   int(index), int(round(inten)),
                   (rnd(dx), rnd(dy), rnd(dz)), rnd(cone))
            records[rec] += 1
    return records


def get_reference(airframe: str) -> str:
    """The canonical Photon reference is the frozen hand-authored OBJ in
    reference/photon/ — a checked-in fixture, deliberately NOT the build output.

    (Historically this read `git show HEAD:<aircraft>/objects/<obj>`, which
    worked only while the committed OBJs were hand-authored. Now that the build
    generates that tree into dist/, reading it back from git would compare
    generated output against generated output and pass unconditionally.)"""
    _, fname = OBJ_PATH[airframe]
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
}
# airframe -> RealWings mod subfolder under the aircraft's objects/
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
    objects, reason = _find_aircraft_dir(airframe)
    if objects is None:
        return None, reason
    rw_folder = REALWINGS_DIR[airframe]
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
    targets = [args.airframe] if args.airframe else list(OBJ_PATH)
    root = (REPO / args.out) if args.out else DIST
    for af in targets:
        text = Emitter(cfg, af, args.wing).emit()
        folder, fname = OBJ_PATH[af]
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
        targets = [args.airframe] if args.airframe else list(OBJ_PATH)
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
    gen_text = Emitter(cfg, af, args.wing).emit()
    ref_text, ref_src = get_reference(af)
    gen = parse_records(gen_text)
    ref = parse_records(ref_text)

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
    b.add_argument("--airframe", choices=list(OBJ_PATH))
    b.add_argument("--wing", choices=["stock", "durantula", "realwings"], default="stock",
                   help="wing-mod variant (selects wing mount transforms)")
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
    c.add_argument("--airframe", choices=list(OBJ_PATH))
    c.add_argument("--wing", choices=["stock", "durantula", "realwings"], default="stock",
                   help="wing-mod variant")
    c.add_argument("--category", help="only compare lights under this category dataref")
    c.add_argument("-v", "--verbose", action="store_true", help="list only-in-reference too")
    c.set_defaults(func=cmd_check)
    p = sub.add_parser("patch-realwings",
                       help="patch the installed RealWings mod's baked wingtip "
                            "lights in place (also run by build --wing realwings "
                            "--write)")
    p.add_argument("--airframe", choices=list(OBJ_PATH),
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
