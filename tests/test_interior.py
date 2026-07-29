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


def _category_of(gate):
    return gate[3].rsplit("/", 1)[-1]


#: Interior categories that are TWO-valued rather than ternary — 0 and 1 only.
#: The dome's ramp (P4 `fluoro`) is the same white in both halogen eras, so a
#: third branch would duplicate the first. The one place the value spaces differ.
TWO_VALUED = {"dome"}

#: How many looks each interior category offers.
INT_VALUES = {"dome": 2, "map": 3, "mainpnl": 3, "pedestal": 3, "console": 3}


def value_for(category, profile):
    """The dataref value `category` carries under interior `profile`
    (0 old halogen / 1 new halogen / 2 LED).

    Mirrors IntValueForProfile in plugin.cpp and must keep mirroring it: the
    two-valued dome folds BOTH halogen profiles onto 0 and LED onto 1, while the
    ternary categories take the profile at face value. At 2 the dome's gate
    encoding hides neither branch, so both would draw."""
    if category in TWO_VALUED:
        return 1 if profile == 2 else 0
    return profile


def colors_by_category(recs, profile):
    """category -> the set of colors its VISIBLE records carry under `profile`.

    Keyed off the gate, so a light is attributed to whatever category actually
    switches it rather than to whatever its name suggests."""
    out = {}
    for rec in recs:
        gates = _interior_gates(rec)
        if any(g[0] == "ANIM_hide"
               and g[1] <= value_for(_category_of(g), profile) <= g[2]
               for g in gates):
            continue
        for g in gates:
            out.setdefault(_category_of(g), set()).add(rec[3])
    return out


def branch_records(recs, profile):
    """Records VISIBLE under interior `profile`, with the gate stripped so they
    can be compared against Gus's ungated files.

    This models X-Plane's OWN visibility rule rather than pattern-matching a gate
    line: a block starts visible, and an ANIM_hide whose range covers the current
    value turns it off. Testing it this way is the point — the bug that shipped
    was a gate that pattern-matched perfectly and still left every branch
    visible. See Emitter.branch_gate.

    Takes a PROFILE, not a raw dataref value, because the categories no longer
    share one value space (see value_for)."""
    out = Counter()
    for rec, n in recs.items():
        gates = _interior_gates(rec)
        if any(g[0] == "ANIM_hide"
               and g[1] <= value_for(_category_of(g), profile) <= g[2]
               for g in gates):
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
        d = "ToLissPhoton/interior/pedestal"
        self.assertEqual(em.branch_gate("pedestal", 0), [f"ANIM_hide 0.5 2.5 {d}"])
        self.assertEqual(em.branch_gate("pedestal", 1),
                         [f"ANIM_hide -0.5 0.5 {d}", f"ANIM_hide 1.5 2.5 {d}"])
        self.assertEqual(em.branch_gate("pedestal", 2), [f"ANIM_hide -0.5 1.5 {d}"])

    def test_two_valued_dome_keeps_the_single_line_gate(self):
        """The dome is two-valued and takes the SAME encoding every exterior
        category uses — one line, hiding at the other branch's value. Not the
        paired open-ended form: with two branches there is nothing beyond to cover.

        Corollary: 2 is not a legal dome value. Neither `ANIM_hide 1 1` nor
        `ANIM_hide 0 0` covers it, so both branches would draw. value_for() is the
        test-side mirror; IntValueForProfile and ClampIntValue are the shipping."""
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="interior")
        d = "ToLissPhoton/interior/dome"
        self.assertEqual(len(em.profiles_of("dome")), 2)
        self.assertEqual(em.branch_gate("dome", 0), [f"ANIM_hide 1 1 {d}"])
        self.assertEqual(em.branch_gate("dome", 1), [f"ANIM_hide 0 0 {d}"])

    def test_every_category_row_actually_changes_something(self):
        """A category whose branches all render the SAME colors is a dead menu row:
        the gating works, the switch does nothing, and in-sim it is indistinguishable
        from the mod not being installed. That shipped twice — the DOME, whose third
        branch duplicated its first (fixed by making it two-valued), and the CONSOLE
        strips, constant warm white in all three of Gus's files (fixed by deviation
        #4, moving them to the era-colored P3).

        So each category must show exactly as many distinct looks as it has values —
        no more (an unreachable branch) and no fewer (a dead one)."""
        recs = interior_records()
        looks = {cat: set() for cat in INT_VALUES}
        for profile in range(3):
            for cat, colors in colors_by_category(recs, profile).items():
                looks[cat].add(frozenset(colors))
        for cat, want in INT_VALUES.items():
            with self.subTest(category=cat):
                self.assertEqual(len(looks[cat]), want,
                                 f"{cat} renders {len(looks[cat])} distinct "
                                 f"look(s) across {want} values: {looks[cat]}")

    def test_no_interior_branch_is_gated_with_anim_show(self):
        """The whole-file guard behind the unit above: one stray ANIM_show is a
        branch that never turns off, and it fails silently."""
        cfg = B.load_config(wing="stock")
        inn = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        self.assertNotIn("ANIM_show", inn)

    def test_every_interior_dataref_value_selects_exactly_one_branch(self):
        """Simulate X-Plane's visibility model over the emitted file: a block is
        visible unless some ANIM_hide range covers the current value. Exactly one
        branch per category must survive at every value in that category's OWN
        range — 0..1 for the two-valued dome, 0..2 for the rest."""
        cfg = B.load_config(wing="stock")
        inn = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        for cat, nvalues in INT_VALUES.items():
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
            # one block per branch per fixture, so this is the fixture count
            fixtures = len(branches) // nvalues
            for value in range(nvalues):
                visible = sum(1 for b in branches
                              if not any(lo <= value <= hi for lo, hi in b))
                self.assertEqual(visible, fixtures,
                                 f"{cat} at {value}: {visible} of {len(branches)} "
                                 f"blocks visible, expected {fixtures}")

    def test_exterior_obj_contains_no_interior_fixtures_and_vice_versa(self):
        cfg = B.load_config(wing="stock")
        ext = B.Emitter(cfg, "a320", "stock", target="exterior").emit()
        inn = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        self.assertNotIn("ToLissPhoton/interior/", ext)
        self.assertNotIn("ToLissPhoton/exterior/", inn)
        self.assertNotIn("airplane_panel_sp", ext)


