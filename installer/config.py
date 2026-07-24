"""Tiny persisted installer preference: the last manually-entered X-Plane root,
so a returning user isn't asked to retype/re-browse it. Lives in a per-OS user
config directory, independent of any X-Plane install (so it survives an
uninstall/reinstall of the aircraft mod, and of the installer itself).

Kept separate from installer/actions.py's filesystem writes (those target the
X-Plane install being modified; this targets the user's own config dir) and
from detect.py (which stays a pure(ish) filesystem-detection module)."""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

_CONFIG_FILENAME = "installer_config.json"


def _config_dir() -> Path:
    if os.name == "nt":
        base = os.environ.get("APPDATA") or str(Path.home())
        return Path(base) / "ToLissPhoton"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "ToLissPhoton"
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "ToLissPhoton"


def _config_path() -> Path:
    return _config_dir() / _CONFIG_FILENAME


def load_saved_root() -> Path | None:
    """The last manually-confirmed X-Plane root, or None if never saved, or
    unreadable. Callers must still revalidate it — it may no longer exist."""
    try:
        data = json.loads(_config_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    root = data.get("xplane_root")
    return Path(root) if root else None


def save_root(root: Path) -> None:
    try:
        _config_dir().mkdir(parents=True, exist_ok=True)
        _config_path().write_text(json.dumps({"xplane_root": str(root)}), encoding="utf-8")
    except OSError:
        pass


def clear_saved_root() -> None:
    try:
        _config_path().unlink(missing_ok=True)
    except OSError:
        pass
