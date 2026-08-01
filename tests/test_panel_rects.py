"""The panel rect table and its group hierarchy, checked against plugin.cpp.

None of this can fail loudly in the simulator. A group that claims a rect it does
not paint, two groups that partially overlap, or a low-res table that has drifted
out of step with the high-res one all produce a tint that lands somewhere
slightly wrong — which reads as "the effect is mistuned", not as "the table is
broken". Hence a test.
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
PLUGIN_CPP = (REPO / "src" / "native" / "src" / "plugin.cpp").read_text(
    encoding="utf-8", errors="replace"
)

RECT_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*(-?[\d.]+)f\s*,\s*(-?[\d.]+)f\s*,'
    r'\s*(-?[\d.]+)f\s*,\s*(-?[\d.]+)f\s*,\s*(-?\d+)\s*\}'
)


def _table(name):
    """The rows of a `static const PanelRect <name>[] = { ... };` block."""
    start = PLUGIN_CPP.index("static const PanelRect %s[] = {" % name)
    end = PLUGIN_CPP.index("};", start)
    return RECT_RE.findall(PLUGIN_CPP[start:end])


def _groups():
    """[(name, [member, ...]), ...] from kPanelGroups."""
    start = PLUGIN_CPP.index("static const PanelGroup kPanelGroups[] = {")
    end = PLUGIN_CPP.index("\n};", start)
    body = PLUGIN_CPP[start:end]
    out = []
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*\{(.*?)nullptr\s*\}\s*\}', body, re.S):
        members = re.findall(r'"([^"]+)"', m.group(2))
        out.append((m.group(1), members))
    return out


class PanelRectTableTests(unittest.TestCase):
    def setUp(self):
        self.hi = _table("kPanelRectsHi")
        self.lo = _table("kPanelRectsLo")

    def test_tables_are_parsed_at_all(self):
        # A silent regex miss would make every other assertion here vacuous.
        self.assertGreaterEqual(len(self.hi), 18)

    def test_both_resolution_tables_hold_the_same_faces_in_order(self):
        # A target index selects a row in whichever table is live, so a length or
        # order difference silently re-aims the tint at another screen.
        self.assertEqual([r[0] for r in self.hi], [r[0] for r in self.lo])

    def test_rect_names_are_unique(self):
        names = [r[0] for r in self.hi]
        self.assertEqual(sorted(names), sorted(set(names)))

    def test_only_the_display_units_are_resolution_switched(self):
        # Everything else has one variant, so its two rows must be identical.
        du = {n for n, *_ in self.hi if _du_index(n) >= 0}
        for hi, lo in zip(self.hi, self.lo):
            if hi[0] in du:
                continue
            self.assertEqual(hi, lo, "%s differs between the rect tables" % hi[0])

    def test_every_rect_is_a_sane_non_empty_box_inside_the_atlas(self):
        for name, u0, u1, v0, v1, _du in self.hi + self.lo:
            u0, u1, v0, v1 = float(u0), float(u1), float(v0), float(v1)
            self.assertLess(u0, u1, name)
            self.assertLess(v0, v1, name)
            for v in (u0, u1, v0, v1):
                self.assertGreaterEqual(v, 0.0, name)
                self.assertLessEqual(v, 4096.0, name)


def _du_index(name):
    for row in _table("kPanelRectsHi"):
        if row[0] == name:
            return int(row[5])
    return -1


class PanelGroupHierarchyTests(unittest.TestCase):
    """The tree is DERIVED from rect sets, so these are its structural rules."""

    def setUp(self):
        self.rects = [r[0] for r in _table("kPanelRectsHi")]
        self.groups = dict(_groups())
        self.assertTrue(self.groups, "kPanelGroups did not parse")

    def _mask(self, name):
        if name in self.rects:
            return 1 << self.rects.index(name)
        m = 0
        for member in self.groups[name]:
            m |= 1 << self.rects.index(member)
        return m

    def test_every_group_member_names_a_real_rect(self):
        # A typo here is invisible: PanelTargetMask just drops the member, so the
        # group quietly paints one face fewer than it claims.
        for gname, members in self.groups.items():
            for member in members:
                self.assertIn(member, self.rects,
                              "group %r names unknown rect %r" % (gname, member))

    def test_no_group_is_empty_or_single_membered(self):
        # A single-member group's mask EQUALS its member's, and containment is
        # strict, so the two would show as siblings instead of nesting.
        for gname, members in self.groups.items():
            self.assertGreaterEqual(len(members), 2,
                                    "group %r has %d member(s)" % (gname, len(members)))

    def test_no_group_lists_a_member_twice(self):
        for gname, members in self.groups.items():
            self.assertEqual(sorted(members), sorted(set(members)), gname)

    def test_sibling_groups_are_disjoint_or_nested_never_partially_overlapping(self):
        # A partially overlapping group is fine for the compositor, which only
        # sorts by breadth, but has no single place in a tree — a rect would need
        # to appear under two parents. See the tripwire in plugin.cpp.
        names = list(self.groups)
        for i, a in enumerate(names):
            for b in names[i + 1:]:
                ma, mb = self._mask(a), self._mask(b)
                shared = ma & mb
                if not shared:
                    continue
                self.assertIn(shared, (ma, mb),
                              "groups %r and %r partially overlap" % (a, b))

    def test_every_rect_has_a_parent_group(self):
        # An orphan rect would sit at the tree's root next to "Whole panel",
        # which reads as a bug rather than as a deliberate top-level target.
        for rect in self.rects:
            mr = self._mask(rect)
            parents = [g for g in self.groups
                       if self._mask(g) != mr and (self._mask(g) & mr) == mr]
            self.assertTrue(parents, "rect %r is in no group" % rect)

    def test_exactly_one_root(self):
        roots = [g for g in self.groups
                 if not any(self._mask(o) != self._mask(g)
                            and (self._mask(o) & self._mask(g)) == self._mask(g)
                            for o in self.groups)]
        self.assertEqual(len(roots), 1, "expected one root group, got %r" % roots)

    def test_the_root_covers_every_rect(self):
        root = [g for g in self.groups
                if not any(self._mask(o) != self._mask(g)
                           and (self._mask(o) & self._mask(g)) == self._mask(g)
                           for o in self.groups)][0]
        self.assertEqual(self._mask(root), (1 << len(self.rects)) - 1,
                         "%r does not cover every rect" % root)

    def test_group_member_capacity_fits_the_largest_group(self):
        # members[] is a fixed array with a nullptr terminator; overflowing it
        # would truncate silently.
        m = re.search(r"const char\* members\[(\d+)\]", PLUGIN_CPP)
        self.assertIsNotNone(m)
        cap = int(m.group(1))
        biggest = max(len(v) for v in self.groups.values())
        self.assertLess(biggest, cap,
                        "largest group has %d members, members[%d] leaves no room "
                        "for the nullptr terminator" % (biggest, cap))


class ContainmentMaskTests(unittest.TestCase):
    """The bitmask the whole hierarchy is derived from."""

    def test_the_rect_table_fits_the_mask(self):
        # ⚠ This was `unsigned` while the table held 23 rects. The pedestal and
        # overhead readouts took it to 36, and `1u << 35` is undefined behaviour
        # that in practice wraps — every rect past the 32nd would alias one of the
        # first four, so a group would claim rects it does not paint and the tree
        # would quietly rearrange itself. Nothing about that looks like overflow.
        rects = _table("kPanelRectsHi")
        m = re.search(r"typedef\s+std::uint(\d+)_t\s+PanelMask\s*;", PLUGIN_CPP)
        self.assertIsNotNone(m, "PanelMask is not a fixed-width typedef any more")
        self.assertLessEqual(len(rects), int(m.group(1)),
                             "%d rects will not fit a %s-bit mask" % (len(rects), m.group(1)))

    def test_the_mask_is_not_silently_an_int(self):
        # A `1 << t` left anywhere in the derivation is the same bug in miniature.
        for line in PLUGIN_CPP.splitlines():
            if "PanelMask" in line and "<<" in line:
                self.assertIn("(PanelMask)1", line,
                              "shift into a PanelMask must be widened first: %s" % line.strip())


class RectOverlapTests(unittest.TestCase):
    """Two rows that overlap in the atlas tint each other's pixels."""

    def test_no_two_rects_overlap_in_uv(self):
        # A face's UV bounding box IS its pixel rect in the panel FBO, so an
        # overlap is one target painting over another — which reads as "the tint
        # bled" rather than as a bad table. The known-adjacent pairs are trimmed
        # deliberately (MCDU v0 clears the ISIS by 0.3 px; the EFB screens stop
        # short of their bezels), and this is what keeps that true.
        rects = _table("kPanelRectsHi")
        boxes = [(r[0], float(r[1]), float(r[2]), float(r[3]), float(r[4])) for r in rects]
        for i in range(len(boxes)):
            for j in range(i + 1, len(boxes)):
                a, b = boxes[i], boxes[j]
                overlap = (a[1] < b[2] and b[1] < a[2]      # u ranges intersect
                           and a[3] < b[4] and b[3] < a[4])  # and so do v
                self.assertFalse(overlap,
                                 "%r and %r overlap in the atlas: u %g..%g / %g..%g, "
                                 "v %g..%g / %g..%g" % (a[0], b[0], a[1], a[2],
                                                        b[1], b[2], a[3], a[4], b[3], b[4]))


