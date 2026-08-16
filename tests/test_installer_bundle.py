"""The two-executable bundle and the black-window remedies (2026-08-15).

A user reported the GUI installer as "only a black screen": the GUI renders
through OpenGL (Slint winit/FemtoVG), and on a machine where GL is present but
broken — Remote Desktop, old drivers, VMs — the window opens and paints nothing.
Two remedies shipped together, and both fail SILENTLY if they regress:

  * the bundle carries `photon-installer-console` beside the GUI — a fallback
    whose absence looks like a complete bundle right up until the one user who
    needs it unzips it;
  * `--software` selects Slint's software renderer, which only works while the
    flag actually reaches the env var Slint reads, through the RIGHT env API.

Everything here is a guard against shipping a bundle that looks right and
strands the black-window user, or a flag that parses and does nothing.
"""
from __future__ import annotations

import pathlib
import re
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
BUILD = REPO / "build"
if str(BUILD) not in sys.path:
    sys.path.insert(0, str(BUILD))

import make_release as MR  # noqa: E402
import readme as README  # noqa: E402

MAIN_CPP = (REPO / "src" / "native" / "src" / "installer" / "main.cpp").read_text(
    encoding="utf-8", errors="replace")
CLI_CPP = (REPO / "src" / "native" / "src" / "installer" / "cli.cpp").read_text(
    encoding="utf-8", errors="replace")
GUI_CPP = (REPO / "src" / "native" / "src" / "installer" / "gui.cpp").read_text(
    encoding="utf-8", errors="replace")
SCREENS_CPP = (REPO / "src" / "native" / "src" / "installer" / "screens.cpp").read_text(
    encoding="utf-8", errors="replace")
WORKFLOW = (REPO / ".github" / "workflows" / "release.yml").read_text(
    encoding="utf-8", errors="replace")


class BinaryClassificationTests(unittest.TestCase):
    """⚠ CONTENT, NEVER LOCATION. Both builds emit `photon-installer[.exe]`, and
    which tree holds which front-end differs between a dev machine (build/ has
    no GUI) and CI (build/ IS the GUI tree). A location rule already shipped the
    wrong front-end once — release.yml's header tells that story."""

    def test_the_marker_is_the_slint_backend_literal(self):
        """The GUI links Slint, whose backend selector embeds the env-var name;
        a PHOTON_GUI=OFF build has no Slint to carry it. If this constant ever
        changes, main.cpp's --software plumbing must change with it — they are
        the same string on purpose."""
        self.assertEqual(MR.GUI_BINARY_MARKER, b"SLINT_BACKEND")
        self.assertIn('"SLINT_BACKEND"', MAIN_CPP)

    def test_classification_reads_bytes_not_names(self):
        with tempfile.TemporaryDirectory() as td:
            gui = pathlib.Path(td) / "photon-installer.exe"
            gui.write_bytes(b"\x00stuff" + MR.GUI_BINARY_MARKER + b"more\x00")
            console = pathlib.Path(td) / "photon-installer"
            console.write_bytes(b"\x00no slint in here\x00")
            self.assertTrue(MR.installer_is_gui(gui))
            self.assertFalse(MR.installer_is_gui(console))

    def test_discovery_skips_a_candidate_of_the_wrong_kind(self):
        """A GUI-only machine asked for the console binary must answer None,
        not hand back the GUI under a fallback's name."""
        with tempfile.TemporaryDirectory() as td:
            base = pathlib.Path(td) / "build"
            base.mkdir()
            exe = base / "photon-installer.exe"
            exe.write_bytes(b"x" + MR.GUI_BINARY_MARKER)
            orig = MR.INSTALLER_BUILD_DIRS
            MR.INSTALLER_BUILD_DIRS = (base,)
            try:
                self.assertEqual(MR.find_installer_binary(gui=True), exe)
                self.assertIsNone(MR.find_installer_binary(gui=False))
            finally:
                MR.INSTALLER_BUILD_DIRS = orig

    def test_the_console_rename_covers_both_platform_spellings(self):
        self.assertEqual(MR.CONSOLE_EXE_NAMES["photon-installer.exe"],
                         "photon-installer-console.exe")
        self.assertEqual(MR.CONSOLE_EXE_NAMES["photon-installer"],
                         "photon-installer-console")


