#!/usr/bin/env python3
"""
ToLiss Photon Lighting — .acf cockpit-spot patcher (interior mod).

ToLiss defines three sim 3D cockpit spot lights in the aircraft's `.acf`. Their
COLOR lives there too, which means it can only be changed by editing the file —
it can't follow a runtime switch. So the interior install:

  1. writes Gus's geometry values (variant-independent, so no runtime state),
  2. DISABLES all three spots via `_spot_name_3d/{0,1,2} = none`, and
  3. re-adds them as OBJ lights in lights_inn.obj, where `ANIM_show` can switch
     their color like every other interior light.

Full rationale: docs/interior_plan.md §3.4 / §3.5.

⚠ THE FORMAT TRAP — READ BEFORE EDITING
---------------------------------------
XP12 and XP11 `.acf` files render the SAME values differently:

    property            XP12              XP11
    _spot_the/0         -70.000000000     -70.0
    _spot_angle/0       0.000000000       0.0
    _spot1_3d_xyz/1     1.000000000       1.0

A literal string patch for `_spot_the/0 -60.000000000` works on the two XP12
files and SILENTLY NO-OPS on the two XP11 ones. So this module always parses
numerically and re-renders in each file's own detected style, and detection
compares numerically with a tolerance — never by string or hash.

DETECTION (decision #12) is by PRESENCE OF OUR LINES, never a file hash: the
`.acf` legitimately carries other mods' edits (Durantula rewrites 312 lines of
the `_obja/*` attachment table in the very same file). The sentinel is
`_spot_name_3d/{0,1,2} == "none"` — a STRING value, so it sidesteps the float
formatting problem entirely, it is unambiguous (stock is a dataref path), and it
is the property that actually means "our patch is live". The numeric geometry
properties are corroboration only.

REVERSAL writes back the stock values recorded in STOCK below rather than
diffing, so it composes with whatever else has touched the file since.

Coexistence: we touch only `acf/_spot*`, a namespace disjoint from Durantula's
`_obja/*`, so its edits and ours don't collide. We are NOT safe from its
UNINSTALL, which restores an `a320.acf.durantula.bak` predating our patch and
silently wipes it — the presence check reports that as "needs reinstall". Same
ordering hazard as the existing RealWings interaction: documented, not prevented.
"""
from __future__ import annotations

import re
from pathlib import Path

# property -> {index: value} that we WRITE. Values from Gus's readme (Plane
# Maker steps) and docs/interior_plan.md §3.4.
#   _spot1_3d_xyz/1  raise spot 1 to 4.00 ft on the vertical ("004.00 on vert")
#   _spot_angle/0    50 degrees (from 0)
#   _spot_size/*     8 / 9 / 10
#   _spot_the/0      pitch -60
#   _spot_psi/0      heading 180
PATCH = {
    "_spot1_3d_xyz/1": {1: 4.0},
    "_spot_angle/0": {0: 50.0},
    "_spot_size/0": {0: 8.0},
    "_spot_size/1": {1: 9.0},
    "_spot_size/2": {2: 10.0},
    "_spot_the/0": {0: -60.0},
    "_spot_psi/0": {0: 180.0},
}
# flattened: full property path -> value we write
NUMERIC_PATCH = {
    "acf/_spot1_3d_xyz/1": 4.0,
    "acf/_spot_angle/0": 50.0,
    "acf/_spot_size/0": 8.0,
    "acf/_spot_size/1": 9.0,
    "acf/_spot_size/2": 10.0,
    "acf/_spot_the/0": -60.0,
    "acf/_spot_psi/0": 180.0,
}

# The sentinel: disabling the three spots. A *string* value, immune to the
# float-format trap. The A339 ships `_spot_name_3d/2 none` itself, so this is
# ToLiss's own idiom for "no spot here", not an invention.
DISABLE = {
    "acf/_spot_name_3d/0": "none",
    "acf/_spot_name_3d/1": "none",
    "acf/_spot_name_3d/2": "none",
}

