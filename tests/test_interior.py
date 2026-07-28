"""Interior ("Gus Mod") support — build side and the three ⚠ traps.

The traps are the three places docs/interior_plan.md §3.2 flags as easy to get
wrong in a way that FAILS SILENTLY rather than loudly. Each has a named test
here so a future refactor trips a red test instead of shipping a subtle bug.
"""
from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
if str(BUILD) not in sys.path:
    sys.path.insert(0, str(BUILD))
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

import build_objs as B  # noqa: E402
from installer import constants as K  # noqa: E402

GUS = REPO / "reference" / "gus"


def interior_records(airframe="a320"):
    cfg = B.load_config(wing="stock")
    return B.parse_records(B.Emitter(cfg, airframe, "stock", target="interior").emit())


def _interior_gates(rec):
    return [g for g in rec[0] if g[3].startswith("ToLissPhoton/interior/")]


def branch_records(recs, value):
    """Records VISIBLE when the interior category datarefs read `value`, with the
    gate stripped so they can be compared against Gus's ungated files.

    This models X-Plane's OWN visibility rule rather than pattern-matching a gate
    line: a block starts visible, and an ANIM_hide whose range covers the current
    value turns it off. Testing it this way is the point — the bug that shipped
    was a gate that pattern-matched perfectly and still left every branch
    visible. See Emitter.branch_gate."""
    out = Counter()
    for rec, n in recs.items():
        gates = _interior_gates(rec)
        if any(g[0] == "ANIM_hide" and g[1] <= value <= g[2] for g in gates):
            continue
        out[tuple(rec[1:])] += n
    return out


class ExteriorIsUnchangedTests(unittest.TestCase):
    """The N-way emitter must leave exterior emission BYTE-IDENTICAL. Every
    exterior category stays two-valued and so keeps the original ANIM_hide
    encoding; switching it to ANIM_show would rewrite every gate line in all
    three frozen reference/photon goldens for no behavioural gain."""

    def test_exterior_categories_are_all_two_way(self):
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="exterior")
        for cat in ("nav", "strobe", "beacon", "landing", "taxi", "to",
                    "rwyturnoff", "wing", "logo"):
            with self.subTest(category=cat):
                self.assertEqual(len(em.profiles_of(cat)), 2)

    def test_exterior_gate_still_uses_anim_hide(self):
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="exterior")
        self.assertEqual(em.branch_gate("nav", 0),
                         ["ANIM_hide 1 1 ToLissPhoton/exterior/nav"])
        self.assertEqual(em.branch_gate("nav", 1),
                         ["ANIM_hide 0 0 ToLissPhoton/exterior/nav"])

    def test_interior_gate_uses_anim_hide_on_both_sides(self):
        """NEVER ANIM_show. An OBJ8 anim block starts VISIBLE and a show/hide
        only acts while its range MATCHES, so a lone `ANIM_show -0.5 0.5` leaves
        the branch visible at dataref 2 as well — all three draw at once and
        switching does nothing (confirmed in-sim 2026-07-28). Each branch must
        instead HIDE the open-ended range either side of its own value."""
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="interior")
        d = "ToLissPhoton/interior/dome"
        self.assertEqual(em.branch_gate("dome", 0), [f"ANIM_hide 0.5 2.5 {d}"])
        self.assertEqual(em.branch_gate("dome", 1),
                         [f"ANIM_hide -0.5 0.5 {d}", f"ANIM_hide 1.5 2.5 {d}"])
        self.assertEqual(em.branch_gate("dome", 2), [f"ANIM_hide -0.5 1.5 {d}"])

    def test_no_interior_branch_is_gated_with_anim_show(self):
        """The whole-file guard behind the unit above: one stray ANIM_show is a
        branch that never turns off, and it fails silently."""
        cfg = B.load_config(wing="stock")
        inn = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        self.assertNotIn("ANIM_show", inn)

    def test_every_interior_dataref_value_selects_exactly_one_branch(self):
        """Simulate X-Plane's visibility model over the emitted file: a block is
        visible unless some ANIM_hide range covers the current value. Exactly one
        branch per category must survive at each of 0/1/2."""
        cfg = B.load_config(wing="stock")
        inn = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        for cat in ("dome", "map", "mainpnl", "pedestal", "console"):
            dref = f"ToLissPhoton/interior/{cat}"
            branches = []          # per block: list of (lo, hi) hidden ranges
            stack = []
            for line in inn.splitlines():
                s = line.strip()
                if s.startswith("ANIM_begin"):
                    stack.append([])
                elif s.startswith("ANIM_end"):
                    if stack:
                        b = stack.pop()
                        if b:
                            branches.append(b)
                elif s.startswith("ANIM_hide") and s.endswith(dref):
                    lo, hi = (float(x) for x in s.split()[1:3])
                    if stack:
                        stack[-1].append((lo, hi))
            self.assertTrue(branches, f"{cat} is not gated at all")
            for value in (0, 1, 2):
                visible = sum(1 for b in branches
                              if not any(lo <= value <= hi for lo, hi in b))
                # one block per fixture; every fixture shows exactly one branch
                self.assertEqual(visible, len(branches) // 3,
                                 f"{cat} at {value}: {visible} of {len(branches)} "
                                 f"blocks visible, expected {len(branches)//3}")

    def test_exterior_obj_contains_no_interior_fixtures_and_vice_versa(self):
        cfg = B.load_config(wing="stock")
        ext = B.Emitter(cfg, "a320", "stock", target="exterior").emit()
        inn = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        self.assertNotIn("ToLissPhoton/interior/", ext)
        self.assertNotIn("ToLissPhoton/exterior/", inn)
        self.assertNotIn("airplane_panel_sp", ext)


