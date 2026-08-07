#!/usr/bin/env python3
"""
ToLiss Photon Lighting — .acf attachment patcher for the screen-glow OBJ.

EXPERIMENTAL (docs/screens_plan.md). `lights_screens.obj` is an OBJ ToLiss does
not ship and does not reference, so nothing draws it until it is added to the
aircraft's `_obja/*` attachment table. That is precisely what makes the whole
feature optional: an install that never runs this patcher carries no trace of it,
and `--reverse` removes every line again.

    python build/patch_acf_screens.py <aircraft_dir> [--reverse] [--dry-run]

WHY THE ATTACHMENT IS COPIED, NOT WRITTEN
-----------------------------------------
An attachment row carries the OBJ's origin in FEET against a per-airframe datum
(`_v10_att_{x,y,z}_acf_prt_ref`), and it differs per airframe — 26.00 / 20.75 /
6.78 ft on the A319 / A320 / A321. Our light coordinates are authored in
`lights_inn.obj`'s frame, so the ONLY correct attachment is a copy of that OBJ's
own row with the filename swapped. Hardcoding a datum would silently mis-place
every screen light on two airframes out of three. If `lights_inn.obj` is not in
the table we refuse rather than guess.

⚠ THIS TOUCHES `_obja/*`, WHICH IS DURANTULA'S NAMESPACE.
The cockpit-spot patcher (build/patch_acf.py) deliberately stays inside
`acf/_spot*` so it cannot collide with Durantula's 312-line rewrite of the
attachment table. This one cannot: attaching an OBJ *is* an `_obja` edit. So:

  * Detection is BY PRESENCE OF OUR FILENAME in a `_v10_att_file_stl` slot,
    never by index and never by a hash — the table legitimately carries other
    mods' edits, and our row's index is whatever was free at the time.
  * If Durantula's uninstall restores a pre-Photon `.acf.durantula.bak`, our row
    vanishes with it. `is_attached()` then reports False, which is the truth:
    re-run the patcher. Same documented hazard as the spot patch, not a new one.

⚠ THE XP11/XP12 FORMAT TRAP applies here exactly as it does in patch_acf.py:
XP12 renders floats with 9 decimals (`20.750000000`), XP11 with the shortest
round-tripping form (`20.75`). Values are copied from the template row verbatim
as STRINGS, so they are already in the host file's own style and no re-rendering
is needed — but `_obja/count` is an integer we compute, so it is written bare.

⚠ Read and write with `newline=""` on both sides. These files are CRLF on disk
and Python's default universal-newline handling would rewrite every line ending
in a file other mods also back up. `errors="surrogateescape"` (rather than
patch_acf.py's older `errors="replace"`) makes the round-trip byte-exact for any
input at all; one stock A321 `.acf` does carry a stray NUL.
"""
from __future__ import annotations

import re
from pathlib import Path

# The OBJ we attach, and the one whose attachment frame it must share.
SCREENS_OBJ = "lights_screens.obj"
FRAME_TEMPLATE_OBJ = "lights_inn.obj"

# A property line: "P <path> <value>" with arbitrary internal whitespace.
_LINE = re.compile(r"^(?P<head>P\s+(?P<prop>\S+)\s+)(?P<val>.*?)(?P<eol>\s*)$")
# An indexed attachment property: _obja/<n>/<field>
_OBJA = re.compile(r"^_obja/(?P<idx>\d+)/(?P<field>\S+)$")

COUNT_PROP = "_obja/count"
FILE_FIELD = "_v10_att_file_stl"

_ENC = {"encoding": "utf-8", "errors": "surrogateescape", "newline": ""}


class AcfError(Exception):
    """The .acf could not be parsed, or has no attachment table."""


def read_props(text: str) -> dict[str, str]:
    """Every `P <prop> <value>` line as {prop: raw value string}."""
    out = {}
    for line in text.splitlines():
        m = _LINE.match(line)
        if m:
            out[m.group("prop")] = m.group("val").strip()
    return out


