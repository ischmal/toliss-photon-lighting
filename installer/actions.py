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
    AIRFRAMES, BACKUP_DIRNAME, GLOW_AIRFRAMES, INTERIOR_AIRFRAMES,
    INTERIOR_LIVERY_EXTS, INTERIOR_OBJ, INTERIOR_TEXTURES,
    INTERIOR_TEXTURES_ADDED, MANIFEST_NAME,
    PLUGIN_DIR_REL, PLUGIN_FOLDER, REALWINGS_DIR, VERSION,
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


# ─── skin-glow live patch (repoint ToLiss's ExternalLightBrightnesses regions) ──
def _import_patch_glow():
    dsl = payload.dsl_dir()
    if not dsl.is_dir():
        raise ActionError(f"bundled skin-glow patch toolchain missing: {dsl}")
    if str(dsl) not in sys.path:
        sys.path.insert(0, str(dsl))
    import patch_glow as G  # bundled copy — see payload.dsl_dir()
    return G


def _patch_glow(objects: Path, airframe: str, dry_run: bool, log) -> int:
    G = _import_patch_glow()
    buf = io.StringIO()
    n = 0
    with contextlib.redirect_stdout(buf):
        try:
            n = G.run(objects, airframe, dry=dry_run) or 0
        except SystemExit as e:
            log.write(f"glow patch: {e}", "WARN")
    for line in buf.getvalue().splitlines():
        log.write(f"[glow] {line}")
    return n


# ─── .acf cockpit-spot patch (delegates to the bundled DSL toolchain copy) ────
def _import_patch_acf():
    dsl = payload.dsl_dir()
    if not dsl.is_dir():
        raise ActionError(f"bundled .acf patch toolchain missing: {dsl}")
    if str(dsl) not in sys.path:
        sys.path.insert(0, str(dsl))
    import patch_acf as A  # bundled copy — see payload.dsl_dir()
    return A


def _patch_acf(aircraft_dir: Path, reverse: bool, dry_run: bool, log) -> list[str]:
    """Patch (or revert) the cockpit spot block in every .acf of an aircraft.

    Returns the .acf file NAMES touched. No backup is taken and none is wanted:
    the reversal writes ToLiss's recorded stock values back rather than restoring
    a snapshot, so it composes with whatever else has edited the file since
    (Durantula rewrites 312 lines of the very same file). Restoring a whole-file
    backup here would silently undo those other mods."""
    A = _import_patch_acf()
    buf = io.StringIO()
    touched = []
    with contextlib.redirect_stdout(buf):
        try:
            touched = A.run(aircraft_dir, reverse=reverse, dry=dry_run) or []
        except SystemExit as e:
            log.write(f"acf patch: {e}", "WARN")
        except A.AcfError as e:
            log.write(f"acf patch: {e}", "WARN")
    for line in buf.getvalue().splitlines():
        log.write(f"[acf] {line}")
    return [Path(p).name for p in touched]


# ─── interior livery scan ─────────────────────────────────────────────────────
def _interior_livery_overrides(aircraft_dir: Path) -> list[Path]:
    """Livery files that would SHADOW our interior textures.

    X-Plane substitutes a `.dds` for the `.png` an OBJ names, so a livery
    shipping `chairs_LIT.dds` wins over our `chairs_LIT.png` in objects/ and the
    mod is simply invisible in that livery. Dropping our PNG into the livery
    folder does nothing — the fix is to back up and REMOVE the livery's file so
    the one in objects/ resolves. (Converting PNG->DDS would be the alternative,
    but needs an encoder we don't have and won't add for what is typically one
    file across dozens of liveries.)

    Only stems matching our texture set are considered, so a livery's own
    unrelated overrides are never touched."""
    stems = {Path(n).stem for n in INTERIOR_TEXTURES}
    hits = []
    liveries = aircraft_dir / "liveries"
    if not liveries.is_dir():
        return hits
    for livery in sorted(p for p in liveries.iterdir() if p.is_dir()):
        objs = livery / "objects"
        if not objs.is_dir():
            continue
        for f in sorted(objs.iterdir()):
            if f.is_file() and f.stem in stems and f.suffix.lower() in INTERIOR_LIVERY_EXTS:
                hits.append(f)
    return hits


# ─── plugin state ────────────────────────────────────────────────────────────
_CMP_CHUNK = 1 << 16


def _files_identical(a: Path, b: Path) -> bool:
    """Byte-equality that avoids reading file contents whenever it can: a size
    check first (a different plugin build almost always differs in size, so the
    common upgrade case is decided from stat() alone, no file opens), then a
    chunked compare that stops at the first differing block. Unreadable/missing
    files count as 'not identical' — callers treat that as needs-(re)write."""
    try:
        if a.stat().st_size != b.stat().st_size:
            return False
        with a.open("rb") as fa, b.open("rb") as fb:
            while True:
                ca = fa.read(_CMP_CHUNK)
                if ca != fb.read(_CMP_CHUNK):
                    return False
                if not ca:
                    return True
    except OSError:
        return False


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
        return all(_files_identical(dest_root / rel, src)
                   for rel, src in payload.plugin_files())
    except (OSError, payload.PayloadError):
        return False


