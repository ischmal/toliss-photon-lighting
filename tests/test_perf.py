"""The performance tool's contracts, checked against plugin.cpp.

Everything pinned here fails quietly. A lever that persists leaves the user's
settings changed by a measurement and looks like the effect broke itself. A run
that is not restored leaves them changed with no window still open to say so. And
a tool that measures its own sampling cost — a wildcard PDH query on the simulator
thread, or a run driven from a draw callback that stops being called — reports a
number that is about the tool.
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
NATIVE = REPO / "src" / "native" / "src"
PLUGIN_CPP = (NATIVE / "plugin.cpp").read_text(encoding="utf-8", errors="replace")
SYSMETRICS = (NATIVE / "sysmetrics.cpp").read_text(encoding="utf-8", errors="replace")
CMAKELISTS = (REPO / "src" / "native" / "CMakeLists.txt").read_text(
    encoding="utf-8", errors="replace")


def _fn(name, src=PLUGIN_CPP):
    """The body of a function, from its signature to the first line-start `}`."""
    m = re.search(r"\n[^\n]*%s[^\n]*\{\n(.*?)\n\}" % re.escape(name), src, re.S)
    assert m, "%s not found (renamed?)" % name
    return m.group(1)


_FUNC_DEF = re.compile(r"^(?:static\s+)?[A-Za-z_][\w:<>,\s\*&]*[\s\*&](\w+)\s*\(")


def _owners():
    """(line number, text, enclosing function name) for every line of plugin.cpp.

    Same brace walk tests/test_ui.py uses. Crude on purpose: a real parse would be
    a C++ front end, and every function in this file starts in column 0."""
    lines = PLUGIN_CPP.splitlines()
    out, cur, brace = [], "", 0
    for n, raw in enumerate(lines, 1):
        code = raw.split("//")[0]
        if brace == 0:
            m = _FUNC_DEF.match(raw)
            if m and not raw.lstrip().startswith(("return", "if", "for", "while")):
                cur = m.group(1)
        out.append((n, raw, cur))
        brace = max(0, brace + code.count("{") - code.count("}"))
    return out


def _guarded(symbol):
    """Whether `symbol`'s definition sits inside `#if PHOTON_DEV`. Same crude
    preprocessor walk test_panel_fx.py uses, and crude for the same reason: it
    reads the file the way the compiler does rather than trusting a comment."""
    idx = PLUGIN_CPP.index(symbol)
    depth = 0
    for line in PLUGIN_CPP[:idx].splitlines():
        s = line.strip()
        if s.startswith("#if PHOTON_DEV"):
            depth += 1
        elif s.startswith("#endif") and depth:
            depth -= 1
        elif s.startswith(("#if", "#ifdef", "#ifndef")) and depth:
            depth += 1
    return depth > 0


class ShippingTests(unittest.TestCase):
    """The tool ships; only its extra levers and the sweep are dev-only, and that
    is a `dev` PARAMETER rather than a second implementation."""

    def test_the_perf_pane_is_compiled_into_the_shipping_build(self):
        for symbol in ("static void BuildPerfPane(",
                       "static float PerfLoop(",
                       "static void PerfEnsureLoop()",
                       "static void PerfShutdown()",
                       "static PerfStats PerfStatsOf("):
            self.assertFalse(_guarded(symbol),
                             "%s must not be behind #if PHOTON_DEV" % symbol)

    def test_both_windows_call_the_same_builder(self):
        """One builder, two callers. A second copy for the Dev window is how the
        two drift, which is the same argument BuildConfigTab and BuildDisplaysTab
        already carry."""
        self.assertIn("BuildPerfTab(false)", PLUGIN_CPP)
        self.assertIn("BuildPerfTab(true)", PLUGIN_CPP)
        self.assertEqual(len(re.findall(r"static void BuildPerfPane\(bool dev\)",
                                        PLUGIN_CPP)), 1)

    def test_the_shipping_pane_offers_no_manual_levers(self):
        """⚠ A switch here is NOT saved — a measurement is not a preference — so
        on the shipping pane it would look exactly like the Displays tab's and
        forget itself on the next launch. The !dev branch has to return before any
        checkbox is submitted."""
        body = _fn("static void PerfDrawLevers(bool dev)")
        head = body[:body.index("return;")]
        self.assertIn("if (!dev)", head)
        self.assertNotIn("ImGui::Checkbox", head)

    def test_the_internal_levers_are_marked_dev(self):
        """The three the shipping A/B run does not move. Marking one wrong would
        put it on the shipping pane's "Current Settings" list — which reads as
        "this is what the test will switch off" — while the run left it alone.

        The first four are the Displays switches, in that pane's own order. The
        last three are the compositor, the waveform engine and the cockpit's
        light stack: the first two a user cannot reach any other way, and the
        third is a Cockpit-tab setting rather than a display effect."""
        table = _fn("static const PerfLever kLevers[kLeverCount] =")
        rows = [r.strip() for r in re.findall(r"\{\"[^\"]+\",(?:[^{}]|\n)*?\}",
                                              table)]
        self.assertEqual(len(rows), 7, "lever count changed: %d" % len(rows))
        self.assertEqual([r.rstrip("}").rstrip().endswith("true") for r in rows],
                         [False, False, False, False, True, True, True])

    def test_the_cockpit_lever_reads_the_expensive_state_as_on(self):
        """Every other lever is "on == the effect is running", and the sweep's
        baseline turns them ALL on to measure from. The cockpit's flag is the
        other way round (`optimized` means fewer lights), so the lever inverts it
        — otherwise the baseline would silently measure the cheap cockpit and
        report the stack as free."""
        self.assertIn("case kLeverCockpitLights: return !gCockpit.optimized;",
                      _fn("static bool PerfLeverGet(int i)"))
        self.assertIn("case kLeverCockpitLights: gCockpit.optimized = !on;",
                      _fn("static void PerfLeverSet(int i, bool on)"))


class RunSafetyTests(unittest.TestCase):
    def test_nothing_a_run_moves_is_persisted(self):
        """⚠ PerfLeverSet must not save. A value written to disk mid-run survives
        the restore that follows it, so the benchmark would permanently change the
        setting it was measuring."""
        body = _fn("static void PerfLeverSet(int i, bool on)")
        for call in ("SaveDisplays", "SavePanelFx", "SaveProfilesFile"):
            self.assertNotIn(call, body)

    def test_the_run_is_driven_from_a_flight_loop(self):
        """Not from the draw callback. A hidden, popped-out or covered window
        stops drawing, and the run would stall with the levers still moved."""
        self.assertIn("PerfAdvanceRun(elapsed)", _fn("static float PerfLoop("))
        self.assertIn("xplm_FlightLoop_Phase_AfterFlightModel",
                      _fn("static void PerfEnsureLoop()"))

    def test_every_exit_restores_the_levers(self):
        self.assertIn("PerfRestoreLevers()", _fn("static void PerfStopRun("))
        self.assertIn("PerfStopRun(nullptr)", _fn("static void PerfShutdown()"))
        self.assertIn("PerfShutdown()", _fn("PLUGIN_API void XPluginDisable"))

    def test_the_engine_lever_is_restored_before_the_loop_is_destroyed(self):
        """⚠ PerfShutdown's restore may START the waveform engine, so XPluginDisable
        has to call it BEFORE its own StopEngine. The other order leaves a disabled
        plugin running a per-frame flight loop."""
        body = _fn("PLUGIN_API void XPluginDisable")
        self.assertLess(body.index("PerfShutdown()"), body.index("StopEngine()"))

    def test_a_hitch_is_excluded_from_every_figure(self):
        """A scenery load is not a frame. One 5000 ms sample would dominate the 1%
        low of whichever step it landed in and read as that step being terrible."""
        body = _fn("static float PerfLoop(")
        self.assertIn("elapsed < kPerfMaxFrame", body)
        self.assertIn("++gPerfDropped", body)

    def test_a_step_is_a_complete_mask(self):
        """Not a delta. A sweep applying one toggle per step carries every earlier
        step's state forward, so one mistake reads as a cost in every row after
        it."""
        self.assertIn("bool        lever[kLeverCount];", PLUGIN_CPP)
        self.assertIn("for (int i = 0; i < kLeverCount; ++i) PerfLeverSet(i, s.lever[i]);",
                      PLUGIN_CPP)


class SysMetricsTests(unittest.TestCase):
    """⚠ Sampled OFF the simulator thread, and it makes no XPLM call there."""

    def test_the_sampler_never_calls_xplm(self):
        self.assertNotIn("XPLM", SYSMETRICS)

    def test_it_runs_on_its_own_thread(self):
        self.assertIn("gThread = std::thread(SampleLoop)", SYSMETRICS)
        self.assertIn("gThread.join()", _fn("void Stop()", SYSMETRICS))

    def test_the_counter_paths_are_the_english_ones(self):
        """PdhAddCounter wants the LOCALIZED name, so the English path resolves on
        an English Windows and on nobody else's. The tool would come up empty on a
        user's machine and be perfect on the developer's."""
        self.assertIn("PdhAddEnglishCounterW", SYSMETRICS)
        self.assertNotRegex(SYSMETRICS, r"PdhAddCounterW?\(")

    def test_threads_are_linked_unconditionally(self):
        """They were a PHOTON_DEV dependency until this shipped."""
        dev = CMAKELISTS.index("if(PHOTON_DEV)")
        self.assertLess(CMAKELISTS.index("find_package(Threads REQUIRED)"), dev)
        self.assertIn("src/sysmetrics.cpp", CMAKELISTS)


class DisplaysMasterTests(unittest.TestCase):
    """The Displays tab's master switch. ⚠ Every consumer goes through one helper:
    a gate forgotten at one of them turns two effects off and leaves the third on,
    which reads as that one effect being broken rather than as a missed gate."""

    #: The functions allowed to read a Displays flag raw. They are the ones that
    #: are ABOUT the setting rather than about the effect: the two halves of
    #: persistence, the pane that edits it, and the performance tool's own
    #: save/restore. Everything else is a consumer and must ask the helper.
    #: ⚠ BuildSettingsTab is in here because it edits gDisplays.spill from the
    #: other direction — "Disable screen backlight illumination", the cost
    #: phrasing of the Displays tab's look phrasing. It is a pane that EDITS the
    #: setting, not a consumer of it, so the master gate is not its to apply.
    RAW_OK = {"DisplaysFromJson", "SaveProfilesFile", "SaveDisplays",
              "BuildDisplaysTab", "BuildSettingsTab",
              "PerfLeverGet", "PerfLeverSet", "PerfStepsAB"}

    def test_the_helper_is_the_only_reader(self):
        offenders = []
        for n, line, owner in _owners():
            code = line.split("//")[0]
            for feature in ("spill", "screenFx", "otherFx"):
                if "gDisplays.%s" % feature not in code:
                    continue
                if "gDisplays.%sGain" % feature in code and \
                        "gDisplays.%s " % feature not in code:
                    continue                       # the strength, not the switch
                if owner in self.RAW_OK or "DisplayFeatureOn" in code:
                    continue
                offenders.append("plugin.cpp:%d in %s(): %s"
                                 % (n, owner, code.strip()))
        self.assertEqual(offenders, [],
                         "read these through DisplayFeatureOn: "
                         + "; ".join(offenders))

    def test_both_consumers_use_it(self):
        # ⚠ The two halves are their own functions (2026-08-10): the compositor
        # asks each of them WITHOUT a rect in hand, to decide whether the pass
        # and each target are worth any GL at all. FxDisplayUserFactor is the
        # per-rect chooser over the same two, so the switch is read in one place
        # per effect however it is asked.
        self.assertIn("DisplayFeatureOn(gDisplays.spill)", PLUGIN_CPP)
        self.assertIn("DisplayFeatureOn(gDisplays.screenFx)",
                      _fn("static float FxScreenUserFactor("))
        self.assertIn("DisplayFeatureOn(gDisplays.otherFx)",
                      _fn("static float FxOtherUserFactor("))
        chooser = _fn("static float FxDisplayUserFactor(")
        self.assertIn("FxScreenUserFactor()", chooser)
        self.assertIn("FxOtherUserFactor()", chooser)

    def test_the_master_is_persisted(self):
        """"Absent means default" cannot express a switch the user turned off, so
        the Displays block is always written whole — the master with it."""
        self.assertIn('flag("enabled",', PLUGIN_CPP)
        self.assertIn('\\"enabled\\": %d', PLUGIN_CPP)


if __name__ == "__main__":
    unittest.main()
