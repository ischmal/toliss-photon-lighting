"""What the plugin does when its own install has been taken off the aircraft.

⚠ THE STATE THIS COVERS. A ToLiss update, or a user reinstalling the aircraft,
restores the stock `objects/` tree — which takes every OBJ Photon wrote with it.
The plugin's menu went on offering nine exterior categories that no longer bound
to anything, so every choice in it did nothing and looked broken; meanwhile the
two features that do NOT live in an OBJ (the waveform engine and the panel-FX
compositor) went on working perfectly, reshaping the strobes and tinting the
displays of an aircraft the user may well have reinstalled precisely to be rid of
modifications.

So: `gInstallStale`, detected from the exterior OBJ, disables everything and
replaces the menu with one row that says why.

Nearly every failure here is silent in the same two directions the aircraft gate
already fails in — a detector that says "stale" too readily switches a working
add-on off and tells the user to reinstall their aeroplane, and one that never
says it leaves the original bug in place. The fail-open rule (`kObjUnreadable`)
is the whole reason the first of those cannot happen on a bad read.

Companion file: `test_aircraft_gate.py`, which owns the per-frame gate itself and
the teardown. This one owns detection, the menu, the tabs and the two new
user-facing switches.
"""
from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
if str(BUILD) not in sys.path:
    sys.path.insert(0, str(BUILD))
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

import build_objs as B  # noqa: E402

PLUGIN_CPP = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
    encoding="utf-8", errors="replace")
CONSTANTS_H = (REPO / "src" / "native" / "src" / "core" / "constants.h").read_text(
    encoding="utf-8", errors="replace")

# Every airframe with an exterior target, and the a339 among them deliberately:
# its OBJ has a different NAME and a different content, and the needle has to
# hold for both or the A330-900 reads as permanently reverted.
EXT_AIRFRAMES = ("a319", "a320", "a321", "a339")


def _enclosing_guards(needle, text=PLUGIN_CPP):
    """Every `#if...` still open at the line `needle` starts on. A grep cannot
    answer "is this inside PHOTON_DEV" — the file nests conditionals — and the
    question matters: a reload reachable from a release is a shipped crash."""
    stack = []
    for line in text.splitlines():
        t = line.strip()
        if re.match(r"#\s*if", t):
            stack.append(t)
        elif re.match(r"#\s*endif", t) and stack:
            stack.pop()
        if t.startswith(needle):
            return stack
    raise AssertionError("%r is not in the file at all" % needle)


def _cpp_string(name, text=CONSTANTS_H):
    """A `constexpr char name[] = "..."` value out of a header, ignoring the
    comment lines that document it (this header quotes its own constants)."""
    for line in text.splitlines():
        if line.lstrip().startswith("//"):
            continue
        m = re.search(rf'\b{name}\[\]\s*=\s*"((?:[^"\\]|\\.)*)"', line)
        if m:
            return m.group(1)
    raise AssertionError(f"{name} not found in core/constants.h")


def _fn(name, text=PLUGIN_CPP):
    """A function body, signature to the first `}` in column 0. Same shape as
    test_aircraft_gate's — the signature must carry a `{` so a forward
    declaration cannot match first.

    ⚠ The `{` may be followed by a trailing comment on the same line
    (CreatePhotonMenu's explains why it is not called CreateMenu), so this is
    looser than test_aircraft_gate's version, which anchors on the newline."""
    m = re.search(r"\nstatic [\w:]+ %s\([^)]*\)\s*\{[^\n]*\n.*?\n\}"
                  % re.escape(name), text, re.S)
    assert m, "%s moved or was renamed" % name
    return m.group(0)


def _code(body):
    """`body` with // comments stripped — so an assertion cannot be satisfied by
    a sentence in a comment that merely mentions the symbol."""
    return "\n".join(line.split("//")[0] for line in body.splitlines())


class NeedleTests(unittest.TestCase):
    """⚠ A CONTRACT ACROSS THE BUILD AND THE PLUGIN, exactly like the interior
    needle's: the DSL decides what the OBJ says and C++ decides what to look for,
    and drift shows up only in-sim. Here it shows up as the WORST possible
    symptom — every airframe reading as reverted, so the add-on switches itself
    off on a perfectly good install and tells the user to reinstall."""

    def test_every_generated_exterior_obj_carries_the_needle(self):
        needle = _cpp_string("kExteriorObjNeedle")
        cfg = B.load_config(wing="stock")
        for airframe in EXT_AIRFRAMES:
            text = B.Emitter(cfg, airframe, "stock", target="exterior").emit()
            self.assertIn(needle, text,
                          "%s's exterior OBJ does not bind %s - the plugin would "
                          "read it as a reverted install" % (airframe, needle))

    def test_the_frozen_goldens_carry_it_too(self):
        """The generated OBJs above are what a fresh build emits. These are what
        actually shipped, and a needle that only holds for new builds would read
        every already-installed aircraft as reverted."""
        needle = _cpp_string("kExteriorObjNeedle")
        goldens = sorted((REPO / "reference" / "photon").glob("lights_out*.obj"))
        self.assertTrue(goldens, "the exterior goldens moved")
        for path in goldens:
            self.assertIn(needle, path.read_text(encoding="utf-8", errors="replace"),
                          "%s does not bind %s" % (path.name, needle))

    def test_the_plugin_reads_the_shared_constant_rather_than_its_own(self):
        """A literal creeping back into plugin.cpp compiles fine and quietly
        reintroduces the drift — the same regression guard the interior needle
        carries."""
        self.assertIn("photon::kExteriorObjNeedle", PLUGIN_CPP)
        self.assertNotRegex(
            PLUGIN_CPP,
            r'kExteriorObjNeedle\s*=\s*"',
            "plugin.cpp defines its own copy of the exterior needle")

    def test_the_needle_is_not_the_version_marker(self):
        """⚠ The test is the OBJ BINDING our datarefs, never the installer's
        version marker. A `build_objs.py build --write` OBJ carries no marker and
        drives the plugin perfectly, so a marker test would call a developer's
        working install reverted."""
        marker = _cpp_string("kMarkerPrefix")
        detect = _code(_fn("DetectInstall"))
        self.assertNotIn("kMarkerPrefix", detect)
        self.assertNotIn(marker, detect)


