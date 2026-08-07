"""Apply the RealWings wingtip patch from PRECOMPUTED data.

RealWings replaces the ToLiss wing objects and bakes its own wingtip lights into
MainNEO/SecondaryNEO/Main OBJs, using X-Plane's *sizeable* named lights
(`airplane_nav_left_size` / `_right_size`, `airplane_strobe_size` / `_sp`) with
fixed colors and no profile switching. Those lights sit inside deep, wing-specific
animation chains, so they can't be safely re-homed into `lights_out`. This module
rewrites each baked light *in place* into a ToLiss parametric light gated by the
ToLissPhoton datarefs — same X/Y/Z, so it keeps RealWings' exact position and
animation, but gains Photon's original/LED color switching. The original line is
preserved (commented) for reversibility.

⚠ WHY THIS MODULE HAS NO DSL IMPORT — AND MUST NEVER GROW ONE
--------------------------------------------------------------
It used to. `build/patch_realwings.py` resolved every emitted parameter out of
`lights.style.phdsl` / `lights.layout.phdsl` through `build_objs`' own resolvers,
at INSTALL time, which is why a release had to bundle a copy of the whole DSL
toolchain under `payload/dsl/` and why `actions.py` carried `sys.path` machinery to
reach it. The C++ installer will not embed a DSL parser, so the split moved to the
same place the OBJs' already is: **resolve at bundle time, apply at install time**
(docs/installer_cpp_plan.md §2a).

`build/patch_realwings.py` keeps the resolvers and emits `realwings_patch.json`
into the payload; this module consumes it. The line-rewriting mechanics — marker
comments, the commented-original, the refresh-from-`.bak` staleness fix — stayed
here, because only *where the numbers come from* moved.

⚠ THIS IS NOT A HARD-CODED POSITION TABLE, and adding one here would break the
"all lighting tuning lives in the DSL" tripwire. The JSON is a GENERATED BUILD
ARTIFACT derived from `lights.layout.phdsl` at bundle time, exactly like the OBJs
in `payload/objs/`. It carries a `_generated` header saying so. What the tripwire
forbids is a hand-MAINTAINED table in source; nothing here is one.

The dev tuning loop is unchanged: `build/patch_realwings.py` resolves the same
structure straight from the DSL in memory and hands it to `run()` below, so
`build_objs.py build --wing realwings --write` (and `watch.py`) still pick up a
`.phdsl` edit with no bundle step in between.
"""
from __future__ import annotations

import json
import os
import shutil
from pathlib import Path

# The comment that marks our rewrite. Also the "is this file already patched?"
# sentinel, so it must not change without a matching thought about upgrades.
MARKER = "# >>> Photon RealWings"

# The bundle-time artifact this module reads, staged beside the OBJs by
# build/make_release.py.
PATCH_JSON = "realwings_patch.json"

# The structure version. Bumped when a record grows/loses a field, so an
# installer meeting an older bundle says so instead of KeyError-ing.
SCHEMA = 1


class RealWingsError(Exception):
    """The patch data is missing, unreadable, or a shape this build can't apply."""


# ─── the precomputed data ─────────────────────────────────────────────────────
def load(path) -> dict:
    """Read and shape-check `realwings_patch.json`."""
    p = Path(path)
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except OSError as e:
        raise RealWingsError(f"RealWings patch data not found: {p} ({e})") from e
    except json.JSONDecodeError as e:
        raise RealWingsError(f"RealWings patch data is corrupt: {p} ({e})") from e
    return validate(data, str(p))


def validate(data: dict, where: str = "<memory>") -> dict:
    if not isinstance(data, dict):
        raise RealWingsError(f"RealWings patch data is not an object: {where}")
    schema = data.get("schema")
    if schema != SCHEMA:
        raise RealWingsError(
            f"RealWings patch data is schema {schema!r}, this installer applies "
            f"schema {SCHEMA} ({where}) — the bundle and the installer are out "
            f"of step")
    for key in ("classes", "airframes", "sourceAirframe"):
        if key not in data:
            raise RealWingsError(f"RealWings patch data has no {key!r}: {where}")
    return data


def baked_classes(data: dict) -> list[str]:
    """The baked RealWings light classes this data can rewrite — what
    `realwings_objs` scans for and what `patch_file` matches a LIGHT_PARAM on."""
    return sorted(data["classes"])