# Stock values, for reversal. Identical across A319/A320/A321 and across all
# four .acf per airframe — verified against the local installs. The only
# per-airframe difference in the whole spot block is `_spot*_3d_xyz/2`
# (longitudinal position: 14.75 / 9.49 / -4.48), which we never touch.
STOCK_NUMERIC = {
    "acf/_spot1_3d_xyz/1": 1.0,
    "acf/_spot_angle/0": 0.0,
    "acf/_spot_size/0": 20.0,
    "acf/_spot_size/1": 28.010000229,
    "acf/_spot_size/2": 38.009998322,
    "acf/_spot_the/0": -70.0,
    "acf/_spot_psi/0": -20.0,
}
STOCK_STRING = {
    "acf/_spot_name_3d/0": "sim/cockpit2/electrical/panel_brightness_ratio[0]",
    "acf/_spot_name_3d/1": "sim/cockpit2/electrical/panel_brightness_ratio[3]",
    "acf/_spot_name_3d/2": "ckpt/lights/map",
}

# A property line: "P <path> <value>" with arbitrary internal whitespace.
_LINE = re.compile(r"^(?P<head>P\s+(?P<prop>\S+)\s+)(?P<val>.*?)(?P<eol>\s*)$")

# A float written the XP12 way: exactly 9 decimal places.
_XP12_FLOAT = re.compile(r"^-?\d+\.\d{9}$")

TOLERANCE = 1e-4


class AcfError(Exception):
    """The .acf could not be parsed or is missing the spot block."""


def detect_style(text: str) -> str:
    """'xp12' (fixed 9-decimal floats) or 'xp11' (shortest round-tripping repr).

    Decided by majority vote over every numeric `acf/` property in the file
    rather than one probe line, because a handful of values (28.010000229)
    happen to need 9 decimals in BOTH styles and would mis-vote on their own."""
    wide = narrow = 0
    for line in text.splitlines():
        m = _LINE.match(line)
        if not m or not m.group("prop").startswith("acf/"):
            continue
        v = m.group("val").strip()
        try:
            float(v)
        except ValueError:
            continue
        if _XP12_FLOAT.match(v):
            wide += 1
        elif "." in v:
            narrow += 1
    return "xp12" if wide > narrow else "xp11"


def render(value: float, style: str) -> str:
    """Render a float the way `style`'s files write it."""
    if style == "xp12":
        return f"{value:.9f}"
    # XP11 writes the shortest representation that round-trips, and always
    # keeps a decimal point (1.0, not 1).
    s = repr(float(value))
    return s if ("." in s or "e" in s or "E" in s) else s + ".0"


def read_props(text: str) -> dict[str, str]:
    """Every `P <prop> <value>` line as {prop: raw value string}."""
    out = {}
    for line in text.splitlines():
        m = _LINE.match(line)
        if m:
            out[m.group("prop")] = m.group("val").strip()
    return out


def _set_props(text: str, values: dict[str, str]) -> tuple[str, int]:
    """Rewrite the value of each named property in place, preserving the line's
    own leading text (so `P acf/_spot_the/0 ` keeps its exact spacing) and its
    line ending. Returns (new text, number of lines changed)."""
    remaining = dict(values)
    out, changed = [], 0
    # splitlines(True) keeps the line terminators, so CRLF files stay CRLF.
    for line in text.splitlines(True):
        stripped = line.rstrip("\r\n")
        eol = line[len(stripped):]
        m = _LINE.match(stripped)
        if m and m.group("prop") in remaining:
            new = m.group("head") + remaining.pop(m.group("prop"))
            if new != stripped:
                changed += 1
            out.append(new + eol)
        else:
            out.append(line)
    if remaining:
        raise AcfError(
            "spot properties not found in this .acf: "
            + ", ".join(sorted(remaining))
            + " — not a ToLiss A3xx .acf, or its format has changed")
    return "".join(out), changed


def is_patched(text: str) -> bool:
    """True if OUR patch is live. Presence-based, per decision #12: the sentinel
    is the three disabled spot names, a string compare immune to the XP11/XP12
    float-format trap. Deliberately NOT a hash — the file legitimately carries
    other mods' edits."""
    props = read_props(text)
    return all(props.get(k, "").strip().lower() == v for k, v in DISABLE.items())