class ScanTests(unittest.TestCase):
    """⚠ THREE ANSWERS, NOT TWO, and the third is the whole safety margin. A file
    we opened and read is evidence; one we could not stat or open is not, and
    answering "no" for it would turn a transient read error into an add-on that
    switched itself off and told the user to reinstall their aircraft."""

    def test_every_unreadable_path_returns_the_unreadable_answer(self):
        body = _code(_fn("ScanObj"))
        # Every BARE `return kObj...` is a guard, and every guard says "could
        # not tell". kObjModded and kObjStock are only ever reached through the
        # ternary below, which is the one place the file has actually been read.
        bare = re.findall(r"return\s+(kObj\w+)\s*;", body)
        self.assertGreaterEqual(len(bare), 3,
                                "ScanObj lost its guards: %s" % bare)
        self.assertEqual(set(bare), {"kObjUnreadable"},
                         "a failure path answers something other than "
                         "'could not tell': %s" % bare)
        self.assertRegex(
            body,
            r"find\(needle\)\s*!=\s*std::string::npos\s*\?\s*kObjModded\s*:\s*kObjStock")

    def test_only_a_read_stock_file_marks_the_install_stale(self):
        """The fail-open rule stated once, where it is decided."""
        body = _code(_fn("DetectInstall"))
        self.assertRegex(body, r"gInstallStale\s*=\s*ext == kObjStock;")

    def test_the_cap_is_a_named_constant_shared_by_both_scans(self):
        self.assertRegex(PLUGIN_CPP, r"static const size_t kObjScanMaxBytes\s*=")
        self.assertIn("size > kObjScanMaxBytes", PLUGIN_CPP)


class DetectTests(unittest.TestCase):
    """One walk of objects/ answers four questions, and each of them has a value
    that changes nothing — which is what every early return has to leave behind."""

    def test_every_flag_is_reset_before_the_first_early_return(self):
        """⚠ A detector that returns early having cleared only some of its
        outputs carries the LAST aircraft's answer onto this one. gInstallStale
        is the dangerous direction: kept true, the add-on stays dark on an
        aircraft nobody has looked at yet."""
        body = _code(_fn("DetectInstall"))
        head = body[:body.index("return;")]
        for flag in ("gAirframeKey.clear();",
                     "gInteriorInstalled = false;",
                     "gInteriorSupported = false;",
                     "gInstallStale      = false;"):
            self.assertIn(flag, head,
                          "%s is not reset before the first early return" % flag)

    def test_an_unrecognized_toliss_is_not_stale(self):
        """⚠ NOT THE SAME STATE. A ToLiss we have no row for — an A340, say — was
        never installed on and had nothing reverted. Calling that stale would put
        "Reinstall required..." on an aircraft the installer has never heard of
        and cannot help with."""
        body = _code(_fn("DetectInstall"))
        guard = re.search(r"if \(gAirframeKey\.empty\(\)\) \{(.*?)\n    \}",
                          body, re.S)
        self.assertIsNotNone(guard, "the unrecognized-airframe branch moved")
        self.assertNotIn("gInstallStale = true", guard.group(1))
        self.assertIn("return;", guard.group(1))

    def test_the_airframe_comes_from_the_fingerprint_object(self):
        """Airframe::objName is a FINGERPRINT (core/constants.h), so the exterior
        OBJ that is present both names the aeroplane and is the file whose
        contents answer whether we are still installed on it. One read, both
        answers.

        ⚠ Since 2026-08-16 the ICAO is read too — but ONLY to corroborate a row
        whose fingerprint is generic (Airframe::acfIcao, the a339's
        ExternalLights_XP12.obj, which an A340 also carries). It must stay a
        guarded veto, never the identifier: only a POSITIVE mismatch may reject,
        because DetectInstall runs once per load with no 1 Hz retry, and an
        empty read rejecting would keep a genuine A339 dark all session."""
        body = _code(_fn("DetectInstall"))
        self.assertIn("photon::Airframes()", body)
        self.assertIn("af.objName", body)
        # the corroboration: gated on the row asking for it, and fail-open on
        # an empty read — the same "evidence may disable, its absence may not"
        # rule as ScanObj's kObjUnreadable.
        self.assertIn("af.acfIcao[0] != '\\0'", body)
        self.assertIn("!icao.empty()", body)

    def test_presence_and_content_are_two_separate_questions(self):
        """⚠ Folding them into one scan would make "present but unreadable"
        indistinguishable from "a different airframe", and the loop would walk on
        to try the other three — ending with no airframe at all on an aircraft we
        do support."""
        body = _code(_fn("DetectInstall"))
        loop = body[body.index("for (const photon::Airframe&"):]
        self.assertLess(loop.index("is_regular_file"), loop.index("ScanObj"),
                        "the airframe is identified by content, not presence")

    def test_the_interior_answer_stays_honest_through_a_stale_install(self):
        """The cockpit OBJ is a separate file and may well have survived. Nothing
        acts on the flag while stale — CockpitTabVisible and the submenu both ask
        gInstallStale first — but a log line saying "cockpit mod NOT installed"
        about an aircraft that still has it sends the next investigation the
        wrong way."""
        body = _code(_fn("DetectInstall"))
        self.assertLess(body.index("gInteriorInstalled = ScanObj"),
                        body.index("if (gInstallStale) {"),
                        "the cockpit scan is skipped on a stale install")

    def test_the_interior_availability_flag_comes_from_the_shared_table(self):
        """`gInteriorSupported` is the A330-900 exclusion, and that list lives in
        core/constants.h where the installer reads it too. A second copy here is
        how the plugin comes to offer a mod the installer will not install."""
        body = _code(_fn("DetectInstall"))
        self.assertIn("photon::AirframeHasInterior(gAirframeKey)", body)


