"""Locate the installable artifacts — the light OBJs, the textures, the plugin,
and the precomputed RealWings patch data.

**ONE MODE: bundled.** Read the pre-built `payload/` directory that sits next to
`install.py`, resolved relative to this file — never CWD, so the installer runs
from any directory (Downloads, a USB stick, or inside
X-Plane/Resources/plugins/). A frozen **loose** snapshot instead reads a `data/`
folder next to the executable (same layout) — see `_default_payload_dir`.

⚠ THE DEV MODE IS GONE, DELIBERATELY (docs/installer_cpp_plan.md §2b). Running
`install.py` from a repo checkout with no `payload/` used to fall back to the
repo's own build system and generate each OBJ on the fly via `build_objs`. That
made the DSL toolchain an INSTALL-TIME dependency, which a compiled installer
cannot honor, and it meant the two paths could drift. The dev path is now an
explicit staging step instead:

    python build/make_release.py --payload-only     # stage payload/ beside install.py
    python install.py

Bundled `payload/` layout (built by build/make_release.py):
    payload/objs/<stock|durantula|realwings>/<obj filename>
    payload/objs/interior/lights_inn.obj
    payload/objs/screens/lights_screens.obj
    payload/textures/interior/*.png                       (Gus's set, ~57 MiB)
    payload/plugin/ToLissPhoton/<arch>/ToLissPhoton.xpl   (arch = win_x64|mac_x64|lin_x64)
    payload/plugindata/{panelfx.txt,overlays/*.png}       (into the plugin folder)
    payload/realwings_patch.json                          (resolved from the DSL)

⚠ There is NO `payload/dsl/` any more. The RealWings live patch was the last
install-time DSL consumer; its numbers are now resolved into
`realwings_patch.json` at bundle time and applied by `installer/realwings.py`.

The plugin is the compiled native `.xpl` (no XPPython3/Python at runtime). It is a
fat plugin: the folder can carry one `.xpl` per platform side by side, and X-Plane
loads the subfolder matching the host OS.
"""
from __future__ import annotations

import sys
from pathlib import Path

from installer import realwings
from installer.constants import (
    AIRFRAMES, INTERIOR_OBJ, INTERIOR_TEXTURES, PANELFX_FILE, PLUGIN_DATA_DIRNAME,
    PLUGIN_FOLDER, PLUGIN_USER_DIRS, SCREENS_OBJ, WINGS_FOR, XPL_NAME,
)

# installer/payload.py -> installer/ -> the dir holding install.py. In a release
# that dir holds payload/; in the repo it *is* the repo root (holds build/, src/).
ROOT = Path(__file__).resolve().parent.parent


def _default_payload_dir() -> Path:
    """Where the installable artifacts live by default.

    A frozen **loose** snapshot (`build/make_exe.py --loose`) ships the installer
    and its files as separate items: `<installer>.exe` beside a plainly-named
    `data/` folder. When such a folder sits next to the executable it wins, so the
    exe reads the real files on disk (the tester can see/swap them). Absent that,
    fall back to `ROOT/payload`: for a frozen **self-contained** exe that is the
    copy PyInstaller embedded under its extraction dir; running from source (or
    under tests, which repoint PAYLOAD_DIR at a fixture) it is the repo/release
    root. `getattr(sys, "frozen", ...)` is only ever True in a PyInstaller build,
    so source and test runs always take the `ROOT/payload` branch unchanged."""
    if getattr(sys, "frozen", False):
        loose = Path(sys.executable).resolve().parent / "data"
        if loose.is_dir():
            return loose
    return ROOT / "payload"


PAYLOAD_DIR = _default_payload_dir()


class PayloadError(Exception):
    """The artifacts couldn't be located or built — a broken/incomplete release
    or a repo checkout missing its build inputs, not a user-fixable condition."""


def _read_obj(path: Path) -> str:
    """Read a bundled OBJ WITHOUT newline translation.

    ⚠ `newline=""` IS LOAD-BEARING. `build_objs.Emitter.emit()` ends with
    `.replace("\\n", "\\r\\n")` — X-Plane's OBJs are CRLF and ours match — so a
    plain `read_text()` universal-newlines the whole file to LF on the way in, and
    the installer then writes an LF copy of a CRLF artifact. Nothing breaks (the
    parser takes either), which is exactly why it went unnoticed: the installed
    file simply stopped being byte-identical to the payload, so any byte-level
    comparison against it — a diff, a checksum, the C++/Python parity harness —
    reported a whole-file difference with no cause visible in either tree.

    Found by `tests/test_parity.py` (2026-08-07); same rule the `.acf` patchers
    already follow for the same reason."""
    return path.read_text(encoding="utf-8", errors="replace", newline="")


# ─── availability ──────────────────────────────────────────────────────────────
def _bundled() -> bool:
    return PAYLOAD_DIR.is_dir()


def available() -> bool:
    return _bundled()


