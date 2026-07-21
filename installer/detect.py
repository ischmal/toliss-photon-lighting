"""Detection: X-Plane root candidates, aircraft scan, and Photon install-status
probing. Pure(ish) functions over the filesystem so they can be unit-tested
against a fake tree without a terminal — no TUI imports here. (The native plugin
needs no XPPython3, so there is no runtime-prerequisite check anymore.)
"""
from __future__ import annotations

import os
import re
import string
import subprocess
import sys
from pathlib import Path

from installer.constants import AIRFRAMES, BACKUP_DIRNAME, MANIFEST_NAME

_MARKER_RE = re.compile(r"ToLissPhoton version:\s*(\S+)\s+wing:\s*(\S+)")


# ─── X-Plane root discovery ────────────────────────────────────────────────────
def _valid_root(p: Path) -> bool:
    try:
        return p.is_dir() and (p / "Aircraft").is_dir() and (p / "Resources").is_dir()
    except OSError:
        return False


def _parse_library_vdf(vdf: Path) -> list[Path]:
    """Extract Steam library folder paths out of libraryfolders.vdf — a tiny
    hand parser for the `"path"  "D:\\SteamLibrary"` lines, avoiding a real vdf
    dependency (this installer ships with zero third-party deps)."""
    out = []
    try:
        text = vdf.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return out
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('"path"'):
            parts = line.split('"')
            if len(parts) >= 4:
                out.append(Path(parts[3].replace("\\\\", "\\")))
    return out


def _steam_library_roots() -> list[Path]:
    """Steam install root(s) to search under `steamapps/common/`, including
    every library declared in libraryfolders.vdf (covers non-default Steam
    library drives), not just the default Steam install location."""
    steam_candidates = []
    if os.name == "nt":
        for env in ("ProgramFiles(x86)", "ProgramFiles"):
            base = os.environ.get(env)
            if base:
                steam_candidates.append(Path(base) / "Steam")
    elif sys.platform == "darwin":
        steam_candidates.append(Path.home() / "Library" / "Application Support" / "Steam")
    else:
        steam_candidates.append(Path.home() / ".steam" / "steam")
        steam_candidates.append(Path.home() / ".local" / "share" / "Steam")

    roots = []
    for steam in steam_candidates:
        roots.append(steam)
        vdf = steam / "steamapps" / "libraryfolders.vdf"
        roots.extend(_parse_library_vdf(vdf))
    return roots


def _common_dirs() -> list[Path]:
    """Per-OS common install locations to probe for an `X-Plane 12` folder."""
    dirs: list[Path] = []
    if os.name == "nt":
        for env in ("ProgramFiles", "ProgramFiles(x86)"):
            base = os.environ.get(env)
            if base:
                dirs.append(Path(base))
        for letter in string.ascii_uppercase:
            root = Path(f"{letter}:/")
            if root.is_dir():
                dirs.append(root)
    elif sys.platform == "darwin":
        dirs += [Path("/Applications"), Path.home()]
    else:
        dirs += [Path.home(), Path("/opt"), Path("/usr/local")]
    return dirs


def _running_from_inside_xplane() -> Path | None:
    """installer.md bullet: the installer may be run from inside an existing
    X-Plane install (e.g. Resources/plugins/ToLiss Photon/). Walk this file's
    own parents looking for an X-Plane 12 root."""
    here = Path(sys.argv[0]).resolve() if sys.argv and sys.argv[0] else Path(__file__).resolve()
    for parent in [here, *here.parents]:
        if _valid_root(parent):
            return parent
    return None


def xplane_candidates(log=None) -> list[Path]:
    """Every plausible X-Plane 12 root, de-duped and validated (must contain
    both Aircraft/ and Resources/). Order: $XPLANE_ROOT override, running-from-
    inside-X-Plane, Steam libraries, then common per-OS install directories."""
    found: list[Path] = []

    def consider(p, why):
        if p is None:
            return
        try:
            p = Path(p).resolve()
        except OSError:
            return
        if not _valid_root(p):
            if log:
                log.write(f"candidate rejected ({why}): {p}", "DEBUG")
            return
        if p in found:
            return
        found.append(p)
        if log:
            log.write(f"candidate accepted ({why}): {p}")

    env = os.environ.get("XPLANE_ROOT")
    if env:
        consider(env, "$XPLANE_ROOT")

    consider(_running_from_inside_xplane(), "running from inside X-Plane")

    for steam_root in _steam_library_roots():
        consider(steam_root / "steamapps" / "common" / "X-Plane 12", "steam library")

    for base in _common_dirs():
        for name in ("X-Plane 12", "X-Plane12", "X-Plane 12 Steam"):
            consider(base / name, "common install dir")

    return found