def attachment_rows(text: str) -> dict[int, dict[str, str]]:
    """The `_obja/*` table as {index: {field: raw value}}."""
    rows: dict[int, dict[str, str]] = {}
    for prop, val in read_props(text).items():
        m = _OBJA.match(prop)
        if m:
            rows.setdefault(int(m.group("idx")), {})[m.group("field")] = val
    return rows


def find_obj(text: str, filename: str) -> int | None:
    """Index of the row attaching `filename`, or None. Detection is by filename
    because our row's index is only ever "whatever was free"."""
    for idx, fields in sorted(attachment_rows(text).items()):
        if fields.get(FILE_FIELD, "").strip() == filename:
            return idx
    return None


def declared_count(text: str) -> int:
    """`_obja/count` — X-Plane reads rows 0..count-1, so appending a row without
    bumping this attaches nothing at all."""
    raw = read_props(text).get(COUNT_PROP)
    if raw is None:
        raise AcfError(f"no {COUNT_PROP} in this .acf — not a ToLiss A3xx .acf, "
                       "or its format has changed")
    try:
        return int(float(raw))
    except ValueError as exc:
        raise AcfError(f"{COUNT_PROP} is not a number: {raw!r}") from exc


def is_attached(text: str) -> bool:
    """True if OUR OBJ is in the attachment table AND inside the declared count.

    Both halves matter: a row at index >= count is present in the file but never
    read, which looks installed to a naive grep and draws nothing in the sim."""
    idx = find_obj(text, SCREENS_OBJ)
    return idx is not None and idx < declared_count(text)


def _row_lines(text: str, idx: int) -> list[str]:
    """The raw lines making up attachment row `idx`, in file order."""
    want = f"_obja/{idx}/"
    out = []
    for line in text.splitlines():
        m = _LINE.match(line.rstrip("\r\n"))
        if m and m.group("prop").startswith(want):
            out.append(line)
    return out


def patch_text(text: str) -> tuple[str, int]:
    """Attach SCREENS_OBJ, copying FRAME_TEMPLATE_OBJ's row. Idempotent."""
    if is_attached(text):
        return text, 0

    tmpl_idx = find_obj(text, FRAME_TEMPLATE_OBJ)
    if tmpl_idx is None:
        raise AcfError(
            f"no attachment row for {FRAME_TEMPLATE_OBJ} in this .acf, so there "
            f"is no known-good frame to copy. {SCREENS_OBJ}'s light coordinates "
            f"are authored in that OBJ's frame; attaching at a guessed datum "
            f"would mis-place every screen light. Refusing.")

    rows = attachment_rows(text)
    count = declared_count(text)
    # Append past BOTH the declared count and the highest index actually present,
    # so a table that already has rows beyond the count (another mod mid-install,
    # or a stale leftover) cannot make us overwrite one.
    new_idx = max(count, max(rows) + 1)

    # Copy the template's fields verbatim — values keep the host file's own float
    # style, which is what sidesteps the XP11/XP12 rendering trap entirely.
    template = rows[tmpl_idx]
    block = []
    for field in sorted(template):
        value = SCREENS_OBJ if field == FILE_FIELD else template[field]
        block.append(f"P _obja/{new_idx}/{field} {value}")

    out, inserted, changed = [], False, 0
    lines = text.splitlines(True)
    # Insert directly after the LAST indexed _obja line, keeping the table
    # contiguous in the file. `_obja/count` sits after the rows, so anchoring on
    # the rows (not on count) puts our block in the right place either way.
    last_row_line = -1
    for i, line in enumerate(lines):
        m = _LINE.match(line.rstrip("\r\n"))
        if m and _OBJA.match(m.group("prop")):
            last_row_line = i

    for i, line in enumerate(lines):
        stripped = line.rstrip("\r\n")
        eol = line[len(stripped):] or "\r\n"
        m = _LINE.match(stripped)
        if m and m.group("prop") == COUNT_PROP:
            new = m.group("head") + str(new_idx + 1)
            if new != stripped:
                changed += 1
            out.append(new + eol)
            continue
        out.append(line)
        if i == last_row_line:
            out.extend(b + eol for b in block)
            inserted = True
            changed += len(block)

    if not inserted:
        raise AcfError("no _obja rows found to append after")
    return "".join(out), changed


