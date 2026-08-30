"""Paths are UTF-8 end to end, and the ANSI code page never gets a say.

⚠ THE BUG THESE PIN (2026-08-22). On MSVC, `std::filesystem::path::string()` and
`generic_string()` narrow through the ANSI code page, and `fs::path(const char*)`
widens through it. Three things followed, none of them visible on an English
machine with ASCII folder names:

  * The interior livery scan spelled its manifest entry with `generic_string()`.
    A livery folder named "Aer Lingus – EI-DEP" came back as CP1252 bytes that
    `PathFromUtf8` turned into U+FFFD (a mangled backup name) and that the
    manifest's JSON writer then REJECTED — the install failed at its LAST step,
    with the livery's texture already removed and no manifest to restore it from.
    A name the page could not spell at all (Turkish ı, Polish ł, Cyrillic, CJK)
    THREW `std::system_error` out of the loop half-way through.
  * `detect.cpp` spelled `Aircraft::folder` the same way, so a hangar or aircraft
    folder outside the page threw out of the aircraft scan — and the GUI had no
    net under `DetectAircraft`, so the installer simply disappeared.
  * The plugin built `fs::path` straight from X-Plane's UTF-8 buffers, which
    `XPLMUtilities.h` says Windows plugins must convert to UTF-16 themselves. An
    X-Plane root or aircraft folder with a non-ASCII character resolved to a path
    that did not exist: no airframe key, "cockpit mod NOT installed", a profiles
    file that never saved. And every `path.string()` in a log line was a throw
    waiting for a character the page could not spell — inside a sim callback.

The C++ side is exercised by `install_tests.cpp` (the three non-ASCII livery
shapes, the deleted-livery uninstall, the non-ASCII hangar). This file pins the
SHAPE of the code, so the narrowing cannot creep back in one call at a time.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
NATIVE = REPO / "src" / "native" / "src"


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def _code(text: str) -> str:
    """The source minus its comments, so a comment that NAMES the trap does not
    trip the test that forbids it.

    WARNING: ONE PASS, NOT TWO REGEXES (2026-08-29). Stripping `/*...*/` first
    lets a `/*` written inside a LINE comment open a block that closes thousands
    of lines later and takes real code with it. That is not hypothetical:
    plugin.cpp's `sim/private/*` note swallowed 144,850 characters and three
    `PathFromXplm(` call sites, so the COUNT-based assertion below failed while
    every call site was present and correct -- a false alarm that outlived the
    0.9.3 release. String literals hide both spellings too ("http://..."), so the
    scan has to know about them as well, and comments are recognized BEFORE
    quotes are, or an apostrophe in English prose opens a string.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        c = text[i]
        if c == '"' or c == "'":
            # A raw string R"(...)" ends at the matching )" -- screens.cpp has six.
            if c == '"' and i and text[i - 1] == "R":
                j = text.find(')"', i + 1)
                if j >= 0:
                    out.append(text[i:j + 2])
                    i = j + 2
                    continue
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                ended = text[i] == c or text[i] == "\n"  # unterminated: stop at EOL
                i += 1
                if ended:
                    break
            continue
        out.append(c)
        i += 1
    return "".join(out)


CORE_CPP = sorted((NATIVE / "core").glob("*.cpp"))
INSTALLER_CPP = sorted((NATIVE / "installer").glob("*.cpp"))
FSUTIL_H = _read(NATIVE / "core" / "fsutil.h")
FSUTIL_CPP = _read(NATIVE / "core" / "fsutil.cpp")
ACTIONS_CPP = _read(NATIVE / "core" / "actions.cpp")
DETECT_CPP = _read(NATIVE / "core" / "detect.cpp")
GUI_CPP = _read(NATIVE / "installer" / "gui.cpp")
CLI_CPP = _read(NATIVE / "installer" / "cli.cpp")
SCREENS_CPP = _read(NATIVE / "installer" / "screens.cpp")
PLUGIN_CPP = _read(NATIVE / "plugin.cpp")


class FsutilOwnsTheConversion(unittest.TestCase):
    """One place turns a path into text. Everything else calls it."""

    def test_the_generic_spelling_is_declared_and_implemented_in_fsutil(self):
        self.assertIn("std::string PathToUtf8Generic(const fs::path& p);", FSUTIL_H)
        self.assertIn("std::string PathToUtf8Generic(const fs::path& p) {", FSUTIL_CPP)
        # The Windows branch goes through the wide string, never a narrow one.
        impl = FSUTIL_CPP.split("std::string PathToUtf8Generic(const fs::path& p) {", 1)[1]
        impl = impl.split("}", 1)[0]
        self.assertIn("generic_wstring()", impl)

    def test_list_files_recursive_spells_rel_in_utf8(self):
        self.assertIn("entry.rel = PathToUtf8Generic(rel);", _code(FSUTIL_CPP))
        self.assertNotIn("entry.rel = rel.generic_string();", FSUTIL_CPP)

    def test_no_code_page_narrowing_outside_fsutil(self):
        """`.string()` / `.generic_string()` on a path are the two calls that
        narrow through the ANSI code page on MSVC. fsutil.cpp is the only file
        allowed to spell them, and only on its non-Windows branches."""
        for f in CORE_CPP + INSTALLER_CPP:
            if f.name == "fsutil.cpp":
                continue
            code = _code(_read(f))
            self.assertNotRegex(code, r"\.generic_string\(\)",
                                "%s narrows a path through generic_string()" % f.name)
            self.assertNotRegex(code, r"\.string\(\)",
                                "%s narrows a path through string()" % f.name)


