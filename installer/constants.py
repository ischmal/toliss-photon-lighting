"""Shared constants for the installer package. Kept dependency-free (no import
of build_objs) so the installer runs standalone from the release payload,
which does not ship the DSL toolchain (except payload/dsl/, used only for the
RealWings live patch — see actions.py)."""
from __future__ import annotations

from pathlib import Path

VERSION = "0.4"

# airframe key -> (aircraft-folder glob under Aircraft/, X-Plane tree folder,
# OBJ filename, fallback pretty name if skunkcrafts_updater.cfg is missing)
AIRFRAMES = {
    "a319": ("ToLissA319*", "A319", "lights_out319_XP12.obj", "ToLiss A319"),
    "a320": ("ToLissA320*", "A320", "lights_out320_XP12.obj", "ToLiss A320"),
    "a321": ("ToLissA321*", "A321", "lights_out321_XP12.obj", "ToLiss A321"),
}

# airframe key -> RealWings mod subfolder under the aircraft's objects/
REALWINGS_DIR = {
    "a319": "RealWings319",
    "a320": "RealWings320",
    "a321": "RealWings321",
}

WINGS = ["stock", "durantula", "realwings"]
WING_LABEL = {"stock": "Default", "durantula": "Durantula", "realwings": "RealWings"}
WING_ACTION_LABEL = {
    "stock": "Install",
    "durantula": "Install for Durantula's Wing Mod",
    "realwings": "Install for RealWings Mod",
}

BACKUP_DIRNAME = "Photon Backup Files"
MANIFEST_NAME = "photon_manifest.json"

# The native plugin is a compiled fat-plugin folder holding one .xpl per platform:
#   Resources/plugins/ToLissPhoton/<arch>/ToLissPhoton.xpl   (arch = win_x64|mac_x64|lin_x64)
# X-Plane loads the subfolder matching the host OS. No XPPython3/Python needed.
PLUGIN_FOLDER = "ToLissPhoton"
XPL_NAME = "ToLissPhoton.xpl"
PLUGIN_DIR_REL = Path("Resources") / "plugins" / PLUGIN_FOLDER

PHOTON_URL = "https://github.com/"  # placeholder — the eventual release page
