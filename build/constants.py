"""The handful of shared facts the BUILD TOOLING needs, in Python.

⚠ THIS IS NOT A SECOND SOURCE OF TRUTH — it is the Python-side mirror of
`src/native/src/core/constants.{h,cpp}`, which is the definition. The C++ header
serves the plugin and the installer; nothing there is importable from a build
script, and the alternative (regex-parsing tables out of C++) is worse than a
mirror that a test pins.

⚠ SO IT IS PINNED, and the pin is not optional: `tests/test_version.py` compares
every table below against `core/constants.cpp` and fails on any divergence. That
test is the whole reason this file is allowed to exist. Add a fact here only when
a build script or a DSL test genuinely needs it, and add the pin in the same
commit — an unpinned constant here is exactly the drift this project has already
been bitten by twice (`kPhotonVersion` vs the installer's VERSION, and the
interior needle across three files).

⚠ VERSION IS NOT MIRRORED, it is READ — `build/version.py` regexes it straight out
of `core/version.h`. A mirrored version is the one that already drifted for a whole
feature, so it does not get to be a copy again.

What is deliberately absent: everything only the INSTALLER needed. Detection
tables, backup/manifest names, the `.acf` suffix list, the glow redirect map and
the wing labels all live solely in C++ now that the Python installer is gone.
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
if str(REPO / "build") not in sys.path:
    sys.path.insert(0, str(REPO / "build"))
from version import VERSION  # noqa: E402,F401  (re-exported: read from core/version.h)

# ─── airframes ────────────────────────────────────────────────────────────────
# key -> (aircraft-folder glob, X-Plane tree folder, OBJ filename, pretty name).
# The build tooling uses the OBJ filename (which OBJ each airframe emits) and the
# key set; the glob and pretty name are here so the pin covers the whole row and
# a reordering in C++ cannot pass unnoticed.
#
# ⚠ The OBJ filename slot is an airframe FINGERPRINT on the C++ side. Never put
# `lights_inn.obj` in it — the interior OBJ is byte-identical across A319/A320/A321
# and would make every A3xx match every other one.
AIRFRAMES = {
    "a319": ("ToLissA319*", "A319", "lights_out319_XP12.obj", "ToLiss A319"),
    "a320": ("ToLissA320*", "A320", "lights_out320_XP12.obj", "ToLiss A320"),
    "a321": ("ToLissA321*", "A321", "lights_out321_XP12.obj", "ToLiss A321"),
    "a339": ("ToLissA339*", "A339", "ExternalLights_XP12.obj", "ToLiss A330-900"),
}

# ─── wing-mod variants ────────────────────────────────────────────────────────
WINGS = ["stock", "durantula", "realwings"]

# ─── interior ("Gus Mod") ─────────────────────────────────────────────────────
INTERIOR_OBJ = "lights_inn.obj"

# The A330-900 is out of scope entirely (docs/interior_plan.md decision #14):
# different cockpit model, different rheostat indices, and Gus's mod does not
# cover it.
INTERIOR_AIRFRAMES = ("a319", "a320", "a321")

# Gus's canonical texture set, staged into the bundle by make_release.py.
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

# ⚠ These two have NO stock counterpart, so an uninstall DELETES them rather than
# restoring a backup that never existed. The rule is enforced in C++; the set is
# mirrored here because the DSL/interior tests assert against it.
INTERIOR_TEXTURES_ADDED = frozenset({"pedals_details_1.png", "pedals_details_2.png"})

# ─── display (screen) glow ────────────────────────────────────────────────────
SCREENS_OBJ = "lights_screens.obj"
SCREENS_AIRFRAMES = ("a319", "a320", "a321")

# ─── the plugin folder ────────────────────────────────────────────────────────
PLUGIN_FOLDER = "ToLissPhoton"
XPL_NAME = "ToLissPhoton.xpl"

# Subfolders of the plugin folder that hold USER content as well as ours, and are
# therefore never swept into a bundle by a `--plugin-dir` that happens to point at
# a DEPLOYED plugin folder — which on a dev machine holds that dev's own images.
PLUGIN_USER_DIRS = ("overlays",)

# Runtime data the `.xpl` reads out of its OWN folder: the authored Panel FX layer
# definition and the overlay images its layers name. Staged under
# `payload/plugindata/`, mirroring the plugin folder's layout.
PANELFX_FILE = "panelfx.txt"
PLUGIN_DATA_DIRNAME = "plugindata"

# ─── links ────────────────────────────────────────────────────────────────────
XPLANE_ORG_URL = ("https://forums.x-plane.org/files/file/"
                  "100717-toliss-photon-exterior-lighting-mod-for-toliss-a319a320a321")
GITHUB_URL = "https://github.com/ischmal/toliss-photon-lighting"