class MenuTests(unittest.TestCase):
    """A stale install gets a DIFFERENT menu, not a grayed version of the usual
    one. Every ordinary row either selects a look the aircraft's OBJs can no
    longer show or opens a page of controls the plugin has just switched off."""

    def _stale_branch(self):
        body = _fn("CreatePhotonMenu")
        m = re.search(r"if \(gMenuStale\) \{(.*?)\n    \}", body, re.S)
        self.assertIsNotNone(m, "the stale menu branch moved")
        return m.group(1)

    def test_the_stale_menu_is_one_row_and_about(self):
        """⚠ ABOUT, NOT SETTINGS, and this is the ONE row where the two menus
        differ. The ordinary menu swapped its About row for Settings (2026-08-24)
        because About is a page nobody needs twice while Settings had no way in
        at all — but a reverted install has no Settings TAB, since it goes dark
        with the axis tabs, so the same swap here would point at nothing."""
        stale = _code(self._stale_branch())
        self.assertIn('"Reinstall required..."', stale)
        self.assertIn('"About..."', stale)
        self.assertNotIn('"Settings..."', stale)
        self.assertIn("XPLMAppendMenuSeparator(gMenuID);", stale)
        for gone in ('"Exterior"', '"Cockpit"', '"Displays..."'):
            self.assertNotIn(gone, stale,
                             "%s is still offered on a reverted install" % gone)

    def test_the_ordinary_menu_offers_settings_under_the_divider(self):
        """The row below the separator is the one that is not an axis. It opens
        the Settings TAB, which is otherwise reachable only by opening the window
        on some other errand and noticing the tab is there."""
        body = _code(_fn("CreatePhotonMenu"))
        ordinary = body[body.index('XPLMAppendMenuItem(gMenuID, "Exterior"'):]
        self.assertRegex(ordinary,
                         r'"Settings\.\.\.",\s*\(void\*\)\(intptr_t\)kTabSettings')
        self.assertNotIn('"About..."', ordinary,
                         "About is a tab now, not a menu row")

    def test_the_row_opens_the_error_tab(self):
        """It is the only place the reason is written down, so the row that
        replaces every other one has to lead there."""
        self.assertRegex(_code(self._stale_branch()),
                         r'"Reinstall required\.\.\.",\s*\n?\s*'
                         r'\(void\*\)\(intptr_t\)kTabError')

    def test_the_stale_branch_returns_before_the_ordinary_menu(self):
        body = _code(_fn("CreatePhotonMenu"))
        self.assertLess(body.index("if (gMenuStale) {"),
                        body.index('XPLMAppendMenuItem(gMenuID, "Exterior"'),
                        "the ordinary rows are appended on a stale install too")
        self.assertIn("return;", self._stale_branch())

    def test_the_stale_branch_still_appends_the_dev_menu(self):
        """The tooling is not what the aircraft update broke, and tuning on a
        reverted install is a reasonable thing to be doing. It is also a no-op
        stub in a shipping build, so this costs a release nothing."""
        self.assertIn("AppendDebugMenu(gMenuID);", _code(self._stale_branch()))

    def test_the_menu_records_which_shape_it_was_built_in(self):
        """⚠ The rebuild check compares this against the live flag on every
        aircraft load. Without it, switching from a stale A320 to a good A321
        keeps whichever menu was up."""
        self.assertRegex(PLUGIN_CPP, r"static bool\s+gMenuStale = false;")
        self.assertIn("gMenuStale = gInstallStale;", PLUGIN_CPP)
        vis = _code(_fn("UpdateMenuVisibility"))
        self.assertIn("gMenuStale != gInstallStale", vis)

    def test_the_stale_menu_states_that_it_has_no_cockpit_submenu(self):
        """⚠ gMenuHasInterior is stated FALSE rather than left over. It is
        compared against gInteriorInstalled on every load, and that flag stays
        honest through a stale install — so left at whatever the last build set,
        a stale A320 with an intact lights_inn.obj would tear its menu down and
        put it back on every single aircraft load."""
        body = _code(_fn("CreatePhotonMenu"))
        self.assertLess(body.index("gMenuHasInterior = false;"),
                        body.index("if (gMenuStale) {"))
        # ...and the rebuild check does not ask the interior question of a menu
        # that has no interior half.
        self.assertIn("(!gInstallStale\n                                 "
                      "&& gMenuHasInterior != gInteriorInstalled)",
                      _fn("UpdateMenuVisibility"))


