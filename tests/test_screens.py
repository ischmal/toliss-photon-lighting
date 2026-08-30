"""Screen glow (EXPERIMENTAL display-glow target) — build, wiring and .acf attach.

The feature is dev-only and opt-in, which means most of its failure modes are
SILENT: a light that binds the wrong screen, a table that drifts out of step with
the plugin, an OBJ that ships in a release it was never meant to reach, or an
attachment row that mis-places every light on two airframes out of three. Each of
those gets a named test here.

See docs/screens_plan.md.
"""
from __future__ import annotations

import json
import re
import math
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
if str(BUILD) not in sys.path:
    sys.path.insert(0, str(BUILD))
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

import build_objs as B          # noqa: E402
import photon_dsl as PD        # noqa: E402

PLUGIN_CPP = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
    encoding="utf-8", errors="replace")
DEV_PLUGIN = (REPO / "src" / "plugin" / "PI_PhotonDevReload.py").read_text(
    encoding="utf-8", errors="replace")

# The A3xx three, which share one OBJ. The a339 is a screen airframe too but its
# OBJ is deliberately NOT one of these — see A339PositionTests.
SCREEN_AIRFRAMES = ("a319", "a320", "a321")
SPILL_PREFIX = "ToLissPhoton/screens/spill/"


def build(airframe="a320", debug=False):
    cfg = B.load_config(wing="stock")
    return B.Emitter(cfg, airframe, "stock", target="screens", debug=debug).emit()


def code(text):
    """The OBJ with its comments dropped.

    The credit banner NAMES the gate dataref and says what it does — which is
    where a reader meets it first — so a test that greps the whole file counts
    the documentation as an occurrence."""
    return "\n".join(ln for ln in text.replace("\r\n", "\n").split("\n")
                     if not ln.strip().startswith("#"))


def spill_lines(text):
    return [ln.strip() for ln in text.replace("\r\n", "\n").split("\n")
            if ln.strip().startswith("LIGHT_SPILL_CUSTOM")]


class BuildTests(unittest.TestCase):
    def test_every_a3xx_builds_six_screen_lights(self):
        for af in SCREEN_AIRFRAMES:
            with self.subTest(airframe=af):
                self.assertEqual(len(spill_lines(build(af))), 6)

    def test_the_three_airframes_emit_identical_screen_objs(self):
        """a319/a321 inherit the fixtures from a320 by `extends`, and the OBJ is
        attached in each aircraft's OWN frame by the patcher — so one file is
        correct for all three, exactly as lights_inn.obj is."""
        texts = {af: build(af) for af in SCREEN_AIRFRAMES}
        self.assertEqual(len(set(texts.values())), 1, texts.keys())

    def test_a339_has_its_own_screens_target(self):
        """Added 2026-08-11. It was excluded alongside the interior, but the two
        exclusions were never the same argument: the interior needs Gus's source
        data and this needs only six positions and AirbusFBW/DUBrightness, both of
        which the A330-900 has. What was true — its coordinates are not the A3xx's
        — is handled by giving it its own fixtures, not by leaving it out."""
        self.assertIn("screens", B.OBJ_TARGETS["a339"])
        self.assertEqual(len(spill_lines(build("a339"))), 6)

    def test_a339_still_has_no_interior_target(self):
        """The other half of the split above: this one stays out (decision #14)."""
        self.assertNotIn("interior", B.OBJ_TARGETS["a339"])

    def test_all_six_lights_are_spill_custom(self):
        """Brightness is per-display, which no rheostat INDEX can express — so
        every one of these must be a LIGHT_SPILL_CUSTOM, never a LIGHT_PARAM."""
        text = build()
        self.assertNotIn("LIGHT_PARAM", text)
        self.assertEqual(len(spill_lines(text)), 6)


class NoGatingTests(unittest.TestCase):
    """The screens have ONE look, so no light here is gated on a LOOK.

    A category with two branches that render identically is a dead menu row, which
    has already shipped twice on the interior. Emitting a category here would
    either do that or need a fake second palette. If someone later gives these a
    category, these tests fail and the reasoning has to be revisited on purpose.

    The ONE gate in the file is the master on/off block (MasterGateTests below),
    which is not a look — it is the whole feature."""

    def test_the_only_gate_is_the_master_block(self):
        text = code(build())
        self.assertNotIn("ANIM_show", text)
        self.assertEqual(text.count("ANIM_hide"), 1)
        self.assertIn(f"ANIM_hide 1 1 {MASTER_DREF}", text)

    def test_no_screens_category_dataref_is_referenced(self):
        self.assertNotIn("ToLissPhoton/screens/" + "<category>", build())
        self.assertNotRegex(code(build()),
                            r"ToLissPhoton/screens/(?!spill/|disabled)")

    def test_one_anim_block_per_light_inside_the_master_block(self):
        text = build().replace("\r\n", "\n")
        self.assertEqual(text.count("ANIM_begin"), 6 + 1)
        self.assertEqual(text.count("ANIM_end"), 6 + 1)


#: The whole-OBJ gate. Named for the OFF state on purpose — see MasterGateTests.
MASTER_DREF = "ToLissPhoton/screens/disabled"


