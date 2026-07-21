"""Real backup / install / uninstall actions — the only module in the
installer with side effects on the user's filesystem. Every write is guarded
by `dry_run` and lands via a temp-file + atomic os.replace, so a crash or a
permission error can't leave a half-written OBJ in place.
"""
from __future__ import annotations

import contextlib
import datetime as _dt
import io
import json
import os
import shutil
import sys
from pathlib import Path

from installer import payload
from installer.constants import (
    AIRFRAMES, BACKUP_DIRNAME, MANIFEST_NAME, PLUGIN_DIR_REL, PLUGIN_FOLDER,
    REALWINGS_DIR, VERSION,
)


class ActionError(Exception):
    """A real, user-facing failure (permission denied, missing payload, no
    installation found, ...) — distinct from a bug, meant to be shown as-is."""


# ─── manifest ───────────────────────────────────────────────────────────────
def _manifest_path(objects: Path) -> Path:
    return objects / BACKUP_DIRNAME / MANIFEST_NAME


def read_manifest(objects: Path) -> dict | None:
    p = _manifest_path(objects)
    if not p.is_file():
        return None
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def _write_manifest(objects: Path, data: dict, dry_run: bool):
    if dry_run:
        return
    p = _manifest_path(objects)
    p.parent.mkdir(parents=True, exist_ok=True)
    _atomic_write_text(p, json.dumps(data, indent=2))


# ─── atomic writes ────────────────────────────────────────────────────────────
def _atomic_write_bytes(dest: Path, data: bytes):
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".photontmp")
    try:
        tmp.write_bytes(data)
        os.replace(tmp, dest)
    except OSError as e:
        with contextlib.suppress(OSError):
            tmp.unlink()
        raise ActionError(
            f"couldn't write {dest} ({e}) — check X-Plane is closed and you "
            f"have permission to write there (try running as administrator)"
        ) from e


def _atomic_write_text(dest: Path, text: str):
    _atomic_write_bytes(dest, text.encode("utf-8"))


def _inject_marker(text: str, version: str, wing: str) -> str:
    """Insert the `# ToLissPhoton version: X wing: Y` marker comment right
    after the OBJ's POINT_COUNTS header line. `detect.photon_status` reads it
    both as the version source when no manifest is present *and* to corroborate
    a manifest: if the manifest survives but this marker is gone (an aircraft
    update reverted the OBJ), the install reads as stale/needs-reinstall."""
    marker = f"# ToLissPhoton version: {version} wing: {wing}"
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.replace("\r\n", "\n").split("\n")
    for i, line in enumerate(lines):
        if line.startswith("POINT_COUNTS"):
            lines.insert(i + 1, marker)
            break
    else:
        lines.insert(0, marker)
    return nl.join(lines)


# ─── backup ───────────────────────────────────────────────────────────────────
def _backup_once(src: Path, backup_dir: Path, manifest: dict, log, dry_run: bool) -> bool:
    """Copy `src`'s *current* contents into backup_dir the first time only —
    never overwrite an existing backup (that would clobber the true original
    with a Photon file on reinstall/upgrade). Returns True if a new backup was
    recorded this call."""
    rel = src.name
    backed_up = manifest.setdefault("backed_up", [])
    if rel in backed_up:
        log.write(f"backup skipped (already have one): {rel}")
        return False
    dest = backup_dir / rel
    if dest.exists():
        log.write(f"backup already present on disk, recording: {rel}", "WARN")
    elif src.exists():
        log.write(f"backing up {src} -> {dest}")
        if not dry_run:
            try:
                backup_dir.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dest)
            except OSError as e:
                raise ActionError(f"couldn't back up {src} ({e})") from e
    else:
        log.write(f"nothing to back up yet (file not present): {src}", "WARN")
        return False
    backed_up.append(rel)
    return True


# ─── RealWings live patch (delegates to the bundled DSL toolchain copy) ───────
def _import_patch_realwings():
    dsl = payload.dsl_dir()
    if not dsl.is_dir():
        raise ActionError(f"bundled RealWings patch toolchain missing: {dsl}")
    if str(dsl) not in sys.path:
        sys.path.insert(0, str(dsl))
    import patch_realwings as P  # bundled copy — see payload.dsl_dir()
    return P


