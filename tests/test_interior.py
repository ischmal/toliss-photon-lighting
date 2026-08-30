"""Interior ("Gus Mod") support — build side and the three ⚠ traps.

The traps are the three places docs/interior_plan.md §3.2 flags as easy to get
wrong in a way that FAILS SILENTLY rather than loudly. Each has a named test
here so a future refactor trips a red test instead of shipping a subtle bug.
"""
from __future__ import annotations

import math
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
import constants as K  # noqa: E402  (build/constants.py — pinned to core/constants.cpp)

GUS = REPO / "reference" / "gus"


def interior_records(airframe="a320"):
    cfg = B.load_config(wing="stock")
    return B.parse_records(B.Emitter(cfg, airframe, "stock", target="interior").emit())


#: The five category datarefs, and the ONE other ToLissPhoton/interior/ gate the
#: OBJ carries. `optimized` is not a category — it selects between Gus's authored
#: light stack (0) and the reduced one-light-per-position set (1) — so the helpers
#: below have to tell the two apart rather than treating every interior gate as a
#: category. They did not, once, and the symptom was a KeyError in a color test.
INT_CATEGORY_KEYS = ("dome", "map", "mainpnl", "pedestal", "console")
OPTIMIZE_DREF = "ToLissPhoton/interior/optimized"


def _interior_gates(rec):
    """The CATEGORY gates on a record — never the optimize gate."""
    return [g for g in rec[0]
            if g[3].startswith("ToLissPhoton/interior/")
            and g[3].rsplit("/", 1)[-1] in INT_CATEGORY_KEYS]


def _category_of(gate):
    return gate[3].rsplit("/", 1)[-1]


def _code(text):
    """The OBJ with its comments dropped. The credit banner NAMES the gate
    datarefs and explains them, so a test that greps the whole file counts its
    own documentation as an occurrence."""
    return "\n".join(ln for ln in text.replace("\r\n", "\n").split("\n")
                     if not ln.strip().startswith("#"))


def _hidden(rec, profile, optimized=0):
    """X-Plane's own visibility rule over one record, for a given interior
    profile and light-count mode.

    A block starts VISIBLE and an ANIM_hide whose range covers the dataref's
    current value turns it off (see Emitter.branch_gate). Modelling it rather
    than pattern-matching gate lines is the point: the bug that shipped was a
    gate that matched perfectly and still left every branch visible.

    `optimized` defaults to 0 — Gus's full stack — so every fidelity test below
    keeps comparing against what he authored without saying so."""
    for g in rec[0]:
        if g[3] == OPTIMIZE_DREF:
            value = optimized
        elif g in _interior_gates(rec):
            value = value_for(_category_of(g), profile)
        else:
            continue                    # someone else's gate (anim/SHARK, ...)
        if g[0] == "ANIM_hide" and g[1] <= value <= g[2]:
            return True
    return False


#: Interior categories that are TWO-valued rather than ternary — 0 and 1 only.
#: EMPTY since 2026-08-03: the dome was the one member, on the reading that its
#: fitting is fluorescent and has no halogen era, and it turned out to be halogen
#: like the rest. The machinery stays on both sides (here, and IntValueForProfile
#: / the one-line gate in the plugin) because a category may need it again — this
#: set is what wires it up, and the exterior exercises the same gate encoding.
TWO_VALUED = set()

#: How many looks each interior category offers.
INT_VALUES = {"dome": 3, "map": 3, "mainpnl": 3, "pedestal": 3, "console": 3}


def value_for(category, profile):
    """The dataref value `category` carries under interior `profile`
    (0 old halogen / 1 new halogen / 2 LED).

    Mirrors IntValueForProfile in plugin.cpp and must keep mirroring it: a
    two-valued category folds BOTH halogen profiles onto 0 and LED onto 1, while
    a ternary one takes the profile at face value. At 2 a two-valued gate hides
    neither branch, so both would draw — which is why the fold is here and not a
    clamp."""
    if category in TWO_VALUED:
        return 1 if profile == 2 else 0
    return profile


def colors_by_category(recs, profile, optimized=0):
    """category -> the set of colors its VISIBLE records carry under `profile`.

    Keyed off the gate, so a light is attributed to whatever category actually
    switches it rather than to whatever its name suggests."""
    out = {}
    for rec in recs:
        if _hidden(rec, profile, optimized):
            continue
        for g in _interior_gates(rec):
            out.setdefault(_category_of(g), set()).add(rec[3])
    return out


def branch_records(recs, profile, optimized=0):
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
        if _hidden(rec, profile, optimized):
            continue
        out[tuple(rec[1:])] += n
    return out


class ExteriorIsUnchangedTests(unittest.TestCase):
    """The N-way emitter must leave exterior emission BYTE-IDENTICAL. Every
    exterior category stays two-valued and so keeps the original ANIM_hide
    encoding; switching it to ANIM_show would rewrite every gate line in all
    three frozen reference/photon goldens for no behavioral gain."""

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

    def test_the_dome_is_ternary_like_every_other_interior_category(self):
        """It was two-valued (fluorescent / LED) from 2026-07-28 to 2026-08-03,
        on the reading that a dome light is a fluorescent fitting with no
        incandescent era to follow. The A320's dome lamps are halogen, so it is
        back on the era ramp and the Dome Lights row offers the same three looks
        as the other four.

        Pinned because the value space is what `interior_schema` versions and what
        a prefs file written in either shape carries: a v2 `dome: 1` meant LED and
        a v3 `dome: 1` means new halogen. If this test ever goes green with two
        profiles again, kIntPrefsSchema has to move with it."""
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="interior")
        d = "ToLissPhoton/interior/dome"
        self.assertEqual(len(em.profiles_of("dome")), 3)
        self.assertEqual(em.branch_gate("dome", 0), [f"ANIM_hide 0.5 2.5 {d}"])
        self.assertEqual(em.branch_gate("dome", 1),
                         [f"ANIM_hide -0.5 0.5 {d}", f"ANIM_hide 1.5 2.5 {d}"])
        self.assertEqual(em.branch_gate("dome", 2), [f"ANIM_hide -0.5 1.5 {d}"])

    def test_a_two_valued_category_would_still_get_the_single_line_gate(self):
        """The encoding TWO_VALUED selects, kept alive on the exterior — one line
        per branch, hiding at the other branch's value, not the paired open-ended
        form (with two branches there is nothing beyond to cover).

        Corollary, and the reason this stays named after the dome's old shape: 2
        is not a legal value on such a category. Neither `ANIM_hide 1 1` nor
        `ANIM_hide 0 0` covers it, so both branches would draw. value_for() is the
        test-side mirror; IntValueForProfile and ClampIntValue are the shipping."""
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="exterior")
        d = "ToLissPhoton/exterior/beacon"
        self.assertEqual(len(em.profiles_of("beacon")), 2)
        self.assertEqual(em.branch_gate("beacon", 0), [f"ANIM_hide 1 1 {d}"])
        self.assertEqual(em.branch_gate("beacon", 1), [f"ANIM_hide 0 0 {d}"])

    def test_every_category_row_actually_changes_something(self):
        """A category whose branches all render the SAME colors is a dead menu row:
        the gating works, the switch does nothing, and in-sim it is indistinguishable
        from the mod not being installed. That shipped twice — the DOME, whose third
        branch duplicated its first while it sat on the fluorescent ramp (fixed then
        by making it two-valued, and since 2026-08-03 by putting it back on the era
        ramp, where all three branches differ), and the CONSOLE strips, constant warm
        white in all three of Gus's files (fixed by deviation #4, moving them to the
        era-colored P3).

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
        range — 0..2 for a ternary category, 0..1 for a two-valued one (INT_VALUES
        is the register of which is which)."""
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


