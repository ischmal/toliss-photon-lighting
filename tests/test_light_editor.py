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
        # The seeding itself lives in DbgReadManifest (one call per manifest
        # file, since 2026-08-10), so the ordering to pin is against those calls.
        body = self._fn("DbgLoadManifest")
        self.assertIn("DbgLoadTuning();", body)
        self.assertGreater(body.index("DbgLoadTuning();"), body.rindex("DbgReadManifest("),
                           "the restore must come after the lights are seeded")
        # ...and the branch values are resolved before it, so a record is checked
        # against the numbers the OBJ actually bakes.
        self.assertGreater(body.index("DbgLoadTuning();"), body.index("DbgApplyVariants();"),
                           "variants must be applied before the restore reads base[]")

    def test_a_read_only_session_never_restores_a_tuning_file(self):
        """A tuning file records edits made against a DEBUG OBJ. With a plain OBJ
        installed the lights bake their authored values and ignore those
        datarefs, so applying the records would put numbers on screen that
        nothing in the cockpit is drawing - an inspector that lies."""
        body = self._fn("DbgLoadManifest")
        self.assertIn("if (!gDbgReadOnly) DbgLoadTuning();", body)
        # ...and per light, which is what covers a MIXED session (one target
        # built --debug, the other not).
        self.assertIn("if (l.readOnly) continue;", self._fn("DbgApplyTuningRecord"))

    def test_a_read_only_light_can_never_be_written_to_the_tuning_file(self):
        # DbgLightChanged is the one gate every writer goes through - the edit
        # count, the dirty check and the per-light record all ask it first.
        m = re.search(r"static bool DbgLightChanged\([^)]*\)\s*\{", PLUGIN_CPP)
        assert m, "DbgLightChanged not found"
        body = PLUGIN_CPP[m.start():PLUGIN_CPP.index("\n}", m.start())]
        self.assertIn("if (l.readOnly) return false;", body)

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