class TabTests(unittest.TestCase):
    """The Error tab REPLACES the three axis tabs rather than joining them."""

    def test_the_error_tab_leads_the_enum(self):
        """⚠ The enum order IS the submission order — gWantTab is matched by the
        BeginTabItem that carries the same value, so an enum that disagrees with
        BuildMainUi opens the window on the wrong tab."""
        m = re.search(r"enum \{ (kTab\w+(?:\s*=\s*0)?(?:,\s*kTab\w+)*)\s*\};",
                      PLUGIN_CPP)
        self.assertIsNotNone(m, "the tab enum moved")
        names = [n.split("=")[0].strip() for n in m.group(1).split(",")]
        self.assertEqual(names,
                         ["kTabError", "kTabExterior", "kTabCockpit",
                          "kTabDisplays", "kTabSettings", "kTabAbout",
                          "kTabHelp", "kTabCount"])
        order = [t for t in re.findall(r"TabFlags\((kTab\w+)\)",
                                       _fn("BuildMainUi"))]
        self.assertEqual(order, names[:-1],
                         "BuildMainUi submits its tabs in a different order than "
                         "the enum declares them")

    def test_the_axis_tabs_are_gone_while_the_error_tab_is_up(self):
        """Grayed beside the explanation, they invite being tried; absent, the
        explanation is the page.

        ⚠ Settings goes with them, not with About. Every switch on it either lets
        Photon drive something on this aircraft or turns an effect off to buy
        frames back, and on a reverted install nothing is being driven and nothing
        is being drawn."""
        body = _fn("BuildMainUi")
        branch = re.search(r"if \(gInstallStale\) \{(.*?)\n    \} else \{(.*?)\n    \}",
                           body, re.S)
        self.assertIsNotNone(branch, "BuildMainUi's stale branch moved")
        stale, ordinary = _code(branch.group(1)), _code(branch.group(2))
        self.assertIn('"Error"', stale)
        for gone in ('"Exterior"', '"Cockpit"', '"Displays"', '"Settings"'):
            self.assertNotIn(gone, stale)
            self.assertIn(gone, ordinary)

    def test_about_and_help_survive_both_shapes(self):
        """The two tabs that are always there — and the stale menu's second row
        points at About, so it cannot live inside either branch.

        ⚠ Help is in the same position for a stronger reason than symmetry: a
        reverted install is the state a user is most likely to be reporting FROM,
        and the support report saved on that tab is the one artifact that can
        explain how they got there. A Help tab that vanished exactly when things
        broke would be the least useful possible arrangement."""
        body = _fn("BuildMainUi")
        branch = re.search(r"if \(gInstallStale\) \{.*?\n    \} else \{.*?\n    \}",
                           body, re.S)
        after = body[branch.end():]
        self.assertIn('BeginTabItem("About"', after)
        self.assertIn('BeginTabItem("Help"', after)

    def test_help_is_last_in_the_bar(self):
        """It is a support page, not a landing page: at the head of the bar it
        would be the first thing a working install shows you."""
        order = re.findall(r'BeginTabItem\("(\w+)"', _fn("BuildMainUi"))
        self.assertEqual(order[-1], "Help")

    def test_the_error_tab_says_all_three_things(self):
        """What happened, what to do, and how to stop being asked. It is the only
        screen in the plugin whose job is to be read once and acted on somewhere
        else, so a missing third makes the second unanswerable."""
        body = _fn("BuildErrorTab")
        self.assertIn("has been disabled due to an aircraft update", body)
        self.assertIn("run the installer for your", body)
        self.assertIn("uninstall it using the Installer program", body)
        self.assertIn("remove the folder", body)
        self.assertIn("OpenUrl(kOrgUrl)", _code(body))

    def test_the_folder_to_delete_is_built_from_the_shared_constant(self):
        """⚠ It is an instruction to DELETE something, so it must name the folder
        the installer actually writes. A typed-out path is one rename away from
        telling a user to remove the wrong directory."""
        body = _code(_fn("PluginFolderHint"))
        self.assertIn("photon::kPluginFolder", body)
        # Both separators, because this string is read off the screen and typed
        # into a file manager.
        self.assertIn(r'"Resources\\plugins\\"', body)
        self.assertIn('"Resources/plugins/"', body)