# The three documented departures from Gus's data. Each is pinned to the exact
# records it touches rather than merely tolerated, so that a FOURTH one — or any
# of these silently spreading to more lights — fails the suite.
#
#   #1  the CA/FO tablet lights, LED variant only:  orange -> warm
#   #2  the dome lights, both halogen variants:     era colour -> fluorescent
#       (the LED variant is untouched: fluoro and white are the same there)
#   #3  the CA/FO table lights, ALL THREE variants: constant warm -> era colour
GUS_OLD_ORANGE = (1.0, 0.37, 0.16)
GUS_NEW_AMBER = (0.8, 0.55, 0.3)
GUS_LED_COOL = (0.82, 0.82, 1.0)
GUS_WARM = (1.0, 0.89, 0.81)
PHOTON_FLUORO = (0.95, 0.95, 0.88)
GUS_ERA = {"current": GUS_OLD_ORANGE, "new": GUS_NEW_AMBER, "led": GUS_LED_COOL}

DOME_POSITIONS = {(-0.472, 1.09, -3.35), (0.472, 1.09, -3.35)}
TABLE_POSITIONS = {(-0.558, 0.333, -4.5), (0.558, 0.333, -4.5)}
TABLET_POSITIONS = {(-0.8, 0.7, -4.0), (0.8, 0.7, -4.0)}
PEDESTAL_FLOOD_POSITION = (0.0, 0.8, -4.1)


def expected_deviations(variant):
    """The (position, rgb) records that MUST differ from Gus in one variant, as
    (ours, theirs). Written out per deviation so the reason each record is here
    stays legible at the point of failure."""
    era = GUS_ERA[variant]
    ours, theirs = set(), set()
    # #3 — the table lamps join the pedestal flood's era ramp, in every variant.
    for p in TABLE_POSITIONS:
        ours.add((p, era))
        theirs.add((p, GUS_WARM))
    if variant == "led":
        # #1 — the tablets alone keep old-halogen orange in Gus's LED file.
        for p in TABLET_POSITIONS:
            ours.add((p, GUS_WARM))
            theirs.add((p, GUS_OLD_ORANGE))
    else:
        # #2 — the dome leaves the era ramp. Nothing to do under LED, where
        # fluoro and Gus's cool white are the same colour.
        for p in DOME_POSITIONS:
            ours.add((p, PHOTON_FLUORO))
            theirs.add((p, era))
    return ours, theirs