class InfoManifestTests(unittest.TestCase):
    """The READ-ONLY manifest a plain build drops beside the installable OBJ
    (INFO_MANIFESTS, 2026-08-10), which is what lets a -Dev plugin show every
    light without -DebugObjs.

    Its whole value is being trustworthy: an inspector that shows the wrong
    branch, or a light on the wrong rheostat, is worse than none — you would act
    on it. Every assertion here is one of the ways it could quietly lie.
    """

    @classmethod
    def setUpClass(cls):
        cls.cfg = B.load_config(wing="stock")
        cls.info = {}
        cls.debug = {}
        for target in ("interior", "screens"):
            em = B.Emitter(cls.cfg, "a320", "stock", target=target, debug=False)
            em.emit()
            cls.info[target] = em.info_lights
            dem = B.Emitter(cls.cfg, "a320", "stock", target=target, debug=True)
            dem.emit()
            cls.debug[target] = dem.debug_lights

    def test_both_manifests_cover_the_same_targets(self):
        # A target with a debug manifest and no info one is a target the Lights
        # tab can tune but not read, which is the wrong way round: reading is
        # the thing you want without a redeploy.
        self.assertEqual(set(B.INFO_MANIFESTS), set(B.DEBUG_MANIFESTS))
        self.assertEqual(set(B.INFO_MANIFESTS), set(B.DEBUG_TARGETS))

    def test_the_two_manifests_are_never_the_same_file(self):
        """⚠ Exactly one exists at a time and each build deletes the other, so
        which FILE is present is how the plugin tells tunable from read-only.
        Sharing a name would make that undecidable without parsing."""
        self.assertFalse(set(B.INFO_MANIFESTS.values())
                         & set(B.DEBUG_MANIFESTS.values()))

    def test_the_plugin_looks_for_exactly_these_filenames(self):
        # The plugin finds them from the aircraft path alone. A rename here with
        # no rename there is a tab that says "no lights" forever, and nothing in
        # the log to say a file was skipped.
        for name in list(B.INFO_MANIFESTS.values()) + list(B.DEBUG_MANIFESTS.values()):
            self.assertIn('"%s"' % name, PLUGIN_CPP,
                          "%s is not in plugin.cpp's candidate table" % name)

    def test_a_light_is_the_same_light_and_the_same_slot_in_both(self):
        """⚠ Slot N must name one light whichever manifest you are reading. The
        two are built by different code paths - a debug build emits ONE branch
        per fixture, a plain build one per profile - so the numbering agreeing is
        a property to check, not one to assume."""
        for target in ("interior", "screens"):
            info, dbg = self.info[target], self.debug[target]
            self.assertEqual(len(info), len(dbg),
                             "%s: the two manifests hold different light counts"
                             % target)
            for a, b in zip(dbg, info):
                self.assertEqual(
                    (a["n"], a["name"], a["fixture"], a["source"], a["index"], a["pos"]),
                    (b["n"], b["name"], b["fixture"], b["source"], b["index"], b["pos"]),
                    "%s: slot %d names a different light in the two manifests"
                    % (target, a["n"]))

    def test_the_slot_bases_keep_the_two_targets_apart(self):
        # Both manifests are merged into one list by the Lights tab, so a slot
        # collision would hide one light behind another.
        slots = [r["n"] for t in self.info for r in self.info[t]]
        self.assertEqual(len(set(slots)), len(slots))
        for r in self.info["screens"]:
            self.assertGreaterEqual(r["n"], B.DEBUG_SLOT_BASE["screens"])

    def test_variant_order_is_the_dataref_value(self):
        """⚠ THE CONTRACT THE WHOLE READ-ONLY VIEW RESTS ON. The plugin indexes
        `variants` with the live category dataref, because that is the same rule
        branch_gate emits the OBJ's ANIM_hides from. If the list were ever
        ordered any other way, every colour shown would be some other era's -
        and it would look entirely plausible."""
        for r in self.info["interior"]:
            profiles = self.cfg["categories"][r["category"]].get("profiles")
            self.assertIsNotNone(profiles, "category %r has no profile list"
                                 % r["category"])
            self.assertEqual([v["profile"] for v in r["variants"]], list(profiles),
                             "light %r on %r lists its branches in a different "
                             "order than the OBJ gates them"
                             % (r["name"], r["category"]))

    def test_every_light_carries_at_least_one_variant(self):
        # A row with none would show as a black omni light at the origin, which
        # reads as a broken fixture rather than as a broken manifest.
        for target in ("interior", "screens"):
            for r in self.info[target]:
                self.assertTrue(r["variants"], "%s has no variants" % r["name"])

    def test_the_category_names_are_ones_the_plugin_can_resolve(self):
        """DbgLiveBranch matches the row's `category` against kIntCategories to
        find which dataref picks the branch. An unmatched name falls back to
        branch 0 - i.e. it silently shows Old Halogen for everything."""
        keys = re.findall(r'\{"(\w+)",\s+"ToLissPhoton/interior/\w+"', PLUGIN_CPP)
        self.assertTrue(keys, "kIntCategories not found in plugin.cpp")
        for r in self.info["interior"]:
            self.assertIn(r["category"], keys)

    def test_a_screens_light_has_one_branch_and_the_du_source(self):
        # No category, so nothing picks between branches; and every screen light
        # rides a DU knob rather than a rheostat.
        for r in self.info["screens"]:
            self.assertEqual(len(r["variants"]), 1)
            self.assertEqual(r["source"], "du")

    def test_the_simplified_floods_are_marked_in_both(self):
        # The Cockpit tab reads `boost` to decide whether to offer the multiplier
        # at all, so the info rows have to carry it for the read-only readout.
        info = sorted(r["n"] for r in self.info["interior"] if "boost" in r)
        dbg = sorted(r["n"] for r in self.debug["interior"] if "boost" in r)
        self.assertEqual(info, dbg)
        self.assertTrue(info, "no `optimize: boost` lights - this test is now vacuous")

    def test_the_manifest_key_distinguishes_a_light_from_its_simplified_copy(self):
        """A boosted light is emitted TWICE - once in the full stack, once
        scaled in the reduced set - and the two are different rows. Merging them
        would fold three profile branches into six variants on one row and put
        every era in the wrong slot."""
        for target in ("interior", "screens"):
            keys = [(r["fixture"], r["name"]) for r in self.info[target]]
            self.assertEqual(len(set(keys)), len(keys),
                             "%s: two info rows share fixture+name" % target)

    def test_a_plain_build_records_no_debug_lights_and_the_reverse(self):
        # The two collections are alternatives. Both filled would mean the OBJ
        # carries dataref-driven lights AND a read-only description of them.
        for target in ("interior", "screens"):
            em = B.Emitter(self.cfg, "a320", "stock", target=target, debug=False)
            em.emit()
            self.assertEqual(em.debug_lights, [])
            self.assertTrue(em.info_lights)
            dem = B.Emitter(self.cfg, "a320", "stock", target=target, debug=True)
            dem.emit()
            self.assertEqual(dem.info_lights, [])
            self.assertTrue(dem.debug_lights)

    def test_the_exterior_gets_no_manifest_of_either_kind(self):
        # It has frozen goldens and no debug build, so there is nothing to read
        # a manifest against - and an extra file beside the exterior OBJ would
        # be the first thing to look wrong in a release diff.
        em = B.Emitter(self.cfg, "a320", "stock", target="exterior", debug=False)
        em.emit()
        self.assertEqual(em.info_lights, [])
        self.assertNotIn("exterior", B.INFO_MANIFESTS)


if __name__ == "__main__":
    unittest.main()
