"""The native Dev-window light editor, checked against the two files it must agree with.

The editor in `src/native/src/plugin.cpp` drives the same debug datarefs that
`build/build_objs.py` bakes into a `--debug` OBJ and that
`src/plugin/PI_PhotonDevReload.py` has driven until now. Three files, one wire
format — and every way they can disagree fails SILENTLY in the simulator:

  * a wrong slot count leaves the tail of the lights black,
  * a wrong keyframe span rescales every position edit by a constant,
  * a wrong marker stride draws one light's arrow on its neighbor,
  * and a wrong parameter ORDER aims the light somewhere the window does not
    say — which is the bug that cost this project a 48-permutation axis hunt
    (see the DATAREF-order note in CLAUDE.md).

None of those produce an error message, so they get a test instead.
"""
import pathlib
import re
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "build"))

import build_objs as B  # noqa: E402

PLUGIN_CPP = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
    encoding="utf-8", errors="replace"
)
DEV_PY = (REPO / "src" / "plugin" / "PI_PhotonDevReload.py").read_text(
    encoding="utf-8", errors="replace"
)


def cpp_int(name):
    m = re.search(r"static const int\s+%s\s*=\s*(-?\d+)\s*;" % name, PLUGIN_CPP)
    assert m, "%s not found in plugin.cpp" % name
    return int(m.group(1))


def cpp_float(name):
    m = re.search(r"static const float\s+%s\s*=\s*(-?[\d.]+)f?\s*;" % name, PLUGIN_CPP)
    assert m, "%s not found in plugin.cpp" % name
    return float(m.group(1))


def cpp_str(name):
    m = re.search(r'static const char\*\s+%s\s*=\s*"([^"]*)"\s*;' % name, PLUGIN_CPP)
    assert m, "%s not found in plugin.cpp" % name
    return m.group(1)


def cpp_int_array(name):
    m = re.search(r"static const int\s+%s\[[^\]]*\]\s*=\s*\{([^}]*)\}" % name, PLUGIN_CPP)
    assert m, "%s not found in plugin.cpp" % name
    return [int(x) for x in re.findall(r"-?\d+", m.group(1))]


def py_int(name):
    m = re.search(r"^%s\s*=\s*(-?\d+)" % name, DEV_PY, re.M)
    assert m, "%s not found in PI_PhotonDevReload.py" % name
    return int(m.group(1))


def py_tuple(name):
    m = re.search(r"^%s\s*=\s*\(([^)]*)\)" % name, DEV_PY, re.M)
    assert m, "%s not found in PI_PhotonDevReload.py" % name
    return [int(x) for x in re.findall(r"-?\d+", m.group(1))]


class WireFormatTests(unittest.TestCase):
    """plugin.cpp vs. build_objs.py — the generator bakes what the editor drives."""

    def test_the_slot_pool_is_the_size_the_generator_assumes(self):
        # Too few and the tail of a big manifest is unreachable; the OBJ still
        # binds those names and they read 0, i.e. black.
        self.assertEqual(cpp_int("kDbgMaxLights"), B.DEBUG_MAX_LIGHTS)

    def test_the_position_keyframe_span_matches(self):
        # The offset rides three ANIM_trans blocks keyed on +-this. A mismatch
        # does not fail, it SCALES every position edit by the ratio.
        self.assertEqual(cpp_float("kDbgPosRange"), B.DEBUG_POS_RANGE)

    def test_the_marker_dot_layout_matches(self):
        self.assertEqual(cpp_int("kDbgMarkAxisDots"), B.DEBUG_MARK_AXIS_DOTS)
        self.assertEqual(cpp_int("kDbgMarkRimDots"), B.DEBUG_MARK_RIM_DOTS)
        # kDbgMarkDots is derived in the header the same way DEBUG_MARK_DOTS is.
        self.assertEqual(
            1 + cpp_int("kDbgMarkAxisDots") + cpp_int("kDbgMarkRimDots"),
            B.DEBUG_MARK_DOTS,
        )

    def test_the_dataref_names_are_the_ones_the_obj_binds(self):
        # An OBJ binds NAMES at load time, so a typo here is not a broken link —
        # it is a light that reads 0 forever with nothing in the log.
        self.assertEqual(cpp_str("kDbgPosDataRef"), B.DEBUG_POS_DREF)
        self.assertEqual(cpp_str("kDbgMarkDataRef"), B.DEBUG_MARK_DREF)
        self.assertEqual(cpp_str("kDbgMarkShowRef"), B.DEBUG_MARK_SHOW_DREF)
        self.assertEqual(cpp_str("kDbgMarkParamRef"), B.DEBUG_MARK_PARAM_DREF)

    def test_the_light_slot_prefix_matches_what_the_generator_emits(self):
        # The generator writes "<prefix><n>"; the editor registers the same.
        self.assertTrue(
            B.DEBUG_LIGHT_DREF.startswith(cpp_str("kDbgLightPrefix").rstrip("/"))
            or cpp_str("kDbgLightPrefix").startswith(B.DEBUG_LIGHT_DREF)
        )