def _patch_realwings(rw_dir: Path, airframe: str, dry_run: bool, log) -> int:
    P = _import_patch_realwings()
    buf = io.StringIO()
    n = 0
    with contextlib.redirect_stdout(buf):
        try:
            # airframe selects per-airframe RealWings offset overrides in the DSL
            n = P.run(rw_dir, airframe=airframe, dry=dry_run) or 0
        except SystemExit as e:
            log.write(f"realwings patch: {e}", "WARN")
    for line in buf.getvalue().splitlines():
        log.write(f"[realwings] {line}")
    return n


def _patch_realwings_reverse(rw_dir: Path, dry_run: bool, log) -> int:
    P = _import_patch_realwings()
    buf = io.StringIO()
    n = 0
    with contextlib.redirect_stdout(buf):
        try:
            n = P.run(rw_dir, dry=dry_run, reverse=True) or 0
        except SystemExit as e:
            log.write(f"realwings reverse: {e}", "WARN")
    for line in buf.getvalue().splitlines():
        log.write(f"[realwings] {line}")
    return n


# ─── plugin state ────────────────────────────────────────────────────────────
def plugin_is_current(xplane_root) -> bool:
    """True if the native plugin already installed in X-Plane is byte-identical to
    the one this installer would write (every bundled `.xpl` present and matching).
    When so, a (re)install only rewrites the aircraft OBJs — which X-Plane loads
    into memory and releases, so overwriting them mid-session is safe — and nothing
    needs to touch the loaded plugin binary (a locked `.xpl`), the only thing
    genuinely unsafe to replace while running."""
    dest_root = Path(xplane_root) / PLUGIN_DIR_REL
    if not dest_root.is_dir():
        return False
    try:
        for rel, src in payload.plugin_files():
            dest = dest_root / rel
            if not dest.is_file() or dest.read_bytes() != src.read_bytes():
                return False
        return True
    except (OSError, payload.PayloadError):
        return False


# ─── install / uninstall ───────────────────────────────────────────────────────
def install(ac: dict, wing: str, xplane_root: Path, log, dry_run: bool = False) -> list[str]:
    """Install (or reinstall/upgrade) Photon onto one aircraft. Returns a list
    of human-readable step descriptions for the Perform screen."""
    if not payload.available():
        raise ActionError(f"bundled payload not found next to the installer "
                          f"({payload.PAYLOAD_DIR}) — this build is incomplete")

    airframe = ac["airframe"]
    _, _, fname, _ = AIRFRAMES[airframe]
    aircraft_dir = Path(ac["path"])
    objects = aircraft_dir / "objects"
    backup_dir = objects / BACKUP_DIRNAME
    steps: list[str] = []

    manifest = read_manifest(objects) or {"version": None, "wing": None,
                                          "installed_at": None, "backed_up": []}

    target = objects / fname
    if _backup_once(target, backup_dir, manifest, log, dry_run):
        steps.append(f"Backed up original {fname} → {objects.name}\\{BACKUP_DIRNAME}")
    else:
        steps.append(f"Original {fname} already backed up — reused")

    text = _inject_marker(payload.obj_text(wing, airframe), VERSION, wing)
    log.write(f"writing {target} ({len(text)} bytes, dry_run={dry_run})")
    if not dry_run:
        _atomic_write_bytes(target, text.encode("utf-8"))
    steps.append(f"Installed {fname} ({wing} wing variant) → {objects.name}")

    # Capture any prior RealWings patch state *before* clearing it, so a
    # switch away from RealWings can undo the in-place mod patch (below).
    was_realwings = bool(manifest.get("realwings_patched"))
    old_rw_dir = manifest.get("realwings_dir")
    manifest.pop("realwings_dir", None)
    manifest.pop("realwings_patched", None)
    if wing == "realwings":
        rw_dir = objects / REALWINGS_DIR[airframe]
        if rw_dir.is_dir():
            n = _patch_realwings(rw_dir, airframe, dry_run, log)
            manifest["realwings_dir"] = str(rw_dir)
            manifest["realwings_patched"] = True
            steps.append(f"Patched {n} RealWings wingtip light(s) in {rw_dir.name}")
        else:
            steps.append(f"RealWings mod not found at {rw_dir.name} — continuing with "
                         f"base install only")
            log.write(f"realwings dir missing: {rw_dir}", "WARN")
    elif was_realwings and old_rw_dir:
        # Switching from a RealWings install to stock/durantula: the new
        # lights_out OBJ carries its OWN wingtip nav/strobe (those fixtures are
        # only omitted under --wing realwings). If the mod's baked lights stay
        # patched they double up, so reverse the previous in-place patch here —
        # otherwise it also leaks, untracked, past a later uninstall.
        rw_dir = Path(old_rw_dir)
        if rw_dir.is_dir():
            n = _patch_realwings_reverse(rw_dir, dry_run, log)
            steps.append(f"Reversed previous RealWings patch ({n} file(s) restored)")
        else:
            log.write(f"previous realwings dir gone, nothing to reverse: {rw_dir}", "WARN")

    plugin_root = Path(xplane_root) / PLUGIN_DIR_REL
    arches = payload.plugin_arches()
    log.write(f"writing native plugin -> {plugin_root} "
              f"({', '.join(arches)}; dry_run={dry_run})")
    for rel, src in payload.plugin_files():
        if not dry_run:
            _atomic_write_bytes(plugin_root / rel, src.read_bytes())
    steps.append(f"Installed ToLiss Photon plugin [{', '.join(arches)}] → "
                 f"Resources\\plugins\\{PLUGIN_FOLDER}")

    manifest.update(version=VERSION, wing=wing,
                    installed_at=_dt.datetime.now().isoformat(timespec="seconds"))
    _write_manifest(objects, manifest, dry_run)
    steps.append("Wrote install manifest")
    return steps