def verify_geometry(text: str) -> list[str]:
    """Corroboration for is_patched(): which numeric geometry properties do NOT
    hold our value. Compared numerically with a tolerance, never as strings.
    An empty list means fully patched; a non-empty one on an otherwise patched
    file means something partially reverted it."""
    props = read_props(text)
    bad = []
    for prop, want in NUMERIC_PATCH.items():
        raw = props.get(prop)
        try:
            got = float(raw)
        except (TypeError, ValueError):
            bad.append(prop)
            continue
        if abs(got - want) > TOLERANCE:
            bad.append(prop)
    return bad


def patch_text(text: str) -> tuple[str, int]:
    """Apply the interior spot patch. Idempotent."""
    style = detect_style(text)
    values = {p: render(v, style) for p, v in NUMERIC_PATCH.items()}
    values.update(DISABLE)
    return _set_props(text, values)


def unpatch_text(text: str) -> tuple[str, int]:
    """Restore ToLiss's stock spot block. Writes the recorded stock values
    rather than diffing, so it composes with other mods' edits to the file."""
    style = detect_style(text)
    values = {p: render(v, style) for p, v in STOCK_NUMERIC.items()}
    values.update(STOCK_STRING)
    return _set_props(text, values)


def acf_files(aircraft_dir: Path) -> list[Path]:
    """The .acf files in an aircraft folder that carry a cockpit spot block.

    Found by CONTENT, not by an assumed `a320{,_StdDef,_XP11,_XP11_StdDef}.acf`
    naming scheme: the folder may have been renamed, and detection-by-content is
    already the principle governing installer/detect.py. In practice this finds
    all four variants per airframe, which is the intent — Photon's OBJ work is
    XP12-only, but lights_inn.obj is shared by every variant, so an XP11 user
    would otherwise get patched OBJ lights alongside unpatched sim spots.

    `.bak` files are excluded: patching a backup would corrupt some other mod's
    uninstall (Durantula keeps an a320.acf.durantula.bak of this very file)."""
    found = []
    for p in sorted(aircraft_dir.glob("*.acf")):
        if p.name.endswith(".bak"):
            continue
        try:
            head = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if "acf/_spot_name_3d/0" in head:
            found.append(p)
    return found


def run(aircraft_dir: Path, *, reverse: bool = False, dry: bool = False):
    """Patch (or revert) every spot-carrying .acf in an aircraft folder."""
    files = acf_files(Path(aircraft_dir))
    if not files:
        print(f"  ! no .acf with a cockpit spot block under {aircraft_dir}")
        return []
    touched = []
    for p in files:
        # newline="" on BOTH sides: real .acf files are CRLF, and Python's
        # default universal-newline translation would silently rewrite every
        # line ending in the file — a gratuitous whole-file diff in a file other
        # mods also read and back up. _set_props preserves each line's own
        # terminator, but only if the read left them intact.
        with p.open("r", encoding="utf-8", errors="replace", newline="") as f:
            text = f.read()
        style = detect_style(text)
        new, changed = (unpatch_text(text) if reverse else patch_text(text))
        state = "revert" if reverse else "patch"
        print(f"  - {p.name} [{style}] {state}: {changed} line(s) changed"
              + (" (dry run)" if dry else ""))
        if not dry and new != text:
            with p.open("w", encoding="utf-8", newline="") as f:
                f.write(new)
        touched.append(p)
    return touched


def main():
    """The dev CLI, reachable as `python build/patch_acf.py <aircraft_dir>` —
    a function rather than an `if __name__` block so the thin wrapper in build/
    can both re-export this module and still run it."""
    import argparse
    ap = argparse.ArgumentParser(description="Patch ToLiss .acf cockpit spots")
    ap.add_argument("aircraft_dir", help="the ToLiss aircraft folder")
    ap.add_argument("--reverse", action="store_true", help="restore stock values")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    run(Path(a.aircraft_dir), reverse=a.reverse, dry=a.dry_run)


if __name__ == "__main__":
    main()
