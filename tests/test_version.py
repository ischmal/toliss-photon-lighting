"""The facts that live on both sides of the C++/Python line must agree.

⚠ THIS FILE EXISTS BECAUSE THEY DIDN'T. The version drifted (0.6 against 0.7)
through a whole feature, because nothing in-sim looks wrong when the plugin and
the installer disagree: the About tab simply reports an older version than the
files beside it, which is exactly the report you get from a half-finished install.

The C++ port's answer is `src/native/src/core/version.h` and
`src/native/src/core/constants.h` — ONE definition that `plugin.cpp`,
`photon-installer` and `photoncore_tests` all read. What is left on the Python side
is the transitional second copy in `installer/constants.py` (which dies with the
Python installer, docs/installer_cpp_plan.md §8c) plus the build tooling that
reads the version to name a bundle. Those are what this pins.

Read by regex rather than by compiling: the point is to fail on a machine with no
C++ toolchain, which is where the drift would otherwise be invisible.
"""
import importlib.util
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
CORE = REPO / "src" / "native" / "src" / "core"
PLUGIN_CPP = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
    encoding="utf-8", errors="replace"
)
VERSION_H = (CORE / "version.h").read_text(encoding="utf-8", errors="replace")
CONSTANTS_H = (CORE / "constants.h").read_text(encoding="utf-8", errors="replace")
CONSTANTS_CPP = (CORE / "constants.cpp").read_text(encoding="utf-8", errors="replace")


def _installer_constants():
    spec = importlib.util.spec_from_file_location(
        "photon_installer_constants", REPO / "installer" / "constants.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _core_version():
    """The version out of core/version.h.

    ⚠ COMMENT LINES ARE SKIPPED. The header documents its own required shape, and
    the first version of this reader matched that EXAMPLE instead of the real
    definition — a regex that finds a placeholder and reports it as the release."""
    for line in VERSION_H.splitlines():
        if line.lstrip().startswith("//"):
            continue
        m = re.search(r'constexpr\s+char\s+kPhotonVersion\[\]\s*=\s*"([^"]*)"', line)
        if m:
            return m.group(1)
    raise AssertionError("kPhotonVersion is gone from core/version.h")


class VersionTests(unittest.TestCase):
    def test_core_version_h_and_the_installer_report_the_same_version(self):
        self.assertEqual(
            _core_version(), _installer_constants().VERSION,
            "kPhotonVersion in src/native/src/core/version.h and VERSION in "
            "installer/constants.py are the same release and must be bumped "
            "together")

    def test_the_version_is_a_dotted_number(self):
        # It reaches a filename and the OBJ marker line, so a stray space or a
        # 'v' prefix would land on disk.
        self.assertRegex(_core_version(), r"^\d+(\.\d+)+$")

    def test_the_plugin_no_longer_defines_its_own_version(self):
        """⚠ THE REGRESSION GUARD. A `static const char* kPhotonVersion = "0.9";`
        reappearing in plugin.cpp would compile, run, and recreate the exact drift
        this whole header exists to end — and nothing in-sim would look wrong."""
        self.assertNotRegex(
            PLUGIN_CPP, r'kPhotonVersion\s*=\s*"',
            "plugin.cpp defines a version string again — it must read "
            "photon::kPhotonVersion from core/version.h")
        self.assertIn("photon::kPhotonVersion", PLUGIN_CPP)


class SharedConstantTests(unittest.TestCase):
    """The other facts `photoncore` exists to single-source. Each of these was
    written down twice before the port, and each pair fails SILENTLY when it
    disagrees — which is why they are asserted rather than trusted."""

    def test_the_interior_obj_name_and_needle_agree(self):
        """⚠ Three files must agree on these: the DSL's gate names, the plugin's
        detection scan, and the installer's. A mismatch shows up in-sim as the
        Cockpit submenu simply not appearing — i.e. exactly like the mod not
        being installed."""
        c = _installer_constants()
        self.assertIn(f'kInteriorObj[] = "{c.INTERIOR_OBJ}"', CONSTANTS_H)
        # the needle is what the OBJ actually binds, so check the DSL emits it
        layout = (REPO / "src" / "lights" / "lights.layout.phdsl").read_text(
            encoding="utf-8", errors="replace")
        m = re.search(r'kInteriorObjNeedle\[\]\s*=\s*"([^"]*)"', CONSTANTS_H)
        self.assertIsNotNone(m, "kInteriorObjNeedle is gone from core/constants.h")
        self.assertIn(m.group(1), layout,
                      "the interior gate prefix in core/constants.h does not "
                      "appear in lights.layout.phdsl — the plugin would never "
                      "detect the cockpit mod")

    def test_the_plugin_reads_the_shared_interior_constants(self):
        self.assertIn("photon::kInteriorObj", PLUGIN_CPP)
        self.assertIn("photon::kInteriorObjNeedle", PLUGIN_CPP)

    def test_the_glow_redirect_table_is_shared_with_the_patcher(self):
        """⚠ A redirected index Photon does NOT drive goes DARK — our array reads
        0 where we never write. GlowMapForIcao and the installer's patcher used to
        each own a copy of the index list; they now read one table, and this pins
        that plugin.cpp really does read it rather than having quietly grown its
        own literals back."""
        from installer.patch_glow import REDIRECT

        for airframe, indices in REDIRECT.items():
            joined = ", ".join(str(i) for i in indices)
            self.assertIn(f'{{"{airframe}", {{{joined}}}}}', CONSTANTS_CPP,
                          f"core/constants.cpp's GlowRedirect() disagrees with "
                          f"installer/patch_glow.REDIRECT for {airframe}")
        self.assertIn("photon::GlowIndicesFor", PLUGIN_CPP,
                      "GlowMapForIcao no longer reads the shared table")

    def test_the_shared_airframe_table_matches_the_python_one(self):
        c = _installer_constants()
        for key, (glob, folder, obj, pretty) in c.AIRFRAMES.items():
            ids = c.AIRFRAME_CFG_IDS[key]
            row = (f'{{"{key}", "{glob}", "{folder}", "{obj}",\n'
                   f'         "{pretty}", "{ids["module"]}", "{ids["name"]}"}}')
            self.assertIn(row, CONSTANTS_CPP,
                          f"core/constants.cpp's Airframes() row for {key} does "
                          f"not match installer/constants.AIRFRAMES")

    def test_the_interior_texture_list_matches(self):
        c = _installer_constants()
        for name in c.INTERIOR_TEXTURES:
            self.assertIn(f'"{name}"', CONSTANTS_CPP)
        # ⚠ and the two with no stock counterpart, which uninstall must DELETE
        # rather than restore
        for name in c.INTERIOR_TEXTURES_ADDED:
            self.assertIn(f'name == "{name}"', CONSTANTS_CPP)

    def test_photoncore_includes_no_xplm_header(self):
        """⚠ THE LIBRARY'S LOAD-BEARING PROPERTY. `photon-installer` and
        `photoncore_tests` build on machines and CI runners with no X-Plane SDK,
        and that independence is what keeps the release matrix simple. One
        `#include "XPLMDataAccess.h"` in core would end it."""
        for f in sorted(CORE.glob("*.*")):
            text = f.read_text(encoding="utf-8", errors="replace")
            self.assertNotIn('#include "XPLM', text,
                             f"{f.name} includes an XPLM header — photoncore must "
                             f"not depend on the X-Plane SDK")
            self.assertNotIn("#include <XPLM", text, f.name)


if __name__ == "__main__":
    unittest.main()