# ─── install / uninstall ───────────────────────────────────────────────────────
def _install_interior(ac: dict, objects: Path, backup_dir: Path, manifest: dict,
                      log, dry_run: bool, progress=None) -> list[str]:
    """The interior ("Gus Mod") install — an INDEPENDENT, OPT-IN axis.

    Five parts: the OBJ, the texture set, the livery-shadow scan, the four-.acf
    cockpit-spot patch, and its own manifest block. A user who wants only the
    exterior mod never runs any of it, and uninstalling one axis does not
    disturb the other.

    `progress`, if given, is called with (done, total, name) while copying
    textures — ~57 MiB of file copying that otherwise looks like a hang."""
    aircraft_dir = objects.parent
    airframe = ac["airframe"]
    steps: list[str] = []
    interior = manifest.setdefault("interior", {})

    # 1. the OBJ. _inject_marker anchors on POINT_COUNTS, which lights_inn.obj
    #    has, so it needs no special-casing.
    target = objects / INTERIOR_OBJ
    if _backup_once(target, backup_dir, manifest, log, dry_run):
        steps.append(f"Backed up original {INTERIOR_OBJ} → {objects.name}\\{BACKUP_DIRNAME}")
    text = _inject_marker(payload.interior_obj_text(), VERSION, "interior")
    log.write(f"writing {target} ({len(text)} bytes, dry_run={dry_run})")
    if not dry_run:
        _atomic_write_bytes(target, text.encode("utf-8"))
    steps.append(f"Installed {INTERIOR_OBJ} (interior lights, mod by Gus) → {objects.name}")

    # 2. the textures. NINE replace a stock file and are restored from backup on
    #    uninstall; TWO (pedals_details_{1,2}.png) have no stock counterpart, so
    #    they are recorded in `added` and DELETED on uninstall instead —
    #    _backup_once would otherwise just log "nothing to back up" and leave
    #    them orphaned in the aircraft folder forever.
    textures = payload.interior_textures()
    added = interior.setdefault("added", [])
    total = len(textures)
    for i, (name, src) in enumerate(textures, 1):
        dest = objects / name
        if name in INTERIOR_TEXTURES_ADDED:
            if name not in added:
                added.append(name)
        else:
            _backup_once(dest, backup_dir, manifest, log, dry_run)
        if progress:
            progress(i, total, name)
        if not dry_run:
            _atomic_write_bytes(dest, src.read_bytes())
    steps.append(f"Installed {total} interior texture(s) → {objects.name}")

    # 3. livery scan. A livery's own .dds shadows our .png (X-Plane substitutes
    #    .dds for the .png an OBJ names), so back it up and remove it.
    liveries_patched = interior.setdefault("liveries_patched", [])
    overrides = _interior_livery_overrides(aircraft_dir)
    for f in overrides:
        rel = f.relative_to(aircraft_dir).as_posix()
        if rel in liveries_patched:
            continue
        dest = backup_dir / "liveries" / rel.replace("/", "__")
        log.write(f"livery override shadows our texture, removing: {f}")
        if not dry_run:
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dest)
            f.unlink()
        liveries_patched.append(rel)
    if overrides:
        steps.append(f"Cleared {len(overrides)} livery texture override(s) that would "
                     f"have hidden the interior mod")
    # Liveries installed LATER aren't covered by this pass — surfaced as a hint
    # rather than pretending the coverage is permanent.
    steps.append("Note: re-run the installer after adding liveries to cover them too")

    # 4. the .acf cockpit spots, all four files per airframe.
    touched = _patch_acf(aircraft_dir, reverse=False, dry_run=dry_run, log=log)
    if touched:
        interior["acf_patched"] = touched
        steps.append(f"Patched cockpit spot lights in {len(touched)} .acf file(s): "
                     + ", ".join(touched))
    else:
        interior["acf_patched"] = []
        steps.append("! No .acf cockpit spot block found — sim spots left as they were")

    interior.update(installed=True, version=VERSION, airframe=airframe)
    return steps