class MasterGateTests(unittest.TestCase):
    """One ANIM_hide around the whole OBJ, so that switching "backlight
    illumination" off costs one dataref read rather than six spill lights drawn
    at alpha 0.

    Turning the alpha down was always enough to make them INVISIBLE; it was never
    enough to make them FREE, and free is the entire point of the switch. Six
    LIGHT_SPILL_CUSTOMs are six lights the renderer sets up and six 9-float
    accessor calls, whatever alpha says."""

    def test_the_gate_wraps_every_light(self):
        text = build().replace("\r\n", "\n")
        lines = [ln.strip() for ln in text.split("\n") if ln.strip()]
        gate = lines.index(f"ANIM_hide 1 1 {MASTER_DREF}")
        self.assertEqual(lines[gate - 1], "ANIM_begin")
        self.assertEqual(lines[-1], "ANIM_end")
        for ln in lines:
            if ln.startswith("LIGHT_SPILL_CUSTOM"):
                self.assertGreater(lines.index(ln), gate)

    def test_the_gate_is_outside_every_per_light_block(self):
        """Nesting matters: a gate INSIDE the six blocks would be six gates, and
        the renderer would still walk six blocks to find them all off."""
        recs = B.parse_records(build())
        for rec in recs:
            self.assertEqual(rec[0][0][3], MASTER_DREF,
                             "the master gate is not the outermost one")

    def test_polarity_is_fail_open(self):
        """⚠ Named for the OFF state, and this is why: an OBJ8 ANIM_hide reading a
        dataref that does not exist sees 0 forever (an OBJ binds dataref NAMES at
        load time). With the plugin absent — or older than the OBJ — 0 has to mean
        DRAW, or the six lights vanish and it reads in-sim as "the .acf attachment
        never happened", sending the next person after the wrong bug entirely.

        Same reasoning that bakes the alpha low rather than at 0 (SCREENS_CREDIT):
        the degraded state is a faint glow, never nothing."""
        self.assertTrue(MASTER_DREF.endswith("/disabled"))
        self.assertIn(f"ANIM_hide 1 1 {MASTER_DREF}", code(build()))

    def test_the_builder_and_the_plugin_name_the_same_dataref(self):
        self.assertEqual(B.TARGET_MASTER_GATE["screens"], MASTER_DREF)
        self.assertIn(f'"{MASTER_DREF}"', PLUGIN_CPP)

    def test_the_gate_is_derived_from_the_user_switch_not_stored(self):
        """One source of truth. A cached copy updated in the engine loop would be
        a second place to forget, and it would lag the checkbox by a frame — and
        it must read the switch through DisplayFeatureOn, so the Displays master
        cannot be forgotten at this one consumer (see DisplaysMasterTests)."""
        m = re.search(r"static int\s+ReadScreensDisabledInt\(void\*\)\s*\{([^}]*)\}",
                      PLUGIN_CPP)
        self.assertIsNotNone(m)
        self.assertIn("DisplayFeatureOn(gDisplays.spill)", m.group(1))
        m = re.search(r"static float ReadScreensDisabledFloat\(void\*\)\s*\{([^}]*)\}",
                      PLUGIN_CPP)
        self.assertIsNotNone(m)
        self.assertIn("DisplayFeatureOn(gDisplays.spill)", m.group(1))

    def test_the_gate_is_registered_at_xpluginstart_as_int_and_float(self):
        """The registration tripwire, same as every category dataref: an OBJ binds
        NAMES at load time and one registered late — or as one type only — reads 0
        forever. Here 0 means "draw", so the failure is a switch that does nothing
        rather than six missing lights, which is the right way round but still
        wrong."""
        start = PLUGIN_CPP.index("PLUGIN_API int XPluginStart")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("PLUGIN_API int XPluginEnable")]
        reg = body[body.index("gScreensDisabledRef = XPLMRegisterDataAccessor("):]
        self.assertIn("xplmType_Int | xplmType_Float", reg[:200])
        self.assertIn("ReadScreensDisabledInt", reg[:300])
        self.assertIn("ReadScreensDisabledFloat", reg[:300])

    def test_the_per_frame_path_still_gates_too(self):
        """BOTH halves stay. The OBJ block is the cheap gate for the user's
        switch; the per-frame size factor is what follows the DU knobs and the DC
        bus, neither of which belongs in an ANIM_hide. Removing the per-frame
        gating because "the block is hidden anyway" would leave the spill lit
        through a bus failure whenever the switch happens to be on."""
        engine = re.search(r"const float userGain = [^\n]*", PLUGIN_CPP)
        self.assertIsNotNone(engine)
        self.assertIn("DisplayFeatureOn(gDisplays.spill)", engine.group(0))
        self.assertIn("const bool busLive = DcBusLive();", PLUGIN_CPP)

    def test_a_debug_build_has_no_master_gate(self):
        """The dev tuner owns every light in a debug OBJ; a master gate could only
        hide the one being tuned, and it is driven by a user switch the tuning
        session has no reason to be holding on."""
        self.assertNotIn(MASTER_DREF, code(build(debug=True)))

    def test_the_interior_has_no_master_gate(self):
        """Deliberately screens-only. The cockpit lights are not one feature with
        one switch — they are five categories the user picks looks for — so there
        is no state in which hiding all of them is what was asked for."""
        self.assertNotIn("interior", B.TARGET_MASTER_GATE)
        cfg = B.load_config(wing="stock")
        inn = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        self.assertNotIn("/disabled", inn)