class CockpitTabTests(unittest.TestCase):
    """⚠ THREE STATES, NOT TWO (2026-08-14). The tab used to appear only where
    the cockpit mod did, on the reading that a present-but-inert tab says "the
    mod is broken" while an absent one says "not installed". True as far as it
    goes — but an absent tab says nothing about how to CHANGE the answer, and the
    cockpit mod is an opt-in on an installer screen the user has already clicked
    past once."""

    def test_the_tab_is_shown_where_the_mod_is_merely_available(self):
        body = _code(_fn("CockpitTabVisible"))
        self.assertIn("gInteriorInstalled", body)
        self.assertIn("gInteriorSupported", body)
        self.assertIn("!gInstallStale", body,
                      "a reverted install still offers a Cockpit tab")

    def test_an_airframe_without_the_mod_gets_no_tab(self):
        """The A330-900: Gus's mod does not cover it, so there is nothing to tell
        the user to install and a disabled tab would be an offer we cannot keep.
        The list is core/constants.h's, shared with the installer."""
        sys.path.insert(0, str(BUILD))
        import constants as K  # noqa: E402  (build/constants.py)
        self.assertNotIn("a339", K.INTERIOR_AIRFRAMES)

    def test_the_inert_tab_states_the_reason_and_the_fix(self):
        body = _fn("BuildConfigTab")
        self.assertIn("Modified cockpit lighting is not installed", body)
        self.assertIn("Cockpit Lighting by Gus Rodrigues", body)

    def test_everything_on_the_inert_tab_is_disabled(self):
        """The whole pane, from the profile dropdown to the last row of the
        grid — there is nothing else on it since the Performance section moved to
        the Settings tab."""
        body = _code(_fn("BuildConfigTab"))
        self.assertRegex(body, r"const bool inert = w\.interior && !gInteriorInstalled;")
        self.assertIn("ImGui::BeginDisabled(inert);", body)
        self.assertLess(body.index("ImGui::BeginDisabled(inert);"),
                        body.index("BuildProfileCombo(w);"))
        self.assertLess(body.index("BuildCategoryGrid(w, inert);"),
                        body.index("ImGui::EndDisabled();"))

    def test_the_disabled_stack_is_balanced_on_every_path(self):
        """⚠ The grid's early `return` on a failed BeginTable is why it is its
        own function now: a return between BeginDisabled and EndDisabled leaves
        ImGui's disabled stack unbalanced for every window drawn after it, which
        is a whole-UI failure reported from somewhere else entirely."""
        body = _code(_fn("BuildConfigTab"))
        self.assertEqual(body.count("ImGui::BeginDisabled(inert);"), 1)
        self.assertEqual(body.count("ImGui::EndDisabled();"), 1)
        between = body[body.index("ImGui::BeginDisabled(inert);"):
                       body.index("ImGui::EndDisabled();")]
        self.assertNotIn("return", between,
                         "a return between BeginDisabled and EndDisabled")
        # ...and the grid it delegates to still has one, which is the point.
        self.assertIn("return;", _code(_fn("BuildCategoryGrid")))

    def test_the_grid_is_told_whether_the_tab_is_inert(self):
        """Rather than asking gInteriorInstalled a second time. One caller with
        two answers to the same question is how the hint at the top and the
        controls under it come apart."""
        self.assertIn("BuildCategoryGrid(w, inert);", _code(_fn("BuildConfigTab")))
        self.assertIn("!inert && UiRadioHovered", _code(_fn("BuildCategoryGrid")))

    def test_the_submenu_is_still_installed_only(self):
        """Not the same test as the tab's. A menu ROW that opens a page of
        disabled controls is a worse thing than a tab that is honest about being
        disabled once you are already looking at it."""
        body = _code(_fn("CreatePhotonMenu"))
        self.assertIn("gMenuHasInterior = gInteriorInstalled;", body)
        self.assertNotIn("gInteriorSupported", body)


