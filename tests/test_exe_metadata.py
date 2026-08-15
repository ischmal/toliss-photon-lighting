"""`photon-installer.exe` must carry the VERSIONINFO a downloaded binary is judged by.

Explorer's Properties ▸ Details tab, the publisher line, the version a user reads
back when reporting a bug — all of it comes from one resource block in
`src/native/res/photon.rc`. It is not a substitute for code signing (SmartScreen's
"unknown publisher" warning answers to an Authenticode certificate and nothing
else), but a mod .exe whose Details tab is completely blank is its own bad look,
and some AV heuristics weight a binary with no version info at all.

⚠ EVERY FAILURE MODE HERE IS SILENT, WHICH IS WHY THIS FILE EXISTS. The build
succeeds in all of them:

* the StringFileInfo BLOCK name and the VarFileInfo Translation value drift apart,
  Windows looks up a language block that is not there, and the Details tab comes
  back EMPTY — indistinguishable from having shipped no VERSIONINFO at all;
* a version gets typed into the .rc, and now there are two definitions again —
  the drift `core/version.h` exists to end (see tests/test_version.py);
* a VALUE line is dropped and one field silently blanks;
* `OriginalFilename` stops matching the name the bundle actually ships, which
  reads to anyone checking as a file renamed after the fact.

Read by regex rather than by building: the point is to fail on a Linux/macOS
runner and on a checkout with no Windows SDK, which is exactly where nobody would
otherwise look at this resource again.
"""
import importlib.util
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
NATIVE = REPO / "src" / "native"
RC = (NATIVE / "res" / "photon.rc").read_text(encoding="utf-8", errors="replace")
RC_TEMPLATE = (NATIVE / "res" / "version_rc.h.in").read_text(
    encoding="utf-8", errors="replace")