class NeoCompensationTests(unittest.TestCase):
    """⚠ THE SCREEN GLOWS ARE SPILL LIGHTS, so a lighting mod that raises
    X-Plane's spill cutoff culls them exactly as it culls the cockpit lamps —
    sooner, in fact, since they are the dimmest wide-cone lights Photon ships.

    ⚠ The cockpit's compensation is core/patch_intensity rewriting the OBJ and it
    CANNOT reach here: CanPatch is gated on the interior needle and
    lights_screens.obj does not carry it. So the screens compensate on the
    per-frame size factor instead. Both halves must move together, or one tick of
    one checkbox brightens half of what the user is looking at."""

    def test_the_screen_size_factor_carries_the_neo_boost(self):
        """The whole contract. It is a single term in an arithmetic expression,
        which is exactly the kind of thing a tidy-up drops silently — the lights
        keep working, they are just back to being culled."""
        m = re.search(r"gScreenSizeFactor\[i\] = .*?: 0\.0f;", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "the per-frame size factor moved")
        self.assertIn("neoBoost", m.group(0))

    def test_the_boost_is_the_cockpits_constant_and_not_a_literal(self):
        """⚠ ONE SEED, TWO READERS. kNeoBoost is unmeasured and pending the in-sim
        sweep (docs/neo_compat_plan.md §7). A literal here would agree with the
        cockpit only until the day someone retunes it, and the disagreement would
        show up as the displays and the lamps compensating by different amounts —
        which reads as a tuning problem, not as a dropped constant."""
        m = re.search(r"const float neoBoost =[^;]*;", PLUGIN_CPP)
        self.assertIsNotNone(m, "the boost is no longer hoisted out of the loop")
        self.assertIn("intensity::kNeoBoost", m.group(0))
        self.assertIn("gCockpit.neo", m.group(0))
        self.assertIn(
            "1.0f", m.group(0),
            "the flag being off must be exact identity, never a second factor")


class FixtureProfileSyntaxTests(unittest.TestCase):
    """`profile:` is the new un-gated fixture form that makes the screens target
    expressible without inventing a category. A fixture takes exactly one of
    `category:` (N gated branches) or `profile:` (one un-gated look)."""

    STYLE = """
axis profile: halogen led;
axis side: port starboard;
axis swatch: bb sp;
axis wing: stock;
palette halogen { white: 1 1 1; }
palette led { white: 0 0 1; }
categories { nav: halogen; }
light lamp { class: airplane_nav_bb; color: white; intensity: 1; cone: 0.1; dir: 0 0 1; }
"""
    LAYOUT = """
airframe a320 {
  fixture f "F" { %s pos: 0 0 0; lamp; }
}
"""

    def _parse(self, decl):
        with tempfile.TemporaryDirectory() as td:
            style = Path(td) / "style.phdsl"
            layout = Path(td) / "layout.phdsl"
            style.write_text(self.STYLE, encoding="utf-8")
            layout.write_text(self.LAYOUT % decl, encoding="utf-8")
            return PD.load_dsl(style, layout, wing="stock")

    def test_the_harness_itself_parses(self):
        """Guards the two error tests below: if the scaffold were malformed they
        would pass on the wrong DslError and prove nothing."""
        cfg = self._parse("category: nav;")
        self.assertIn("f", cfg["airframes"]["a320"]["fixtures"])

    def test_neither_category_nor_profile_is_an_error(self):
        with self.assertRaises(PD.DslError) as cm:
            self._parse("")
        self.assertIn("category", str(cm.exception))
        self.assertIn("profile", str(cm.exception))

    def test_both_at_once_is_an_error(self):
        """Ambiguous: gated in which palette? Refused rather than picking one."""
        with self.assertRaises(PD.DslError) as cm:
            self._parse("category: nav; profile: led;")
        self.assertIn("exactly one", str(cm.exception))

    def test_profile_alone_parses_and_carries_no_category(self):
        cfg = self._parse("profile: led;")
        gating = cfg["airframes"]["a320"]["fixtures"]["f"]["gating"]
        self.assertEqual(gating.get("profile"), "led")
        self.assertNotIn("category", gating)

    def test_unknown_profile_is_caught_at_emit(self):
        cfg = self._parse("profile: nosuchpalette;")
        with self.assertRaises(SystemExit):
            B.Emitter(cfg, "a320", "stock").emit()

    def test_screens_fixtures_all_use_the_profile_form(self):
        cfg = B.load_config(wing="stock")
        fixtures = cfg["airframes"]["a320"]["fixtures"]
        screens = {n: f for n, f in fixtures.items()
                   if f and f.get("target") == "screens"}
        self.assertEqual(len(screens), 6)
        for name, f in screens.items():
            self.assertEqual(f["gating"].get("profile"), "screen", name)
            self.assertNotIn("category", f["gating"], name)