class SettingsTabTests(unittest.TestCase):
    """The Settings tab (2026-08-15) — the plugin's own switches, as against the
    three tabs before it, which choose how a light LOOKS. Two sections: what
    Photon may COST, and how much of the aeroplane it may DRIVE.

    Both used to hang off whichever axis tab their subject was nearest, where each
    was the one control on its page not answering "which era?"."""

    def test_the_sections_are_there_and_none_of_them_is_collapsed(self):
        """⚠ ADVANCED IS AN ORDINARY SECTION (2026-08-24), not a CollapsingHeader.
        It was collapsed by default on the reading that "how much may the plugin
        change?" has no obvious right answer and should not lead the page. What
        that bought was a page whose last section was invisible until clicked —
        and both things under it are things a user comes to this tab to find: the
        override you switch off to troubleshoot, and the reload you press
        afterwards."""
        body = _fn("BuildSettingsTab")
        self.assertIn('ImGui::SeparatorText("Performance")', body)
        self.assertIn('ImGui::SeparatorText("Advanced")', body)
        self.assertNotIn("CollapsingHeader", _code(body),
                         "nothing on this page hides behind a click")

    def test_advanced_points_at_XPLANES_reload_and_offers_none_of_its_own(self):
        """⚠ PHOTON CANNOT RELOAD THE AIRCRAFT AT ALL, from any context it has, and
        this page is where that keeps being forgotten. It was a "Reload
        aircraft..." button with a modal, then a Photon menu row; both crashed the
        simulator, and the second crash is the one that closes the question:

            Fault Module Name = AirbusFBW_A320_XP11.xpl_unloaded   (WER BEX64)
            Exception Code    = c0000005

        — X-Plane executed a draw callback belonging to the PREVIOUS, unloaded copy
        of ToLiss's plugin. The log names the leak, identically on both reloads, at
        the same record address and creation time: `XPLMDrawCallbackRecord ... is
        being deleted by a different plugin. - previously created by AirbusFBW`.
        The teardown runs on OUR stack, so X-Plane refuses AirbusFBW's own cleanup
        and then unloads the module under a record that is still registered. It is
        CUMULATIVE, which is the "worked once, crashed the second time" signature,
        and no choice of callback context changes who is on the stack."""
        body = _code(_fn("BuildSettingsTab"))
        self.assertLess(body.index('SeparatorText("Advanced")'),
                        body.index("Developer menu"))
        self.assertIn("next time this aircraft is loaded", body)
        for gone in ("BuildReloadRow", "PostInputAction", "XPLMCommandOnce",
                     "reload_aircraft", "IssueAircraftReload"):
            self.assertNotIn(gone, body, "%s is back on a user-facing page" % gone)

    def test_the_only_reload_left_is_dev_only(self):
        """It stays for the dev loop, which has always reloaded this way and can
        spend the hazard knowingly. ⚠ A shipping build must have no path to it —
        the row is appended by AppendDebugMenu, which is an empty stub without
        PHOTON_DEV, and IssueAircraftReload is defined inside that block."""
        self.assertEqual(1, PLUGIN_CPP.count('"sim/operation/reload_aircraft"'))
        # Its one caller is the Dev submenu's handler.
        callers = [ln.strip() for ln in PLUGIN_CPP.splitlines()
                   if "IssueAircraftReload()" in ln and "static void" not in ln]
        self.assertEqual(["IssueAircraftReload();"], callers)
        self.assertIn("#if PHOTON_DEV", _enclosing_guards(
            "static void IssueAircraftReload"),
            "the reload escaped the PHOTON_DEV guard and is in a release")
        # ...and the shipping menu has no row of its own.
        menu = _code(_fn("CreatePhotonMenu"))
        self.assertNotIn("Reload aircraft", menu)
        self.assertNotIn("kMenuReload", PLUGIN_CPP)

    def test_the_backend_offers_no_way_to_defer_out_of_a_draw_callback(self):
        """PostInputAction stashed a function for the window's next mouse/cursor/
        key callback to run, on the reading that a window INPUT callback is the
        same dispatch class as a menu handler. It is not — it is a third mutated
        list. Deleted rather than left unused: a helper whose whole documented
        purpose is the thing that crashes is one someone reaches for again."""
        for name in ("imgui_xplm.h", "imgui_xplm.cpp"):
            text = (REPO / "src" / "native" / "src" / name).read_text(
                encoding="utf-8", errors="replace")
            self.assertNotIn("void PostInputAction", text)
            self.assertNotIn("InputActionDrain", text)
        # The lesson stays where the next attempt would be written.
        hdr = (REPO / "src" / "native" / "src" / "imgui_xplm.h").read_text(
            encoding="utf-8", errors="replace")
        self.assertIn("out == theWindow->visible", hdr)

    def test_the_axis_tabs_no_longer_carry_either_section(self):
        """⚠ MOVED, not copied. Two live checkboxes on one flag, on two tabs, is
        a setting that appears to forget itself depending on where it was last
        looked at."""
        body = _code(_fn("BuildConfigTab"))
        for gone in ("SetCockpitOptimized", "SetWaveformOverride",
                     'SeparatorText("Performance")', 'SeparatorText("Advanced")',
                     'CollapsingHeader("Advanced")'):
            self.assertNotIn(gone, body)

    def test_the_cockpit_tab_only_POINTS_at_the_brightness_setting(self):
        """⚠ A SENTENCE, NOT A CONTROL. It stores nothing, switches nothing and
        goes nowhere — which is the whole of why it does not break the rule above.
        (It was a button that jumped to the Settings tab, briefly, on 2026-08-24;
        a button is a control, and one under a grid of era choices reads as a
        sixth thing to choose.)"""
        body = _code(_fn("BuildConfigTab"))
        self.assertIn("Cockpit light brightness can be adjusted in the Settings "
                      "tab.", body)
        self.assertNotIn("gWantTab =", body,
                         "the axis tab is navigating rather than pointing")

    def test_the_backlight_row_is_the_inverse_of_the_displays_tabs(self):
        """⚠ DELIBERATE, and both stay. "Do I want the screens to light the
        cockpit?" is a look and belongs on the Displays tab; "may I have that back
        for the frame rate?" is a cost and belongs here, phrased as the thing
        being switched OFF so a tick means cheaper like every other row in the
        section.

        ⚠ Inverted AT THE WIDGET. A second stored flag with the opposite sense is
        how two pages come to disagree about one setting, so the field keeps its
        one meaning and only the checkbox is turned around."""
        body = _code(_fn("BuildSettingsTab"))
        self.assertIn('"Disable screen backlight illumination"', body)
        self.assertIn("bool spillOff = !gDisplays.spill;", body)
        self.assertIn("gDisplays.spill = !spillOff;", body)
        # ...and it reaches the disk, like every other switch that edits a
        # persisted flag straight rather than through a setter.
        self.assertIn("SaveDisplays();", body)
        # The Displays tab still owns the positive form.
        self.assertIn('"Enable backlight illumination"', _fn("BuildDisplaysTab"))

    def test_the_flood_row_is_hidden_without_the_cockpit_mod(self):
        """⚠ Hidden, not disabled — the one place in the plugin that hides rather
        than grays. The Cockpit TAB is shown inert because a whole page has room
        to say how to get the mod; a single row here has nowhere to explain itself
        and would read as a setting that does nothing."""
        body = _code(_fn("BuildSettingsTab"))
        m = re.search(r"if \(gInteriorInstalled\) \{(.*?)\n    \}", body, re.S)
        self.assertIsNotNone(m, "the simplified-flood row is no longer gated")
        self.assertIn("SetCockpitOptimized", m.group(1))
        self.assertIn('"Use simplified panel flood lights"', m.group(1))

    def test_the_performance_tool_is_reachable_from_here(self):
        """It is a performance section; the tool that measures one belongs in it.
        The Displays tab's button stays — the switches up there are most of what a
        run moves."""
        self.assertIn("ShowPerfWindow();", _code(_fn("BuildSettingsTab")))
        self.assertIn("ShowPerfWindow();", _code(_fn("BuildDisplaysTab")))

    def test_the_dev_window_builds_the_same_pane(self):
        """The Dev window's first tabs call the SHIPPING builders, so a tuning
        pass never has to wonder whether a duplicated copy has drifted. The
        dev-only flood-boost knob is appended after it — outside the shared
        builder, which the release window also builds."""
        body = _code(_fn("BuildDevUi"))
        self.assertRegex(body, r'DevTab\("Settings",\s*\[\] \{ BuildSettingsTab\(\); '
                               r'BuildDevCockpitTuning\(\); \}\);')
        self.assertNotIn("BuildDevCockpitTuning", body.split('DevTab("Settings"')[0])