class BundleRefusalTests(unittest.TestCase):
    """⚠ EVERY REFUSAL RUNS BEFORE ANYTHING IS STAGED, so these can be tested
    with two tiny fake binaries and no payload: a refusal that needed the
    payload first would also mean a failed bundle deletes a good staging."""

    def _exe(self, td, name, gui):
        p = pathlib.Path(td) / name
        p.write_bytes((MR.GUI_BINARY_MARKER if gui else b"") + b"\x00binary")
        return p

    def test_a_missing_console_binary_is_fatal(self):
        """The bundle without it looks complete, installs fine, and strands
        exactly the black-window user it exists for."""
        with tempfile.TemporaryDirectory() as td:
            gui = self._exe(td, "photon-installer.exe", gui=True)
            with self.assertRaises(SystemExit) as ctx:
                MR.build_bundle(pathlib.Path(td), pathlib.Path(td) / "payload",
                                gui, None, "bundle", "zip")
            self.assertIn("photon-installer-console", str(ctx.exception))

    def test_a_console_binary_that_contains_the_gui_is_fatal(self):
        """A 'fallback' that links Slint fails exactly when the GUI does."""
        with tempfile.TemporaryDirectory() as td:
            gui = self._exe(td, "photon-installer.exe", gui=True)
            fake = self._exe(td, "photon-installer-console.exe", gui=True)
            with self.assertRaises(SystemExit):
                MR.build_bundle(pathlib.Path(td), pathlib.Path(td) / "payload",
                                gui, fake, "bundle", "zip")

    def test_a_gui_binary_without_slint_is_fatal(self):
        """The mirror image: a bundle whose `photon-installer` is really the
        console build opens a terminal when double-clicked — the exact silent
        wrong-front-end CI's configure-log grep exists for, local edition."""
        with tempfile.TemporaryDirectory() as td:
            fake = self._exe(td, "photon-installer.exe", gui=False)
            console = self._exe(td, "photon-installer-console.exe", gui=False)
            with self.assertRaises(SystemExit):
                MR.build_bundle(pathlib.Path(td), pathlib.Path(td) / "payload",
                                fake, console, "bundle", "zip")


class ReadmeTests(unittest.TestCase):
    """The README is the one place a stuck user looks. The console binary and
    the black-window section have to be IN it, found by the word the user would
    search for, and the install steps must still name exactly one executable."""

    def _render(self, kind):
        ext = ".exe" if kind == "Windows" else ""
        return README.render(kind, exe_name="photon-installer" + ext,
                             arch="win_x64",
                             console_name="photon-installer-console" + ext)

    def test_troubleshooting_names_the_symptom_and_both_remedies(self):
        for kind in ("Windows", "macOS", "Linux"):
            with self.subTest(kind=kind):
                text = self._render(kind)
                self.assertIn("TROUBLESHOOTING", text)
                self.assertIn("BLACK OR BLANK WINDOW", text)
                self.assertIn("photon-installer-console", text)
                self.assertIn("--software", text)

    def test_the_install_steps_name_exactly_one_executable(self):
        """The old one-executable rule's reason, kept: HOW TO INSTALL must not
        hand the user a choice. The console binary appears only after the
        install steps (under TROUBLESHOOTING and WHAT'S INCLUDED)."""
        for kind in ("Windows", "macOS", "Linux"):
            with self.subTest(kind=kind):
                text = self._render(kind)
                steps = text[text.index("HOW TO INSTALL"):
                             text.index("TROUBLESHOOTING")]
                self.assertNotIn("console", steps)

    def test_the_readme_is_pure_ascii(self):
        """Opened in Notepad; a smart quote or arrow mojibakes there."""
        for kind in ("Windows", "macOS", "Linux"):
            with self.subTest(kind=kind):
                self._render(kind).encode("ascii")


class SoftwareFlagTests(unittest.TestCase):
    """`--software` is remedy #2, and every part of it can quietly do nothing:
    a flag that parses but never reaches the env var, an env var set through
    the API Slint does not read, or a build where the flag errors out of the
    support instruction that names it."""

    def test_the_flag_is_parsed_in_the_interactive_branch(self):
        self.assertIn('a == "--software"', MAIN_CPP)

    def test_it_sets_the_env_var_through_both_windows_apis(self):
        """Slint is Rust and reads the OS environment block
        (GetEnvironmentVariableW); our diagnostics read the CRT copy (getenv).
        The two only sync one way at startup, so ONE call leaves the other
        stale — the failure being a --software run that renders on the GPU
        anyway, indistinguishable from the flag working on a healthy machine."""
        self.assertIn('SetEnvironmentVariableW(L"SLINT_BACKEND", L"software")',
                      MAIN_CPP)
        self.assertIn('_putenv_s("SLINT_BACKEND", "software")', MAIN_CPP)
        self.assertIn('setenv("SLINT_BACKEND", "software", 1)', MAIN_CPP)

    def test_it_is_set_before_rungui(self):
        """The backend initializes lazily on first use; an env var set after
        App::create() is a no-op with no error."""
        self.assertLess(MAIN_CPP.index('SetEnvironmentVariableW(L"SLINT_BACKEND"'),
                        MAIN_CPP.index("RunGui(xplaneRoot, dryRun)"))

    def test_a_non_gui_build_accepts_and_ignores_it(self):
        """Same rule as --tui: a support instruction that says "add --software"
        must not error on the build where it is redundant."""
        self.assertIn("(void)forceSoftware;", MAIN_CPP)

    def test_the_usage_text_names_the_flag_and_the_symptom(self):
        self.assertIn("--software", CLI_CPP)
        self.assertIn("BLACK", CLI_CPP)

    def test_the_gui_logs_the_requested_renderer(self):
        """A black-window report's log has to say which path was in force —
        the window itself may be unreadable."""
        self.assertIn('std::getenv("SLINT_BACKEND")', GUI_CPP)
        self.assertIn("renderer requested", GUI_CPP)