def mode() -> str:
    return "bundled" if _bundled() else "unavailable"


def _require_bundle() -> None:
    """The one place that explains a missing bundle, including the dev recipe —
    since the fix for a repo checkout is no longer "it builds itself"."""
    if _bundled():
        return
    raise PayloadError(
        f"no bundled payload at {PAYLOAD_DIR} — this build is incomplete. "
        f"From a repo checkout, stage one first: "
        f"python build/make_release.py --payload-only")


# ─── OBJs ──────────────────────────────────────────────────────────────────────
def obj_path(wing: str, airframe: str) -> Path:
    """Path to a pre-built OBJ in the bundle."""
    _, _, fname, _ = AIRFRAMES[airframe]
    p = PAYLOAD_DIR / "objs" / wing / fname
    if not p.is_file():
        raise PayloadError(f"missing bundled OBJ: {p}")
    return p


def obj_text(wing: str, airframe: str) -> str:
    """The OBJ text to install, read from the bundle.

    Checked against WINGS_FOR first even though a missing file would already fail
    in `obj_path`: the message "a339 has no 'durantula' variant" is the true
    reason, where "missing bundled OBJ" reads as a broken download. `make_release`
    applies the same gate when building the payload (SUPPORTED_WINGS), so the two
    ends agree on which combinations exist at all."""
    if wing not in WINGS_FOR[airframe]:
        raise PayloadError(f"{airframe} has no {wing!r} wing-mod variant "
                           f"(supported: {', '.join(WINGS_FOR[airframe])})")
    _require_bundle()
    return _read_obj(obj_path(wing, airframe))


# ─── interior OBJ + textures ("Gus Mod") ───────────────────────────────────────
# The interior artifact is ONE file for everything: no wing variants (wing mods
# don't touch the cockpit) and no per-airframe variants (stock lights_inn.obj is
# byte-identical across A319/A320/A321). Hence no arguments — a per-(wing,
# airframe) signature like obj_text's would imply a choice that doesn't exist,
# and in the bundle all three airframes would resolve to the same path anyway.
def interior_obj_path() -> Path:
    p = PAYLOAD_DIR / "objs" / "interior" / INTERIOR_OBJ
    if not p.is_file():
        raise PayloadError(f"missing bundled interior OBJ: {p}")
    return p


def interior_obj_text() -> str:
    """The interior OBJ text to install. Built from the a320 fixtures, which
    a319/a321 inherit verbatim (a339 has no interior target at all)."""
    _require_bundle()
    return _read_obj(interior_obj_path())


# ─── screen-glow OBJ ───────────────────────────────────────────────────────────
# Same "one file for everything" shape as the interior OBJ, and for the same
# reasons: no wing variants (wing mods don't touch the cockpit) and no per-airframe
# variants (the A3xx flight decks are the same model, so the six display positions
# are identical). Hence no arguments. a339 has no `screens` target at all.
def screens_obj_path() -> Path:
    p = PAYLOAD_DIR / "objs" / "screens" / SCREENS_OBJ
    if not p.is_file():
        raise PayloadError(f"missing bundled screen-glow OBJ: {p}")
    return p


def screens_obj_text() -> str:
    """The screen-glow OBJ text to install.

    ⚠ It must never be a `--debug` build. That variant is a tuning artifact bound
    to `ToLissPhoton/debug/light/*`, is NOT installable, and would leave six lights
    ignoring the DU knobs. `make_release.build_screens_payload` emits it straight
    from the Emitter for exactly this reason, rather than copying `dist/` — which
    on a dev machine may well hold a debug build."""
    _require_bundle()
    return _read_obj(screens_obj_path())


def screens_available() -> bool:
    """Whether this build can install the screen glow at all. Unlike the interior
    this is not a user opt-in, so a False here just drops the step rather than
    hiding a menu — an older bundle with no `objs/screens/` installs everything
    else and says so."""
    try:
        screens_obj_text()
        return True
    except (PayloadError, OSError):
        return False


def interior_texture_dir() -> Path:
    """Where Gus's canonical texture set lives in the bundle. `make_release`
    stages it from the vendored `reference/gus/textures/` (the build must NOT
    depend on `.scratch/`, which is gitignored and absent from a fresh clone)."""
    return PAYLOAD_DIR / "textures" / "interior"


def interior_textures() -> list[tuple[str, Path]]:
    """The texture set as (filename, absolute source). Ordered by name so the
    install progress display is stable run to run.

    Only files actually present are returned, but a MISSING one is an error, not
    a silent skip: shipping 9 of 11 would leave the cockpit half-retextured."""
    src = interior_texture_dir()
    if not src.is_dir():
        raise PayloadError(
            f"interior texture set not found: {src} — this build is incomplete "
            f"(see reference/gus/README.md)")
    out, missing = [], []
    for name in INTERIOR_TEXTURES:
        p = src / name
        (out if p.is_file() else missing).append((name, p) if p.is_file() else name)
    if missing:
        raise PayloadError(
            f"interior texture set is incomplete in {src} — missing: "
            + ", ".join(missing))
    return out


