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
import patch_acf_screens as PS  # noqa: E402
import photon_dsl as PD        # noqa: E402

PLUGIN_CPP = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
    encoding="utf-8", errors="replace")
DEV_PLUGIN = (REPO / "src" / "plugin" / "PI_PhotonDevReload.py").read_text(
    encoding="utf-8", errors="replace")

SCREEN_AIRFRAMES = ("a319", "a320", "a321")
SPILL_PREFIX = "ToLissPhoton/screens/spill/"


def build(airframe="a320", debug=False):
    cfg = B.load_config(wing="stock")
    return B.Emitter(cfg, airframe, "stock", target="screens", debug=debug).emit()


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

    def test_a339_has_no_screens_target(self):
        """Its cockpit is a different model, so these coordinates are meaningless
        for it — the same reasoning that keeps it out of the interior mod."""
        self.assertNotIn("screens", B.OBJ_TARGETS["a339"])
        cfg = B.load_config(wing="stock")
        with self.assertRaises(SystemExit):
            B.Emitter(cfg, "a339", "stock", target="screens").emit()

    def test_all_six_lights_are_spill_custom(self):
        """Brightness is per-display, which no rheostat INDEX can express — so
        every one of these must be a LIGHT_SPILL_CUSTOM, never a LIGHT_PARAM."""
        text = build()
        self.assertNotIn("LIGHT_PARAM", text)
        self.assertEqual(len(spill_lines(text)), 6)