class SlotOrderTests(unittest.TestCase):
    """⚠ The one that matters most: the dataref order is NOT the OBJ line's order."""

    def test_the_wire_order_matches_the_python_tool(self):
        # Measured in-sim 2026-07-30 with the slot probe: the cone comes FIRST in
        # the trailing group of four. Both tools must permute identically or the
        # same light aims two different ways depending on which one is driving.
        self.assertEqual(cpp_int_array("kDbgWireOrder"), py_tuple("DebugDataRefOrder"))

    def test_alpha_and_size_are_index_stable_in_both_orderings(self):
        # This is why the SHIPPING lights were never affected by the rotation, and
        # why plugin.cpp's kSpillAlphaSlot is correct. If it ever stops being
        # true, every spill light in the project needs re-checking.
        order = cpp_int_array("kDbgWireOrder")
        self.assertEqual(order[3], 3)
        # ⚠ Slot 4 matters for the same reason since 2026-08-09: the screen glow
        # is driven on SIZE, not alpha. The rotation is confined to the trailing
        # four (cone, then dx/dy/dz), so 0..4 are fixed points — which is what
        # makes driving either 3 or 4 safe without a translation.
        self.assertEqual(order[4], 4)
        self.assertEqual(cpp_int("kSpillAlphaSlot"), 3)
        self.assertEqual(cpp_int("kSpillSizeSlot"), 4)

    def test_the_param_count_is_the_nine_a_custom_light_takes(self):
        self.assertEqual(cpp_int("kDbgParamCount"), 9)
        self.assertEqual(cpp_int("kDbgParamCount"), py_int("DebugParamCount"))

    def test_the_wire_order_is_a_permutation_and_not_a_typo(self):
        order = cpp_int_array("kDbgWireOrder")
        self.assertEqual(sorted(order), list(range(cpp_int("kDbgParamCount"))))


class OwnershipTests(unittest.TestCase):
    def test_the_editor_stands_down_when_the_python_tool_owns_the_pool(self):
        # Two owners cannot share these names: the Python accessors are read-only
        # computed values, so writing to them from C++ is impossible rather than
        # merely rude. The C++ side must therefore CHECK before registering.
        self.assertIn("XPLMFindDataRef((std::string(kDbgLightPrefix)", PLUGIN_CPP)
        self.assertIn("gDbgOwnsRefs = false", PLUGIN_CPP)

    def test_registration_happens_at_plugin_start(self):
        # An OBJ binds dataref names at LOAD time. Registering later - in
        # XPluginEnable, or lazily when the tab opens - leaves every debug light
        # reading 0 for the life of that aircraft.
        start = PLUGIN_CPP.index("static void StartPanelPass() {")
        end = PLUGIN_CPP.index("\n}", start)
        self.assertIn("RegisterDbgLightRefs();", PLUGIN_CPP[start:end])