CMAKELISTS = (NATIVE / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
VERSION_H = (NATIVE / "src" / "core" / "version.h").read_text(
    encoding="utf-8", errors="replace")

# The eight fields Explorer shows. Dropping one is a blank row, not an error.
REQUIRED_VALUES = (
    "CompanyName",
    "FileDescription",
    "FileVersion",
    "InternalName",
    "LegalCopyright",
    "OriginalFilename",
    "ProductName",
    "ProductVersion",
)


def _load(name, relpath):
    """Import a `build/` script by path — the directory is not a package."""
    spec = importlib.util.spec_from_file_location(name, REPO / relpath)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _rc_values():
    """{key: raw right-hand side} for every `VALUE "key", <rhs>` in the .rc."""
    return {m.group(1): m.group(2).strip()
            for m in re.finditer(r'VALUE\s+"(\w+)"\s*,\s*(.+)', RC)}


class TheResourceIsPresentAndComplete(unittest.TestCase):
    def test_the_rc_declares_a_versioninfo_block(self):
        self.assertRegex(
            RC, r"(?m)^\s*VS_VERSION_INFO\s+VERSIONINFO\b",
            "photon.rc has no VERSIONINFO — the installer would ship with a blank "
            "Properties > Details tab, which is what this resource exists to fill")

    def test_every_field_explorer_shows_is_filled_in(self):
        values = _rc_values()
        missing = [k for k in REQUIRED_VALUES if k not in values]
        self.assertEqual(missing, [],
                         "these VERSIONINFO fields are gone; each one is a blank "
                         "row in Explorer, not a build error")
        blank = [k for k in REQUIRED_VALUES if values[k] in ('""', "''")]
        self.assertEqual(blank, [], "these VERSIONINFO fields are empty strings")

    def test_the_file_type_is_an_application(self):
        """VFT_APP, not VFT_DLL — this .rc reaches photon-installer only. The
        `.xpl` has no VERSIONINFO of its own (deliberate: X-Plane does not read
        one, and the About tab is how a user checks the plugin's version)."""
        self.assertRegex(RC, r"(?m)^\s*FILETYPE\s+VFT_APP\b")


class TheLanguageBlockAndTheTranslationAgree(unittest.TestCase):
    """⚠ THE SILENT ONE. Windows finds the string block by the hex name under
    StringFileInfo, and that name is the VarFileInfo Translation pair written as
    two 4-digit hex numbers, language first. When they disagree the lookup fails
    and Explorer shows an EMPTY Details tab — the exact appearance of a binary
    with no version resource, from a file that compiled without a murmur."""

    def setUp(self):
        after = RC.split('BLOCK "StringFileInfo"', 1)
        self.assertEqual(len(after), 2, "no StringFileInfo block in photon.rc")
        m = re.search(r'BLOCK\s+"([0-9A-Fa-f]{8})"', after[1])
        self.assertIsNotNone(m, "the StringFileInfo block has no 8-hex-digit name")
        self.block = m.group(1).lower()

        t = re.search(r'VALUE\s+"Translation"\s*,\s*(0[xX][0-9A-Fa-f]+|\d+)\s*,'
                      r'\s*(0[xX][0-9A-Fa-f]+|\d+)', RC)
        self.assertIsNotNone(t, "no VarFileInfo Translation value in photon.rc")
        self.lang = int(t.group(1), 0)
        self.codepage = int(t.group(2), 0)

    def test_the_block_name_is_the_translation_pair(self):
        self.assertEqual(self.block, f"{self.lang:04x}{self.codepage:04x}",
                         "the StringFileInfo BLOCK name must be the Translation "
                         "language and codepage as hex; a mismatch empties the "
                         "whole Details tab")

    def test_the_strings_are_declared_as_the_unicode_codepage(self):
        """1200 (0x4B0) is Unicode, which is what rc.exe emits. Declaring 1252
        here is the other half of the same blank-tab failure."""
        self.assertEqual(self.codepage, 1200)


class TheVersionIsStillSingleSourced(unittest.TestCase):
    """⚠ `core/version.h` IS THE ONE DEFINITION. A dotted literal typed into the
    .rc recreates the drift that ran through a whole feature unnoticed — see
    tests/test_version.py, which pins the same rule on the Python side."""

    def test_the_rc_names_the_macros_instead_of_a_literal(self):
        values = _rc_values()
        for key in ("FileVersion", "ProductVersion"):
            self.assertEqual(
                values[key], "PHOTON_RC_VERSION_STRING",
                f'VERSIONINFO {key} must stay the generated macro — a literal '
                f'here is a second version definition')
        for field in ("FILEVERSION", "PRODUCTVERSION"):
            m = re.search(rf"(?m)^\s*{field}\s+(.+)$", RC)
            self.assertIsNotNone(m, f"{field} is missing from photon.rc")
            self.assertEqual(m.group(1).strip(), "PHOTON_RC_FILEVERSION",
                             f"{field} must stay the generated macro")

    def test_the_macros_the_rc_uses_are_the_ones_the_template_defines(self):
        for macro in ("PHOTON_RC_FILEVERSION", "PHOTON_RC_VERSION_STRING"):
            self.assertRegex(RC_TEMPLATE, rf"(?m)^#define\s+{macro}\b",
                             f"res/version_rc.h.in no longer defines {macro}")

    def test_cmake_derives_the_version_and_configures_the_header(self):
        self.assertIn("configure_file(res/version_rc.h.in", CMAKELISTS,
                      "CMake no longer generates the version header the .rc "
                      "includes")
        self.assertRegex(
            CMAKELISTS,
            r"target_include_directories\(photon-installer[\s\S]{0,200}?generated",
            "the generated header must stay on photon-installer's include path — "
            "rc.exe takes its -I set from the target's")

    def test_cmakes_own_regex_still_finds_the_real_definition(self):
        """⚠ NOT A TAUTOLOGY — it pins CMake's READER, the same way
        test_version.py pins build/version.py's. Both are anchored at line start
        so that version.h's own comments, which discuss `kPhotonVersion` by name,
        cannot match; the Python reader's first draft matched a comment and
        reported a placeholder as the release."""
        lines = [ln for ln in VERSION_H.splitlines()
                 if re.match(r"^constexpr char kPhotonVersion\[\]", ln)]
        self.assertEqual(len(lines), 1,
                         "CMake's file(STRINGS ... REGEX) must match exactly one "
                         "line of core/version.h")
        m = re.search(r'"([^"]*)"', lines[0])
        self.assertEqual(m.group(1), _load("photon_version", "build/version.py").VERSION)

    def test_the_version_survives_the_four_field_padding(self):
        """FILEVERSION is four 16-bit numbers whatever kPhotonVersion carries.
        CMake pads; this pins that the padded result is still legal, which a
        date-based scheme would quietly overflow."""
        parts = _load("photon_version", "build/version.py").VERSION.split(".")
        self.assertLessEqual(len(parts), 4,
                             "a 5-component version cannot fit FILEVERSION")
        for p in parts:
            self.assertTrue(p.isdigit(), f"FILEVERSION field {p!r} is not a number")
            self.assertLessEqual(int(p), 65535, "FILEVERSION fields are 16-bit")


class TheFilenameMatchesWhatTheBundleShips(unittest.TestCase):
    def test_originalfilename_is_the_name_make_release_stages(self):
        """⚠ Explorer shows OriginalFilename beside the real name, and tools
        compare them. `make_release.py` stages the binary under its own name and
        never renames it; if a bundle ever does, this string has to follow or the
        installer reads as having been renamed after signing."""
        declared = _rc_values()["OriginalFilename"].strip().strip('"')
        staged = _load("photon_make_release", "build/make_release.py")
        self.assertIn(declared, staged.INSTALLER_EXE_NAMES,
                      "the .rc's OriginalFilename is not a name make_release.py "
                      "stages the installer under")


if __name__ == "__main__":
    unittest.main()
