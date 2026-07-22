"""Shared constants for the installer package. Kept dependency-free (no import
of build_objs) so the installer runs standalone from the release payload,
which does not ship the DSL toolchain (except payload/dsl/, used only for the
RealWings live patch — see actions.py)."""
from __future__ import annotations

from pathlib import Path

VERSION = "0.4"

# airframe key -> (aircraft-folder glob under Aircraft/, X-Plane tree folder,
# OBJ filename, fallback pretty name if skunkcrafts_updater.cfg is missing).
# The glob is now only a LAST-RESORT identifier — detection primarily reads the
# skunkcrafts_updater.cfg contents (AIRFRAME_CFG_IDS below), so a ToLiss install
# is found regardless of its folder name or where it sits under Aircraft/.
AIRFRAMES = {
    "a319": ("ToLissA319*", "A319", "lights_out319_XP12.obj", "ToLiss A319"),
    "a320": ("ToLissA320*", "A320", "lights_out320_XP12.obj", "ToLiss A320"),
    "a321": ("ToLissA321*", "A321", "lights_out321_XP12.obj", "ToLiss A321"),
    "a339": ("ToLissA339*", "A339", "ExternalLights_XP12.obj", "ToLiss A330-900"),
}

# How to recognise a ToLiss airframe from its skunkcrafts_updater.cfg, so
# detection no longer depends on the folder NAME or its location under Aircraft/
# (the user may rename it or drop it in a sub-hangar like Aircraft/My Hangar/…).
# Matched case-insensitively as substrings, `module` first (the ToLiss update-repo
# URL, e.g. …/aircraft-repositories/A330-900/ — the most stable machine id), then
# the display `name`. Tokens are mutually exclusive so order between airframes is
# irrelevant. detect._identify_airframe falls back to the airframe-specific OBJ
# filename above, then the folder glob, if a cfg is absent/unrecognised.
AIRFRAME_CFG_IDS = {
    "a319": {"module": "/A319/",     "name": "A319"},
    "a320": {"module": "/A320/",     "name": "A320"},
    "a321": {"module": "/A321/",     "name": "A321"},
    "a339": {"module": "/A330-900/", "name": "A330-900"},
}

# Airframes whose install repoints ToLiss's skin-glow LIT-texture regions to
# Photon's own dataref (build/patch_glow.py). Gate only — the actual indices live
# in patch_glow.REDIRECT, which must agree with GlowMapForIcao in
# src/native/src/plugin.cpp. Absent airframes keep ToLiss's own glow fade.
GLOW_AIRFRAMES = {"a339"}

# airframe key -> RealWings mod subfolder under the aircraft's objects/. No
# entry means the airframe has no wing mods at all (e.g. a339).
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

# airframe key -> wing-mod variants actually offered for it in the installer UI.
# Source of truth for which options screen_actions() builds; a339 has no wing
# mods at all, so it only ever offers "stock" (labeled "Install", no mod choice).
WINGS_FOR = {
    "a319": ["stock", "durantula", "realwings"],
    "a320": ["stock", "durantula", "realwings"],
    "a321": ["stock", "durantula", "realwings"],
    "a339": ["stock"],
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