# The documented departures from Gus's data. Each is pinned to the exact records
# it touches rather than merely tolerated, so that a NEW one — or any of these
# silently spreading to more lights — fails the suite.
#
#   #1  the CA/FO tablet lights, LED variant only:  orange -> warm
#   #2  WITHDRAWN 2026-08-03. It moved the dome lights off the era ramp onto a
#       fluorescent white in both halogen variants, on the reading that the
#       fitting is fluorescent. It is halogen, so the dome is back on Gus's ramp
#       and there is nothing left to pin. The NUMBERING IS NOT REUSED: #3 and #4
#       are named by number in the DSL, in interior_plan.md and in the tests
#       below, and renumbering them to close a gap would silently re-point every
#       one of those references.
#   #3  the CA/FO table lights, ALL THREE variants: constant warm -> era color
#   #4  the console strips, the two halogen variants: constant warm -> era
#       color (the LED variant is untouched: ramp P3 is warm there too)
GUS_OLD_ORANGE = (1.0, 0.37, 0.16)
GUS_NEW_AMBER = (0.8, 0.55, 0.3)
GUS_LED_COOL = (0.82, 0.82, 1.0)
GUS_WARM = (1.0, 0.89, 0.81)
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
        # #4 — the console strips join the era ramp. Nothing under LED:
        # ramp P3 lands on warm white there, which is what Gus had all along.
        for p in CONSOLE_POSITIONS:
            ours.add((p, era))
            theirs.add((p, GUS_WARM))
    return ours, theirs


# Panel flood 3's first line: Gus has INDEX 1, Photon corrects it to 2 (§1.3e).
# The deviation machinery above compares on (position, color) and this changes
# neither, so the pair is canceled out here and pinned properly by
# test_panel_flood_3_sits_wholly_on_one_rheostat.
# Written in UNIT form because every comparison against Gus goes through
# `_canon` first — his is 0 -1.5 0.4 and ours 0 -0.966 0.258, the same aim.
FLOOD3_DIR, FLOOD3_CONE = (0.0, -0.966, 0.258), 0.906


def _unit(d):
    """A direction reduced to aim alone, at the project's 3 dp.

    DEVIATION #5 (2026-07-29) normalized every interior `dir:` — X-Plane compares
    `cone` against a DOT PRODUCT with it, so a vector 0.18 long (Gus's outer panel
    floods) makes the effective cone threshold cone/|d|, which above 1 no direction
    can satisfy. That is length, never aim: `0 -1.5 0.4` and `0 -0.966 0.258` point
    the same way. So the color-fidelity comparison canonicalizes dir out, and the
    normalization itself is pinned by its own test — otherwise a change of aim, the
    thing that would actually be a regression, would hide inside the same diff."""
    length = math.sqrt(sum(v * v for v in d))
    if length < 1e-9:
        return (0.0, 0.0, 0.0)                 # omnidirectional
    return tuple(round(v / length, 3) for v in d)


def _canon(rec):
    """One record with its dir reduced to aim. Records are
    (cls, pos, rgb, index, size, dir, cone)."""
    return rec[:5] + (_unit(rec[5]),) + rec[6:]


def _canon_all(counter):
    out = Counter()
    for rec, n in counter.items():
        out[_canon(rec)] += n
    return out