class AutoFallbackTests(unittest.TestCase):
    """The automatic remedies: the GL pre-flight probe and the black-frame
    watchdog. Both act without the user asking, which is exactly why every rule
    here is about NOT acting on weak evidence — a false positive silently
    downgrades a healthy machine to CPU rendering, and nothing on screen says
    so."""

    def test_the_probe_runs_before_the_backend_is_fixed(self):
        """Slint reads SLINT_BACKEND lazily at FIRST use — App::create(). An
        env var set after that is a no-op with no error, so ordering is the
        entire mechanism."""
        self.assertLess(GUI_CPP.index("MaybeForceSoftwareRendering(fileLog.get())"),
                        GUI_CPP.index("auto app = App::create();"))

    def test_an_explicit_choice_always_wins_over_the_probe(self):
        """--software and a hand-set SLINT_BACKEND land in the same env var;
        the probe only fills silence."""
        body = re.search(r"void MaybeForceSoftwareRendering\((.*?)\n\}", GUI_CPP,
                         re.S).group(1)
        self.assertLess(body.index('std::getenv("SLINT_BACKEND")'),
                        body.index("ProbeOpenGl()"))
        self.assertIn("return;", body.split("ProbeOpenGl()")[0])

    def test_the_probe_sets_the_env_var_through_both_apis(self):
        """Same two-API rule as main.cpp's --software, same reason: Rust reads
        the OS block, our getenv reads the CRT copy, and they only sync one
        way at startup."""
        body = re.search(r"void MaybeForceSoftwareRendering\((.*?)\n\}", GUI_CPP,
                         re.S).group(1)
        self.assertIn('SetEnvironmentVariableW(L"SLINT_BACKEND", L"software")', body)
        self.assertIn('_putenv_s("SLINT_BACKEND", "software")', body)

    def test_probe_scaffolding_failures_never_accuse_the_driver(self):
        """RegisterClass/CreateWindow failing is the PROBE broken, not GL —
        switching a healthy machine to CPU rendering over it would be a
        transient error becoming a degraded session."""
        body = re.search(r"GlProbeResult ProbeOpenGl\(\)(.*?)\n\}", GUI_CPP,
                         re.S).group(1)
        self.assertIn("do not accuse", body)

    def test_the_watchdog_needs_positive_evidence_twice(self):
        """A failed capture, a minimized window, a zero-area client — each is
        `unknown`, never a strike. The relaunch fires only on two consecutive
        ALL-black captures of a window whose splash always draws bright
        pixels."""
        self.assertIn("kBlackStrikesNeeded = 2", GUI_CPP)
        self.assertIn("FrameLook::unknown", GUI_CPP)
        body = re.search(r"FrameLook SampleWindow\((.*?)\n\}", GUI_CPP,
                         re.S).group(1)
        self.assertIn("IsIconic", body)
        self.assertIn("PW_RENDERFULLCONTENT", body)

    def test_the_relaunch_is_one_shot_from_two_independent_sides(self):
        """The child gets --software (which sets SLINT_BACKEND) AND the guard
        env; the watchdog arms only when BOTH are absent. A fallback that can
        cascade is a process fork bomb with a user attached."""
        self.assertIn('kFallbackGuardEnv[] = "PHOTON_SW_FALLBACK"', GUI_CPP)
        arm = re.search(r"if \(std::getenv\(\"SLINT_BACKEND\"\) == nullptr &&\s*"
                        r"std::getenv\(kFallbackGuardEnv\) == nullptr\)", GUI_CPP)
        self.assertIsNotNone(arm, "the watchdog arm condition lost a guard")
        relaunch = re.search(r"bool RelaunchWithSoftwareRendering\(\)(.*?)\n\}",
                             GUI_CPP, re.S).group(1)
        self.assertIn('SetEnvironmentVariableW(L"PHOTON_SW_FALLBACK", L"1")',
                      relaunch)
        self.assertIn("--software", relaunch)

    def test_the_gui_links_the_probe_and_capture_libraries(self):
        """ChoosePixelFormat/SetPixelFormat are gdi32, PrintWindow is user32 —
        neither was linked before, and the failure is unresolved externals that
        read as a broken Slint build."""
        cmake = (REPO / "src" / "native" / "CMakeLists.txt").read_text(
            encoding="utf-8", errors="replace")
        m = re.search(r"target_link_libraries\(photon-installer PRIVATE "
                      r"opengl32 imm32([^)]*)\)", cmake)
        self.assertIsNotNone(m, "the GUI's Windows link line moved")
        self.assertIn("gdi32", m.group(1))
        self.assertIn("user32", m.group(1))