class AimSugarTests(unittest.TestCase):
    """`aim: <pitch> <yaw>;` and `spread: <deg>;` — angles instead of a vector and a
    backwards-running cosine.

    Both are rewritten to `dir:`/`cone:` at LOAD, so the Emitter, the OBJ and `check`
    never see them. Two problems solved: a hand-authored vector's length is not free
    (X-Plane compares `cone` against a dot product with it), and "10 degrees further
    down" is not an edit anyone can do to three components in their head."""

    STYLE = """
axis profile: halogen led;
axis side: port starboard;
axis swatch: bb sp;
axis wing: stock;
palette halogen { white: 1 1 1; }
palette led { white: 0 0 1; }
categories { nav: halogen; }
light lamp { class: airplane_nav_bb; color: white; intensity: 1; %s }
"""
    # `light_props` is either ";" (bare reference) or a " { ... }" block — a block form
    # must NOT also carry a trailing semicolon, so the caller supplies it.
    LAYOUT = """
airframe a320 {
  fixture f "F" { category: nav; pos: 0 0 0; lamp%s }
}
"""

    def _parse(self, type_props, light_props=";"):
        with tempfile.TemporaryDirectory() as td:
            style = Path(td) / "style.phdsl"
            layout = Path(td) / "layout.phdsl"
            style.write_text(self.STYLE % type_props, encoding="utf-8")
            layout.write_text(self.LAYOUT % light_props, encoding="utf-8")
            return PD.load_dsl(style, layout, wing="stock")

    def _type(self, cfg):
        return cfg["lightTypes"]["lamp"]

    def test_the_harness_itself_parses(self):
        cfg = self._parse("cone: 0.1; dir: 0 0 1;")
        self.assertEqual(self._type(cfg)["dir"], [0, 0, 1])

    def test_aim_becomes_a_unit_dir_vector(self):
        cfg = self._parse("spread: 30; aim: 0 180;")
        self.assertEqual(self._type(cfg)["dir"], [0.0, 0.0, 1.0])
        self.assertNotIn("aim", self._type(cfg))

    def test_spread_becomes_the_cosine_and_runs_the_right_way(self):
        """The reason `spread` exists: a WIDER beam must be a BIGGER number here and a
        smaller `cone`. If that inverts, every angle in the DSL means its opposite."""
        narrow = self._parse("spread: 10; aim: 0 0;")
        wide = self._parse("spread: 70; aim: 0 0;")
        self.assertGreater(self._type(narrow)["cone"], self._type(wide)["cone"])
        self.assertAlmostEqual(self._type(narrow)["cone"],
                               math.cos(math.radians(10)), places=4)
        self.assertNotIn("spread", self._type(narrow))

    def test_declaring_both_forms_is_an_error(self):
        """No reading of `dir: 0 0 1; aim: -30 0;` is not a mistake, and silently
        preferring one would be a value the author never sees."""
        for props in ("cone: 0.1; dir: 0 0 1; aim: 0 180;",
                      "cone: 0.1; spread: 30; dir: 0 0 1;"):
            with self.subTest(props=props):
                with self.assertRaises(PD.DslError) as cm:
                    self._parse(props)
                self.assertIn("Keep one", str(cm.exception))

    def test_aim_wants_exactly_two_numbers(self):
        with self.assertRaises(PD.DslError) as cm:
            self._parse("cone: 0.1; aim: 0 0 1;")
        self.assertIn("2 numbers", str(cm.exception))

    def test_a_per_light_aim_overrides_the_type_s(self):
        """Angles have to work at both levels or they are only half a feature — most
        real aims are per-fixture (the panel floods differ only in direction)."""
        cfg = self._parse("spread: 30; aim: 0 0;", " { aim: -90 0; }")
        light = cfg["airframes"]["a320"]["fixtures"]["f"]["lights"][0]
        self.assertEqual(light["dir"], [0.0, -1.0, -0.0])

    def test_angles_survive_a_condition_block(self):
        """`@led { aim: ...; }` must convert inside the condition dict too, or a
        conditioned aim would reach the Emitter as an unrecognized property."""
        cfg = self._parse("cone: 0.1; aim: 0 0;  @led { aim: 0 180; }")
        got = self._type(cfg)["dir"]
        self.assertEqual(got["halogen"], [0.0, 0.0, -1.0])
        self.assertEqual(got["led"], [0.0, 0.0, 1.0])

    def test_every_aim_the_dsl_can_express_is_unit_length(self):
        """The structural half of the fix. `build` warns about non-unit `dir:`; an
        `aim:` cannot produce one at all, whatever angles are given."""
        for pitch in (-90, -47, 0, 12, 90):
            for yaw in (-180, -33, 0, 90, 271):
                d = PD.aim_to_dir(pitch, yaw)
                self.assertAlmostEqual(math.sqrt(sum(v * v for v in d)), 1.0,
                                       places=3, msg="aim %s %s -> %s"
                                                     % (pitch, yaw, d))

    def test_the_screens_use_the_angle_form(self):
        """They are the clearest case — one aim, shared by all six — so they are where
        the form is exercised against the real config rather than a scaffold."""
        src = (REPO / "src" / "lights" / "lights.style.phdsl").read_text(
            encoding="utf-8")
        block = src.split("light screen_glow {", 1)[1].split("}", 1)[0]
        # Comments stripped: they legitimately MENTION `dir:`/`cone:` while explaining
        # why the declarations are not those.
        decls = "\n".join(ln.split("//", 1)[0] for ln in block.splitlines())
        self.assertIn("aim:", decls)
        self.assertIn("spread:", decls)
        self.assertNotIn("dir:", decls)
        self.assertNotIn("cone:", decls)
        # Assert the SUGAR resolved, not one particular aim: the angles are a
        # live tuning knob (they moved off `0 180` on 2026-07-30), and pinning
        # the vector here turns every retune into a test failure that says
        # nothing about the form. What must hold is that `aim:` produced exactly
        # the vector those angles name, and that it is unit — which is the whole
        # reason to prefer the angle form (docs/dsl.md).
        m = re.search(r"aim:\s*(-?[\d.]+)\s+(-?[\d.]+)\s*;", decls)
        self.assertIsNotNone(m, "screen_glow has no parseable aim:")
        want = PD.aim_to_dir(float(m.group(1)), float(m.group(2)))
        cfg = B.load_config(wing="stock")
        got = cfg["lightTypes"]["screen_glow"]["dir"]
        for a, b in zip(got, want):
            self.assertAlmostEqual(a, b, places=3, msg=f"{got} != aim {m.group(0)}")
        self.assertAlmostEqual(math.sqrt(sum(v * v for v in got)), 1.0, places=3,
                               msg=f"aim: produced a non-unit dir: {got}")