# The four documented departures from Gus's data. Each is pinned to the exact
# records it touches rather than merely tolerated, so that a FIFTH one — or any
# of these silently spreading to more lights — fails the suite.
#
#   #1  the CA/FO tablet lights, LED variant only:  orange -> warm
#   #2  the dome lights, both halogen variants:     era color -> fluorescent
#       (the LED variant is untouched: fluoro and white are the same there)
#   #3  the CA/FO table lights, ALL THREE variants: constant warm -> era color
#   #4  the console strips, the two halogen variants: constant warm -> era
#       color (the LED variant is untouched: ramp P3 is warm there too)
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
CONSOLE_POSITIONS = {(-1.147, 0.033, -4.2), (-1.346, 0.003, -3.65),
                     (-1.437, -0.01, -3.3), (1.147, 0.033, -4.2),
                     (1.346, 0.003, -3.65), (1.437, -0.01, -3.3)}


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
        # fluoro and Gus's cool white are the same color.
        for p in DOME_POSITIONS:
            ours.add((p, PHOTON_FLUORO))
            theirs.add((p, era))
        # #4 — the console strips join the era ramp. Again nothing under LED:
        # ramp P3 lands on warm white there, which is what Gus had all along.
        for p in CONSOLE_POSITIONS:
            ours.add((p, era))
            theirs.add((p, GUS_WARM))
    return ours, theirs


# Panel flood 3's first line: Gus has INDEX 1, Photon corrects it to 2 (§1.3e).
# The deviation machinery above compares on (position, color) and this changes
# neither, so the pair is cancelled out here and pinned properly by
# test_panel_flood_3_sits_wholly_on_one_rheostat.
FLOOD3_DIR, FLOOD3_CONE = (0.0, -1.5, 0.4), 0.906