def _is_flood3_line(rec, index):
    """The single record the §1.3e rheostat correction moves, at `index`.
    Expects an already-canonicalized record."""
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

    def test_each_branch_matches_gus_except_the_documented_normalization(self):
        recs = interior_records()
        for value, name in ((0, "current"), (1, "new"), (2, "led")):
            with self.subTest(variant=name):
                # Both sides canonicalized: this test is about COLOR, and
                # deviation #5 changed only the LENGTH of every dir. Comparing raw
                # would drop 20 records into the diff that say nothing about color
                # and would swamp the ones that do. _unit keeps the aim, so a real
                # change of direction still shows up here.
                mine = _canon_all(branch_records(recs, value))
                gus_text = (GUS / f"lights_inn_{name}.obj").read_text(
                    encoding="utf-8", errors="replace")
                theirs = _canon_all(Counter({tuple(r[1:]): n
                                             for r, n in
                                             B.parse_records(gus_text).items()}))
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

    def test_every_direction_is_unit_length_but_still_gus_s_aim(self):
        """DEVIATION #5 (2026-07-29): every interior `dir:` normalized.

        Why it is not mere tidiness — X-Plane compares `cone` against a DOT PRODUCT
        with `dir`, so the effective threshold is cone/|d|. Gus's outer panel floods
        run 0.180 and 0.258 long against cones of 0.819 and 0.906, i.e. thresholds
        of 4.5 and 3.5. Nothing can satisfy those, so those lamps could not act as
        cones at all and `cone` had nothing left to change — which is exactly the
        pair reported in-sim as "the outer floods are wrong under all 48
        permutations" and "cone edits have no visible effect".

        Two halves, and the second is the one that matters: normalizing must change
        LENGTH ONLY. If it ever changes aim it is no longer a fix, it is a silent
        re-aiming of Gus's measured geometry."""
        recs = interior_records()
        for value, name in ((0, "current"), (1, "new"), (2, "led")):
            with self.subTest(variant=name):
                dirs = [rec[5] for rec in branch_records(recs, value)]
                self.assertTrue(dirs)
                for d in dirs:
                    length = math.sqrt(sum(v * v for v in d))
                    if length < 1e-9:
                        continue                       # the dome is omnidirectional
                    self.assertAlmostEqual(
                        length, 1.0, delta=B.UNIT_DIR_TOLERANCE,
                        msg="dir %s has length %.3f — normalize it in the .phdsl "
                            "(`build` prints the unit form)" % (d, length))
                # ...and the set of AIMS is unchanged from Gus's. Compared as a set
                # of unit vectors, so length is out of the picture on both sides.
                gus_text = (GUS / f"lights_inn_{name}.obj").read_text(
                    encoding="utf-8", errors="replace")
                theirs = {_unit(r[6]) for r in B.parse_records(gus_text)}
                mine = {_unit(d) for d in dirs}
                # Photon adds three spot lights Gus never had (§3.4), so ours is a
                # superset; nothing of his may go missing.
                self.assertTrue(theirs <= mine,
                                "aims lost vs Gus: %s" % sorted(theirs - mine))

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
            collapse to one key and the bug is invisible unless counts are honored."""
            out = []
            for rec, n in counter.items():
                # Aim, not the raw vector: Gus's file carries 0 -1.5 0.4 and ours
                # the unit form of it (deviation #5), and this test predates that.
                if rec[pos_at] == (0.0, 0.0, 0.0) and _unit(rec[dir_at]) == FLOOD3_DIR:
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
        lamps, so it has to sit on their color ramp. Putting them on different
        ramps is invisible in the diff against Gus (he has no spots at all to
        compare to) and shows up in-sim as one overhead fixture lit two colors at
        once. It was the reason the two moved onto the fluorescent ramp together
        in 2026-07, and it is why they moved back off it together."""
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
        """The pedestal flood must keep swinging orange -> amber -> blue-white.

        It was the control for withdrawn deviation #2 (the dome going
        fluorescent): the point of that change was the contrast between a white
        dome and warm panels, so flattening the flood too would have quietly
        undone it. The dome is era-colored again and the contrast is gone, but the
        flood's own ramp is Gus's measured value and this is still what pins it."""
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
            if _hidden(rec, 0):     # not the old-halogen branch (full stack)
                continue
            for g in _interior_gates(rec):
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
        labeled the panel flood "Map Lights"; and `map` holding only the spot made
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
                    if _hidden(rec, profile):
                        continue
                    cats = {_category_of(g) for g in _interior_gates(rec)}
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
    binds our interior datarefs (kInteriorObjNeedle / DetectInterior). A contract
    ACROSS the build and the plugin — the DSL decides what the OBJ says, C++ what it
    looks for — and drift shows up only in-sim, as a missing menu.

    ⚠ THE TWO STRINGS MOVED to `src/native/src/core/constants.h` (2026-08-07), the
    shared library the plugin and the installer both link, so there is now ONE
    definition instead of a plugin copy and a Python copy that had to agree. This
    reads them from there and separately checks that plugin.cpp is still wired to
    it — a literal creeping back into plugin.cpp would compile fine and quietly
    reintroduce the drift."""

    PLUGIN = REPO / "src" / "native" / "src" / "plugin.cpp"
    CONSTANTS_H = REPO / "src" / "native" / "src" / "core" / "constants.h"

    def _cpp_string(self, name):
        import re
        text = self.CONSTANTS_H.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if line.lstrip().startswith("//"):
                continue        # the header documents its own constants
            m = re.search(rf'\b{name}\[\]\s*=\s*"((?:[^"\\]|\\.)*)"', line)
            if m:
                return m.group(1)
        self.fail(f"{name} not found in core/constants.h")

    def test_detector_looks_for_the_installed_filename(self):
        self.assertEqual(self._cpp_string("kInteriorObj"), K.INTERIOR_OBJ)

    def test_the_plugin_reads_the_shared_constants_rather_than_its_own(self):
        """The regression guard for the move: a `static const char*
        kInteriorObjNeedle = "…"` reappearing in plugin.cpp would build, run, and
        put the two definitions back out of reach of each other."""
        text = self.PLUGIN.read_text(encoding="utf-8", errors="replace")
        self.assertIn("photon::kInteriorObjNeedle", text)
        self.assertIn("photon::kInteriorObj", text)
        self.assertNotRegex(text, r'kInteriorObjNeedle\s*=\s*"')
        self.assertNotRegex(text, r'kInteriorObjName\s*=\s*"')

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


class PluginSideValueSpaceTests(unittest.TestCase):
    """The plugin's half of the interior value space — a contract with the OBJ the
    build emits, and one whose failure is a cockpit lit two ways at once.

    The dome is the reason this class exists. It went ternary → two-valued
    (2026-07-28) → ternary (2026-08-03), and the SAME NUMBER means a different look
    on either side of each of those moves."""

    PLUGIN = REPO / "src" / "native" / "src" / "plugin.cpp"

    def setUp(self):
        self.cpp = self.PLUGIN.read_text(encoding="utf-8", errors="replace")

    def test_the_prefs_schema_matches_the_value_space(self):
        """⚠ Bump kIntPrefsSchema whenever a category's value space changes, not
        merely when a category is added. A clamp cannot undo a meaning change: a
        v2 `dome: 1` was LED and a v3 `dome: 1` is new halogen, and both are in
        range — and a v3 `integral: 1` was LED where a v4 one is new halogen.

        TWO categories have now moved, and each needs its own migration line:
        the dome at v2, `integral` at v4."""
        import re
        m = re.search(r"kIntPrefsSchema\s*=\s*(\d+)", self.cpp)
        self.assertIsNotNone(m)
        self.assertEqual(int(m.group(1)), 4)
        # v1 and v3 are the same space, so ONLY v2 is migrated for the dome.
        # `schema < 3` here would move a v1 file that is already correct.
        self.assertIn("if (schema == 2)", self.cpp)
        # ⚠ `integral` is the opposite shape: EVERY schema below 4 needs moving,
        # because the key simply did not exist before v3 (reads 0, which is right
        # on both scales) and meant LED at v3. Written as `<= 3` rather than
        # `== 3` so a version inserted between them cannot slip past.
        self.assertIn(
            "if (schema <= 3 && out.intCats[kIntIntegralIndex] == 1)", self.cpp)
        self.assertIn("out.intCats[kIntIntegralIndex] = 2;", self.cpp)

    #: Interior categories that are NOT gated in `lights_inn.obj` and so have no
    #: DSL branches to compare against. ⚠ `integral` switches which `text_LIT.png`
    #: / `knobs_LIT.png` ToLiss's OWN cockpit OBJs are bound to
    #: (core/patch_integral), so the branches live in `lamps.obj`/`knobs.obj` and
    #: the DSL never sees them. Anything added here has to be a deliberate
    #: exception, which is why it is a named set rather than a skip.
    NOT_IN_THE_DSL = {"integral"}

    def test_the_plugin_and_the_dsl_agree_on_how_many_looks_each_category_has(self):
        """The OBJ emits one gated branch per profile; the plugin's `values` says
        how many buttons the row offers and what a profile folds onto. They are
        edited in different files and nothing but this compares them.

        ⚠ EVERY ROW IS PARSED, including the ones whose dataref is a shared
        CONSTANT rather than a string literal. A regex that only matched literals
        would silently stop counting `integral` — and would then still pass,
        because the count it compares against would match too."""
        import re
        table = re.search(r"kIntCategories\[NINT\] = \{(.*?)\n\};", self.cpp, re.S)
        self.assertIsNotNone(table)
        rows = re.findall(
            r'\{"(\w+)",\s*(?:"[^"]+"|[\w:]+),\s*"[^"]+",\s*(\d+),',
            table.group(1))
        keys = [k for k, _ in rows]
        self.assertEqual(set(keys), set(INT_VALUES) | self.NOT_IN_THE_DSL)
        self.assertEqual(len(keys), len(set(keys)), "a category key is duplicated")
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="interior")
        for key, values in rows:
            with self.subTest(category=key):
                if key in self.NOT_IN_THE_DSL:
                    # No DSL branches to count — but its value space still has to
                    # be one this codebase can gate, and 2 is the only other one.
                    self.assertIn(int(values), (2, 3))
                    # ⚠ And it really must be ABSENT from the DSL, not merely
                    # unused by it. A category that grew branches in
                    # lights_inn.obj while still listed here as an exception
                    # would go uncompared by the loop above — which is the exact
                    # hole this branch exists to keep shut.
                    self.assertNotIn(key, em.categories)
                    continue
                self.assertEqual(int(values), INT_VALUES[key])
                self.assertEqual(len(em.profiles_of(key)), INT_VALUES[key])

    def test_the_integral_row_is_ternary_and_shares_its_dataref_name(self):
        """⚠ Integral lighting shipped TWO-valued for three days (2026-08-24 to
        2026-08-27) and is ternary again, and three separate things have to agree
        about it.

        1. Three values, like its five neighbours. It was two while one value had
           to cover both halogen eras — a filament behind a placard was taken not
           to read as two looks — and Gus settled that by sending finished artwork
           for all three. ⚠ At 2 a two-valued gate hides NEITHER branch and
           both draw at once, so the row's `values` and the OBJ's ANIM_hide ranges
           are one contract.
        2. The grid's own words. The row sits beside five offering "Old Halogen",
           "New Halogen" and "LED", and anything else here reads as a different
           question. It said "Incandescent" while it was two-valued, which was the
           honest name for a fold and is the wrong name without one.
        3. Its dataref comes from `photon::integral::kDataRef`, the same constant
           the installer writes into the OBJ's ANIM_hide. A literal here would
           compile, run, and gate nothing — the branch would simply never draw."""
        import re
        table = re.search(r"kIntCategories\[NINT\] = \{(.*?)\n\};", self.cpp, re.S)
        row = re.search(
            r'\{"integral",\s*([\w:]+),\s*"([^"]+)",\s*(\d+),\s*'
            r'\{"([^"]+)",\s*"([^"]+)",\s*"([^"]+)"\}\}',
            table.group(1))
        self.assertIsNotNone(row, "the integral row is not in its expected shape")
        dataref, label, values, first, second, third = row.groups()
        self.assertEqual(dataref, "photon::integral::kDataRef")
        self.assertEqual(label, "Integral Lights")
        self.assertEqual(int(values), 3)
        self.assertEqual((first, second, third),
                         ("Old Halogen", "New Halogen", "LED"))
        # ⚠ And the row is HIDDEN where the twin OBJs are not installed, not
        # shown inert: one dead row in a grid of five reads as a broken look
        # rather than an absent one.
        self.assertIn("if (row == kIntIntegralIndex) return gIntegralInstalled;",
                      self.cpp)

    def test_detection_demands_every_era_twin_not_merely_one(self):
        """⚠ THE UPGRADE GUARD. An aircraft installed before the row went ternary
        carries only the `_photon_led` twin, gated the TWO-valued way — the stock
        OBJ hides at exactly 1, the twin at exactly 0. Read on the ternary scale,
        value 2 hides NEITHER and both copies of the flight deck draw at once,
        while value 1 draws the LED branch under the label "New Halogen". Both
        failures are silent in the log and loud in the cockpit.

        So detection has to demand the FULL set and let such an aircraft read as
        not-installed until a reinstall writes the missing twin."""
        import re
        fn = re.search(r"static bool DetectIntegral\(.*?\n\}", self.cpp, re.S)
        self.assertIsNotNone(fn, "DetectIntegral is no longer a single function")
        body = fn.group(0)
        # It walks the era table rather than testing one spelled-out suffix.
        self.assertIn("EraSuffixes()", body)
        self.assertIn("kEraCount", body)
        self.assertNotIn("kLedSuffix", body)
        # And it answers false unless every era was seen — an `any` would be the
        # bug above.
        self.assertIn("if (!seen[e]) return false;", body)

    def test_a_row_button_is_named_the_same_as_the_profile_that_sets_it(self):
        """The Custom rows and the profile dropdown are two lists of the same
        words, deliberately: the same name has to select the same look, or a user
        comparing them is being told they differ."""
        import re
        table = re.search(r"kIntCategories\[NINT\] = \{(.*?)\n\};", self.cpp, re.S)
        labels = set(re.findall(r'\{"(?:Old Halogen|New Halogen|LED)"', table.group(1)))
        self.assertTrue(labels, "the ternary rows no longer use the era names")
        profiles = re.search(r"kIntProfileItems\[\] = \{(.*?)\n\};", self.cpp, re.S)
        for want in ("Old Halogen", "New Halogen", "LED"):
            with self.subTest(label=want):
                self.assertIn('"%s"' % want, table.group(1))
                self.assertIn('"%s"' % want, profiles.group(1))
        # The old parenthesised spelling is gone from both.
        self.assertNotIn("Halogen (Old)", self.cpp)
        self.assertNotIn("Halogen (New)", self.cpp)


#: The four main panel flood lamps' authored stack sizes and cones. Gus stacks
#: two or three coincident spill lights per lamp to fake a soft falloff; the
#: optimized set keeps the WIDEST cone of each (cone runs backwards, so the
#: smallest number) and boosts its size.
FLOOD_BOOST = 1.5
FLOOD_SIZE = 0.471
FLOOD_STACK_LIGHTS = 10          # 2 + 3 + 3 + 2
FLOOD_OPTIMIZED_LIGHTS = 4       # one per lamp


class OptimizedLightCountTests(unittest.TestCase):
    """The reduced-light-count cockpit, gated on ToLissPhoton/interior/optimized.

    Gus's stack is an EFFECT TRICK and stays the default: 0 is his ten flood
    lights, 1 is four. The failure modes are the interior's usual ones — both
    sets drawing at once (twice the cost and a double-bright panel), neither
    drawing (four dark lamps), or the reduced set silently keeping the NARROW
    cone of a stack and lighting a bright spot in a dark pool."""

    def setUp(self):
        self.recs = interior_records()

    def test_zero_is_gus_s_full_stack_and_one_is_the_reduced_set(self):
        """Polarity, and it is the load-bearing half: an OBJ8 ANIM_hide reading a
        dataref that does not exist sees 0 FOREVER (an OBJ binds dataref names at
        load time). So 0 has to be the authored look — with the plugin absent, or
        older than the OBJ, the cockpit is Gus's, never a reduced set nobody
        asked for."""
        full = branch_records(self.recs, 0, optimized=0)
        lean = branch_records(self.recs, 0, optimized=1)
        self.assertEqual(sum(full.values()), 26)   # Gus's 23 + the 3 .acf spots
        self.assertEqual(sum(lean.values()),
                         26 - FLOOD_STACK_LIGHTS + FLOOD_OPTIMIZED_LIGHTS)

    def test_exactly_one_set_is_visible_at_each_value(self):
        """The ANIM_show trap in its two-valued form: a block starts VISIBLE, so a
        gate that covers neither value leaves BOTH sets drawing — which is not a
        crash, not a log line, just a panel lit twice at whatever cost the switch
        was meant to save."""
        cfg = B.load_config(wing="stock")
        inn = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        blocks, stack = [], []
        for line in inn.splitlines():
            s = line.strip()
            if s.startswith("ANIM_begin"):
                stack.append([])
            elif s.startswith("ANIM_end"):
                if stack:
                    b = stack.pop()
                    if b:
                        blocks.append(b)
            elif s.startswith("ANIM_hide") and s.endswith(OPTIMIZE_DREF):
                lo, hi = (float(x) for x in s.split()[1:3])
                if stack:
                    stack[-1].append((lo, hi))
        # one full + one reduced block per lamp per era branch
        self.assertEqual(len(blocks), 4 * 2 * 3)
        for value in (0, 1):
            visible = sum(1 for b in blocks
                          if not any(lo <= value <= hi for lo, hi in b))
            self.assertEqual(visible, len(blocks) // 2,
                             f"optimized={value}: {visible} of {len(blocks)} "
                             f"blocks visible, expected half")

    def test_only_the_panel_floods_change(self):
        """Nothing else in the cockpit is a stack, so nothing else may move. A
        `drop` that reached the dome or the console strips would delete a light
        the user can see, not one hiding inside another."""
        for profile in (0, 1, 2):
            with self.subTest(profile=profile):
                full = colors_by_category(self.recs, profile, optimized=0)
                lean = colors_by_category(self.recs, profile, optimized=1)
                self.assertEqual(full, lean)      # color is untouched either way
                for cat in INT_VALUES:
                    a = Counter(r for r in branch_records(self.recs, profile, 0))
                    b = Counter(r for r in branch_records(self.recs, profile, 1))
                    changed = {r for r in (a - b) | (b - a)}
                    # every changed record is a panel flood: position 0 0 0
                    # (their placement rides the mount animation)
                    self.assertTrue(all(r[1] == (0.0, 0.0, 0.0) for r in changed),
                                    changed)

    def test_the_survivor_is_the_widest_cone_of_its_stack(self):
        """The authoring rule, checked per FIXTURE against the DSL — the emitted
        OBJ cannot answer this, since two lamps share a rheostat index and their
        records flatten together.

        Keeping a NARROW cone would leave a bright spot ringed by the dark the
        stack existed to fill in, and it looks deliberate enough in a screenshot
        to survive review."""
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="interior")
        stacks = 0
        for name, fx in B.resolve_airframe(cfg, "a320")["fixtures"].items():
            lights = fx.get("lights") or []
            opts = [(l, em.optimize_of(l, name)) for l in lights
                    if fx.get("target") == "interior"]
            if not any(o for _, o in opts):
                continue
            stacks += 1
            cones = [em.field({**em.types[l["type"]], **l}["cone"], "int_old", None)
                     for l, _ in opts]
            kept = [c for (l, o), c in zip(opts, cones) if o and o[0] == "boost"]
            self.assertEqual(len(kept), 1,
                             f"{name}: the reduced set must be exactly one light")
            self.assertEqual(kept[0], min(cones),
                             f"{name}: kept cone {kept[0]} is not the widest "
                             f"(smallest) of {cones}")
        self.assertEqual(stacks, 4)     # the four panel floods, and only those
        # ...and the concrete numbers, so a re-tune is a visible diff here too:
        # lamps 1 and 4 keep cos(35°), lamps 2 and 3 cos(50°).
        floods = [r for r in branch_records(self.recs, 0, optimized=1).elements()
                  if r[1] == (0.0, 0.0, 0.0)]
        self.assertEqual(len(floods), FLOOD_OPTIMIZED_LIGHTS)
        self.assertEqual(sorted(r[6] for r in floods),
                         [0.643, 0.643, 0.819, 0.819])

    def test_the_survivor_is_boosted_and_nothing_else_is(self):
        """`boost` scales slot 8 — SIZE on an airplane_panel_sp, the only
        magnitude such a light has (color is the palette's and brightness is the
        rheostat's). It must not touch color, aim or cone: this is meant to
        stand in for the missing lights, not to become a different lamp."""
        full = branch_records(self.recs, 0, optimized=0)
        lean = branch_records(self.recs, 0, optimized=1)
        for r in lean.elements():
            if r[1] != (0.0, 0.0, 0.0):
                # every non-flood light is untouched between the two sets
                self.assertIn(r, full)
                continue
            with self.subTest(cone=r[6]):
                self.assertAlmostEqual(r[4], round(FLOOD_SIZE * FLOOD_BOOST, 3))
                # the same light at its authored size is in the full stack, and
                # differs ONLY in that slot
                twin = [s for s in full.elements()
                        if s[:4] == r[:4] and s[5:] == r[5:]]
                self.assertTrue(twin, f"no unboosted twin for {r}")
                self.assertAlmostEqual(twin[0][4], FLOOD_SIZE)

    def test_the_dsl_and_the_plugin_name_the_same_dataref(self):
        """Two files, one wire. A rename on either side is silent: the OBJ's gate
        reads 0 forever and the switch does nothing."""
        cpp = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
            encoding="utf-8", errors="replace")
        self.assertIn(f'"{OPTIMIZE_DREF}"', cpp)
        self.assertEqual(B.OPTIMIZE_DREF.format(target="interior"), OPTIMIZE_DREF)

    def test_a_debug_build_keeps_the_optimize_gate_and_marks_the_copies(self):
        """A debug build emits BOTH flood sets as tunable lights (2026-08-09; it
        used to skip `optimize:` entirely, which left the simplified floods
        untunable) and keeps the ToLissPhoton/interior/optimized gate between
        them — the Settings tab's "Use simplified panel flood lights" checkbox
        picks which set draws, exactly as on a shipping OBJ. Each simplified copy's manifest
        row records the authored (unboosted) size plus the factor under `boost`,
        which is what lets the Dev window drive the factor as ONE live
        multiplier; its baked seed is size*boost so a plugin-absent load still
        shows the authored optimized look."""
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="interior", debug=True)
        text = em.emit()
        self.assertIn(OPTIMIZE_DREF, _code(text))
        plain = B.Emitter(cfg, "a320", "stock", target="interior")
        plain.emit()
        boosted = [d for d in em.debug_lights if "boost" in d]
        # one simplified copy per flood fixture, same count as the shipping OBJ's
        # lean set (branch_records over optimized=1 counts the four floods)
        self.assertEqual(len(boosted), 4)
        for d in boosted:
            self.assertGreater(d["boost"], 1.0)
            self.assertIn("(simplified)", d["name"])
            # the manifest size is the AUTHORED one; the OBJ line bakes the
            # boosted seed for the plugin-absent case
            self.assertAlmostEqual(
                d["size"], cfg["lightTypes"]["int_panelflood"]["size"], places=6)
        for line in _code(text).splitlines():
            line = line.strip()
            if not line.startswith("LIGHT_SPILL_CUSTOM"):
                continue
            for d in boosted:
                if line.endswith(f"{B.DEBUG_LIGHT_DREF}/{d['n']}"):
                    baked_size = float(line.split()[8])
                    self.assertAlmostEqual(baked_size, d["size"] * d["boost"],
                                           places=2)

    def test_optimize_on_the_exterior_is_refused(self):
        """It would fail OPEN and silently — the plugin registers no
        ToLissPhoton/exterior/optimized, so the reduced set could never be
        selected — and the frozen goldens would start failing `check` with no
        clue as to why."""
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="exterior")
        with self.assertRaises(SystemExit):
            em.optimize_of({"type": "nav_beam", "optimize": "drop"}, "test")

    def test_a_bad_optimize_value_is_an_error_not_a_shrug(self):
        """A typo must not fall through as "no optimize", which would leave the
        whole stack in the reduced set and read in-sim as the switch doing
        nothing at all."""
        cfg = B.load_config(wing="stock")
        em = B.Emitter(cfg, "a320", "stock", target="interior")
        for bad in ("keep", ["boost"], ["boost", "lots"], ["boost", -1]):
            with self.subTest(value=bad):
                with self.assertRaises(SystemExit):
                    em.optimize_of({"type": "int_panelflood", "optimize": bad},
                                   "test")

    def test_the_switch_is_registered_at_xpluginstart_as_int_and_float(self):
        """Both tripwires at once, and both fail silently.

        An OBJ binds dataref NAMES at load time, so a name registered later than
        XPluginStart leaves the gate reading 0 for the life of that aircraft; and
        registering one type makes X-Plane advertise the type wrong, which does
        the same thing. Either way the switch simply never works — and 0 being
        the fail-open value is what makes that a dud switch rather than a dark
        cockpit."""
        cpp = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
            encoding="utf-8", errors="replace")
        start = cpp.index("PLUGIN_API int XPluginStart")
        body = cpp[start:cpp.index("PLUGIN_API int XPluginEnable")]
        self.assertIn("kIntOptimizedRef", body)
        reg = body[body.index("gIntOptimizedRef = XPLMRegisterDataAccessor("):]
        self.assertIn("xplmType_Int | xplmType_Float", reg[:200])
        self.assertIn("ReadIntOptimizedInt", reg[:300])
        self.assertIn("ReadIntOptimizedFloat", reg[:300])

    def test_it_is_a_global_setting_not_a_per_livery_one(self):
        """It is a statement about the MACHINE, not about the aeroplane. Per
        livery it would appear to forget itself on every repaint — and a user who
        turned it on for a 40-livery folder did not mean "on this one paint"."""
        cpp = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
            encoding="utf-8", errors="replace")
        # The literal moved to constants.h (2026-08-16) because the INSTALLER
        # now reads this block too (the intensity multiplier) — the plugin must
        # bind the shared constant, not respell the key, or reader and writer
        # drift apart.
        self.assertIn("kCockpitKey = photon::kPrefsCockpitKey", cpp)
        constants_h = (REPO / "src" / "native" / "src" / "core"
                       / "constants.h").read_text(encoding="utf-8",
                                                  errors="replace")
        self.assertIn('kPrefsCockpitKey[] = "$cockpit"', constants_h)
        # skipped by the livery loop, or EntryFromJson would read it as a
        # phantom aircraft and write it straight back as one
        self.assertIn("if (kv.first == kCockpitKey)", cpp)
        # ...and always written, like the Displays block: "absent means default"
        # cannot express a switch someone deliberately turned off.
        self.assertIn('\\"optimized\\": %d', cpp)
        self.assertNotIn("interior_optimized", cpp)

    def test_a_fixture_with_no_optimize_emits_nothing_extra(self):
        """The floor under every other fixture and the whole exterior: no
        `optimize:` anywhere in a group means not one extra byte, so the goldens
        and Gus's own comparison are untouched."""
        cfg = B.load_config(wing="stock")
        inn = B.Emitter(cfg, "a320", "stock", target="interior").emit()
        # the gate appears only inside the four flood fixtures
        for fixture in ("Dome Lights", "Pedestal & Tables", "Map Lights",
                        "Console Lights"):
            block = inn.split(fixture)[1].split("ANIM_end\r\n\r\n")[0]
            self.assertNotIn(OPTIMIZE_DREF, block)
        ext = B.Emitter(cfg, "a320", "stock", target="exterior").emit()
        self.assertNotIn("optimized", ext)


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

    # ⚠ TRAP 3 — interior status is a SECOND, INDEPENDENT axis. `PhotonStatus`
    # scans only the EXTERIOR OBJs, so an aircraft with the interior installed and
    # the exterior reverted would report as "not installed at all" and hide a live
    # modification from the user.
    #
    # MOVED TO C++ (2026-08-09): the detection code is `core/detect.cpp` and there
    # is no Python installer left to test. The case lives on as
    # `detect: the interior is a SECOND, INDEPENDENT status axis` in
    # src/native/tests/install_tests.cpp. It is named here because the trap is an
    # INTERIOR trap and this is the file someone editing the interior reads.


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

    def _debug(self, airframe="a320", pos=True):
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
        """One branch, no CATEGORY gate: the dev plugin owns color while a debug
        OBJ is installed, so a gate could only hide the light being looked at.
        The banner says so — one left installed by accident looks like a broken
        Cockpit menu. The one interior gate a debug build KEEPS is the
        simplified-lighting switch (.../optimized): both flood sets are emitted
        as tunable lights, so both must stay switchable."""
        em, text = self._debug()
        gates = [line for line in text.splitlines()
                 if "ANIM_hide" in line and "ToLissPhoton/interior/" in line]
        category_gates = [g for g in gates if "/optimized" not in g]
        self.assertEqual(category_gates, [])
        self.assertNotEqual([g for g in gates if "/optimized" in g], [])
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
        dataref reads directly as meters; any other keying rescales every edit."""
        em, text = self._debug()
        lines = [l.strip() for l in text.splitlines() if l.strip().startswith("ANIM_trans")]
        moves = [l for l in lines if B.DEBUG_POS_DREF in l]
        self.assertEqual(len(moves), 3 * len(em.debug_lights))
        r = B.fmt(B.DEBUG_POS_RANGE)
        want0 = [f"ANIM_trans\t-{r} 0 0\t{r} 0 0\t-{r} {r}\t{B.DEBUG_POS_DREF}[0]",
                 f"ANIM_trans\t0 -{r} 0\t0 {r} 0\t-{r} {r}\t{B.DEBUG_POS_DREF}[1]",
                 f"ANIM_trans\t0 0 -{r}\t0 0 {r}\t-{r} {r}\t{B.DEBUG_POS_DREF}[2]"]
        self.assertEqual(moves[:3], want0)
        # light n owns exactly elements [3n, 3n+1, 3n+2] — the mapping the plugin's
        # DbgReadPos mirrors, and an off-by-one nudges the neighboring light
        indices = [int(l.rsplit("[", 1)[1].rstrip("]")) for l in moves]
        self.assertEqual(indices, list(range(3 * len(em.debug_lights))))

    def test_position_wrappers_nest_inside_any_mount(self):
        """A mounted lamp's offset must apply INSIDE ToLiss's ckpt/lights/<n> stack, so
        it nudges the light relative to where the animation puts it. Emitting it outside
        would offset in aircraft space and fight the mount."""
        em, text = self._debug()
        lines = [l.strip() for l in text.splitlines()]
        mount_at = lines.index(next(l for l in lines if "ckpt/lights/1/z/dir" in l))
        move_after = next(i for i, l in enumerate(lines)
                          if i > mount_at and B.DEBUG_POS_DREF in l)
        light_after = next(i for i, l in enumerate(lines)
                           if i > mount_at and l.startswith("LIGHT_SPILL_CUSTOM"))
        self.assertLess(mount_at, move_after)
        self.assertLess(move_after, light_after)

    def test_position_wrappers_are_on_by_default(self):
        """A debug build moves again. They were briefly opt-in (2026-07-28) after the
        debug lights turned up mis-placed in-sim; that was traced to a separate cause,
        so the default is back and X/Y/Z is a knob without a flag."""
        em, _ = self._debug()
        self.assertTrue(em.debug_pos)

    def test_no_debug_pos_drops_the_wrappers_and_nothing_else(self):
        """The escape hatch. The wrappers are the one part of a debug build that changes
        how lights RENDER rather than where their params come from, so ruling them out
        must stay one flag away — and must not take the lights with them."""
        em, text = self._debug(pos=False)
        self.assertFalse(em.debug_pos)
        self.assertNotIn(B.DEBUG_POS_DREF, text)
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
        and cannot. Since it is now the DEFAULT, every non-debug build asks for exactly
        this combination, and the Emitter has to refuse it rather than pretend."""
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


class IntensityMultiplierTests(unittest.TestCase):
    """The cockpit-light intensity multiplier (2026-08-16): a 1x-4x factor baked
    into lights_inn.obj's own light lines by core/patch_intensity — the light
    TYPES stay exactly as Gus authored them (converting them to dataref-driven
    custom lights was tried and rejected: it changes the look and adds per-frame
    callbacks), so the value lives in the file and needs an aircraft reload.

    TWO writers apply it — the plugin (Settings-tab slider, and every aircraft
    load) and the installer (right after staging a fresh interior OBJ) — and
    these pin the cross-file agreements between them that no compiler checks."""

    @classmethod
    def setUpClass(cls):
        native = REPO / "src" / "native" / "src"
        cls.plugin = (native / "plugin.cpp").read_text(
            encoding="utf-8", errors="replace")
        cls.actions = (native / "core" / "actions.cpp").read_text(
            encoding="utf-8", errors="replace")
        cls.header = (native / "core" / "patch_intensity.h").read_text(
            encoding="utf-8", errors="replace")

    def test_the_json_key_is_one_constant_on_both_sides(self):
        """The plugin WRITES "$cockpit"."intensity" and the installer READS it
        back (SavedFactor). Each side spelling its own literal is how a reader
        and a writer drift, so the plugin must go through the shared constants
        for both the block key and the value key — never a bare literal."""
        self.assertIn('kIntensityJsonKey[] = "intensity"', self.header)
        # read (CockpitFromJson) and write (SaveProfilesFile) both go through it
        self.assertGreaterEqual(
            self.plugin.count("photon::intensity::kIntensityJsonKey"), 2)
        self.assertNotIn('"intensity"', self.plugin)

    def test_the_slider_commits_on_release_never_per_drag_tick(self):
        """The commit rewrites a FILE where every neighboring control flips a
        flag — per drag tick it would be dozens of rewrites per adjustment."""
        i = self.plugin.index('SliderFloat("Brightness"')
        window = self.plugin[i:i + 500]
        self.assertIn("IsItemDeactivatedAfterEdit()", window)
        self.assertIn("CommitCockpitIntensity()", window)

    def test_the_apply_gates_on_installed_and_not_stale(self):
        """⚠ Both, deliberately: gInteriorInstalled because there is nothing of
        ours to rewrite otherwise, and !gInstallStale because a reverted
        install's contract is to do NOTHING — file writes included."""
        self.assertIn("if (!gInteriorInstalled || gInstallStale) return;",
                      self.plugin)

    def test_the_aircraft_load_path_reapplies_after_the_stale_gate(self):
        """One saved setting reaches all three A3xx airframes by re-applying on
        every load — and the call must sit AFTER the stale early-return, or a
        reverted install would get its OBJ rewritten by the very code path that
        just promised to do nothing."""
        i = self.plugin.index("static void UpdateMenuVisibility() {")
        body = self.plugin[i:self.plugin.index("\n}", i)]
        self.assertIn('ApplyInteriorIntensity("aircraft load")', body)
        self.assertGreater(body.index('ApplyInteriorIntensity("aircraft load")'),
                           body.index("if (gInstallStale) {"))

    def test_the_installer_applies_the_saved_factor_before_the_write(self):
        """The installer half: SavedFactor(opts.xplaneRoot) patched into the
        in-memory text BEFORE WriteOrThrow, so the aircraft sees exactly one
        atomic write and a reinstall cannot silently reset the brightness."""
        i = self.actions.index("std::vector<std::string> InstallInterior(")
        body = self.actions[i:i + 5000]
        saved = body.index("intensity::SavedFactor(opts.xplaneRoot)")
        patched = body.index("intensity::PatchText(")
        write = body.index("WriteOrThrow(target, text)")
        self.assertLess(saved, patched)
        self.assertLess(patched, write)

    def test_the_installer_persists_the_neo_flag_before_it_bakes_the_boost(self):
        """⚠ THE TWO HALVES MUST AGREE ON THE FACTOR. The plugin recomputes
        EffectiveFactor(SavedFactor, SavedNeo) on every aircraft load and
        rewrites the OBJ when it disagrees — so an installer that baked a
        compensation the plugin could not read back would have it patched out
        on the very next load, and the cockpit would go dim again some time
        after a successful install with nothing to connect the two."""
        i = self.actions.index("std::vector<std::string> InstallInterior(")
        body = self.actions[i:i + 5000]
        saved_neo = body.index("intensity::SaveNeo(opts.xplaneRoot, opts.neo)")
        effective = body.index("intensity::EffectiveFactor(")
        patched = body.index("intensity::PatchText(")
        self.assertLess(saved_neo, effective)
        self.assertLess(effective, patched)

    def test_both_halves_bake_the_factor_through_EffectiveFactor(self):
        """The one place the user setting and the NEO flag are combined. If
        either half multiplied by hand, the two would drift and re-patch the
        OBJ against each other on alternate loads."""
        self.assertIn("intensity::EffectiveFactor(", self.actions)
        self.assertIn("photon::intensity::EffectiveFactor(", self.plugin)
        # ⚠ and the plugin's on-load apply must not pass the RAW setting.
        i = self.plugin.index("static void ApplyInteriorIntensity(")
        body = self.plugin[i:i + 1600]
        self.assertIn("EffectiveFactor(", body)

    def test_the_settings_row_is_hidden_without_the_mod(self):
        """Same rule as the simplified-floods row above it: HIDDEN, not grayed,
        where the cockpit mod is not installed — it drives an OBJ that is not
        on this aircraft, and a lone row has nowhere to explain itself."""
        i = self.plugin.index('SeparatorText("Cockpit lighting")')
        self.assertIn("if (gInteriorInstalled)", self.plugin[i - 800:i])

    def test_the_patcher_only_touches_a_photon_interior_obj(self):
        """CanPatch requires the interior needle and refuses a --debug build —
        the stock ToLiss lights_inn.obj must never be rewritten, and a debug
        OBJ's baked numbers are dead weight the tuning session must not meet."""
        cpp = (REPO / "src" / "native" / "src" / "core"
               / "patch_intensity.cpp").read_text(encoding="utf-8",
                                                  errors="replace")
        self.assertIn("kInteriorObjNeedle", cpp)
        self.assertIn("kDebugNeedle", cpp)


if __name__ == "__main__":
    unittest.main()
