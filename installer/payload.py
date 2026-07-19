"""Locate the installable artifacts — the light OBJs, the plugin, and the DSL
toolchain the RealWings live patch needs.

Two modes, chosen at call time (never cached, so tests can point PAYLOAD_DIR
at a fixture):

* **Bundled** (a shipped release): read the pre-built `payload/` directory that
  sits next to `install.py`, resolved relative to this file — never CWD, so the
  installer runs from any directory (Downloads, a USB stick, or inside
  X-Plane/Resources/plugins/).

* **Dev tree** (running `install.py` straight from the repo, where no `payload/`
  has been built): fall back to the repo's own build system — generate each OBJ
  on the fly from the DSL via `build_objs`, take the plugin from `src/plugin/`,
  and point the RealWings patch at the repo's `build/`. This is why the
  installer runs in-place in the project folder without first building a
  release.

Bundled `payload/` layout (built by build/make_release.py):
    payload/objs/<stock|durantula|realwings>/<obj filename>
    payload/plugin/PI_ToLissPhoton.py
    payload/dsl/build/{build_objs,photon_dsl,patch_realwings}.py
    payload/dsl/src/lights/*.phdsl
"""
from __future__ import annotations

import sys
from pathlib import Path

from installer.constants import AIRFRAMES

# installer/payload.py -> installer/ -> the dir holding install.py. In a release
# that dir holds payload/; in the repo it *is* the repo root (holds build/, src/).
ROOT = Path(__file__).resolve().parent.parent
PAYLOAD_DIR = ROOT / "payload"


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
    so the two modes are byte-identical."""
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


def _import_build_objs(repo: Path):
    bp = str(repo / "build")
    if bp not in sys.path:
        sys.path.insert(0, bp)
    import build_objs as B  # repo copy
    return B


# ─── plugin ────────────────────────────────────────────────────────────────────
def plugin_path() -> Path:
    if _bundled():
        p = PAYLOAD_DIR / "plugin" / "PI_ToLissPhoton.py"
    else:
        repo = _dev_repo()
        p = (repo / "src" / "plugin" / "PI_ToLissPhoton.py") if repo else None
    if not p or not p.is_file():
        raise PayloadError(f"missing plugin source: {p}")
    return p


def plugin_bytes() -> bytes:
    return plugin_path().read_bytes()


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