class TuningPersistenceTests(unittest.TestCase):
    """⚠ A tuning session must survive the sim closing.

    It did not, once, and the cost was an afternoon of screen-glow color work
    that existed only in the plugin's memory: no file, no log line, nothing in
    git, and no way to recover it. Every assertion here is one of the paths that
    would have saved it.
    """

    def _fn(self, name):
        """The body of a function DEFINITION.

        ⚠ Not `index("static void NAME(")` — several of these are forward-declared
        hundreds of lines above the definition (they have to be: the aircraft-load
        path calls them long before the tuning file's own section), and matching
        the declaration slices whatever function happens to follow it. That
        silently tests the wrong code."""
        m = re.search(r"static void %s\([^)]*\)\s*\{" % name, PLUGIN_CPP)
        assert m, "%s definition not found in plugin.cpp" % name
        return PLUGIN_CPP[m.start():PLUGIN_CPP.index("\n}", m.start())]

    def test_the_session_is_saved_when_the_aircraft_reloads(self):
        # DbgLoadManifest reseeds every light from the manifest, so it is the
        # moment a session's edits are dropped. The save must come BEFORE it.
        start = PLUGIN_CPP.index("static void UpdateMenuVisibility() {")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
        self.assertIn("DbgAutoSaveTuning();", body)
        self.assertLess(body.index("DbgAutoSaveTuning();"),
                        body.index("DbgLoadManifest();"),
                        "the auto-save must run before the manifest reseeds the lights")

    def test_the_session_is_saved_when_the_plugin_shuts_down(self):
        # The ordinary way a tuning session ends: the user quits X-Plane.
        start = PLUGIN_CPP.index("PLUGIN_API void XPluginDisable(void) {")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
        self.assertIn("DbgAutoSaveTuning();", body)
        self.assertLess(body.index("DbgAutoSaveTuning();"), body.index("StopPanelPass();"),
                        "save before StopPanelPass tears the light pool down")

    def test_the_saved_session_is_read_back_after_the_manifest_loads(self):
        # A saved session is EDITS on top of the baked values, so the restore has
        # to run last - the manifest's numbers are the seeds it is checked against.
        body = self._fn("DbgLoadManifest")
        self.assertIn("DbgLoadTuning();", body)
        self.assertGreater(body.index("DbgLoadTuning();"), body.rindex("gDbgLights.push_back"),
                           "the restore must come after the lights are seeded")

    def test_an_untouched_session_cannot_blank_an_existing_file(self):
        # Auto-save fires on every aircraft load. Without this guard, loading an
        # aircraft and touching nothing would overwrite a previous session's file
        # with an empty one - the same loss, by a different route.
        body = self._fn("DbgAutoSaveTuning")
        self.assertIn("DbgTuningDirty()", body)
        self.assertIn("return;", body)

    def test_a_record_is_skipped_when_the_obj_has_moved_past_it(self):
        """Otherwise editing the .phdsl and rebuilding would appear to do
        nothing: the saved session would keep overwriting the new authored
        values, and the only symptom is a number that will not change."""
        body = self._fn("DbgApplyTuningRecord")
        self.assertIn("DbgTuningSeed", body)
        self.assertIn("++stale", body)
        # ...and the boost factor gets the same treatment, in the reader.
        reader = self._fn("DbgLoadTuning")
        self.assertIn("boost_from", reader)
        self.assertIn("gDbgFloodBoostSeed", reader)

    def test_records_are_keyed_on_fixture_and_name_not_on_slot(self):
        """Slots shift whenever the DSL gains or loses a light, and the cockpit
        NAMES repeat - `int_panelflood#a` is two different lamps. Only the pair
        is unique, so only the pair can be the key."""
        body = self._fn("DbgApplyTuningRecord")
        self.assertIn("l.name == r.name && l.fixture == r.fixture", body)
        # The writer has to emit the other half of that key.
        writer = self._fn("DbgSaveTuning")
        self.assertIn('"  fixture: "', writer)

    def test_the_manifest_key_really_is_unique(self):
        """The assumption the whole file format rests on, checked against the
        generator rather than assumed: names alone are NOT unique."""
        cfg = B.load_config(wing="stock")
        rows = []
        for target in ("interior", "screens"):
            em = B.Emitter(cfg, "a320", "stock", target=target, debug=True)
            em.emit()
            rows += em.debug_lights
        names = [r["name"] for r in rows]
        keys = [(r["fixture"], r["name"]) for r in rows]
        self.assertLess(len(set(names)), len(names),
                        "names became unique - the comment explaining why the key "
                        "is a pair is now misleading, even if nothing is broken")
        self.assertEqual(len(set(keys)), len(keys),
                         "fixture+name is no longer unique, so a restored tuning "
                         "would land on the wrong light")


if __name__ == "__main__":
    unittest.main()