class GusFidelityTests(unittest.TestCase):
    """The generated interior OBJ must reproduce Gus's three files exactly, with
    exactly THREE deliberate deviations (interior_plan.md §1.3, §1.3b, §1.3c)."""

    def test_each_branch_matches_gus_except_the_documented_normalisation(self):
        recs = interior_records()
        for value, name in ((0, "current"), (1, "new"), (2, "led")):
            with self.subTest(variant=name):
                mine = branch_records(recs, value)
                gus_text = (GUS / f"lights_inn_{name}.obj").read_text(
                    encoding="utf-8", errors="replace")
                theirs = Counter({tuple(r[1:]): n
                                  for r, n in B.parse_records(gus_text).items()})
                # Photon ADDS three lights Gus never had — the re-added .acf
                # cockpit spots (§3.4). Compare only what he authored.
                mine_gus_only = Counter({k: v for k, v in mine.items()
                                         if k[0] != "LIGHT_SPILL_CUSTOM"})
                for k in list(mine_gus_only):
                    if k not in theirs and _is_spot_replacement(k):
                        del mine_gus_only[k]
                only_mine = mine_gus_only - theirs
                only_theirs = theirs - mine_gus_only
                want_mine, want_theirs = expected_deviations(name)
                # Compared on (position, colour): a record may ALSO differ in
                # cone — the captain's table lamp does — and that asymmetry is
                # pinned separately by its own test.
                self.assertEqual({(rec[1], rec[2]) for rec in only_mine},
                                 want_mine)
                self.assertEqual({(rec[1], rec[2]) for rec in only_theirs},
                                 want_theirs)
                self.assertEqual(len(only_mine), len(want_mine), only_mine)
                self.assertEqual(len(only_theirs), len(want_theirs), only_theirs)

    def test_dome_spot_tracks_the_dome_lamps_in_every_branch(self):
        """The re-added .acf dome spot lights the SAME fitting as Gus's two dome
        lamps, so it has to sit on their colour ramp (P4), not the era-coloured
        one the other two spots use. Putting them on different ramps is invisible
        in the diff against Gus (he has no spots at all to compare to) and shows
        up in-sim as one overhead fixture lit two colours at once."""
        recs = interior_records()
        for value, name in ((0, "old halogen"), (1, "new halogen"), (2, "led")):
            with self.subTest(profile=name):
                branch = branch_records(recs, value)
                dome = {rec[2] for rec in branch if rec[1] in DOME_POSITIONS}
                spot = {rec[2] for rec in branch
                        if rec[1] == (0.0, 1.097, -3.432)}
                self.assertEqual(len(dome), 1, f"dome lamps disagree: {dome}")
                self.assertEqual(len(spot), 1, f"dome spot missing/split: {spot}")
                self.assertEqual(dome, spot)

    def test_pedestal_flood_stays_era_coloured(self):
        """Deviation #2 moves the DOME off Gus's era ramp and nothing else. The
        pedestal flood must keep swinging orange -> amber -> blue-white: the
        white-dome-over-warm-panels contrast is the point of that change, and
        flattening the flood too would quietly undo it."""
        recs = interior_records()
        seen = []
        for value in (0, 1, 2):
            flood = {rec[2] for rec in branch_records(recs, value)
                     if rec[1] == PEDESTAL_FLOOD_POSITION}
            self.assertEqual(len(flood), 1, flood)
            seen.append(flood.pop())
        self.assertEqual(seen, [GUS_OLD_ORANGE, GUS_NEW_AMBER, GUS_LED_COOL])

    def test_table_lamps_track_the_pedestal_flood_in_every_branch(self):
        """Deviation #3. The CA/FO table lamps share panel[3] with the pedestal
        flood, so they brighten and dim with it; leaving them a constant warm
        white made one rheostat drive two lights that disagreed about the era.
        They must now carry the flood's colour in all three profiles."""
        recs = interior_records()
        for value, name in ((0, "old halogen"), (1, "new halogen"), (2, "led")):
            with self.subTest(profile=name):
                branch = branch_records(recs, value)
                flood = {rec[2] for rec in branch
                         if rec[1] == PEDESTAL_FLOOD_POSITION}
                tables = {rec[2] for rec in branch if rec[1] in TABLE_POSITIONS}
                self.assertEqual(len(flood), 1, flood)
                self.assertEqual(tables, flood)

    def test_table_lamps_are_a_symmetric_pair(self):
        """Deviation #3 changes their COLOUR and nothing else: geometry stays
        Gus's, and the pair stays a mirror image apart from the x sign.

        A 2026-07-28 request to narrow "the captain's flood" was applied here
        first and was aimed at the wrong light — these wash the footwells, not
        the main panel. Reverted. This test is the marker: if a per-side cone or
        size override is ever wanted on this pair, it should be a deliberate
        edit that trips a red test, not a quiet divergence."""
        recs = interior_records()
        for value in (0, 1, 2):
            with self.subTest(profile=value):
                by_pos = {rec[1]: rec for rec in branch_records(recs, value)}
                ca = by_pos[(-0.558, 0.333, -4.5)]
                fo = by_pos[(0.558, 0.333, -4.5)]
                self.assertEqual(ca[6], 0.701)      # cone, Gus's value
                self.assertEqual(fo[6], 0.701)
                # everything but the position must be identical
                self.assertEqual(ca[:1] + ca[2:], fo[:1] + fo[2:])

    def test_light_count_per_branch(self):
        recs = interior_records()
        for value in (0, 1, 2):
            with self.subTest(value=value):
                # Gus's 23 + the 3 re-added .acf cockpit spots
                self.assertEqual(sum(branch_records(recs, value).values()), 26)

    def test_categories_carry_the_expected_lights(self):
        recs = interior_records()
        per_cat = Counter()
        for rec, n in recs.items():
            gates = _interior_gates(rec)
            if any(g[0] == "ANIM_hide" and g[1] <= 0 <= g[2] for g in gates):
                continue            # not the old-halogen branch
            for g in gates:
                per_cat[g[3].rsplit("/", 1)[-1]] += n
        # mainpnl is ONE light — the re-added .acf flood spot — and that is the
        # point of it having its own category: it runs off ckpt/lights/map (the
        # MAIN PNL knob), the other ten `map` lights off panel[1]/[2].
        self.assertEqual(dict(per_cat),
                         {"dome": 3, "map": 10, "mainpnl": 1,
                          "pedestal": 6, "console": 6})

    def test_mainpnl_is_the_spill_light_and_map_is_the_reading_lamps(self):
        """The split, from the OBJ's side. A category is one BRIGHTNESS SOURCE:
        `mainpnl` holds exactly the LIGHT_SPILL_CUSTOM flood (ckpt/lights/map,
        the MAIN PNL knob) and `map` holds exactly the movable reading lamps
        (panel[1]/[2]). Merging them back — the state before 2026-07-28 — puts
        two knobs on one colour switch, which is what made the flood change look
        whenever the reading lamps did."""
        recs = interior_records()
        for value in (0, 1, 2):
            with self.subTest(profile=value):
                # Counted WITH multiplicity: parse_records is a Counter, and map
                # lamp 3's first light is byte-identical to map lamp 2's, so the
                # ten lamp lines collapse to nine distinct records.
                mainpnl, mapped = Counter(), Counter()
                for rec, n in recs.items():
                    gates = _interior_gates(rec)
                    if any(g[0] == "ANIM_hide" and g[1] <= value <= g[2]
                           for g in gates):
                        continue
                    cats = {g[3].rsplit("/", 1)[-1] for g in gates}
                    if "mainpnl" in cats:
                        mainpnl[rec] += n
                    elif "map" in cats:
                        mapped[rec] += n
                self.assertEqual(sum(mainpnl.values()), 1, mainpnl)
                self.assertEqual(sum(mapped.values()), 10, mapped)
                # Slot 1 is the light CLASS; the spill light has no class, so
                # parse_records puts the literal LIGHT_SPILL_CUSTOM there.
                self.assertEqual({rec[1] for rec in mainpnl},
                                 {"LIGHT_SPILL_CUSTOM"})
                self.assertEqual({rec[1] for rec in mapped},
                                 {"airplane_panel_sp"})

    def test_map_lamp_transform_stacks_are_present(self):
        """The four movable map lamps ride ToLiss's own ckpt/lights/<n>/{x,y,z}
        animation, transcribed verbatim into mount snippets."""
        cfg = B.load_config(wing="stock")
        text = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        for n in (1, 2, 3, 4):
            for axis in ("x", "y", "z"):
                self.assertIn(f"ckpt/lights/{n}/{axis}/dir", text)


