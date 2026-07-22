#!/usr/bin/env python3
"""Redirect ToLiss's skin-glow LIT-texture regions to Photon's own dataref.

ToLiss's exterior mesh OBJs (on the A339: WingL / Fuselage_Fwd / ExteriorGlass)
carry lines like

    ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[N] <nits>

that make painted "glow" regions — wingtip strobe halos, the tail strobe, the
beacon domes — light up. ToLiss's own plugin animates that array with its
strike-and-fade envelope, and the array is READ-ONLY to other plugins, so Photon
can't make the skin flash in lockstep with its crisp LED-square billboards by
writing it.

The fix is a one-token-per-region string swap: repoint each region Photon drives
at `ToLissPhoton/exterior/glow[N]`, an array the Photon plugin registers and drives
from the same engine value its billboard gets. No OBJ content is added — only the
dataref token changes; the index `N` and the trailing nits are left exactly as
ToLiss wrote them, so X-Plane's OBJ parser sees an identical ATTR_light_level line
(ToLiss already uses the `[index]` subscript form, which proves the parser accepts
an array-element dataref there).

Only the indices Photon actually drives are redirected (REDIRECT below); every
other ExternalLightBrightnesses region — landing, taxi, logo, ... — keeps reading
ToLiss's array untouched. A redirected index Photon did NOT drive would go dark
(our array reads 0 where we never write), so REDIRECT MUST match GlowMapForIcao in
src/native/src/plugin.cpp.

The installer backs up each modified OBJ into "Photon Backup Files/" before
patching and restores it on uninstall, so no `.bak` siblings are written here (the
installer uses backup+restore, not this module's --reverse). --reverse is provided
for the dev/live-tuning path — the swap is exactly invertible (only the dataref
token changed), so it restores ToLiss's original line byte-for-byte.

Dev CLI (patch a live install directly, e.g. for in-sim testing):

    python build/patch_glow.py --airframe a339 --root <aircraft objects dir>
    python build/patch_glow.py --airframe a339 --root <objects dir> --dry-run
    python build/patch_glow.py --airframe a339 --root <objects dir> --reverse
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path

OLD_DATAREF = "AirbusFBW/ExternalLightBrightnesses"
NEW_DATAREF = "ToLissPhoton/exterior/glow"

# airframe -> ExternalLightBrightnesses indices Photon's engine drives (and thus
# redirects). MUST match GlowMapForIcao in src/native/src/plugin.cpp:
#   A339  strobe [12]=L wingtip, [13]=R wingtip, [14]=tail;  beacon [15],[16].
# Airframes absent here are not patched (their skin-glow stays ToLiss's own).
REDIRECT: dict[str, list[int]] = {
    "a339": [12, 13, 14, 15, 16],
}


def indices_for(airframe: str) -> list[int]:
    return REDIRECT.get(airframe, [])


def _atomic_write(path: Path, data: bytes) -> None:
    """Temp-file + os.replace, matching the rest of the toolchain's crash-safe
    writes (a concurrent in-sim reload could be reading this OBJ)."""
    tmp = path.with_name(path.name + ".photontmp")
    try:
        tmp.write_bytes(data)
        os.replace(tmp, path)
    except OSError:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise


def _token_pairs(indices, reverse=False):
    """Yield (search, replace) dataref-token pairs for the given indices. The
    closing `]` is part of each token, so `[1]` can never match inside `[12]`."""
    for n in indices:
        old = f"{OLD_DATAREF}[{n}]"
        new = f"{NEW_DATAREF}[{n}]"
        yield (new, old) if reverse else (old, new)


def target_files(root: Path, airframe: str, reverse: bool = False) -> list[Path]:
    """OBJs under `root` that still need (un)patching for this airframe — i.e.
    that contain the search-side token for any redirected index. Used by the
    installer to know which files to back up before patching (scanning, rather
    than a hard-coded file list, survives ToLiss moving a region between OBJs)."""
    indices = indices_for(airframe)
    if not indices:
        return []
    needles = [s for s, _ in _token_pairs(indices, reverse)]
    hits = []
    for p in sorted(Path(root).glob("*.obj")):
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if any(needle in text for needle in needles):
            hits.append(p)
    return hits


def patch_file(path: Path, airframe: str, dry: bool = False, reverse: bool = False) -> int:
    """Swap the glow datarefs in one OBJ. Returns the number of regions changed.
    Idempotent: re-running finds nothing left to swap (a no-op)."""
    text = path.read_text(encoding="utf-8", errors="replace")
    n = 0
    for search, repl in _token_pairs(indices_for(airframe), reverse):
        c = text.count(search)
        if c:
            text = text.replace(search, repl)
            n += c
    if n and not dry:
        _atomic_write(path, text.encode("utf-8"))
    verb = "would change" if dry else ("restored" if reverse else "redirected")
    print(f"  {'~' if n else '.'} {path.name}: {verb} {n} glow region(s)")
    return n


def run(root, airframe: str, dry: bool = False, reverse: bool = False) -> int:
    """(Un)patch every glow region for `airframe` under `root`. Entry point shared
    by the installer and the CLI. Returns the total number of regions changed."""
    root = Path(root)
    if not root.exists():
        raise SystemExit(f"not found: {root}")
    if not indices_for(airframe):
        print(f"no skin-glow redirect defined for airframe {airframe!r} — nothing to do")
        return 0
    files = target_files(root, airframe, reverse)
    total = sum(patch_file(p, airframe, dry=dry, reverse=reverse) for p in files)
    verb = "would change" if dry else ("restored" if reverse else "redirected")
    print(f"{verb} {total} glow region(s) across {len(files)} file(s)")
    return total


def _main():
    ap = argparse.ArgumentParser(
        description="Redirect ToLiss skin-glow LIT regions to Photon's own dataref.")
    ap.add_argument("--root", required=True, type=Path,
                    help="the aircraft's objects/ directory")
    ap.add_argument("--airframe", required=True, help="airframe key, e.g. a339")
    ap.add_argument("--dry-run", action="store_true", help="report, change nothing")
    ap.add_argument("--reverse", action="store_true",
                    help="restore ToLiss's original ExternalLightBrightnesses token")
    args = ap.parse_args()
    run(args.root, args.airframe, dry=args.dry_run, reverse=args.reverse)


if __name__ == "__main__":
    _main()