class SpillDataRefTests(unittest.TestCase):
    """The three places that must agree: the DSL's per-fixture spill_dref, the
    plugin's kScreenCount, and the plugin's kSpillScreenPrefix."""

    def test_drefs_are_zero_through_five_and_unique(self):
        drefs = [ln.split()[-1] for ln in spill_lines(build())]
        self.assertEqual(sorted(drefs),
                         sorted(f"{SPILL_PREFIX}{n}" for n in range(6)))
        self.assertEqual(len(set(drefs)), 6, "two lights share a spill dataref")

    def test_plugin_declares_the_same_count(self):
        m = re.search(r"kScreenCount\s*=\s*(\d+)", PLUGIN_CPP)
        self.assertIsNotNone(m, "kScreenCount missing from plugin.cpp")
        self.assertEqual(int(m.group(1)), len(spill_lines(build())))

    def test_plugin_declares_the_same_dataref_prefix(self):
        m = re.search(r'kSpillScreenPrefix\s*=\s*"([^"]+)"', PLUGIN_CPP)
        self.assertIsNotNone(m, "kSpillScreenPrefix missing from plugin.cpp")
        self.assertEqual(m.group(1), SPILL_PREFIX)

    def test_the_screens_accessor_drives_size_and_the_map_one_drives_alpha(self):
        """⚠ THE TWO SPILL ACCESSORS DRIVE DIFFERENT SLOTS (2026-08-09), and
        nothing in the sim complains if they are swapped: driving alpha on a
        screen makes the knob fade a fixed pool instead of spreading it, and
        driving size on the map spot makes the map knob grow a pool at constant
        opacity. Both look plausible in a screenshot and wrong in motion.

        The slot numbers themselves are the load-bearing part: 3 and 4 are the
        only two that mean the same thing in both custom-light orderings (the
        rotation is confined to the trailing four), which is what makes driving
        either one safe at all. test_light_editor.py pins that."""
        screens = PLUGIN_CPP[PLUGIN_CPP.index("static int ReadSpillScreen("):]
        screens = screens[:screens.index("\n}")]
        self.assertIn("kSpillSizeSlot", screens)
        self.assertNotIn("kSpillAlphaSlot", screens)

        mapspot = PLUGIN_CPP[PLUGIN_CPP.index("static int ReadSpillMap("):]
        mapspot = mapspot[:mapspot.index("\n}")]
        self.assertIn("kSpillAlphaSlot", mapspot)
        self.assertNotIn("kSpillSizeSlot", mapspot)

        self.assertIn("static const int   kSpillAlphaSlot  = 3;", PLUGIN_CPP)
        self.assertIn("static const int   kSpillSizeSlot   = 4;", PLUGIN_CPP)

    def test_the_authored_size_is_captured_once_and_reset_per_aircraft(self):
        """The plugin scales the OBJ's authored `size:` rather than owning a copy
        of it, so the .phdsl stays the only place a size is written. Two things
        make that safe and both fail silently if dropped:

        * it CAPTURES on first read and assigns thereafter — multiplying the
          buffer in place would compound to zero if X-Plane ever fed our own
          output back, and nothing here has measured that it does not;
        * the capture is dropped on aircraft load, or a rebuilt OBJ with a new
          `size:` would be scaled against the previous build's reach forever."""
        self.assertIn("gScreenBakedSize", PLUGIN_CPP)
        acc = PLUGIN_CPP[PLUGIN_CPP.index("static int ReadSpillScreen("):]
        acc = acc[:acc.index("\n}")]
        self.assertIn("gScreenBakedSeen[slot] = true;", acc)
        self.assertIn("gScreenBakedSize[slot] * gScreenSizeFactor[slot]", acc)

        # ⚠ The DEFINITION, not the forward declaration 1800 lines above it —
        # matching the bare signature slices the wrong function entirely.
        load = PLUGIN_CPP[PLUGIN_CPP.index("static void UpdateMenuVisibility() {"):]
        load = load[:load.index("\n}")]
        self.assertIn("gScreenBakedSeen[i] = false;", load,
                      "the captured sizes must be dropped on aircraft load")

    def test_the_baked_alpha_is_the_shipping_intensity_and_is_never_zero(self):
        """⚠ ALPHA IS NOW A CONSTANT THE PLUGIN NEVER WRITES (2026-08-09): the DU
        knob drives SIZE, so what the DSL bakes here is the intensity at every
        knob position, with the plugin loaded or absent.

        Zero is still the one value that must not appear. An invisible light
        reads in-sim as "the .acf attachment never happened" and sends the next
        person after the wrong bug — the same reasoning behind naming the master
        gate for its OFF state. The upper bound is 1 because that is the range,
        not a taste judgement: with alpha no longer a degraded-state fallback,
        how bright the glow should be is a tuning decision for the .phdsl.

        Slot 6 of LIGHT_SPILL_CUSTOM (x y z r g b a ...) is alpha."""
        for ln in spill_lines(build()):
            alpha = float(ln.split()[7])
            self.assertGreater(alpha, 0.0, ln)
            self.assertLessEqual(alpha, 1.0, f"baked alpha out of range: {ln}")

    def test_the_baked_size_is_the_reach_at_full_and_is_never_zero(self):
        """The driven slot. The plugin captures this from the pre-filled buffer
        and scales it by the DU knob, so a zero here would multiply out to a
        light with no reach at every knob position — invisible, and invisible is
        the failure this feature keeps having to design against. Slot 8 is size
        (x y z r g b a s ...)."""
        for ln in spill_lines(build()):
            size = float(ln.split()[8])
            self.assertGreater(size, 0.0, ln)