def record_for(data: dict, airframe, cls_size: str, wingtip: str, side: str) -> dict:
    """The fully resolved replacement for one baked light.

    `airframe` selects per-airframe offset overrides (a319/a321 `extends` a320)
    and falls back to `sourceAirframe` when unknown — the same rule the DSL-side
    resolver used, so `--airframe ?` behaves as it always did. Appearance does not
    vary with the choice; only the placement correction does."""
    frames = data["airframes"]
    af = airframe if airframe in frames else data["sourceAirframe"]
    try:
        return frames[af][cls_size][wingtip][side]
    except KeyError as e:
        raise RealWingsError(
            f"RealWings patch data has no record for "
            f"{af}/{cls_size}/{wingtip}/{side} (missing {e})") from e


def side_for(data: dict, cls_size: str, x) -> str:
    """Which side a baked light is on. RealWings bakes ONE strobe class for both
    wingtips and distinguishes them only by position, so for those the side is
    recoverable only from the sign of the baked X; the nav classes name their own
    side and pin it."""
    fixed = data["classes"][cls_size].get("side")
    if fixed:
        return fixed
    return "port" if float(x) < 0 else "starboard"


# ─── position correction ──────────────────────────────────────────────────────
def _fmt_pos(v) -> str:
    """Format a corrected coordinate: trim trailing zeros but keep RealWings'
    precision (unlike the DSL's 3-dp `fmt`, which is far too coarse for a
    position)."""
    s = f"{float(v):.7f}".rstrip("0").rstrip(".")
    return s or "0"


def shift_pos(x, y, z, vec):
    """Add an ALREADY-RESOLVED, ALREADY-MIRRORED offset to a baked position.

    Returns x/y/z as strings. A null/zero offset passes them through UNCHANGED —
    the verbatim-position guarantee, which is what keeps an untuned fixture at
    RealWings' exact bytes — and only a moved axis is reformatted.

    ⚠ The side-mirroring and the DSL's nested `@realwings { offset: … }` walk are
    NOT here; they happen at bundle time, so `vec` is already this side's."""
    if not vec or all(float(d) == 0.0 for d in vec):
        return x, y, z
    return tuple(orig if float(d) == 0 else _fmt_pos(float(orig) + float(d))
                 for orig, d in zip((x, y, z), vec))


# ─── line rendering ───────────────────────────────────────────────────────────
def photon_block(data: dict, cls_size: str, rec: dict, side: str,
                 x, y, z, indent: str) -> list[str]:
    """Render one baked light as an original/LED pair of Photon parametric lights
    at RealWings' (corrected) position."""
    x, y, z = shift_pos(x, y, z, rec.get("offset"))
    src_path = data["classes"][cls_size]["path"]
    lines = [f"{indent}{MARKER} — {cls_size} -> {src_path} ({side}); "
             f"edit it in lights.style.phdsl / lights.layout.phdsl"]
    for br in rec["branches"]:
        lp = (f"{indent}\tLIGHT_PARAM {rec['class']} {x} {y} {z} {br['rgb']} "
              f"{rec['index']} {br['intensity']}cd {rec['dir']} {br['cone']}")
        lines += [f"{indent}ANIM_begin",
                  f"{indent}\tANIM_hide {br['hide']} "
                  f"ToLissPhoton/exterior/{rec['category']}",
                  lp,
                  f"{indent}ANIM_end"]
    return lines