def _is_spot_replacement(rec):
    """The two LIGHT_PARAM cockpit-spot replacements (spots 1 and 2) Photon adds
    on top of Gus's lights, identified by their derived positions."""
    pos = rec[1]
    return pos in ((0.0, 1.097, -3.432), (0.0, 1.036, -3.52))


class InteriorHeaderTests(unittest.TestCase):
    """§3.6 — the interior header is NOT the exterior one, and its trailing tab
    is load-bearing (which is why emit() keeps the header out of reindent())."""

    def test_header_matches_stock_lights_inn(self):
        cfg = B.load_config(wing="stock")
        text = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        self.assertTrue(text.startswith(
            "I\r\n800\r\nOBJ\r\n\r\nTEXTURE\t\r\nPOINT_COUNTS\t0 0 1 0\r\n"))

    def test_texture_line_keeps_its_trailing_tab(self):
        cfg = B.load_config(wing="stock")
        text = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        self.assertIn("TEXTURE\t\r\n", text)
        self.assertNotIn("\r\nTEXTURE\r\n", text)

    def test_no_global_cockpit_lit(self):
        cfg = B.load_config(wing="stock")
        text = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        self.assertNotIn("GLOBAL_cockpit_lit", text)

    def test_gus_is_credited_in_the_generated_obj(self):
        cfg = B.load_config(wing="stock")
        text = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        self.assertIn("GUS", text)