class BrightnessSourceTests(unittest.TestCase):
    """kPanelSources — which dataref dims which face."""

    def setUp(self):
        start = PLUGIN_CPP.index("static const PanelSource kPanelSources[] = {")
        end = PLUGIN_CPP.index("\n};", start)
        body = PLUGIN_CPP[start:end]
        self.rows = re.findall(
            r'\{\s*"([^"]+)"\s*,\s*("(?:[^"]*)"|k\w+)\s*,\s*(-?\d+)\s*,'
            r'\s*[-\d.f]+\s*,\s*[-\d.f]+\s*,\s*("(?:[^"]*)"|k\w+)\s*,\s*(-?\d+)',
            body)
        self.names = [r[0] for r in self.rows]

    def test_the_table_is_parsed_at_all(self):
        self.assertGreaterEqual(len(self.rows), 20)

    def test_every_source_names_a_real_rect(self):
        # A typo here is not an error, it is a face that never dims. The C++ side
        # logs it, but only in a dev build with the tab open.
        rects = {r[0] for r in _table("kPanelRectsHi")}
        for name in self.names:
            self.assertIn(name, rects,
                          "kPanelSources names %r, which is not a rect" % name)

    def test_no_rect_is_listed_twice(self):
        self.assertEqual(sorted(self.names), sorted(set(self.names)))

    def test_the_display_unit_indices_match_the_screen_glow_map(self):
        # ⚠ kScreenDU is the one entry in kPanelSources that is SETTLED in-sim
        # (2026-07-29): AirbusFBW/DUBrightness is in Airbus DU numbering, not
        # left-to-right. Two tables holding the same six numbers is exactly the
        # "three places must agree" shape this project keeps tripping over.
        m = re.search(r"kScreenDU\[kScreenCount\]\s*=\s*\{([^}]*)\}", PLUGIN_CPP)
        self.assertIsNotNone(m)
        screen_du = [int(x) for x in re.findall(r"\d+", m.group(1))]
        du_rows = [(n, int(idx)) for n, bright, idx, _p, _pi in self.rows
                   if "DUBrightness" in bright]
        self.assertEqual(len(du_rows), 6, "expected the six display units")
        # kScreenDU is in the screen-glow's own slot order, which is the same
        # CA PFD / CA ND / E-WD / SD / FO ND / FO PFD order the rect table uses.
        self.assertEqual([idx for _n, idx in du_rows], screen_du)


class FcuRowTests(unittest.TestCase):
    """The one group the tint code names by string."""

    def test_the_fcu_row_group_exists_under_the_name_the_code_uses(self):
        m = re.search(r'kFcuRowName\s*=\s*"([^"]+)"', PLUGIN_CPP)
        self.assertIsNotNone(m, "kFcuRowName missing from plugin.cpp")
        self.assertIn(m.group(1), dict(_groups()),
                      "kFcuRowName names a group that is not in kPanelGroups — "
                      "FcuRowTarget() would return -1 and the preset would no-op")


if __name__ == "__main__":
    unittest.main()