class WaveformSwitchTests(unittest.TestCase):
    """The Settings tab's Advanced section: "Allow ToLiss Photon to control
    strobe/beacon timing". The two halves of the exterior are separable — color
    comes from the OBJ, timing from the flight loop — and this is the only control
    in the plugin that tells Photon to leave one of them alone."""

    def test_the_control_exists_and_is_worded_as_a_permission(self):
        body = _fn("BuildSettingsTab")
        self.assertIn('ImGui::SeparatorText("Advanced")', body)
        self.assertIn('"Allow ToLiss Photon to control strobe/beacon timing"', body)

    def test_the_default_is_what_the_add_on_has_always_done(self):
        """An upgrade must not silently change the look. A prefs file written
        before this switch existed has no key for it, so the struct default is
        what every existing user gets."""
        m = re.search(r"struct ExteriorSettings \{(.*?)\};", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "ExteriorSettings moved")
        self.assertRegex(m.group(1), r"bool waveform = true;")

    def test_it_persists_globally_beside_the_other_two(self):
        """⚠ NOT per livery. It answers "how much may this plugin change?",
        which is a statement about the user rather than about the paint scheme —
        per livery it would appear to forget itself on every repaint."""
        self.assertRegex(PLUGIN_CPP, r'kExteriorKey = "\$exterior";')
        self.assertIn("if (kv.first == kExteriorKey) { ExteriorFromJson", PLUGIN_CPP)

    def test_the_block_is_always_written(self):
        """"Absent means default" reads an old file correctly but cannot express
        a switch the user has deliberately turned off — the same argument the
        Displays and Cockpit blocks already carry."""
        save = _code(_fn("SaveProfilesFile"))
        self.assertIn('kExteriorKey, gExterior.waveform ? 1 : 0', save)
        # Unconditional: no `if` between the cockpit block and this one.
        tail = save[save.index("kCockpitKey, gCockpit.optimized"):
                    save.index("kExteriorKey, gExterior.waveform")]
        self.assertNotIn("if (", tail)

    def test_one_writer(self):
        """Every writer through the setter, so the field, the prefs file and the
        log can never disagree — the rule SetCockpitOptimized already states.
        The prefs READER is the one other assignment, and it is the same
        exception the other two global blocks carry."""
        owners = []
        for m in re.finditer(r"gExterior\.waveform\s*=(?!=)", PLUGIN_CPP):
            head = PLUGIN_CPP[:m.start()]
            fn = re.findall(r"\nstatic [\w:]+ (\w+)\([^)]*\)\s*\{", head)
            owners.append(fn[-1] if fn else "?")
        self.assertEqual(set(owners), {"SetWaveformOverride", "ExteriorFromJson"},
                         "gExterior.waveform is written somewhere unexpected: %s"
                         % sorted(set(owners)))
        # ⚠ And the UI goes through the setter, not the field: a checkbox bound
        # straight to it would change the look and never reach the disk.
        self.assertIn("SetWaveformOverride(waveform)", _fn("BuildSettingsTab"))

    def test_the_engine_stops_at_the_waveform_work_and_no_earlier(self):
        """⚠ The map spill alpha and the six screen-glow sizes live in the same
        flight loop and are NOT waveform work — they belong to two features this
        switch says nothing about. Gating the whole loop would turn one checkbox
        into three."""
        body = _code(_fn("EngineLoop"))
        gate = body.index("if (!gExterior.waveform) {")
        self.assertLess(body.index("gMapSpillAlpha"), gate)
        self.assertLess(body.index("gScreenSizeFactor"), gate)
        self.assertLess(gate, body.index("gBeaconRatioRef"))
        self.assertLess(gate, body.index("gStrobeRatioRef"))