class AirframeScopeTests(unittest.TestCase):
    """Decision #14 — the A330-900 is out of scope for interior lighting."""

    def test_a339_has_no_interior_target(self):
        self.assertNotIn("interior", B.OBJ_TARGETS["a339"])

    def test_a339_interior_build_is_an_error(self):
        cfg = B.load_config(wing="stock")
        with self.assertRaises(SystemExit):
            B.Emitter(cfg, "a339", "stock", target="interior").emit()

    def test_a339_absent_from_installer_interior_airframes(self):
        self.assertNotIn("a339", K.INTERIOR_AIRFRAMES)

    def test_a3xx_all_share_one_identical_interior_obj(self):
        """a319/a321 `extends a320`, so they inherit the interior fixtures
        verbatim. If that ever stops holding, one shared payload file (and one
        shared golden) silently becomes wrong for two airframes."""
        cfg = B.load_config(wing="stock")
        texts = {af: B.Emitter(cfg, af, "stock", target="interior").emit()
                 for af in ("a319", "a320", "a321")}
        self.assertEqual(texts["a319"], texts["a320"])
        self.assertEqual(texts["a321"], texts["a320"])


class SpillCustomTests(unittest.TestCase):
    """§3.4 step 4 — the map spot is a LIGHT_SPILL_CUSTOM driven by a dataref we
    own, because its brightness source is a ToLiss dataref, not a rheostat."""

    def test_map_spot_emits_a_spill_custom_pointing_at_our_dataref(self):
        cfg = B.load_config(wing="stock")
        text = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        lines = [l.strip() for l in text.splitlines()
                 if "LIGHT_SPILL_CUSTOM" in l]
        self.assertEqual(len(lines), 3, "one per interior branch")
        for line in lines:
            self.assertTrue(line.endswith("ToLissPhoton/interior/spill/map"))
            # 12 numbers + the dataref, after the directive
            self.assertEqual(len(line.split()), 14)

    def test_baked_alpha_is_conservative(self):
        """Plugin absent => dataref not found => X-Plane draws the baked params
        unmodified. That degraded state must be dim, never glaring."""
        cfg = B.load_config(wing="stock")
        text = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        for line in text.splitlines():
            if "LIGHT_SPILL_CUSTOM" in line:
                alpha = float(line.split()[7])
                self.assertLessEqual(alpha, 0.5)


