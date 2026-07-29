"""Shared constants for the installer package. Kept dependency-free (no import
of build_objs) so the installer runs standalone from the release payload,
which does not ship the DSL toolchain (except payload/dsl/, used only for the
RealWings live patch — see actions.py)."""
from __future__ import annotations

from pathlib import Path

VERSION = "0.6"

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

# ─── interior ("Gus Mod") ─────────────────────────────────────────────────────
# The interior mod is a SEPARATE, OPT-IN install axis from the exterior one.
#
# ⚠ Deliberately NOT folded into AIRFRAMES above. That tuple's OBJ-filename slot
# feeds detect._identify_airframe / _obj_marker as an airframe FINGERPRINT, and
# lights_inn.obj is byte-identical across A319/A320/A321 — adding it there would
# make every A3xx match every other one.
INTERIOR_OBJ = "lights_inn.obj"

# Airframes that ship the interior mod. The A330-900 is out of scope entirely
# (docs/interior_plan.md decision #14): different cockpit model, different
# rheostat indices, and Gus's mod does not cover it.
INTERIOR_AIRFRAMES = ("a319", "a320", "a321")

# Gus Rodrigues' Light Mod on the .org — a SEPARATE file entry per variant, so the
# opt-in screen's "Open X-Plane.org page…" picks by airframe rather than reusing
# XPLANE_ORG_URL (Photon's own page). Keys must cover INTERIOR_AIRFRAMES; no a339.
INTERIOR_ORG_URL = {
    "a319": "https://forums.x-plane.org/files/file/93336-a319-light-mod/",
    "a320": "https://forums.x-plane.org/files/file/93337-a320-light-mod/",
    "a321": "https://forums.x-plane.org/files/file/93338-a321-light-mod/",
}

# Gus's canonical texture set, installed into <aircraft>/objects/.
# NINE of these replace a stock ToLiss file and are restored from backup on
# uninstall. The TWO in INTERIOR_TEXTURES_ADDED have NO stock counterpart, so
# uninstall must DELETE them instead — restoring a backup that never existed
# would leave them in place forever.
INTERIOR_TEXTURES = (
    "chairs_LIT.png",
    "kitchens_LIT.png",
    "knobs.png",
    "knobs_LIT.png",
    "panels_overhead_LIT.png",
    "pedals_details_1.png",
    "pedals_details_2.png",
    "text_LIT.png",
    "walls_bottom_LIT.png",
    "walls_outer_LIT.png",
    "walls_top_LIT.png",
)
INTERIOR_TEXTURES_ADDED = frozenset({"pedals_details_1.png", "pedals_details_2.png"})

# Livery overrides that shadow our textures. X-Plane substitutes a .dds for the
# .png an OBJ names, so a livery shipping chairs_LIT.dds wins over our
# chairs_LIT.png in objects/ and our work is invisible in that livery. The fix
# is to back up and REMOVE the livery's .dds so the PNG resolves (§5.4).
# Matched by stem against INTERIOR_TEXTURES.
INTERIOR_LIVERY_EXTS = (".dds", ".png")

# The four .acf files per airframe that carry the cockpit spot block. All four
# hold identical spot values, so this is one constant patch applied four times.
# Photon's OBJ work is XP12-only, but this patch is not: lights_inn.obj is
# shared by every variant, so an XP11 user would otherwise get patched OBJ
# lights alongside unpatched sim spots.
ACF_SUFFIXES = ("", "_StdDef", "_XP11", "_XP11_StdDef")

# ─── skin glow ────────────────────────────────────────────────────────────────
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

XPLANE_ORG_URL = ("https://forums.x-plane.org/files/file/"
                  "100717-toliss-photon-exterior-lighting-mod-for-toliss-a319a320a321")
GITHUB_URL = "https://github.com/ischmal/toliss-photon-lighting"