class DUMappingTests(unittest.TestCase):
    """Which AirbusFBW/DUBrightness index drives which light.

    The DSL states it per fixture (`index:`) so the debug tool can show it; the
    plugin states it in kScreenDU. A mismatch is invisible until someone dims one
    display in the sim and the wrong glow moves."""

    def _du_from_plugin(self):
        m = re.search(r"kScreenDU\s*\[\s*kScreenCount\s*\]\s*=\s*\{([^}]*)\}",
                      PLUGIN_CPP)
        self.assertIsNotNone(m, "kScreenDU missing from plugin.cpp")
        return [int(x) for x in m.group(1).replace(",", " ").split()]

    def _du_from_manifest(self):
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="screens", debug=True)
        em.emit()
        return {light["n"]: light["index"] for light in em.debug_lights}

    def test_dsl_and_plugin_agree_on_every_slot(self):
        plugin = self._du_from_plugin()
        manifest = self._du_from_manifest()
        self.assertEqual(len(plugin), 6)
        # Debug slots start at the screens' DEBUG_SLOT_BASE; kScreenDU indexes
        # the six spill lights from 0, so the base is subtracted to compare.
        base = B.DEBUG_SLOT_BASE["screens"]
        for slot, du in sorted(manifest.items()):
            self.assertEqual(
                du, plugin[slot - base],
                f"slot {slot}: the DSL says DUBrightness[{du}] but kScreenDU "
                f"says [{plugin[slot - base]}] — one of them binds the wrong screen")

    def test_each_screen_has_its_own_display(self):
        self.assertEqual(len(set(self._du_from_plugin())), 6,
                         "two screen lights ride the same display")

    def test_debug_manifest_marks_them_as_display_sourced(self):
        """`source: "du"` is what makes the dev tool read DUBrightness rather
        than a panel rheostat. These are spills WITH an index, which the
        interior's classifier would otherwise read as a panel light."""
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="screens", debug=True)
        em.emit()
        self.assertTrue(em.debug_lights)
        for light in em.debug_lights:
            self.assertEqual(light["source"], "du", light["fixture"])


