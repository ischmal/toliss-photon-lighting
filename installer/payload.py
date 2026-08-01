"""Locate the installable artifacts — the light OBJs, the plugin, and the DSL
toolchain the RealWings live patch needs.

Two modes, chosen at call time (never cached, so tests can point PAYLOAD_DIR
at a fixture):

* **Bundled** (a shipped release): read the pre-built `payload/` directory that
  sits next to `install.py`, resolved relative to this file — never CWD, so the
  installer runs from any directory (Downloads, a USB stick, or inside
  X-Plane/Resources/plugins/). A frozen **loose** snapshot instead reads a
  `data/` folder next to the executable (same layout) — see
  `_default_payload_dir` and `build/make_exe.py --loose`.

* **Dev tree** (running `install.py` straight from the repo, where no `payload/`
  has been built): fall back to the repo's own build system — generate each OBJ
  on the fly from the DSL via `build_objs`, take the plugin from `src/plugin/`,
  and point the RealWings patch at the repo's `build/`. This is why the
  installer runs in-place in the project folder without first building a
  release.

Bundled `payload/` layout (built by build/make_release.py):
    payload/objs/<stock|durantula|realwings>/<obj filename>
    payload/plugin/ToLissPhoton/<arch>/ToLissPhoton.xpl   (arch = win_x64|mac_x64|lin_x64)
    payload/dsl/build/{build_objs,photon_dsl,patch_realwings}.py
    payload/dsl/src/lights/*.phdsl

The plugin is the compiled native `.xpl` (no XPPython3/Python at runtime). It is a
fat plugin: the folder can carry one `.xpl` per platform side by side, and X-Plane
loads the subfolder matching the host OS. In dev mode it comes from the CMake build
output under `src/native/build/` (so the native plugin must be built first).
"""
from __future__ import annotations

import sys
from pathlib import Path

from installer.constants import (
    AIRFRAMES, INTERIOR_OBJ, INTERIOR_TEXTURES, PLUGIN_FOLDER, PLUGIN_USER_DIRS,
    WINGS_FOR, XPL_NAME,
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


# ─── mode detection ────────────────────────────────────────────────────────────
def _bundled() -> bool:
    return PAYLOAD_DIR.is_dir()


def _dev_repo() -> Path | None:
    """ROOT, if it looks like the source repo (has the DSL build toolchain and
    config); else None. Guarded so a real release never falls into dev mode."""
    if ((ROOT / "build" / "build_objs.py").is_file()
            and (ROOT / "src" / "lights" / "lights.style.phdsl").is_file()):
        return ROOT
    return None


def available() -> bool:
    return _bundled() or _dev_repo() is not None


def mode() -> str:
    return "bundled" if _bundled() else ("dev" if _dev_repo() else "unavailable")


# ─── OBJs ──────────────────────────────────────────────────────────────────────
def obj_path(wing: str, airframe: str) -> Path:
    """Bundled-mode path to a pre-built OBJ. (Dev mode has no such file — use
    obj_text, which builds it.)"""
    _, _, fname, _ = AIRFRAMES[airframe]
    p = PAYLOAD_DIR / "objs" / wing / fname
    if not p.is_file():
        raise PayloadError(f"missing bundled OBJ: {p}")
    return p


def obj_text(wing: str, airframe: str) -> str:
    """The OBJ text to install: read from the bundle, or generated on the fly
    from the DSL in a dev checkout. Both go through the same build_objs.Emitter,
    so the two modes are byte-identical.

    Checked against WINGS_FOR before either path runs: in bundled mode a missing
    file already fails via obj_path(), but the dev-mode path below calls the
    Emitter directly, and resolve_mount()'s fallback to whatever mount variant
    exists would otherwise silently emit an airframe's stock content under an
    unsupported wing's name (e.g. a339 has no Durantula/RealWings mount at all)
    instead of erroring — this keeps both modes equally strict."""
    if wing not in WINGS_FOR[airframe]:
        raise PayloadError(f"{airframe} has no {wing!r} wing-mod variant "
                           f"(supported: {', '.join(WINGS_FOR[airframe])})")
    if _bundled():
        return obj_path(wing, airframe).read_text(encoding="utf-8", errors="replace")
    repo = _dev_repo()
    if repo is None:
        raise PayloadError(
            f"no bundled payload at {PAYLOAD_DIR} and not a source checkout — "
            f"nothing to install from")
    B = _import_build_objs(repo)
    cfg = B.load_config(wing=wing)
    return B.Emitter(cfg, airframe, wing).emit()


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
    """The interior OBJ text to install: read from the bundle, or generated on
    the fly from the DSL in a dev checkout. Built from the a320 fixtures, which
    a319/a321 inherit verbatim (a339 has no interior target at all)."""
    if _bundled():
        return interior_obj_path().read_text(encoding="utf-8", errors="replace")
    repo = _dev_repo()
    if repo is None:
        raise PayloadError(
            f"no bundled payload at {PAYLOAD_DIR} and not a source checkout — "
            f"nothing to install from")
    B = _import_build_objs(repo)
    cfg = B.load_config(wing="stock")
    return B.Emitter(cfg, "a320", "stock", target="interior").emit()


def interior_texture_dir() -> Path:
    """Where Gus's canonical texture set lives. Bundled: payload/textures/
    interior/. Dev: the vendored reference/gus/textures/ (the build must NOT
    depend on .scratch/, which is gitignored and absent from a fresh clone)."""
    if _bundled():
        return PAYLOAD_DIR / "textures" / "interior"
    repo = _dev_repo()
    if repo is not None:
        return repo / "reference" / "gus" / "textures"
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


def _import_build_objs(repo: Path):
    bp = str(repo / "build")
    if bp not in sys.path:
        sys.path.insert(0, bp)
    import build_objs as B  # repo copy
    return B


# ─── plugin (native fat-plugin folder) ──────────────────────────────────────────
def plugin_src_root() -> Path:
    """The source `ToLissPhoton/` fat-plugin folder to install. Bundled: under
    payload/plugin/; dev: the CMake build output under src/native/build/ (so the
    native plugin has to be built first)."""
    if _bundled():
        p = PAYLOAD_DIR / "plugin" / PLUGIN_FOLDER
        hint = "this release bundle is incomplete"
    else:
        repo = _dev_repo()
        p = (repo / "src" / "native" / "build" / PLUGIN_FOLDER) if repo else None
        hint = ("build the native plugin first — see src/native/README.md "
                "(e.g. cmake --build src/native/build --config Release)")
    if not p or not p.is_dir():
        raise PayloadError(f"native plugin folder not found: {p} — {hint}")
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


def plugin_arches() -> list[str]:
    """Platform arch subfolders that actually contain a `.xpl` (win_x64/mac_x64/
    lin_x64), for reporting which platforms this bundle ships."""
    root = plugin_src_root()
    return sorted(d.name for d in root.iterdir()
                  if d.is_dir() and (d / XPL_NAME).is_file())


# ─── RealWings live-patch toolchain ────────────────────────────────────────────
def dsl_dir() -> Path:
    """The directory to put on sys.path to `import patch_realwings`. In a bundle
    this mirrors the repo's build/ + src/lights/ relative layout under
    payload/dsl/ (build_objs.py locates its config relative to its own file);
    in a dev checkout it is simply the repo's build/."""
    if _bundled():
        return PAYLOAD_DIR / "dsl" / "build"
    repo = _dev_repo()
    if repo is not None:
        return repo / "build"
    return PAYLOAD_DIR / "dsl" / "build"  # unavailable: let the caller error out