class GlowMirrorTests(unittest.TestCase):
    """⚠ TURNING THE OVERRIDE OFF MUST NOT BLACK OUT THE SKIN. The install
    repointed the A339's ATTR_light_level regions from ToLiss's
    ExternalLightBrightnesses at ToLissPhoton/exterior/glow — a permanent edit to
    the mesh OBJs that a runtime switch does not undo. An array nobody writes
    reads 0, so those painted strobe and beacon regions would go dark: a dead
    patch on the fuselage, which is a good deal worse than the soft xenon fade
    the user asked to keep."""

    def test_the_off_path_mirrors_toliss_own_array(self):
        body = _code(_fn("EngineLoop"))
        off = body[body.index("if (!gExterior.waveform) {"):]
        self.assertIn("MirrorToLissGlow();", off[:off.index("}")])

    def test_it_mirrors_only_the_indices_we_redirected(self):
        """Anything else in our array is not read by any OBJ, and writing it
        would be inventing values for regions ToLiss still owns."""
        body = _code(_fn("MirrorToLissGlow"))
        self.assertIn("gGlowMap.strobe[i]", body)
        self.assertIn("gGlowMap.beacon[i]", body)
        self.assertIn("kGlowNone", body)

    def test_it_costs_nothing_where_there_is_no_redirect(self):
        """Only the A339 has a glow map, so every other airframe must fall out on
        the first line."""
        body = _code(_fn("MirrorToLissGlow"))
        self.assertRegex(body, r"if \(!gGlowMapActive \|\| !gToLissGlowRef\) return;")

    def test_the_source_handle_is_bound_dropped_and_retried(self):
        """⚠ A ToLiss handle, so all three. Its failure mode is the quietest of
        the four this file has collected: it is only READ while the override is
        off, so an unbound handle costs nothing at all until the day someone
        unticks the box."""
        self.assertIn("ResolveToLissGlow();", _code(_fn("UpdateMenuVisibility")))
        self.assertIn("if (!gToLissGlowRef) ResolveToLissGlow();",
                      _code(_fn("AutoLoop")))
        self.assertIn("gToLissGlowRef = nullptr;",
                      _code(_fn("DropAircraftBindings")))

    def test_the_source_is_the_shared_constant(self):
        """The name of the array the patcher redirects AWAY from — one definition
        in core/constants.h, read by the patcher and by this."""
        self.assertIn("XPLMFindDataRef(photon::kGlowOldDataRef)", PLUGIN_CPP)


class ScreensGateTests(unittest.TestCase):
    """The screens OBJ's master gate answers the stale flag too."""

    def test_a_stale_install_hides_the_screen_glow_block(self):
        """⚠ Not covered by the per-frame gate. This is a dataref an OBJ reads,
        and a stale install normally takes lights_screens.obj with it — but the
        `.acf` attachment and the OBJ are two separate edits, and only one of
        them has to survive for six of our spill lights to still be drawing on an
        aircraft we have decided to leave alone."""
        for fn in ("ReadScreensDisabledInt", "ReadScreensDisabledFloat"):
            m = re.search(r"static \w+ %s\(void\*\) \{([^}]*)\}" % fn, PLUGIN_CPP)
            self.assertIsNotNone(m, "%s moved" % fn)
            self.assertIn("!gInstallStale", m.group(1))


if __name__ == "__main__":
    unittest.main()