class A339PositionTests(unittest.TestCase):
    """The A330-900's six lights (2026-08-11).

    ⚠ THE ONE THING THAT MAY DIFFER FROM THE A3xx IS POSITION. Color, alpha, size,
    aim and spread all come from the shared `screen_glow` light type, so a retune
    of the look moves both airframes together. If that stops being true, someone
    has put a look decision in the layout file, and the next retune will silently
    apply to one airframe only."""

    def _lights(self, airframe):
        """Each spill line as (pos, tail) — tail being everything the look owns:
        rgb, alpha, size, dir, cone and the spill dataref."""
        out = []
        for ln in spill_lines(build(airframe)):
            f = ln.split()
            out.append((tuple(f[1:4]), tuple(f[4:])))
        return out

    def test_only_the_positions_differ_from_the_a320(self):
        a320 = self._lights("a320")
        a339 = self._lights("a339")
        self.assertEqual(len(a320), 6)
        self.assertEqual(len(a339), 6)
        for i, ((pos20, tail20), (pos39, tail39)) in enumerate(zip(a320, a339)):
            with self.subTest(light=i):
                self.assertEqual(
                    tail20, tail39,
                    f"light {i}: the a339 differs from the a320 in something "
                    f"other than position — color/alpha/size/aim/spread belong "
                    f"to the shared light type, not to one airframe")
                self.assertNotEqual(
                    pos20, pos39,
                    f"light {i}: the a339 reuses the a320's position, which is a "
                    f"different flight deck against a different datum")

    def test_the_positions_are_in_the_a339_cockpit_frame(self):
        """A sanity floor, not a tuned value. The A3xx flight deck sits near
        z = -4.7 in lights_inn.obj's frame; the A330-900's sits near z = -29 in
        CockpitLighting_XP12.obj's. A number from the wrong frame is off by ~24 m
        and lands outside the aeroplane, which is the one error here that a
        screenshot could not tell you about — the light simply never appears."""
        for pos, _tail in self._lights("a339"):
            x, y, z = (float(v) for v in pos)
            self.assertLess(z, -25.0, f"z={z} is not in the a339 cockpit frame")
            self.assertGreater(z, -32.0, f"z={z} is behind the flight deck")
            self.assertLess(abs(x), 1.0, f"x={x} is outside the flight deck")
            self.assertTrue(0.0 < y < 1.5, f"y={y} is not at panel height")

    def test_the_du_indices_match_the_a320s(self):
        """Same six displays in the same Airbus numbering, so `index:` per screen
        is identical — only where the light SITS changes. A339 fixtures are
        hand-written rather than inherited, so nothing but this checks it."""
        def du_by_slot(airframe):
            cfg = B.load_config(wing="stock")
            em = B.Emitter(cfg, airframe, "stock", target="screens", debug=True)
            em.emit()
            return {light["n"]: light["index"] for light in em.debug_lights}
        self.assertEqual(du_by_slot("a339"), du_by_slot("a320"))


class ReleaseIsolationTests(unittest.TestCase):
    """`screens` must not reach dist/ from a bare `build`.

    ⚠ This is no longer the same statement as "must not reach a release bundle".
    make_release.py now emits the screen-glow OBJ deliberately, straight from the
    Emitter (build_screens_payload) — which is precisely WHY these tests still
    matter: `dist/` is a dev tree that a tuning pass rewrites constantly, and the
    bundle must be built from the DSL rather than from whatever happens to be
    sitting there. A debug build of this target is bound to
    ToLissPhoton/debug/light/* and is not installable."""

    def test_default_build_targets_exclude_screens(self):
        self.assertNotIn("screens", B.DEFAULT_TARGETS)
        self.assertIn("screens", B.TARGETS)

    def test_a_bare_build_writes_no_screens_obj(self):
        with tempfile.TemporaryDirectory() as td:
            args = type("A", (), {"airframe": "a320", "wing": "stock", "out": None,
                                  "flat": False, "write": False, "target": "both",
                                  "debug": False, "debug_pos": False,
                                  "no_debug_pos": False})()
            args.out = Path(td)
            B.cmd_build(args)
            written = [p.name for p in Path(td).rglob("*.obj")]
            self.assertTrue(written, "build produced nothing at all")
            self.assertNotIn("lights_screens.obj", written)

    def test_target_all_does_write_the_screens_obj(self):
        """The opt-in that keeps a tuning pass from leaving a stale OBJ behind:
        `--target all` is `both` PLUS screens, so the display glow rebuilds
        alongside the other two. It must stay a distinct word from `both` —
        widening `both` is what would leak the experiment into a release."""
        with tempfile.TemporaryDirectory() as td:
            args = type("A", (), {"airframe": "a320", "wing": "stock", "out": None,
                                  "flat": False, "write": False, "target": "all",
                                  "debug": False, "debug_pos": False,
                                  "no_debug_pos": False})()
            args.out = Path(td)
            B.cmd_build(args)
            written = [p.name for p in Path(td).rglob("*.obj")]
            self.assertIn("lights_screens.obj", written)
            for name in ("lights_inn.obj", "lights_out320_XP12.obj"):
                self.assertIn(name, written)

    def test_target_all_skips_airframes_without_the_target(self):
        """a339 has no INTERIOR key (decision #14). A named target on it is an
        error; a set-valued one (`both`, `all`) must skip it, not raise.

        ⚠ It DOES have a screens key since 2026-08-11, so this no longer asserts
        that a339 builds one file — that reading made the test a restatement of
        the airframe table rather than of the skip behavior it is named for. What
        it checks is that the missing target is stepped over silently while the
        present ones still emit."""
        with tempfile.TemporaryDirectory() as td:
            args = type("A", (), {"airframe": "a339", "wing": "stock", "out": None,
                                  "flat": False, "write": False, "target": "all",
                                  "debug": False, "debug_pos": False,
                                  "no_debug_pos": False})()
            args.out = Path(td)
            B.cmd_build(args)
            written = sorted(p.name for p in Path(td).rglob("*.obj"))
            self.assertEqual(written,
                             ["ExternalLights_XP12.obj", "lights_screens.obj"])
            self.assertNotIn("lights_inn.obj", written)