class TrapTests(unittest.TestCase):
    """The three ⚠ traps of docs/interior_plan.md §3.2. Each fails SILENTLY if
    reintroduced, which is exactly why each gets a named test."""

    def test_trap1_airframes_fingerprint_stays_the_exterior_obj(self):
        """⚠ TRAP 1 — constants.AIRFRAMES' OBJ-filename slot feeds
        detect._identify_airframe / _obj_marker as an airframe FINGERPRINT.
        lights_inn.obj is byte-identical across A319/A320/A321, so putting it
        there would make every A3xx match every other one."""
        for af, (_glob, _folder, fname, _name) in K.AIRFRAMES.items():
            with self.subTest(airframe=af):
                self.assertNotEqual(fname, K.INTERIOR_OBJ)
                self.assertEqual(fname, B.OBJ_TARGETS[af]["exterior"])

    def test_trap2_interior_payload_is_written_once_outside_the_wing_loop(self):
        """⚠ TRAP 2 — three airframes writing the same lights_inn.obj would
        collide on one path under payload/objs/<wing>/. The interior is wing- and
        airframe-independent, so it is staged once at payload/objs/interior/."""
        import make_release as M
        out = Path(tempfile.mkdtemp(prefix="photon_test_payload_"))
        self.addCleanup(shutil.rmtree, out, ignore_errors=True)
        M.build_objs_payload(out)
        M.build_interior_payload(out)
        # exactly one interior OBJ in the whole payload, and not under a wing dir
        found = sorted(p.relative_to(out).as_posix()
                       for p in out.rglob(K.INTERIOR_OBJ))
        self.assertEqual(found, [f"objs/interior/{K.INTERIOR_OBJ}"])
        for wing in K.WINGS:
            self.assertFalse((out / "objs" / wing / K.INTERIOR_OBJ).exists())

    def test_trap3_interior_status_is_a_second_axis(self):
        """⚠ TRAP 3 — detect._obj_marker scans only the EXTERIOR OBJs, so an
        aircraft with the interior installed and the exterior reverted would
        report as 'not installed at all' and hide a live modification."""
        from installer import detect
        objects = Path(tempfile.mkdtemp(prefix="photon_test_status_"))
        self.addCleanup(shutil.rmtree, objects, ignore_errors=True)
        (objects / K.INTERIOR_OBJ).write_text(
            "I\n800\nOBJ\n\nTEXTURE\t\nPOINT_COUNTS\t0 0 1 0\n"
            "# ToLissPhoton version: 9.9 wing: interior\n", encoding="utf-8")
        # the exterior axis correctly reports nothing...
        self.assertEqual(detect.photon_status(objects), (None, None, False))
        # ...but the interior axis must NOT, or the install is invisible
        ver, installed, stale = detect.interior_status(objects)
        self.assertTrue(installed)
        self.assertEqual(ver, "9.9")
        self.assertFalse(stale)


class TextureSetTests(unittest.TestCase):
    def test_added_textures_are_a_subset_of_the_set(self):
        self.assertTrue(set(K.INTERIOR_TEXTURES_ADDED) <= set(K.INTERIOR_TEXTURES))

    def test_vendored_texture_set_is_complete(self):
        """Phase 0 vendoring is a hard prerequisite — the textures cannot be
        reproduced from any document, and shipping 9 of 11 would leave the
        cockpit half-retextured."""
        src = GUS / "textures"
        missing = [n for n in K.INTERIOR_TEXTURES if not (src / n).is_file()]
        self.assertEqual(missing, [])

    def test_added_textures_really_have_no_stock_counterpart(self):
        """pedals_details_{1,2}.png are the two files with no stock original.
        They must be DELETED on uninstall, not 'restored' from a backup that was
        never taken — hence the separate `added` list in the manifest."""
        self.assertEqual(set(K.INTERIOR_TEXTURES_ADDED),
                         {"pedals_details_1.png", "pedals_details_2.png"})


if __name__ == "__main__":
    unittest.main()
