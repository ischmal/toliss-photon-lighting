"""Nothing Photon does may run on an aircraft that is not a ToLiss.

⚠ THE BUG THIS CLOSES (2026-08-11, from a user report): the panel-FX compositor
never asked. `PanelPassDraw` is registered for as long as the plugin is enabled
and paints into whatever panel texture the loaded aircraft has, so the four
UNSCOPED stacks — the six DUs, both MCDUs, both DCDUs, the ISIS — composited
A3xx-shaped tints over every cockpit in the sim. The `family` scope added the
same week did not save the other six either: `gFxFamily` was resolved on the
ToLiss branch of `UpdateMenuVisibility` only, so switching from an A320 to
anything else left it reading "a3xx" and the scoped stacks kept painting too.

The invariant is now one flag, `gIsToLiss`, with one writer, read by every
per-frame entry point. Every test here fails silently in the sim in one of two
opposite ways: a gate that fails open paints (or writes) on somebody else's
aircraft, and one that fails closed makes the whole add-on look uninstalled on a
ToLiss. When a new per-frame path is added, it gets a line in
`test_every_per_frame_entry_point_is_gated` — that list is the contract.
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
PLUGIN_CPP = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
    encoding="utf-8", errors="replace")

# The flag's one gate expression, spelled the same way at every site.
GATE = "!gIsToLiss"


def _fn(name):
    """A function body, from its signature to the first `}` in column 0.

    ⚠ The signature has to end in `{`, or a forward declaration matches first and
    the "body" returned is every line between it and some later function's
    closing brace — which passes an `assertIn` for entirely the wrong reason.
    Several of these functions are forward-declared (see UpdateMenuVisibility)."""
    m = re.search(r"\nstatic [\w:]+ %s\([^)]*\)\s*\{\n.*?\n\}" % re.escape(name),
                  PLUGIN_CPP, re.S)
    assert m, "%s moved or was renamed" % name
    return m.group(0)


def _dev_guarded(symbol):
    """Whether `symbol`'s definition sits inside `#if PHOTON_DEV`. The same crude
    preprocessor walk test_panel_fx.py uses — it reads the file the way the
    compiler does rather than trusting a comment."""
    idx = PLUGIN_CPP.index(symbol)
    depth = 0
    for line in PLUGIN_CPP[:idx].splitlines():
        s = line.strip()
        if s.startswith("#if PHOTON_DEV"):
            depth += 1
        elif s.startswith("#endif") and depth:
            depth -= 1
        elif s.startswith(("#if", "#ifdef", "#ifndef")) and depth:
            # ⚠ A nested, unrelated #if INSIDE the guard. Without this its
            # #endif closes ours, and every dev-only symbol after it reads as
            # shipping — an assertion that then passes for the wrong reason.
            depth += 1
    return depth > 0


class FlagOwnershipTests(unittest.TestCase):
    """One writer, and it is the aircraft-load path."""

    def test_the_flag_exists_and_starts_false(self):
        """Until an aircraft has been looked at, the honest answer is "no" — and
        it is the answer that draws nothing rather than the one that draws."""
        self.assertRegex(PLUGIN_CPP, r"static bool gIsToLiss = false;")

    def test_only_update_menu_visibility_writes_it(self):
        """A second writer is how a flag like this drifts from what it names.
        Every other site reads it."""
        writes = [m.start() for m in re.finditer(r"gIsToLiss\s*=", PLUGIN_CPP)
                  if not re.match(r"gIsToLiss\s*==", PLUGIN_CPP[m.start():])]
        self.assertTrue(writes, "nothing assigns gIsToLiss")
        vis = _fn("UpdateMenuVisibility")
        start = PLUGIN_CPP.index(vis)
        for pos in writes:
            # The declaration itself is the only assignment outside the setter.
            if PLUGIN_CPP[:pos].rstrip().endswith("static bool"):
                continue
            self.assertTrue(start <= pos < start + len(vis),
                            "gIsToLiss is written outside UpdateMenuVisibility "
                            "at offset %d" % pos)

    def test_it_is_set_before_the_branch_that_reads_it(self):
        """⚠ ResolveFxFamily answers from the flag and is called from BOTH
        branches, so a flag set inside the ToLiss branch would leave the family
        resolving against the previous aircraft on the way out."""
        vis = _fn("UpdateMenuVisibility")
        self.assertLess(vis.index("gIsToLiss = IsToLiss();"),
                        vis.index("if (gIsToLiss) {"),
                        "the branch is taken before the flag is set")

    def test_the_gate_is_not_the_menu(self):
        """gMenuCreated is UI state — DestroyPhotonMenu is also how the Cockpit
        submenu is rebuilt between two ToLiss aircraft — so a per-frame path
        gated on it is one menu change away from silently changing what runs."""
        for name in ("EngineLoop", "AutoLoop"):
            self.assertNotIn("!gMenuCreated", _fn(name),
                             "%s still uses the menu as its aircraft gate" % name)

    def test_the_gate_ships(self):
        """Behind `#if PHOTON_DEV` the dev machine would be the only place the
        bug was fixed, which is the exact shape of every other silent failure in
        this file."""
        self.assertFalse(_dev_guarded("static bool gIsToLiss = false;"))


class PerFrameGateTests(unittest.TestCase):
    """Every path X-Plane calls on its own, whatever aircraft is loaded."""

    def test_every_per_frame_entry_point_is_gated(self):
        """⚠ THIS LIST IS THE CONTRACT. A new flight loop or draw callback that
        is not on it is not covered by anything else here."""
        for name in ("EngineLoop",       # the waveform engine, every frame
                     "AutoLoop",         # the 1 Hz Auto/rebind loop
                     "PanelPassDraw",    # the panel-pass draw callback
                     "DrawPanelFx"):     # and the compositor it calls
            self.assertIn(GATE, _fn(name),
                          "%s runs whatever aircraft is loaded" % name)

    def test_the_panel_callback_gates_before_it_touches_anything(self):
        """The point of gating the CALLBACK as well as the compositor: on another
        aircraft the whole panel pass costs one bool test. It must therefore come
        above the render-type read, which is the first thing that was there."""
        body = _fn("PanelPassDraw")
        self.assertLess(body.index(GATE), body.index("gPanelRenderTypeRef"),
                        "the callback reads the render type before the gate")

    def test_the_probe_is_gated_with_the_compositor(self):
        """A dev build's probe paints from the same A3xx rect table, so on
        another cockpit its magenta is the same bug wearing a dev build's name.
        The gate is above the `#if`, so one return covers both halves."""
        body = _fn("PanelPassDraw")
        self.assertLess(body.index(GATE), body.index("#if PHOTON_DEV"))

    def test_the_compositor_gates_above_the_ambient_sampler(self):
        """⚠ The one ordering that is not obvious. FxSampleAmbient sits ABOVE
        every other early-out in DrawPanelFx deliberately (a blend that stops
        while the effects are off resumes with a visible fade), so the aircraft
        gate has to be stated above it rather than folded in with the rest."""
        body = _fn("DrawPanelFx")
        self.assertLess(body.index(GATE), body.index("FxSampleAmbient()"),
                        "the ambient sampler runs on a non-ToLiss")
        self.assertLess(body.index(GATE), body.index("gFxEnabled"),
                        "the aircraft gate is not the first question asked")


class FamilyResetTests(unittest.TestCase):
    """The family value must not survive the aircraft it was read from."""

    def test_a_non_toliss_resolves_to_no_family(self):
        """⚠ Matching on the ICAO alone is not enough: a third-party A320 reports
        A20N exactly as readily as ToLiss's does, and every `a3xx` stack would
        treat that cockpit as one of ours."""
        body = _fn("ResolveFxFamily")
        self.assertRegex(
            body, r"gIsToLiss \?\s*FxFamilyForIcao\(icao\)\s*:\s*kFxFamilyNone")

    def test_it_is_re_resolved_when_the_aircraft_is_not_a_toliss(self):
        """AutoLoop returns early off the same flag, so the aircraft-load path is
        the ONLY place this can be put right — and it was on the ToLiss branch
        only, which is what left the scoped stacks painting after a switch."""
        vis = _fn("UpdateMenuVisibility")
        _, sep, other = vis.partition("\n    } else {")
        self.assertTrue(sep, "UpdateMenuVisibility's else branch moved")
        self.assertIn("ResolveFxFamily();", other,
                      "the FX family keeps the last ToLiss's answer")

    def test_a_perf_run_does_not_outlive_the_aircraft(self):
        """Its window goes with the menu, so a run left going would flip the
        user's display switches with nothing on screen to abort it — and its
        sampler is a real thread polling PDH for a cockpit we have left."""
        vis = _fn("UpdateMenuVisibility")
        _, _, other = vis.partition("\n    } else {")
        self.assertIn("PerfShutdown();", other,
                      "a benchmark run survives the aircraft change")


if __name__ == "__main__":
    unittest.main()