# ─── file I/O ─────────────────────────────────────────────────────────────────
def _atomic_write(path: Path, blob: bytes) -> None:
    """Temp-file + os.replace, matching the rest of the toolchain's crash-safe
    writes. The dev path passes `build_objs.atomic_write_bytes` instead (it adds
    the retry loop that the watch.py reload race needs); this plain one is what
    the installer uses, where no in-sim reload is racing the write."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".photontmp")
    try:
        tmp.write_bytes(blob)
        os.replace(tmp, path)
    except OSError:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise


def _read_obj(path: Path) -> str:
    """Read a mod OBJ WITHOUT newline translation.

    ⚠ `newline=""` IS LOAD-BEARING, and its absence made the `nl` detection below
    DEAD CODE. RealWings ships its OBJs CRLF; a plain `read_text()` universal-
    newlines them to LF on the way in, so `"\\r\\n" in text` was never true and every
    patched mod file came back out LF — a whole-file rewrite of somebody else's mod,
    where the intended change is a handful of lines. Nothing breaks (X-Plane parses
    either), which is why it survived: the only visible symptom is that a diff
    against the mod's own `.bak` shows every line changed.

    Found by `tests/test_parity.py` (2026-08-07); the `.acf` patchers already
    followed this rule."""
    return Path(path).read_text(encoding="utf-8", errors="replace", newline="")


def patch_file(path: Path, data: dict, airframe=None, dry: bool = False,
               write=None) -> int:
    """Rewrite every baked wingtip light in one mod OBJ. Returns how many."""
    write = write or _atomic_write
    path = Path(path)
    raw = _read_obj(path)
    # An already-patched file is REFRESHED, not skipped: re-derive from the
    # pristine .bak so DSL changes (offset tuning, palette edits, a new Photon
    # version) reach a mod that was patched under older values. Without this,
    # re-running the patch — dev builds, watch.py, an installer upgrade — left the
    # stale patch in place. The .bak is the refresh source and is NOT re-copied
    # (that would clobber the true original with patched content).
    refreshing = MARKER in raw
    if refreshing:
        bak = path.with_suffix(path.suffix + ".bak")
        if not bak.exists():
            print(f"  = {path.name}: patched but no .bak to refresh from, skipping")
            return 0
        raw = _read_obj(bak)
        if MARKER in raw:
            print(f"  = {path.name}: .bak itself contains a patch (?), skipping")
            return 0
    nl = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.replace("\r\n", "\n").split("\n")
    # RealWings ships CEO and NEO as separate OBJs (Main.obj vs MainNEO.obj); the
    # baked lights carry no marker, so the wingtip variant (which keys the DSL
    # @realwings offset: fence=CEO / sharklet=NEO) is read from the filename.
    wingtip = "sharklet" if "NEO" in path.stem.upper() else "fence"
    known = data["classes"]
    out, n = [], 0
    for line in lines:
        s = line.strip()
        tok = s.split()
        if len(tok) >= 5 and tok[0] == "LIGHT_PARAM" and tok[1] in known:
            indent = line[: len(line) - len(line.lstrip())]
            cls_size, x, y, z = tok[1], tok[2], tok[3], tok[4]
            side = side_for(data, cls_size, x)
            rec = record_for(data, airframe, cls_size, wingtip, side)
            out.append(f"{indent}#{s}")          # keep original, commented
            out.extend(photon_block(data, cls_size, rec, side, x, y, z, indent))
            n += 1
        else:
            out.append(line)
    if n and not dry:
        if not refreshing:  # refresh: .bak already holds the true original
            shutil.copy2(path, path.with_suffix(path.suffix + ".bak"))
        write(path, nl.join(out).encode("utf-8"))
    verb = (("would refresh" if dry else "refreshed (from .bak)") if refreshing
            else ("would patch" if dry else "patched"))
    print(f"  {'~' if n else '.'} {path.name}: {verb} {n} wingtip light(s)")
    return n


def reverse_bak(bak: Path, write=None) -> int:
    """Restore the original `.obj` from its `.obj.bak` sibling."""
    write = write or _atomic_write
    orig = bak.with_suffix("")   # strip the trailing .bak -> foo.obj
    write(orig, bak.read_bytes())
    bak.unlink()
    print(f"  < {orig.name}: restored from .bak")
    return 1


def realwings_objs(root: Path, data: dict):
    """OBJs under `root` that contain a baked RealWings wingtip light."""
    classes = baked_classes(data)
    for p in sorted(Path(root).rglob("*.obj")):
        try:
            head = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if any(c in head for c in classes):
            yield p


def run(root: Path, data: dict, airframe=None, dry: bool = False,
        reverse: bool = False, write=None) -> int:
    """Patch (or reverse) every RealWings OBJ under `root`.

    The entry point shared by the installer (which loads `data` from the bundled
    JSON) and `build/patch_realwings.py` (which resolves it live from the DSL)."""
    root = Path(root)
    if not root.exists():
        raise RealWingsError(f"not found: {root}")

    if reverse:
        baks = sorted(root.rglob("*.obj.bak"))
        if dry:
            for b in baks:
                print(f"  < {b.with_suffix('').name}: would restore from .bak")
            print(f"would restore {len(baks)} file(s)")
            return len(baks)
        total = sum(reverse_bak(b, write) for b in baks)
        print(f"restored {total} file(s)")
        return total

    validate(data)
    objs = list(realwings_objs(root, data))
    if not objs:
        raise RealWingsError(
            "no RealWings OBJs with baked wingtip lights found under root")
    total = sum(patch_file(p, data, airframe=airframe, dry=dry, write=write)
                for p in objs)
    print(f"{'would patch' if dry else 'patched'} {total} light(s) across "
          f"{len(objs)} file(s)")
    return total