def _is_flood3_line(rec, index):
    """The single record the §1.3e rheostat correction moves, at `index`."""
    cls, pos, _rgb, idx, _size, d, cone = rec
    return (cls == "airplane_panel_sp" and pos == (0.0, 0.0, 0.0)
            and idx == index and d == FLOOD3_DIR and cone == FLOOD3_CONE)


def _drop_one(counter, pred):
    """Remove ONE matching element. Deliberately not `del`: flood 2 carries a
    byte-identical INDEX 1 line, so dropping every match would also hide a real
    regression on that lamp."""
    for key in list(counter):
        if pred(key):
            counter[key] -= 1
            if counter[key] <= 0:
                del counter[key]
            return counter
    return counter


class GusFidelityTests(unittest.TestCase):
    """The generated interior OBJ must reproduce Gus's three files, deviating only
    where interior_plan.md records a decision: the four color moves (§1.3, §1.3b,
    §1.3c, §1.3d) and the flood-3 rheostat fix (§1.3e).

    Per §0.1 this suite makes a deviation deliberate and visible in the diff; it is
    not a rule that Gus's data may never change."""

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
                # §1.3e — cancel the one record the rheostat fix moves.
                only_mine = _drop_one(only_mine, lambda r: _is_flood3_line(r, 2))
                only_theirs = _drop_one(only_theirs, lambda r: _is_flood3_line(r, 1))
                want_mine, want_theirs = expected_deviations(name)
                # Compared on (position, color): a record may ALSO differ in
                # cone — the captain's table lamp does — and that asymmetry is
                # pinned separately by its own test.
                self.assertEqual({(rec[1], rec[2]) for rec in only_mine},
                                 want_mine)
                self.assertEqual({(rec[1], rec[2]) for rec in only_theirs},
                                 want_theirs)
                self.assertEqual(len(only_mine), len(want_mine), only_mine)
                self.assertEqual(len(only_theirs), len(want_theirs), only_theirs)

    def test_panel_flood_3_sits_wholly_on_one_rheostat(self):
        """§1.3e. Gus's panel flood 3 carries INDEX 1 on the first of its three
        lines and INDEX 2 on the other two, so a third of that lamp dims on the
        wrong knob. Stock ToLiss has it wholly on panel[2], and his flood 2 is
        byte-identical apart from the index — a duplicated block whose
        find-and-replace missed the first line.

        Asserted BOTH ways: that his file really carries the bug (so this fails if
        the vendored source is ever fixed upstream) and that ours is corrected."""
        def indices(counter, pos_at, idx_at, dir_at):
            """Flatten a record multiset to one index per emitted LINE. Counted WITH
            multiplicity: floods 2 and 3 share a byte-identical INDEX 1 line, so they
            collapse to one key and the bug is invisible unless counts are honoured."""
            out = []
            for rec, n in counter.items():
                if rec[pos_at] == (0.0, 0.0, 0.0) and rec[dir_at] == FLOOD3_DIR:
                    out.extend([int(rec[idx_at])] * n)
            return sorted(out)

        gus_text = (GUS / "lights_inn_current.obj").read_text(
            encoding="utf-8", errors="replace")
        # Floods 2 and 3 are six lines, of which Gus puts FOUR on panel[1].
        self.assertEqual(indices(B.parse_records(gus_text), 2, 4, 6),
                         [1, 1, 1, 1, 2, 2],
                         "vendored source no longer shows the flood-3 bug")

        recs = interior_records()
        for value, name in ((0, "current"), (1, "new"), (2, "led")):
            with self.subTest(variant=name):
                # ...and Photon puts three on each, a clean 2+2 lamp split.
                self.assertEqual(indices(branch_records(recs, value), 1, 3, 5),
                                 [1, 1, 1, 2, 2, 2])

    def test_dome_spot_tracks_the_dome_lamps_in_every_branch(self):
        """The re-added .acf dome spot lights the SAME fitting as Gus's two dome
        lamps, so it has to sit on their color ramp (P4), not the era-colored
        one the other two spots use. Putting them on different ramps is invisible
        in the diff against Gus (he has no spots at all to compare to) and shows
        up in-sim as one overhead fixture lit two colors at once."""
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

    def test_pedestal_flood_stays_era_colored(self):
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
        They must now carry the flood's color in all three profiles."""
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
        """Deviation #3 changes their COLOR and nothing else: geometry stays
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
            if any(g[0] == "ANIM_hide"
                   and g[1] <= value_for(_category_of(g), 0) <= g[2]
                   for g in gates):
                continue            # not the old-halogen branch
            for g in gates:
                per_cat[_category_of(g)] += n
        # `map` is THREE lights: the two overhead reading lamps (Gus's "tablet"
        # pair) plus the re-added .acf spot on ckpt/lights/map. The ten panel
        # lamps are `mainpnl` — they run off panel[1]/[2], the FLOOD LT MAIN PNL
        # knob — and the pedestal keeps its flood and two table lamps.
        self.assertEqual(dict(per_cat),
                         {"dome": 3, "map": 3, "mainpnl": 10,
                          "pedestal": 4, "console": 6})

    def test_map_holds_the_reading_lamps_and_mainpnl_the_panel_lamps(self):
        """The split, from the OBJ's side, and which way round it goes.

        `mainpnl` holds exactly the ten lamp lines (panel[1]/[2], the FLOOD LT MAIN
        PNL knob). `map` holds the two overhead reading lamps and the
        ckpt/lights/map spot — three lights, and the ONE category grouped by fixture
        role rather than rheostat, the reading lamps running off panel[3] with the
        pedestal. See the int_map_lamps fixture.

        Every arrangement of these has been wrong in-sim at least once: merging map
        into mainpnl put two knobs on one color switch; the reverse assignment
        labelled the panel flood "Map Lights"; and `map` holding only the spot made
        the row look dead, that spot being invisible with the map knob down.
        Identify a light by its rheostat and position, never by its name."""
        recs = interior_records()
        for profile in (0, 1, 2):
            with self.subTest(profile=profile):
                # Counted WITH multiplicity: parse_records is a Counter, and
                # lamp 3's first light is byte-identical to lamp 2's, so the ten
                # lamp lines collapse to nine distinct records.
                mainpnl, mapped = Counter(), Counter()
                for rec, n in recs.items():
                    gates = _interior_gates(rec)
                    if any(g[0] == "ANIM_hide"
                           and g[1] <= value_for(_category_of(g), profile) <= g[2]
                           for g in gates):
                        continue
                    cats = {_category_of(g) for g in gates}
                    if "mainpnl" in cats:
                        mainpnl[rec] += n
                    elif "map" in cats:
                        mapped[rec] += n
                self.assertEqual(sum(mapped.values()), 3, mapped)
                self.assertEqual(sum(mainpnl.values()), 10, mainpnl)
                # Slot 1 is the light CLASS; the spill light has no class, so
                # parse_records puts the literal LIGHT_SPILL_CUSTOM there. The
                # map category spans both kinds — one spot, two panel lamps.
                self.assertEqual({rec[1] for rec in mapped},
                                 {"LIGHT_SPILL_CUSTOM", "airplane_panel_sp"})
                self.assertEqual({rec[2] for rec in mapped},
                                 TABLET_POSITIONS | {(0.244, 1.097, -3.463)})
                self.assertEqual({rec[1] for rec in mainpnl},
                                 {"airplane_panel_sp"})

    def test_the_reading_lamps_left_the_pedestal_category(self):
        """Deviation from Gus's grouping, and why the Map Lights row now does
        something visible. The pedestal keeps its flood and the two table lamps
        sharing panel[3] with it; the reading lamps are gated by `map`.

        Drifting back is a subtle symptom: the Pedestal row starts recoloring the
        lights over the pilots' tables and the Map row controls one invisible spot."""
        recs = interior_records()
        for rec in recs:
            cats = {_category_of(g) for g in _interior_gates(rec)}
            if rec[2] in TABLET_POSITIONS:
                self.assertEqual(cats, {"map"}, rec)
            if rec[2] in TABLE_POSITIONS or rec[2] == PEDESTAL_FLOOD_POSITION:
                self.assertEqual(cats, {"pedestal"}, rec)

    def test_panel_flood_transform_stacks_are_present(self):
        """The four movable panel flood lamps ride ToLiss's own
        ckpt/lights/<n>/{x,y,z} animation, transcribed verbatim into mount
        snippets."""
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