class DebugManifestTests(unittest.TestCase):
    def test_targets_have_distinct_manifest_names(self):
        names = list(B.DEBUG_MANIFESTS.values())
        self.assertEqual(len(names), len(set(names)),
                         "two targets share a manifest filename — one would "
                         "overwrite the other in the same objects/ folder")

    def test_dev_plugin_looks_for_every_manifest_the_builder_writes(self):
        """DebugManifests in the dev plugin is the read side of DEBUG_MANIFESTS.
        A target the builder writes but the tool never looks for is a debug build
        that silently does nothing."""
        for name in B.DEBUG_MANIFESTS.values():
            self.assertIn(name, DEV_PLUGIN,
                          f"{name} is written by build_objs but the dev plugin "
                          f"never looks for it")

    def test_debug_build_is_still_six_lights(self):
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="screens", debug=True)
        em.emit()
        self.assertEqual(len(em.debug_lights), 6)
        self.assertLessEqual(len(em.debug_lights), B.DEBUG_MAX_LIGHTS)

    def test_a_plain_build_removes_a_stale_debug_manifest(self):
        """⚠ Going BACK to a normal OBJ has to take the manifest with it.

        The manifest is what the Dev window's Lights tab reads. Left behind after
        a non-debug rebuild it describes an OBJ that no longer binds
        ToLissPhoton/debug/light/*, so the tab lists every light as usual and not
        one knob moves anything — which reads as the light editor being broken
        rather than as "you are not running a debug OBJ any more"."""
        with tempfile.TemporaryDirectory() as td:
            def build_args(debug):
                a = type("A", (), {"airframe": "a320", "wing": "stock", "flat": False,
                                   "write": False, "target": "screens", "debug": debug,
                                   "debug_pos": False, "no_debug_pos": False})()
                a.out = Path(td)
                return a

            B.cmd_build(build_args(True))
            manifest = (Path(td) / "A320" / "objects"
                        / B.DEBUG_MANIFESTS["screens"])
            self.assertTrue(manifest.exists(), "the debug build wrote no manifest")

            B.cmd_build(build_args(False))
            self.assertFalse(manifest.exists(),
                             "a plain build left the debug manifest behind — the "
                             "Lights tab will offer knobs that drive nothing")
            # ...and the OBJ itself is back to the real datarefs.
            obj = (Path(td) / "A320" / "objects" / "lights_screens.obj").read_text()
            self.assertNotIn(B.DEBUG_LIGHT_DREF, obj)

    def test_the_two_debug_targets_have_disjoint_slot_windows(self):
        """Slots come from per-target bases so both debug OBJs can be installed
        and driven at once (the native Dev window merges the manifests). The
        interior must stay below the screens base and the screens below the pool
        end, or two lights silently share a dataref and one knob moves two lamps."""
        cfg = B.load_config(wing="stock")
        inn = B.Emitter(cfg, "a320", "stock", target="interior", debug=True)
        inn.emit()
        scr = B.Emitter(cfg, "a320", "stock", target="screens", debug=True)
        scr.emit()
        inn_slots = [d["n"] for d in inn.debug_lights]
        scr_slots = [d["n"] for d in scr.debug_lights]
        base = B.DEBUG_SLOT_BASE["screens"]
        self.assertEqual(min(inn_slots), B.DEBUG_SLOT_BASE["interior"])
        self.assertLess(max(inn_slots), base,
                        "the interior debug build grew into the screens' slot "
                        "window — move the bases apart in DEBUG_SLOT_BASE")
        self.assertEqual(scr_slots, list(range(base, base + 6)))
        self.assertLess(max(scr_slots), B.DEBUG_MAX_LIGHTS)
        self.assertFalse(set(inn_slots) & set(scr_slots))


# ─── .acf attachment ─────────────────────────────────────────────────────────

def make_acf(count=3, style="xp12", with_inn=True, eol="\r\n"):
    """A minimal .acf with an attachment table, in either float style."""
    def num(v):
        return f"{v:.9f}" if style == "xp12" else str(float(v))

    rows = []
    names = ["fuselage.obj", "wingL.obj", "lights_inn.obj" if with_inn else "gear.obj"]
    for i, name in enumerate(names[:count]):
        rows += [
            f"P _obja/{i}/_obj_flags 5",
            f"P _obja/{i}/_v10_att_file_stl {name}",
            f"P _obja/{i}/_v10_att_x_acf_prt_ref {num(0)}",
            f"P _obja/{i}/_v10_att_y_acf_prt_ref {num(0.400000006)}",
            f"P _obja/{i}/_v10_att_z_acf_prt_ref {num(20.75)}",
            f"P _obja/{i}/_v10_is_internal 6",
        ]
    lines = ["I", "1 version", "ACF"] + rows + [f"P _obja/count {count}",
                                                "P acf/_tailnum ABCDE"]
    return eol.join(lines) + eol


# ⚠ THE .acf ATTACHMENT TESTS MOVED TO C++ (2026-08-09). `AcfAttachTests`
# lived here and drove `installer/patch_acf_screens.py`, which is gone with the
# Python installer. The attachment table — copying the lights_inn frame, the
# idempotent re-attach, the stale-row-beyond-the-count case and the renumber-down
# on detach — is pinned by the `screens:` cases in
# src/native/tests/core_tests.cpp. What stays in THIS file is the DSL side: which
# fixtures the screens target emits and how they are gated.

if __name__ == "__main__":
    unittest.main()