def _remove_manifest_and_backups(backup_dir: Path, manifest: dict):
    for rel in manifest.get("backed_up", []):
        f = backup_dir / rel
        if f.is_file():
            f.unlink()
    mf = backup_dir / MANIFEST_NAME
    if mf.is_file():
        mf.unlink()
    try:
        backup_dir.rmdir()  # only succeeds if now empty
    except OSError:
        pass


def uninstall(ac: dict, xplane_root: Path, log, dry_run: bool = False,
             remove_plugin: bool = False) -> list[str]:
    """Restore every backed-up file, reverse any RealWings patch, and drop the
    manifest. The shared plugin is left in place unless `remove_plugin`."""
    objects = Path(ac["path"]) / "objects"
    backup_dir = objects / BACKUP_DIRNAME
    manifest = read_manifest(objects)
    if not manifest:
        raise ActionError("no Photon installation found to uninstall (no manifest)")
    steps: list[str] = []

    for rel in manifest.get("backed_up", []):
        src = backup_dir / rel
        dest = objects / rel
        if not src.is_file():
            log.write(f"backup missing, cannot restore: {src}", "WARN")
            steps.append(f"! Backup missing for {rel} — left as-is")
            continue
        log.write(f"restoring {dest} <- {src} (dry_run={dry_run})")
        if not dry_run:
            _atomic_write_bytes(dest, src.read_bytes())
        steps.append(f"Restored original {rel}")

    if manifest.get("realwings_patched"):
        rw_dir = Path(manifest["realwings_dir"])
        if rw_dir.is_dir():
            n = _patch_realwings_reverse(rw_dir, dry_run, log)
            steps.append(f"Reversed RealWings patch ({n} file(s) restored)")
        else:
            log.write(f"realwings dir gone, nothing to reverse: {rw_dir}", "WARN")
            steps.append("RealWings mod folder no longer present — nothing to reverse")

    log.write(f"removing {backup_dir} (dry_run={dry_run})")
    if not dry_run:
        _remove_manifest_and_backups(backup_dir, manifest)
    steps.append(f"Removed {BACKUP_DIRNAME} and manifest")

    if remove_plugin:
        plugin_root = Path(xplane_root) / PLUGIN_DIR_REL
        log.write(f"removing shared plugin {plugin_root} (dry_run={dry_run})")
        if plugin_root.is_dir() and not dry_run:
            shutil.rmtree(plugin_root, ignore_errors=True)
        steps.append("Removed shared ToLiss Photon plugin (no other airframe uses Photon)")

    return steps