class CockpitMenuGateTests(unittest.TestCase):
    """The plugin hides its Cockpit submenu unless the aircraft's lights_inn.obj
    binds our interior datarefs (plugin.cpp kInteriorObjNeedle / DetectInterior).
    A contract ACROSS the build and the plugin — the DSL decides what the OBJ says,
    C++ what it looks for — and drift shows up only in-sim, as a missing menu."""

    PLUGIN = REPO / "src" / "native" / "src" / "plugin.cpp"

    def _cpp_string(self, name):
        import re
        text = self.PLUGIN.read_text(encoding="utf-8", errors="replace")
        m = re.search(rf'\b{name}\s*=\s*"((?:[^"\\]|\\.)*)"', text)
        self.assertIsNotNone(m, f"{name} not found in plugin.cpp")
        return m.group(1)

    def test_detector_looks_for_the_installed_filename(self):
        self.assertEqual(self._cpp_string("kInteriorObjName"), K.INTERIOR_OBJ)

    def test_generated_interior_obj_trips_the_detector(self):
        """A false negative here is a modded cockpit with no menu to drive it."""
        needle = self._cpp_string("kInteriorObjNeedle")
        cfg = B.load_config(wing="stock")
        for af in K.INTERIOR_AIRFRAMES:
            with self.subTest(airframe=af):
                text = B.Emitter(cfg, af, "stock", target="interior").emit()
                self.assertIn(needle, text)

    def test_a_debug_build_trips_it_too(self):
        """`build --target interior --debug --write` carries no installer version
        marker, which is why the sentinel is the dataref binding and not the
        marker — a dev build is as switchable as a released one."""
        needle = self._cpp_string("kInteriorObjNeedle")
        cfg = B.load_config(wing="stock")
        text = B.Emitter(cfg, "a320", "stock", target="interior", debug=True).emit()
        self.assertIn(needle, text)

    def test_gus_original_objs_do_not_trip_the_detector(self):
        """Gus's own three files are the nearest thing here to an unmodded cockpit
        OBJ. They carry no gates, so a hand-installed copy must read as 'not
        installed' rather than as a menu whose every row does nothing."""
        needle = self._cpp_string("kInteriorObjNeedle")
        for p in sorted(GUS.glob("lights_inn_*.obj")):
            with self.subTest(obj=p.name):
                self.assertNotIn(needle, p.read_text(encoding="utf-8",
                                                     errors="replace"))


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