class TuiParityTests(unittest.TestCase):
    """The console build is now a SHIPPED fallback, so a console user must not
    get a lesser installer. The two gaps found in the 2026-08-15 parity audit,
    pinned closed."""

    def test_a_dry_run_skips_the_close_xplane_wait(self):
        """gui.cpp calls the TUI's dry-run gating "a wart worth not copying" —
        a dry run writes nothing, and it is exactly what you do WHILE the sim
        is open. The skip must run before PluginIsCurrent, whose two full file
        reads are the wait's other cost."""
        body = re.search(r"void ScreenCheckNotRunning\((.*?)\n\}", SCREENS_CPP,
                         re.S).group(1)
        self.assertIn("if (gDryRun)", body)
        self.assertLess(body.index("if (gDryRun)"),
                        body.index("PluginIsCurrent"))

    def test_a_fresh_install_seeds_the_wing_from_detection(self):
        """The GUI preselects the wing radio from wingmod::Detect; without this
        the console default was always the first row, and installing the wrong
        variant binds the wingtip lights to a driver the wing no longer uses —
        visible only airborne, with the wing flexing."""
        self.assertIn("wingmod::Detect(ac.path, ac.airframe)", SCREENS_CPP)
        body = re.search(r"std::string SeedWing\((.*?)\n\}", SCREENS_CPP,
                         re.S).group(1)
        # Clamped to what the airframe offers — resolve_mount()'s fallback is
        # how a stock OBJ ships mislabeled as a mod build.
        self.assertIn("WingsFor", body)
        # And the verdict is logged: a preselected row explains itself to
        # nobody, and the log is where a WRONG detection leaves a trace.
        self.assertIn("LogLine", body)


class CiConsoleBuildTests(unittest.TestCase):
    """release.yml must produce the console binary on every runner, in its own
    tree, with the Slint grep pointing the OTHER way — Slint configured into
    the fallback ships a fallback that fails exactly when the GUI does."""

    def test_the_console_tree_is_built_without_the_gui(self):
        m = re.search(r"-B build-console[^\n]*", WORKFLOW)
        self.assertIsNotNone(m, "the console build step is gone from release.yml")
        self.assertIn("-DPHOTON_GUI=OFF", m.group(0))
        self.assertIn("-DPHOTON_CORE_ONLY=ON", m.group(0))

    def test_the_console_configure_log_is_grepped_against_slint(self):
        step = re.search(r"Build the console installer(.*?)- name:", WORKFLOW,
                         re.S)
        self.assertIsNotNone(step)
        self.assertIn("configure-console.log", step.group(1))
        self.assertIn("! grep -q", step.group(1))

    def test_ci_runs_the_console_binary_and_pins_its_version(self):
        """Linked is not started: a console binary that dies on startup ships
        green without this. And the version compare against core/version.h —
        THE version — is what catches a tree built from different sources than
        the rest of the job."""
        step = re.search(r"Build the console installer(.*?)- name:", WORKFLOW,
                         re.S).group(1)
        self.assertIn('$("$exe" version)', step)
        self.assertIn("kPhotonVersion", step)
        self.assertIn("src/core/version.h", step)

    def test_make_release_searches_the_ci_console_tree(self):
        self.assertIn(REPO / "src" / "native" / "build-console",
                      MR.INSTALLER_BUILD_DIRS)

    def test_the_dedicated_trees_shadow_the_deploy_tree(self):
        """`build/` also holds a console photon-installer — a side effect of the
        plugin build that nothing keeps fresh. Searched first, it shadowed a
        just-built build-core binary with a stale one, which is the stale-.xpl
        bug wearing the installer's clothes."""
        dirs = list(MR.INSTALLER_BUILD_DIRS)
        native = REPO / "src" / "native"
        for dedicated in ("build-gui", "build-core", "build-console"):
            self.assertLess(dirs.index(native / dedicated),
                            dirs.index(native / "build"))


if __name__ == "__main__":
    unittest.main()