def unpatch_text(text: str) -> tuple[str, int]:
    """Detach SCREENS_OBJ and close the gap it leaves. Idempotent.

    Rows above ours are RENUMBERED DOWN by one rather than left with a hole:
    X-Plane reads indices 0..count-1, so a missing index in that span would drop
    whatever the table says it is — silently unloading some other mod's OBJ. In
    the normal case ours is the last row and nothing is renumbered."""
    idx = find_obj(text, SCREENS_OBJ)
    if idx is None:
        return text, 0
    try:
        count = declared_count(text)
    except AcfError:
        count = max(attachment_rows(text)) + 1

    out, changed = [], 0
    for line in text.splitlines(True):
        stripped = line.rstrip("\r\n")
        eol = line[len(stripped):] or "\r\n"
        m = _LINE.match(stripped)
        if not m:
            out.append(line)
            continue
        prop = m.group("prop")
        if prop == COUNT_PROP:
            new = m.group("head") + str(max(0, count - 1))
            if new != stripped:
                changed += 1
            out.append(new + eol)
            continue
        om = _OBJA.match(prop)
        if not om:
            out.append(line)
            continue
        row = int(om.group("idx"))
        if row == idx:
            changed += 1
            continue                      # drop our row entirely
        if row > idx:                     # close the gap
            out.append(f"P _obja/{row - 1}/{om.group('field')} "
                       f"{m.group('val').strip()}" + eol)
            changed += 1
            continue
        out.append(line)
    return "".join(out), changed


def acf_files(aircraft_dir: Path) -> list[Path]:
    """The .acf files in an aircraft folder that carry an attachment table.

    By CONTENT, not by an assumed `a320{,_StdDef,_XP11,_XP11_StdDef}.acf` naming
    scheme — the folder may have been renamed, and detection-by-content is the
    principle already governing installer/detect.py. `.bak` files are excluded:
    patching a backup would corrupt some other mod's uninstall (Durantula keeps
    an a320.acf.durantula.bak of this very file)."""
    found = []
    for p in sorted(Path(aircraft_dir).glob("*.acf")):
        if p.name.endswith(".bak"):
            continue
        try:
            with p.open("r", **_ENC) as f:
                text = f.read()
        except OSError:
            continue
        if COUNT_PROP in text:
            found.append(p)
    return found


def run(aircraft_dir: Path, *, reverse: bool = False, dry: bool = False):
    """Attach (or detach) the screen-glow OBJ in every .acf of an aircraft."""
    files = acf_files(Path(aircraft_dir))
    if not files:
        print(f"  ! no .acf with an attachment table under {aircraft_dir}")
        return []
    touched = []
    for p in files:
        with p.open("r", **_ENC) as f:
            text = f.read()
        state = "detach" if reverse else "attach"
        # PER FILE, not per run. A ToLiss folder holds four .acf variants and they
        # are not identical — the XP11 pair uses a different field set — so one
        # that cannot be patched must not abort the other three. The refusal is
        # reported and that file is left exactly as it was.
        try:
            new, changed = (unpatch_text(text) if reverse else patch_text(text))
        except AcfError as e:
            print(f"  ! {p.name} {state} refused: {e}")
            continue
        note = "" if changed else " (already done)"
        print(f"  - {p.name} {state}: {changed} line(s) changed{note}"
              + (" [dry run]" if dry else ""))
        if not dry and new != text:
            with p.open("w", **_ENC) as f:
                f.write(new)
        touched.append(p)
    return touched


def main():
    """The dev CLI (`python build/patch_acf_screens.py <aircraft_dir>`) — see
    patch_acf.main for why this is a function rather than an `if __name__` block."""
    import argparse
    ap = argparse.ArgumentParser(
        description="Attach/detach lights_screens.obj in a ToLiss .acf "
                    "(EXPERIMENTAL screen glow)")
    ap.add_argument("aircraft_dir", help="the ToLiss aircraft folder")
    ap.add_argument("--reverse", action="store_true",
                    help="detach the OBJ and restore _obja/count")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    run(Path(a.aircraft_dir), reverse=a.reverse, dry=a.dry_run)


if __name__ == "__main__":
    main()