class TheTwoScansSpellPathsInUtf8(unittest.TestCase):
    def test_the_livery_scan_uses_the_generic_helper(self):
        self.assertIn("const std::string relPosix = fsutil::PathToUtf8Generic(rel);",
                      _code(ACTIONS_CPP))

    def test_detect_describes_the_folder_in_utf8(self):
        self.assertIn("fsutil::PathToUtf8Generic(rel)", _code(DETECT_CPP))


class UninstallDoesNotResurrectALivery(unittest.TestCase):
    """The restore loop checks the livery FOLDER before writing into it, and what
    it keeps stays explained."""

    def test_restore_is_gated_on_the_livery_folder(self):
        body = _code(ACTIONS_CPP)
        # The LAST loop over liveriesPatched is the uninstall's restore; the
        # progress plan and the install scan walk the same list earlier on.
        i = body.rindex("m.interior.liveriesPatched) {")
        loop = body[i:i + 3000]
        self.assertIn("const fs::path liveryDir = dest.parent_path().parent_path();", loop)
        self.assertRegex(loop, r"if \(!fs::is_directory\(liveryDir, le\) \|\| le\) \{")
        self.assertIn("++keptLiveryBackups;", loop)

    def test_the_readme_stays_with_a_kept_backup(self):
        self.assertIn("if (keptLiveryBackups == 0) fs::remove(backupDir / backup::kReadmeName, ec);",
                      _code(ACTIONS_CPP))


class DetectionIsGuardedInEveryFrontEnd(unittest.TestCase):
    """A scan that throws is reported where the user is, never left to propagate
    out of a UI callback."""

    def test_the_gui_never_calls_detect_aircraft_bare(self):
        code = _code(GUI_CPP)
        self.assertIn("std::vector<detect::Aircraft> DetectAircraftOrReport(Gui& g)", code)
        self.assertNotIn("g.aircraft = detect::DetectAircraft(", code)
        self.assertEqual(code.count("g.aircraft = DetectAircraftOrReport(g);"), 2)

    def _call_is_inside_try(self, code: str, name: str):
        i = code.index("detect::DetectAircraft(")
        before = code[max(0, i - 400):i]
        self.assertIn("try {", before, "%s calls DetectAircraft outside a try" % name)
        after = code[i:i + 1200]
        self.assertIn("catch (const std::exception& e)", after,
                      "%s does not catch what DetectAircraft throws" % name)

    def test_the_cli_reports_a_failed_scan_as_ok_false(self):
        self._call_is_inside_try(_code(CLI_CPP), "cli.cpp")
        self.assertIn('out["error"] = std::string("could not scan the Aircraft folder: ") + e.what();',
                      CLI_CPP)

    def test_the_tui_goes_back_to_the_directory_screen(self):
        self._call_is_inside_try(_code(SCREENS_CPP), "screens.cpp")


class ThePluginConvertsXplmPathsOnce(unittest.TestCase):
    """Every path X-Plane hands over crosses ONE function on the way in, and one on
    the way back out to a log line."""

    def test_the_helpers_exist(self):
        self.assertIn("static fs::path PathFromXplm(const char* utf8)", PLUGIN_CPP)
        self.assertIn("static std::string PathStr(const fs::path& p)", PLUGIN_CPP)
        self.assertIn('#include "core/fsutil.h"', PLUGIN_CPP)

    def test_no_fs_path_is_built_from_an_xplm_buffer(self):
        """`char root[512]` / `char path[512]` / the plugin-info path: the buffers
        XPLMGetSystemPath, XPLMGetNthAircraftModel and XPLMGetPluginInfo fill."""
        code = _code(PLUGIN_CPP)
        self.assertNotRegex(code, r"fs::path\((root|path|fileName)\)")
        self.assertNotRegex(code, r"fs::path\s+\w+\((root|path|fileName)\);")
        self.assertGreaterEqual(code.count("PathFromXplm("), 12)

    def test_every_xplm_path_call_has_a_conversion_in_its_function(self):
        """Weaker than the regex above but forward-looking: a NEW caller of one
        of the three path APIs that builds a path has to route it through
        PathFromXplm, and `std::string(path)` used purely as a KEY is allowed."""
        code = _code(PLUGIN_CPP)
        for api in ("XPLMGetSystemPath(", "XPLMGetNthAircraftModel(", "XPLMGetPluginInfo("):
            for m in re.finditer(re.escape(api), code):
                window = code[m.end():m.end() + 800]
                uses_path = re.search(r"fs::path\b|/ \"", window) is not None
                if uses_path:
                    self.assertIn("PathFromXplm(", window,
                                  "a %s caller builds a path without PathFromXplm" % api)

    def test_no_path_string_narrowing_in_the_plugin(self):
        code = _code(PLUGIN_CPP)
        self.assertNotRegex(code, r"\.string\(\)")
        self.assertNotRegex(code, r"\.generic_string\(\)")


if __name__ == "__main__":
    unittest.main()