def interior_available() -> bool:
    """Whether this build can install the interior mod at all, so the TUI can
    hide the opt-in rather than offer a step that would fail."""
    try:
        interior_obj_text()
        interior_textures()
        return True
    except (PayloadError, OSError):
        return False


# ─── the RealWings patch data ──────────────────────────────────────────────────
# ⚠ Resolved from the DSL at BUNDLE time (build/patch_realwings.py) and applied at
# install time (installer/realwings.py). This is the file that replaced
# `payload/dsl/` — see that module's header for why the split exists.
def realwings_patch_path() -> Path:
    return PAYLOAD_DIR / realwings.PATCH_JSON


def realwings_patch_data() -> dict:
    """The resolved RealWings replacement parameters.

    Loaded per call rather than cached: PAYLOAD_DIR is repointed by tests, and the
    file is read at most once per install."""
    return realwings.load(realwings_patch_path())


def realwings_available() -> bool:
    """Whether this bundle can patch the RealWings mod at all. A bundle built
    before the split has no such file; the install then continues without the
    wingtip patch rather than refusing to run."""
    try:
        realwings_patch_data()
        return True
    except realwings.RealWingsError:
        return False


# ─── plugin (native fat-plugin folder) ──────────────────────────────────────────
def plugin_src_root() -> Path:
    """The `ToLissPhoton/` fat-plugin folder to install, from the bundle."""
    p = PAYLOAD_DIR / "plugin" / PLUGIN_FOLDER
    if not p.is_dir():
        raise PayloadError(f"native plugin folder not found: {p} — "
                           f"this release bundle is incomplete")
    return p


def plugin_files() -> list[tuple[str, Path]]:
    """Every file in the fat-plugin folder as (folder-relative POSIX path, absolute
    source) — normally `<arch>/ToLissPhoton.xpl` for each built platform. Copied
    verbatim into <X-Plane>/Resources/plugins/ToLissPhoton/ at install time.

    Deliberately a whole-tree walk rather than a hard-coded `<arch>/<XPL_NAME>`
    list, so a future runtime data file ships without touching this — except for
    PLUGIN_USER_DIRS, which is user territory (`overlays/`). A source folder that
    happens to be a *deployed* plugin folder would otherwise carry a dev machine's
    overlay images into every install."""
    root = plugin_src_root()
    files = [(rel, p) for rel, p in
             ((p.relative_to(root).as_posix(), p) for p in sorted(root.rglob("*")))
             if p.is_file() and not _is_user_owned(rel)]
    if not files:
        raise PayloadError(f"native plugin folder is empty (no .xpl built?): {root}")
    return files


def _is_user_owned(rel: str) -> bool:
    """Whether a plugin-folder-relative POSIX path lies under a PLUGIN_USER_DIRS
    folder — matched on the first path SEGMENT, so a `.xpl` named `overlays.xpl`
    is not caught by a prefix test."""
    head = rel.split("/", 1)[0]
    return head in PLUGIN_USER_DIRS


def plugin_data_root() -> Path | None:
    """The tree of runtime DATA files that go into the plugin folder alongside the
    `.xpl` — `panelfx.txt` and `overlays/` — staged at `payload/plugindata/`.

    None, not an error, when it does not exist: a bundle built before the
    compositor shipped installs the plugin and nothing else, rather than refusing
    to run."""
    p = PAYLOAD_DIR / PLUGIN_DATA_DIRNAME
    return p if p.is_dir() else None


def plugin_data_files() -> list[tuple[str, Path]]:
    """The shipped plugin-folder data files as (folder-relative POSIX path,
    absolute source): `panelfx.txt` plus every image under `overlays/`.

    ⚠ Enumerated from a fixed set of ROOTS, not from a whole-tree walk — the roots
    are what `make_release.stage_plugin_data` writes, and keeping the two on the
    same shape is what stops a stray file in `plugindata/` becoming an installed
    artifact. `plugin_files()` can walk its root because that root is only ever a
    built plugin folder; this one is a staging directory."""
    root = plugin_data_root()
    if root is None:
        return []
    out: list[tuple[str, Path]] = []
    fx = root / PANELFX_FILE
    if fx.is_file():
        out.append((PANELFX_FILE, fx))
    for d in PLUGIN_USER_DIRS:
        src = root / d
        if not src.is_dir():
            continue
        for f in sorted(src.rglob("*")):
            if f.is_file():
                out.append((f"{d}/{f.relative_to(src).as_posix()}", f))
    return out


def plugin_arches() -> list[str]:
    """Platform arch subfolders that actually contain a `.xpl` (win_x64/mac_x64/
    lin_x64), for reporting which platforms this bundle ships."""
    root = plugin_src_root()
    return sorted(d.name for d in root.iterdir()
                  if d.is_dir() and (d / XPL_NAME).is_file())