def xplane_running() -> bool:
    """Best-effort process check — OBJ reloads mid-session misbehave (CLAUDE.md
    gotcha #5), so the installer warns rather than writing under a running sim.
    Never raises: any failure to introspect processes is treated as 'not
    running' so the check can't itself block installation."""
    try:
        if os.name == "nt":
            out = subprocess.run(["tasklist"], capture_output=True, text=True, timeout=5)
            return "X-Plane.exe" in out.stdout
        out = subprocess.run(["ps", "-A", "-o", "comm="], capture_output=True,
                             text=True, timeout=5)
        return any("X-Plane" in line for line in out.stdout.splitlines())
    except Exception:
        return False


# ─── aircraft scan ──────────────────────────────────────────────────────────────
def _read_skunk_cfg(folder: Path) -> dict:
    """Parse ToLiss's Skunk Crafts updater config (`skunkcrafts_updater.cfg`), a
    pipe-delimited `key|value` file: `version|v1.12` -> aircraft version,
    `name|ToLiss A319` -> pretty name. Key order varies, so parse the whole file."""
    cfg: dict[str, str] = {}
    path = folder / "skunkcrafts_updater.cfg"
    if not path.exists():
        hit = next((p for p in folder.glob("*.cfg")
                    if p.name.lower() == "skunkcrafts_updater.cfg"), None)
        if hit is None:
            return cfg
        path = hit
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "|" in line:
                k, _, v = line.partition("|")
                cfg[k.strip()] = v.strip()
    except OSError:
        return cfg
    if "version" in cfg:
        cfg["version"] = cfg["version"].lstrip("vV").strip()
    return cfg


def _parse_marker(text: str):
    m = _MARKER_RE.search(text)
    if not m:
        return None, None
    return m.group(1), m.group(2)


def _obj_marker(objects: Path):
    """Read the `# ToLissPhoton version: X wing: Y` marker out of whichever
    lights_out OBJ lives in this objects/ folder (only one airframe's file is
    ever present). Returns (version, wing) or (None, None) if no marker — the
    latter meaning the OBJ is stock, i.e. Photon's edit is not in the file."""
    for _, _, fname, _ in AIRFRAMES.values():
        p = objects / fname
        if not p.is_file():
            continue
        try:
            head = p.read_text(encoding="utf-8", errors="replace")[:2000]
        except OSError:
            continue
        ver, wing = _parse_marker(head)
        if ver:
            return ver, wing
    return None, None


def photon_status(objects: Path):
    """Photon install status for one aircraft's objects/ folder, as
    (version, wing, stale):
      - (None, None, False)  — not installed.
      - (ver, wing, False)   — installed; the lights_out OBJ still carries our
        marker. From the manifest if present, else the OBJ marker itself
        (a manifest-less hand-copy / pre-manifest install).
      - (ver, wing, True)    — STALE: the manifest says we installed `ver`, but
        the lights_out OBJ no longer carries our marker. An aircraft update
        (SkunkCrafts) reverted the OBJ to stock out from under us while leaving
        the manifest untouched in `Photon Backup Files/`. The install needs
        redoing even though the bookkeeping survives.
    The OBJ marker thus *corroborates* the manifest rather than being a mere
    fallback: a surviving manifest alone is not proof the edit is still live."""
    from installer import actions  # local import: actions owns the manifest schema

    manifest = actions.read_manifest(objects)
    obj_ver, obj_wing = _obj_marker(objects)

    if manifest and manifest.get("version"):
        # Manifest is authoritative for version/wing, but the OBJ marker decides
        # whether the edit is actually still in place. Marker gone => stale.
        return manifest["version"], manifest.get("wing"), obj_ver is None

    if obj_ver:
        return obj_ver, obj_wing, False
    return None, None, False


def detect_aircraft(xplane_root, log=None) -> list[dict]:
    """Scan Aircraft/ for every supported ToLiss airframe folder. Each result:
    folder, airframe, ac_ver, name, photon (version or None), wing,
    stale (bool — installed per the manifest but the OBJ marker is gone, e.g. an
    aircraft update reverted our edit), path."""
    root = Path(xplane_root)
    found: list[dict] = []
    ac_dir = root / "Aircraft"
    if not ac_dir.is_dir():
        return found
    for airframe, (glob_pat, _folder, _fname, pretty) in AIRFRAMES.items():
        for folder in sorted(ac_dir.glob(glob_pat)):
            objects = folder / "objects"
            ver, wing, stale = (photon_status(objects) if objects.is_dir()
                                else (None, None, False))
            cfg = _read_skunk_cfg(folder)
            found.append(dict(
                folder=folder.name, airframe=airframe,
                ac_ver=cfg.get("version") or "?",
                name=cfg.get("name") or pretty,
                photon=ver, wing=wing, stale=stale,
                path=str(folder),
            ))
    if log:
        log.write(f"detected {len(found)} ToLiss aircraft under {ac_dir}")
    return found


def any_other_airframe_has_photon(xplane_root, exclude_folder: str) -> bool:
    """Used by the uninstall flow to decide the shared plugin's fate: leave it
    installed unless this is the last airframe with Photon on it."""
    for ac in detect_aircraft(xplane_root):
        if ac["folder"] != exclude_folder and ac["photon"]:
            return True
    return False