class DebugLightBuildTests(unittest.TestCase):
    """`build --target interior --debug` — the dev light-tuner OBJ. Never shipped,
    so the bar is "does the dev plugin get what it needs", not fidelity."""

    def _debug(self, airframe="a320", pos=False):
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, airframe, "stock", target="interior", debug=True,
                       debug_pos=pos)
        return em, em.emit()

    def test_every_light_becomes_a_tunable_spill_light(self):
        em, text = self._debug()
        # one manifest row per emitted light, numbered from 0 with no gaps
        self.assertEqual([d["n"] for d in em.debug_lights],
                         list(range(len(em.debug_lights))))
        # Keyed off the trailing dataref, not the directive: int_spot3 is ALREADY a
        # LIGHT_SPILL_CUSTOM, so its preserved original is one too and a directive
        # match would count 27 where there are 26 tunable copies.
        emitted = [line.split()[-1] for line in text.splitlines()
                   if B.DEBUG_LIGHT_DREF in line and not line.lstrip().startswith("#")]
        self.assertEqual(emitted,
                         [f"{B.DEBUG_LIGHT_DREF}/{d['n']}" for d in em.debug_lights])

    def test_the_original_line_is_kept_beside_each_debug_copy(self):
        """The A/B pair. Converting a light to LIGHT_SPILL_CUSTOM is not obviously a
        no-op — `airplane_panel_sp` is a NAMED light resolved through X-Plane's own
        SPILL_SW handler — so the debug OBJ keeps the untouched original beside the
        tunable copy, gated on DEBUG_COMPARE_DREF (0 = originals, 1 = copies). It is
        the only way to tell a bad conversion from bad authored values."""
        em, text = self._debug()
        lines = [l.strip() for l in text.splitlines() if not l.strip().startswith("#")]
        copies = [l for l in lines if B.DEBUG_LIGHT_DREF in l]
        originals = [l for l in lines
                     if l.startswith(("LIGHT_PARAM", "LIGHT_SPILL_CUSTOM"))
                     and B.DEBUG_LIGHT_DREF not in l]
        self.assertEqual(len(copies), len(em.debug_lights))
        self.assertEqual(len(originals), len(em.debug_lights))
        # int_spot3 is already a spill light, so exactly one original is not a
        # LIGHT_PARAM — the rest are the named-light classes being converted.
        self.assertEqual(sum(1 for l in originals
                             if l.startswith("LIGHT_SPILL_CUSTOM")), 1)
        shows_original = [l for l in lines
                          if l == f"ANIM_hide 0.5 1.5 {B.DEBUG_COMPARE_DREF}"]
        shows_debug = [l for l in lines
                       if l == f"ANIM_hide -0.5 0.5 {B.DEBUG_COMPARE_DREF}"]
        self.assertEqual(len(shows_original), len(em.debug_lights))
        self.assertEqual(len(shows_debug), len(em.debug_lights))

    def test_the_ab_pair_is_identical_apart_from_the_directive(self):
        """The comparison only means anything if both sides carry the SAME numbers.
        Position, color, size, aim and cone must match; only the alpha/index slot
        differs, that slot meaning different things to the two directives."""
        em, text = self._debug()
        # Split by gate: the debug copy is the branch hidden when compare reads 0.
        shown_at = {0: [], 1: []}
        for rec, n in B.parse_records(text).items():
            gates = [g for g in rec[0] if g[3] == B.DEBUG_COMPARE_DREF]
            for value in (0, 1):
                if any(g[0] == "ANIM_hide" and g[1] <= value <= g[2] for g in gates):
                    continue
                # WITH multiplicity: parse_records is a Counter and two of the panel
                # lamp lines are byte-identical, so counting distinct records loses one.
                # (pos, rgb, dir, cone) — slot 5 is index/alpha, which legitimately
                # differs: it is a rheostat INDEX on one side and an ALPHA on the other
                shown_at[value].extend([(rec[2], rec[3], rec[6], rec[7])] * n)
        self.assertEqual(len(shown_at[0]), len(em.debug_lights))
        self.assertEqual(len(shown_at[1]), len(em.debug_lights))
        self.assertEqual(sorted(shown_at[0]), sorted(shown_at[1]))

    def test_debug_build_has_no_category_gating(self):
        """One branch, no gate: the dev plugin owns color while a debug OBJ is
        installed, so a gate could only hide the light being looked at. The banner
        says so — one left installed by accident looks like a broken Cockpit menu."""
        em, text = self._debug()
        gates = [line for line in text.splitlines()
                 if "ANIM_hide" in line and "ToLissPhoton/interior/" in line]
        self.assertEqual(gates, [])
        self.assertIn("NOT INSTALLABLE", text)

    def test_manifest_carries_the_brightness_source_of_each_light(self):
        """LIGHT_SPILL_CUSTOM has no INDEX slot, so a debug light cannot follow a
        rheostat on its own — the plugin multiplies it in from these fields. Wrong,
        and every light sits at full brightness, which is the one state that makes
        telling lights apart impossible."""
        em, _ = self._debug()
        by_name = {d["name"]: d for d in em.debug_lights}
        self.assertEqual((by_name["int_dome#left"]["source"],
                          by_name["int_dome#left"]["index"]), ("panel", 0))
        self.assertEqual((by_name["int_flood"]["source"],
                          by_name["int_flood"]["index"]), ("panel", 3))
        self.assertEqual((by_name["int_console#l1"]["source"],
                          by_name["int_console#l1"]["index"]), ("inst", 22))
        self.assertEqual(by_name["int_spot3"]["source"], "map")

    def test_a_spill_light_with_an_index_names_its_rheostat_not_the_map_knob(self):
        """A LIGHT_SPILL_CUSTOM has no INDEX slot, so the manifest is the only thing
        that can say which knob drives it. `int_spot3` has no index and is genuinely
        on ckpt/lights/map, but the source used to be hard-coded to "map" for EVERY
        spill light — so a rheostat-driven one would have been multiplied by the map
        knob and gone dark whenever that knob was down.

        Tested on DECLARATION rather than value: `index` defaults to 0, itself a real
        rheostat (panel[0], the dome)."""
        cfg = B.load_config(wing="stock")
        self.assertNotIn("index", cfg["lightTypes"]["int_spot3"],
                         "int_spot3 must stay index-less")

        em = B.Emitter(cfg, "a320", "stock", target="interior", debug=True)
        em.emit()
        rows = {d["name"]: d for d in em.debug_lights}
        self.assertEqual((rows["int_spot3"]["source"],
                          rows["int_spot3"]["index"]), ("map", 0))

        # ...and the same light gains a rheostat the moment it declares one.
        cfg["lightTypes"]["int_spot3"]["index"] = 2
        em2 = B.Emitter(cfg, "a320", "stock", target="interior", debug=True)
        em2.emit()
        row = {d["name"]: d for d in em2.debug_lights}["int_spot3"]
        self.assertEqual((row["source"], row["index"]), ("panel", 2))

    def test_movable_panel_lamps_keep_their_mount(self):
        """The four flood lamps ride ToLiss's ckpt/lights/<n> transform stack, so
        their OBJ position is 0 0 0 and means nothing alone. The manifest names the
        mount, without which 'position 0 0 0' in the tuner is a mystery."""
        em, text = self._debug()
        mounts = {d["mount"] for d in em.debug_lights if d["category"] == "mainpnl"}
        self.assertEqual(mounts, {"panelflood1", "panelflood2",
                                  "panelflood3", "panelflood4"})
        self.assertIn("ckpt/lights/1/x/dir", text)
        # ...and every other light says so by carrying no mount at all
        self.assertEqual({d["mount"] for d in em.debug_lights
                          if d["category"] != "mainpnl"}, {None})

    def test_every_light_is_wrapped_in_three_dataref_driven_translations(self):
        """Position is live too: LIGHT_SPILL_CUSTOM keeps x/y/z baked and passes only
        nine params through its dataref, but ANIM_trans takes a dataref of its own.

        The keyframes must span ∓DEBUG_POS_RANGE keyed on the SAME values, so the
        dataref reads directly as metres; any other keying rescales every edit.
        Opt-in via --debug-pos, so this asks for it explicitly."""
        em, text = self._debug(pos=True)
        lines = [l.strip() for l in text.splitlines() if l.strip().startswith("ANIM_trans")]
        moves = [l for l in lines if B.DEBUG_POS_DREF in l]
        self.assertEqual(len(moves), 3 * len(em.debug_lights))
        r = B.fmt(B.DEBUG_POS_RANGE)
        want0 = [f"ANIM_trans\t-{r} 0 0\t{r} 0 0\t-{r} {r}\t{B.DEBUG_POS_DREF}[0]",
                 f"ANIM_trans\t0 -{r} 0\t0 {r} 0\t-{r} {r}\t{B.DEBUG_POS_DREF}[1]",
                 f"ANIM_trans\t0 0 -{r}\t0 0 {r}\t-{r} {r}\t{B.DEBUG_POS_DREF}[2]"]
        self.assertEqual(moves[:3], want0)
        # light n owns exactly elements [3n, 3n+1, 3n+2] — the mapping the plugin's
        # DbgReadPos mirrors, and an off-by-one nudges the neighbouring light
        indices = [int(l.rsplit("[", 1)[1].rstrip("]")) for l in moves]
        self.assertEqual(indices, list(range(3 * len(em.debug_lights))))

    def test_position_wrappers_nest_inside_any_mount(self):
        """A mounted lamp's offset must apply INSIDE ToLiss's ckpt/lights/<n> stack, so
        it nudges the light relative to where the animation puts it. Emitting it outside
        would offset in aircraft space and fight the mount."""
        em, text = self._debug(pos=True)
        lines = [l.strip() for l in text.splitlines()]
        mount_at = lines.index(next(l for l in lines if "ckpt/lights/1/z/dir" in l))
        move_after = next(i for i, l in enumerate(lines)
                          if i > mount_at and B.DEBUG_POS_DREF in l)
        light_after = next(i for i, l in enumerate(lines)
                           if i > mount_at and l.startswith("LIGHT_SPILL_CUSTOM"))
        self.assertLess(mount_at, move_after)
        self.assertLess(move_after, light_after)

    def test_position_wrappers_are_off_unless_asked_for(self):
        """--debug-pos is OPT-IN: the ANIM_trans wrappers are the one part of a debug
        build that changes how lights RENDER rather than where their params come from,
        and in-sim they mis-place them — which made the tool unusable for the
        color/aim/cone work that does not need them."""
        em, text = self._debug()
        self.assertFalse(em.debug_pos)
        self.assertNotIn(B.DEBUG_POS_DREF, text)
        # ...and the tunable copies are still all there. Dropping the wrappers must not
        # drop the lights they wrapped.
        copies = [l for l in text.splitlines()
                  if B.DEBUG_LIGHT_DREF in l and not l.lstrip().startswith("#")]
        self.assertEqual(len(copies), len(em.debug_lights))

    def test_anim_blocks_stay_balanced_with_and_without_the_wrappers(self):
        """Each wrapper is an ANIM_begin/ANIM_end pair, so the trailing ANIM_end count
        has to track the flag. One stray ANIM_end unbalances the rest of the file:
        X-Plane rejects the OBJ, or worse, silently mis-nests what follows."""
        for pos in (False, True):
            _, text = self._debug(pos=pos)
            lines = [l.strip() for l in text.splitlines()]
            self.assertEqual(lines.count("ANIM_begin"), lines.count("ANIM_end"),
                             "unbalanced with debug_pos=%s" % pos)

    def test_debug_pos_without_debug_is_not_a_half_build(self):
        """`debug_pos` alone would describe an OBJ with position wrappers around
        ordinary light lines that no dataref drives — a build that looks like it works
        and cannot. The CLI implies --debug; the Emitter refuses to pretend."""
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="interior", debug=False,
                       debug_pos=True)
        self.assertFalse(em.debug_pos)
        self.assertNotIn(B.DEBUG_POS_DREF, em.emit())

    def test_debug_is_interior_only(self):
        """The exterior has frozen goldens; a debug build could never match them,
        so asking for one is an error rather than a silently unusable OBJ."""
        cfg = B.load_config(wing="stock")
        with self.assertRaises(SystemExit):
            B.Emitter(cfg, "a320", "stock", target="exterior", debug=True)

    def test_light_pool_is_big_enough_for_the_manifest(self):
        """DEBUG_MAX_LIGHTS here and DebugMaxLights in PI_PhotonDevReload.py size the
        same dataref pool and must not drift: a light past the end of it binds to a
        dataref that does not exist and draws black forever."""
        em, _ = self._debug()
        self.assertLessEqual(len(em.debug_lights), B.DEBUG_MAX_LIGHTS)
        dev = (REPO / "src" / "plugin" / "PI_PhotonDevReload.py").read_text(
            encoding="utf-8", errors="replace")
        self.assertIn(f"DebugMaxLights  = {B.DEBUG_MAX_LIGHTS}", dev)
        self.assertIn(f'DebugDataRefPrefix   = "{B.DEBUG_LIGHT_DREF}/"', dev)


if __name__ == "__main__":
    unittest.main()
