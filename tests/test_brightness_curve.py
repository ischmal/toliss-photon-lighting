"""The display brightness response curve — kBrightnessCurve in plugin.cpp.

A knob at 0..1 does not become an opacity at 0..1 linearly: the bottom fifth is
dead and half a turn is still dim. Two consumers share that one curve — the panel
FX tint (dev only) and the six screen-glow spill lights (shipping) — and the whole
point of sharing it is that a tint and the glow under it come up TOGETHER. Nothing
in the sim would flag them drifting apart; it would just look slightly wrong.

So this file pins three separate things:

  * the knots, as literal numbers, because they are the spec the user gave;
  * the SHAPE the interpolation produces — monotone, inside [0,1], flat through
    the dead zone. Plain Hermite through these knots undershoots below zero just
    past 0.2, and a clamped undershoot is a visible step in the middle of a fade;
  * that both consumers actually call it, and that the FX one applies it in the
    right place (after lo/hi normalization, before the floor).

The curve is re-implemented here from the knots rather than being read out of the
C++ — an independent implementation is what makes the shape assertions worth
anything.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PLUGIN_CPP = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
    encoding="utf-8", errors="replace")

# The spec, verbatim (requested 2026-08-01). x = knob, y = opacity.
EXPECTED_KNOTS = [(0.0, 0.0), (0.2, 0.0), (0.5, 0.2), (1.0, 1.0)]


def parse_knots():
    """The kBrightnessCurve table as [(x, y), ...]."""
    m = re.search(r"static const CurveKnot kBrightnessCurve\[\]\s*=\s*\{(.*?)\};",
                  PLUGIN_CPP, re.S)
    assert m, "kBrightnessCurve table not found in plugin.cpp"
    return [(float(a), float(b))
            for a, b in re.findall(r"\{\s*([-\d.]+)f\s*,\s*([-\d.]+)f\s*\}", m.group(1))]


def fritsch_carlson(knots):
    """Monotone cubic tangents — the same construction plugin.cpp uses."""
    n = len(knots)
    secant = [0.0] * n
    for i in range(n - 1):
        dx = knots[i + 1][0] - knots[i][0]
        secant[i] = (knots[i + 1][1] - knots[i][1]) / dx if dx > 0 else 0.0
    t = [0.0] * n
    t[0] = secant[0]
    t[n - 1] = secant[n - 2]
    for i in range(1, n - 1):
        t[i] = 0.5 * (secant[i - 1] + secant[i])
    for i in range(n - 1):
        if secant[i] == 0.0:
            t[i] = t[i + 1] = 0.0
    for i in range(n - 1):
        if secant[i] == 0.0:
            continue
        a, b = t[i] / secant[i], t[i + 1] / secant[i]
        s = a * a + b * b
        if s > 9.0:
            k = 3.0 / (s ** 0.5)
            t[i] = k * a * secant[i]
            t[i + 1] = k * b * secant[i]
    return t


def response(x, knots=None):
    knots = knots or EXPECTED_KNOTS
    tan = fritsch_carlson(knots)
    n = len(knots)
    if x <= knots[0][0]:
        return knots[0][1]
    if x >= knots[n - 1][0]:
        return knots[n - 1][1]
    i = 0
    while i < n - 2 and x > knots[i + 1][0]:
        i += 1
    h = knots[i + 1][0] - knots[i][0]
    t = (x - knots[i][0]) / h
    t2, t3 = t * t, t * t * t
    return ((2 * t3 - 3 * t2 + 1) * knots[i][1]
            + (t3 - 2 * t2 + t) * h * tan[i]
            + (-2 * t3 + 3 * t2) * knots[i + 1][1]
            + (t3 - t2) * h * tan[i + 1])


class KnotTests(unittest.TestCase):
    def test_the_knots_are_the_requested_ones(self):
        self.assertEqual(parse_knots(), EXPECTED_KNOTS)

    def test_the_knots_are_sorted_and_non_decreasing(self):
        """Both are assumed by the interpolator: the segment search walks x
        forward, and monotone tangents only bound the result inside [0, 1] if y
        never goes backwards."""
        knots = parse_knots()
        for a, b in zip(knots, knots[1:]):
            self.assertLess(a[0], b[0], "knot x values must strictly increase")
            self.assertLessEqual(a[1], b[1], "knot y values must not decrease")

    def test_the_count_constant_matches_the_table(self):
        self.assertIn("sizeof(kBrightnessCurve) / sizeof(kBrightnessCurve[0])",
                      PLUGIN_CPP)


class ShapeTests(unittest.TestCase):
    def test_it_passes_through_every_knot(self):
        for x, y in EXPECTED_KNOTS:
            self.assertAlmostEqual(response(x), y, places=6, msg="knot x=%s" % x)

    def test_the_dead_zone_is_really_dead(self):
        """Anything up to the second knot is zero — not 'nearly', zero. A glow
        that is faintly on with the knob at its stop is the artifact this curve
        was asked for."""
        for x in (0.0, 0.05, 0.1, 0.15, 0.1999):
            self.assertEqual(response(x), 0.0, "x=%s should be fully dark" % x)

    def test_it_never_leaves_zero_to_one(self):
        """⚠ The undershoot check. Plain Catmull-Rom through these knots dips
        below zero just past the dead zone; the clamp then turns the dip into a
        visible step. Fritsch-Carlson cannot do it — this is the guard on that
        choice, so a future 'simplify the interpolation' has to fail here."""
        for i in range(1001):
            x = i / 1000.0
            y = response(x)
            self.assertGreaterEqual(y, 0.0, "undershoot at x=%.3f" % x)
            self.assertLessEqual(y, 1.0, "overshoot at x=%.3f" % x)

    def test_plain_catmull_rom_really_would_undershoot(self):
        """The claim the comment in plugin.cpp makes, checked rather than
        asserted: with Catmull-Rom tangents the middle knot pulls the FLAT first
        segment down to about -0.012, so the curve is negative across most of the
        dead zone. That is what the monotone construction is there to prevent, so
        if this ever stops being true the comment is what needs fixing."""
        knots = EXPECTED_KNOTS
        n = len(knots)
        tan = [0.0] * n
        tan[0] = (knots[1][1] - knots[0][1]) / (knots[1][0] - knots[0][0])
        tan[n - 1] = (knots[n - 1][1] - knots[n - 2][1]) / (knots[n - 1][0] - knots[n - 2][0])
        for i in range(1, n - 1):
            tan[i] = (knots[i + 1][1] - knots[i - 1][1]) / (knots[i + 1][0] - knots[i - 1][0])

        # first segment only — it is the flat one, and it is where the dip is
        h = knots[1][0] - knots[0][0]
        worst = min(
            ((2 * t ** 3 - 3 * t ** 2 + 1) * knots[0][1]
             + (t ** 3 - 2 * t ** 2 + t) * h * tan[0]
             + (-2 * t ** 3 + 3 * t ** 2) * knots[1][1]
             + (t ** 3 - t ** 2) * h * tan[1])
            for t in (i / 200.0 for i in range(201)))
        self.assertLess(worst, -0.01)
        self.assertEqual(response(knots[0][0] + h * 2 / 3), 0.0)   # ours: flat

    def test_it_is_monotone(self):
        prev = -1.0
        for i in range(1001):
            y = response(i / 1000.0)
            self.assertGreaterEqual(y + 1e-9, prev, "curve goes backwards at %d" % i)
            prev = y

    def test_the_top_half_of_the_knob_carries_most_of_the_range(self):
        """The shape's whole purpose. Below half a turn the tint is worth at most
        a fifth; the other four fifths live in the top half."""
        self.assertLessEqual(response(0.5), 0.2 + 1e-6)
        self.assertGreaterEqual(response(1.0) - response(0.5), 0.75)

    def test_it_leaves_the_dead_zone_smoothly(self):
        """A zero secant pins both its tangents to zero, so the curve leaves the
        second knot horizontally rather than with a corner. Concretely: the first
        step out of the dead zone must be smaller than the average step over the
        segment, which a piecewise-linear fit would fail."""
        step = response(0.21) - response(0.20)
        average = (response(0.5) - response(0.2)) / 30.0
        self.assertLess(step, average)


class WiringTests(unittest.TestCase):
    def test_the_screen_glow_alpha_goes_through_the_curve(self):
        """The shipping consumer. Unconditional — there is no dev switch on this
        side, so the curve IS the shipped look of the spill lights."""
        m = re.search(r"gScreenAlpha\[i\]\s*=(.*?);", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "the screen-glow alpha assignment moved")
        self.assertIn("BrightnessResponse", m.group(1))
        self.assertIn("kScreenGain", m.group(1))

    def test_the_fx_driver_factor_curves_after_normalizing_and_before_the_floor(self):
        """Order matters twice over: lo/hi must normalize first or the curve is
        being handed a value in the source's own units, and the floor must lift
        AFTER or 'never fully off' stops meaning that once the curve has
        flattened the bottom of the knob.

        The normalization lives in FxDriverNorm since the test pin was added, so
        the order now spans two functions — but it is the same three stages and
        the same reason, and the split is what keeps the curve editor's live
        marker reading the number the compositor reads."""
        norm = re.search(
            r"static bool FxDriverNorm\(FxDriver& d, float\* out\)\s*\{(.*?)\n\}",
            PLUGIN_CPP, re.S)
        self.assertIsNotNone(norm, "FxDriverNorm moved")
        self.assertIn("(v - d.lo) / span", norm.group(1),
                      "lo/hi must normalize before anything else sees the value")
        self.assertNotIn("FxCurveEval", norm.group(1),
                         "the shape is the factor's business, not the reading's")

        m = re.search(
            r"static float FxDriverFactor\(FxDriver& d,\s*const FxCurve\* shape\)"
            r"\s*\{(.*?)\n\}",
            PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "FxDriverFactor moved")
        body = m.group(1)
        normalize = body.index("FxDriverNorm")
        curve = body.index("FxCurveEval")
        floor = body.index("d.floorF + f")
        self.assertLess(normalize, curve, "curve must come after lo/hi")
        self.assertLess(curve, floor, "curve must come before the floor")

    def test_the_test_pin_replaces_the_reading_and_not_the_factor(self):
        """⚠ The dev brightness pin sits inside FxDriverNorm, ahead of the curve
        and the floor. Pinning the FACTOR instead would draw a straight line
        through whatever shape is authored — and the editor plots that shape
        beside the slider, so the one control for sweeping a curve would be the
        one control that bypasses it.

        It also must not reach FxDriverPowered: power, bus and breaker are facts
        about the aircraft, and a slider able to fake them would let a look be
        authored against an electrical state the cockpit is not in."""
        norm = re.search(
            r"static bool FxDriverNorm\(FxDriver& d, float\* out\)\s*\{(.*?)\n\}",
            PLUGIN_CPP, re.S)
        self.assertIn("gFxDriveManual", norm.group(1))

        powered = re.search(r"static bool FxDriverPowered\(FxDriver& d\)\s*\{(.*?)\n\}",
                            PLUGIN_CPP, re.S)
        self.assertIsNotNone(powered, "FxDriverPowered moved")
        self.assertNotIn("gFxDriveManual", powered.group(1))
        self.assertNotIn("FxDriverNorm", powered.group(1),
                         "power is a threshold on the RAW value, never a knob")

    def test_the_held_pin_has_a_watchdog(self):
        """⚠ It is held, not latched, and the pane that would release it is not
        drawn when the Dev window is hidden. Without an expiry, a drag
        interrupted by anything that hides the window pins every brightness
        driver for the rest of the session, with the control that did it out of
        sight."""
        self.assertIn("static void FxExpireDriveOverride()", PLUGIN_CPP)
        m = re.search(r"static void DrawPanelFx\(\)\s*\{(.*?)\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "DrawPanelFx moved")
        self.assertIn("FxExpireDriveOverride()", m.group(1),
                      "the expiry must run from a path that is not the pane")

    def test_the_fx_curve_switch_does_not_reach_the_screen_lights(self):
        """gFxCurve is a tuning switch on a dev-only feature. If it ever gated
        the spill lights too, a dev session left with it off would ship one."""
        m = re.search(r"gScreenAlpha\[i\]\s*=(.*?);", PLUGIN_CPP, re.S)
        self.assertNotIn("gFxCurve", m.group(1))

    def test_the_curve_switch_is_persisted(self):
        """Written and read back, or the Panel FX tab silently resets it every
        time the sim restarts."""
        self.assertIn('f << "curve "', PLUGIN_CPP)
        self.assertRegex(PLUGIN_CPP, r'key == "curve"')

    def test_the_curve_is_a_parameter_not_a_global_read(self):
        """FxDriverFactor takes the shape rather than reading gFxCurve, because
        it is per TARGET and now per LAYER: two stacks can share one driver and
        want different shapes, so a global read there would make the second one
        silently follow the first."""
        m = re.search(r"static float FxDriverFactor\(.*?\n\}", PLUGIN_CPP, re.S)
        self.assertNotIn("gFxCurve", m.group(0))

    def test_one_interpolator_serves_the_built_in_and_the_authored_curves(self):
        """A layer may carry knots of its own. If those went through a second
        implementation, a custom curve reading slightly differently from the
        built-in one THROUGH THE SAME KNOTS would be indistinguishable in-sim
        from having placed a knot wrong."""
        for fn in ("BrightnessResponse", "FxCurveEval"):
            m = re.search(r"static float %s\(.*?\n\}" % fn, PLUGIN_CPP, re.S)
            self.assertIsNotNone(m, "%s moved" % fn)
            self.assertIn("EvalMonotoneCurve", m.group(0),
                          "%s must go through the shared interpolator" % fn)
        # And the built-in FxCurve really is kBrightnessCurve, so "which curve is
        # in force" can be one pointer rather than a pointer plus a bool.
        m = re.search(r"static const FxCurve& FxBuiltInCurve\(\).*?\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m)
        self.assertIn("kBrightnessCurve", m.group(0))

    def test_an_authored_curve_never_reaches_the_spill_lights(self):
        """Same argument as the gFxCurve test above, one level further in. The
        spill lights are shipping and unswitchable; a layer's curve is a property
        of one painted layer."""
        m = re.search(r"gScreenAlpha\[i\]\s*=(.*?);", PLUGIN_CPP, re.S)
        self.assertNotIn("FxCurveEval", m.group(1))
        self.assertNotIn("FxShapeFor", m.group(1))

    def test_the_authored_curve_machinery_is_not_dev_only(self):
        """panelfx.txt is a SHIPPED artifact and may carry an `lcurve` line, so
        every user's plugin has to be able to evaluate one. Only the editor is
        dev tooling."""
        for symbol in ("static void BuildMonotoneTangents(",
                       "static float EvalMonotoneCurve(",
                       "struct FxCurve {",
                       "static float FxCurveEval("):
            self.assertLess(PLUGIN_CPP.index(symbol),
                            PLUGIN_CPP.index("#if PHOTON_DEV\nstatic"),
                            "%s must not be dev-only" % symbol)

    def test_the_curve_is_defined_outside_the_dev_guard(self):
        """The screen glow is a shipping feature and calls BrightnessResponse
        from the flight loop. A dev-only definition would fail the release build
        — but only the release build, which is the one nobody compiles while
        iterating."""
        define = PLUGIN_CPP.index("static float BrightnessResponse(float x)")
        first_dev = PLUGIN_CPP.index("#if PHOTON_DEV\nstatic")
        self.assertLess(define, PLUGIN_CPP.index("static float FxDriverFactor"))
        self.assertLess(define, first_dev)


if __name__ == "__main__":
    unittest.main(verbosity=2)