class NoGatingTests(unittest.TestCase):
    """The screens have ONE look, so the OBJ is deliberately un-gated.

    A category with two branches that render identically is a dead menu row, which
    has already shipped twice on the interior. Emitting a gate here would either
    do that or need a fake second palette. If someone later gives these a category,
    these tests fail and the reasoning has to be revisited on purpose."""

    def test_no_anim_hide_or_show_anywhere(self):
        text = build()
        self.assertNotIn("ANIM_hide", text)
        self.assertNotIn("ANIM_show", text)

    def test_no_screens_category_dataref_is_referenced(self):
        self.assertNotIn("ToLissPhoton/screens/" + "<category>", build())
        self.assertNotRegex(build(), r"ToLissPhoton/screens/(?!spill/)")

    def test_one_anim_block_per_light(self):
        text = build().replace("\r\n", "\n")
        self.assertEqual(text.count("ANIM_begin"), 6)
        self.assertEqual(text.count("ANIM_end"), 6)


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
        conditioned aim would reach the Emitter as an unrecognised property."""
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

    def test_alpha_is_baked_low_for_the_plugin_absent_case(self):
        """With the plugin absent X-Plane draws the baked params unmodified, so
        the degraded state must be a faint glow — never a glare in a dark
        cockpit. Slot 6 of LIGHT_SPILL_CUSTOM (x y z r g b a ...) is alpha."""
        for ln in spill_lines(build()):
            alpha = float(ln.split()[7])
            self.assertGreater(alpha, 0.0, ln)
            self.assertLessEqual(alpha, 0.25, f"baked alpha too bright: {ln}")


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
        for slot, du in sorted(manifest.items()):
            self.assertEqual(
                du, plugin[slot],
                f"slot {slot}: the DSL says DUBrightness[{du}] but kScreenDU "
                f"says [{plugin[slot]}] — one of them binds the wrong screen")

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


class ReleaseIsolationTests(unittest.TestCase):
    """The experiment must not reach dist/ — and so a release bundle — by default."""

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
        """a339 has no interior/screens key. A named target on it is an error;
        a set-valued one (`both`, `all`) must skip it, not raise."""
        with tempfile.TemporaryDirectory() as td:
            args = type("A", (), {"airframe": "a339", "wing": "stock", "out": None,
                                  "flat": False, "write": False, "target": "all",
                                  "debug": False, "debug_pos": False,
                                  "no_debug_pos": False})()
            args.out = Path(td)
            B.cmd_build(args)
            written = [p.name for p in Path(td).rglob("*.obj")]
            self.assertEqual(written, ["ExternalLights_XP12.obj"])


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


class AcfAttachTests(unittest.TestCase):
    def test_attach_adds_a_row_and_bumps_the_count(self):
        text = make_acf()
        self.assertFalse(PS.is_attached(text))
        new, changed = PS.patch_text(text)
        self.assertTrue(PS.is_attached(new))
        self.assertEqual(PS.declared_count(new), PS.declared_count(text) + 1)
        self.assertEqual(PS.find_obj(new, PS.SCREENS_OBJ), 3)
        self.assertGreater(changed, 0)

    def test_attach_is_idempotent(self):
        once, _ = PS.patch_text(make_acf())
        twice, changed = PS.patch_text(once)
        self.assertEqual(changed, 0)
        self.assertEqual(twice, once)

    def test_detach_restores_the_file_byte_for_byte(self):
        for style in ("xp12", "xp11"):
            for eol in ("\r\n", "\n"):
                with self.subTest(style=style, eol=eol):
                    text = make_acf(style=style, eol=eol)
                    patched, _ = PS.patch_text(text)
                    back, _ = PS.unpatch_text(patched)
                    self.assertEqual(back, text)

    def test_detach_is_idempotent(self):
        text = make_acf()
        back, changed = PS.unpatch_text(text)
        self.assertEqual(changed, 0)
        self.assertEqual(back, text)

    def test_row_is_copied_from_lights_inn_not_invented(self):
        """Our light coordinates are authored in lights_inn.obj's frame, and that
        frame's origin differs per airframe (26.00 / 20.75 / 6.78 ft). Copying is
        the only thing that is correct on all three."""
        text = make_acf()
        new, _ = PS.patch_text(text)
        rows = PS.attachment_rows(new)
        src = rows[PS.find_obj(text, PS.FRAME_TEMPLATE_OBJ)]
        ours = rows[PS.find_obj(new, PS.SCREENS_OBJ)]
        self.assertEqual(ours[PS.FILE_FIELD], PS.SCREENS_OBJ)
        for field, value in src.items():
            if field != PS.FILE_FIELD:
                self.assertEqual(ours[field], value, field)

    def test_float_style_is_preserved_not_reformatted(self):
        """The XP11/XP12 trap: values are copied as strings, so each file keeps
        its own rendering and nothing is re-formatted."""
        xp11, _ = PS.patch_text(make_acf(style="xp11"))
        row = PS.attachment_rows(xp11)[PS.find_obj(xp11, PS.SCREENS_OBJ)]
        self.assertEqual(row["_v10_att_z_acf_prt_ref"], "20.75")
        xp12, _ = PS.patch_text(make_acf(style="xp12"))
        row = PS.attachment_rows(xp12)[PS.find_obj(xp12, PS.SCREENS_OBJ)]
        self.assertEqual(row["_v10_att_z_acf_prt_ref"], "20.750000000")

    def test_refuses_when_there_is_no_frame_to_copy(self):
        """Better to fail loudly than to attach at a guessed datum and mis-place
        every light on two airframes out of three."""
        with self.assertRaises(PS.AcfError):
            PS.patch_text(make_acf(with_inn=False))

    def test_missing_count_is_an_error_not_a_guess(self):
        text = make_acf().replace("P _obja/count 3\r\n", "")
        with self.assertRaises(PS.AcfError):
            PS.declared_count(text)

    def test_a_row_outside_the_declared_count_does_not_read_as_installed(self):
        """A row at index >= count is in the file but never read by X-Plane. It
        looks installed to a grep and draws nothing in the sim."""
        patched, _ = PS.patch_text(make_acf())
        broken = patched.replace("P _obja/count 4", "P _obja/count 3")
        self.assertIsNotNone(PS.find_obj(broken, PS.SCREENS_OBJ))
        self.assertFalse(PS.is_attached(broken))

    def test_detach_closes_the_gap_when_a_row_was_added_after_ours(self):
        """X-Plane reads indices 0..count-1, so leaving a hole would silently
        unload whatever the table says that index is — another mod's OBJ."""
        patched, _ = PS.patch_text(make_acf())
        later = patched.replace(
            "P _obja/count 4",
            "P _obja/4/_v10_att_file_stl other_mod.obj\r\nP _obja/count 5")
        back, _ = PS.unpatch_text(later)
        rows = PS.attachment_rows(back)
        self.assertIsNone(PS.find_obj(back, PS.SCREENS_OBJ))
        self.assertEqual(PS.find_obj(back, "other_mod.obj"), 3)
        self.assertEqual(PS.declared_count(back), 4)
        self.assertEqual(sorted(rows), list(range(4)))

    def test_acf_files_skips_backups(self):
        """Patching a .bak would corrupt another mod's uninstall — Durantula
        keeps an a320.acf.durantula.bak of this very file."""
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d / "a320.acf").write_text(make_acf(), encoding="utf-8", newline="")
            (d / "a320.acf.durantula.bak").write_text(make_acf(), encoding="utf-8",
                                                      newline="")
            found = [p.name for p in PS.acf_files(d)]
            self.assertEqual(found, ["a320.acf"])

    def test_run_round_trips_a_file_on_disk(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            p = d / "a320.acf"
            original = make_acf().encode("utf-8")
            p.write_bytes(original)
            PS.run(d)
            self.assertNotEqual(p.read_bytes(), original)
            with p.open("r", **PS._ENC) as f:
                self.assertTrue(PS.is_attached(f.read()))
            PS.run(d, reverse=True)
            self.assertEqual(p.read_bytes(), original)


class AcfDryRunTests(unittest.TestCase):
    def test_dry_run_writes_nothing(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            p = d / "a320.acf"
            original = make_acf().encode("utf-8")
            p.write_bytes(original)
            PS.run(d, dry=True)
            self.assertEqual(p.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