def install(ac: dict, wing: str, xplane_root: Path, log, dry_run: bool = False,
            interior: bool = False, progress=None) -> list[str]:
    """Install (or reinstall/upgrade) Photon onto one aircraft. Returns a list
    of human-readable step descriptions for the Perform screen.

    `interior` opts into the cockpit-lighting mod, a separate axis from the
    exterior one (docs/interior_plan.md §5). It is False by default so every
    existing caller keeps the exterior-only behaviour."""
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

    # Install the shared plugin FIRST. Its `.xpl` is the one artifact X-Plane
    # locks while loaded — Windows keeps a loaded DLL/`.xpl` memory-mapped and
    # refuses to overwrite it — so on a running sim this is the step that fails.
    # Doing it before we touch lights_out means a locked-plugin failure aborts
    # *before* the version marker is injected into the OBJ. Otherwise a
    # marked-but-orphaned OBJ (marker written, plugin never copied, manifest
    # never reached) reads back as a completed install on the next run — the
    # "installer thinks it succeeded even though it failed" bug.
    plugin_root = Path(xplane_root) / PLUGIN_DIR_REL
    arches = payload.plugin_arches()
    # Only (over)write a plugin `.xpl` that isn't already byte-identical. X-Plane
    # keeps a loaded `.xpl` memory-mapped and locked, so re-replacing an unchanged
    # one fails with "file in use" on a running sim for zero benefit — and skipping
    # it is precisely what lets an OBJ-only reinstall/upgrade run while X-Plane is
    # open (screen_check_not_running trusts this same plugin_is_current fact to
    # decide it can skip the "please close X-Plane" wait).
    wrote_plugin = False
    for rel, src in payload.plugin_files():
        dest = plugin_root / rel
        if _files_identical(dest, src):
            continue  # identical — leave the (possibly locked) file untouched
        wrote_plugin = True
        if not dry_run:
            _atomic_write_bytes(dest, src.read_bytes())
    if wrote_plugin:
        log.write(f"wrote native plugin -> {plugin_root} "
                  f"({', '.join(arches)}; dry_run={dry_run})")
        steps.append(f"Installed ToLiss Photon plugin [{', '.join(arches)}] → "
                     f"Resources\\plugins\\{PLUGIN_FOLDER}")
    else:
        log.write(f"native plugin already current at {plugin_root} — not rewriting "
                  f"(avoids locking a loaded .xpl)")
        steps.append(f"ToLiss Photon plugin already current [{', '.join(arches)}] — kept")

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

    # Redirect ToLiss's skin-glow LIT-texture regions to Photon's own dataref so the
    # painted skin flashes in lockstep with the LED billboards (A339 only for now —
    # see GLOW_AIRFRAMES / patch_glow.REDIRECT / plugin.cpp GlowMapForIcao). Each
    # affected mesh OBJ is backed up once before patching; uninstall restores it from
    # that backup via the existing backed_up list, so there is no extra manifest
    # bookkeeping or .bak file. Gated on GLOW_AIRFRAMES so airframes that don't need
    # it never even load the patch toolchain (matching the RealWings block).
    if airframe in GLOW_AIRFRAMES:
        glow = _import_patch_glow()
        for f in glow.target_files(objects, airframe):
            if _backup_once(f, backup_dir, manifest, log, dry_run):
                steps.append(f"Backed up original {f.name} → {objects.name}\\{BACKUP_DIRNAME}")
        n = _patch_glow(objects, airframe, dry_run, log)
        if n:
            steps.append(f"Redirected {n} skin-glow region(s) to Photon dataref")

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

    # Interior — opt-in and independent of everything above. Gated on
    # INTERIOR_AIRFRAMES so the A330-900 can never pick it up (decision #14):
    # different cockpit model, different rheostat indices, and Gus's mod does not
    # cover it.
    if interior:
        if airframe not in INTERIOR_AIRFRAMES:
            log.write(f"interior mod not available for {airframe}", "WARN")
            steps.append(f"Interior lighting not available for this airframe — skipped")
        else:
            steps += _install_interior(ac, objects, backup_dir, manifest, log,
                                       dry_run, progress)

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

    # ── interior ──────────────────────────────────────────────────────────────
    # The OBJ and the nine stock-replacing textures are already restored by the
    # backed_up loop above. What remains is what a backup can't express.
    interior = manifest.get("interior") or {}
    if interior.get("installed"):
        aircraft_dir = objects.parent

        # 1. Files we ADDED, which never had a stock original to restore. These
        #    must be deleted or they linger in the aircraft folder forever.
        for name in interior.get("added", []):
            f = objects / name
            if f.is_file():
                log.write(f"removing added interior file {f} (dry_run={dry_run})")
                if not dry_run:
                    with contextlib.suppress(OSError):
                        f.unlink()
                steps.append(f"Removed added file {name}")

        # 2. Livery overrides we backed up and removed so our PNG could resolve.
        for rel in interior.get("liveries_patched", []):
            src = backup_dir / "liveries" / rel.replace("/", "__")
            dest = aircraft_dir / rel
            if not src.is_file():
                log.write(f"livery backup missing, cannot restore: {src}", "WARN")
                steps.append(f"! Livery backup missing for {rel} — left as-is")
                continue
            log.write(f"restoring livery override {dest} <- {src} (dry_run={dry_run})")
            if not dry_run:
                _atomic_write_bytes(dest, src.read_bytes())
                with contextlib.suppress(OSError):
                    src.unlink()
            steps.append(f"Restored livery override {rel}")

        # 3. The .acf spot patch, reversed by writing ToLiss's stock values back
        #    rather than restoring a snapshot — so other mods' edits to the same
        #    file (Durantula rewrites 312 lines of it) survive.
        if interior.get("acf_patched"):
            touched = _patch_acf(aircraft_dir, reverse=True, dry_run=dry_run, log=log)
            steps.append(f"Restored stock cockpit spot lights in {len(touched)} .acf file(s)")

        with contextlib.suppress(OSError):
            if not dry_run:
                (backup_dir / "liveries").rmdir()   # only if now empty

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
