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

PANELFX = REPO / "src" / "native" / "panelfx.txt"

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
        # overhead readouts took it to 36, and `1u << 35` is undefined behavior
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

    def test_the_transponder_has_no_power_source(self):
        """⚠ AirbusFBW/XPDRPower is the WRONG QUANTITY and must not come back.

        It reports whether the transponder is interrogating — the STBY/AUTO/ON
        selector — not whether the unit is alive. The code window stays lit and
        perfectly readable in STBY, so gating the tint on it blanked a screen the
        pilot can see (reported in-sim 2026-08-02). The bus gate covers this face
        instead, and that one really does answer "is it on".

        This is the recurring shape in this table: a dataref that resolves
        cleanly, reads plausibly, and reports the fact NEXT DOOR."""
        power = {n: p for n, _b, _bi, p, _pi in self.rows}
        self.assertIn("XPDR code", power)
        self.assertEqual('""', power["XPDR code"],
                         "the transponder must have no power source")
        # ⚠ The ROWS, not the file: the comment above that row names the dataref
        # in order to explain why it is gone, so a substring search over the
        # table's text fails on its own documentation.
        self.assertNotIn("XPDRPower", {p.strip('"') for p in power.values()},
                         "XPDRPower is back on some face - it reports "
                         "interrogation, not power")

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


class ShippedTargetNameTests(unittest.TestCase):
    """Every `target` in panelfx.txt still names a rect or a group.

    ⚠ A rect or group NAME is wire identity — panelfx.txt addresses targets by
    name, so renaming one is a breaking change to the shipped look. A stack whose
    target no longer resolves is DROPPED at load with one line in Log.txt, so the
    symptom in the cockpit is one readout that simply has no tint on it, which is
    indistinguishable from a layer that was never authored. This is the check
    that turns that into a failing test instead — it caught nothing when written
    (2026-08-02, alongside the ovhd/Center-readouts renames) because the file was
    updated in the same change, which is exactly when it is cheapest to add.
    """

    def test_every_target_resolves(self):
        known = {r[0] for r in _table("kPanelRectsHi")}
        known |= {name for name, _members in _groups()}
        seen = 0
        for line in PANELFX.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line.startswith("target "):
                continue
            seen += 1
            name = line[len("target "):].strip()
            self.assertIn(name, known,
                          "panelfx.txt targets %r, which is neither a rect nor a "
                          "group - the stack will be dropped at load" % name)
        self.assertTrue(seen, "no targets found in panelfx.txt - parser drift?")


class ScreenFxRectTests(unittest.TestCase):
    """Which rects the Displays tab's "backlight visual effect" switch owns.

    A rect on the wrong switch is invisible as a bug: the effect still draws, it
    just answers to the other checkbox, so the report is "the switch only half
    works" and there is nothing in a log to look at.
    """

    def setUp(self):
        start = PLUGIN_CPP.index("static const char* const kScreenFxExtraRects[]")
        end = PLUGIN_CPP.index("};", start)
        self.extras = re.findall(r'"([^"]+)"', PLUGIN_CPP[start:end])
        self.rects = _table("kPanelRectsHi")

    def test_the_mcdus_dcdus_and_isis_are_on_the_screen_switch(self):
        # The ISIS was on the other-displays switch until 2026-08-02. It is a
        # backlit screen by every test except carrying a DUBrightness index —
        # which it does not carry because ToLiss gives it its own ISIBrightness.
        self.assertEqual(sorted(self.extras),
                         ["DCDU FO", "DCDU capt", "ISIS", "MCDU FO", "MCDU capt"])

    def test_every_extra_names_a_real_rect(self):
        # Resolved by name at runtime; a miss logs and falls through to the
        # other-displays switch, which is the failure above.
        names = {r[0] for r in self.rects}
        for name in self.extras:
            self.assertIn(name, names,
                          "kScreenFxExtraRects names %r, which is not a rect" % name)

    def test_no_extra_is_already_a_display_unit(self):
        # A DU is on the screen switch by virtue of its du index. Listing one
        # here too would be dead weight that reads as if it were load-bearing.
        du_names = {r[0] for r in self.rects if int(r[5]) >= 0}
        self.assertEqual(set(self.extras) & du_names, set())

    def test_the_gate_goes_through_the_helper_and_not_the_du_field(self):
        # FxDisplayUserFactor used to test `.du >= 0` inline. Leaving that in
        # place would silently drop the extras.
        start = PLUGIN_CPP.index("static float FxDisplayUserFactor(int rectIndex)")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
        self.assertIn("PanelRectIsScreen(rectIndex)", body)
        self.assertNotIn(".du >= 0", body)


class ElectricalGateTests(unittest.TestCase):
    """The DC-bus and circuit-breaker gates.

    Both fail silently and in opposite directions: a gate that never fires
    paints a tint over a dead screen (the artifact the feature exists to remove),
    and a gate that always fires blanks a readout with nothing in the log. The
    "unreadable means LIVE" rule is what keeps the second one from happening to
    an aircraft that simply lacks the dataref.
    """

    def setUp(self):
        start = PLUGIN_CPP.index("static const PanelBus kPanelBuses[] = {")
        end = PLUGIN_CPP.index("};", start)
        self.buses = re.findall(r'\{\s*"([^"]+)"', PLUGIN_CPP[start:end])
        self.rects = _table("kPanelRectsHi")

    def test_the_bus_gate_covers_every_driven_display(self):
        # The rule is "does this face go DARK with the bus", not "is it a
        # screen": a driven readout blanks, an integral-lit legend keeps its
        # tint because the tint is then a property of the lighting. The five
        # beyond the DUs and DCDUs were added 2026-08-02.
        du_names = [r[0] for r in self.rects if int(r[5]) >= 0]
        self.assertEqual(
            sorted(self.buses),
            sorted(du_names + ["DCDU capt", "DCDU FO", "XPDR code",
                               "rudder trim", "FMS Load",
                               "ovhd aft L1", "ovhd aft L2"]))

    def test_no_face_carries_a_bus_of_its_own(self):
        # ⚠ One bus, one number. A second index here would be a per-face
        # electrical model nothing has confirmed — if a partial failure really
        # ought to kill only some screens, that is a deliberate change to
        # kPanelBuses with in-sim evidence behind it, not a drifting default.
        start = PLUGIN_CPP.index("static const PanelBus kPanelBuses[] = {")
        end = PLUGIN_CPP.index("};", start)
        for _rect, idx in re.findall(r'\{\s*"([^"]+)"\s*,\s*([^\s}]+)',
                                     PLUGIN_CPP[start:end]):
            self.assertEqual(idx, "kDcBusIndex")

    def test_every_bus_row_names_a_real_rect(self):
        names = {r[0] for r in self.rects}
        for name in self.buses:
            self.assertIn(name, names)

    def test_the_bus_threshold_is_the_one_the_spill_lights_use(self):
        # ⚠ Two consumers, one constant. The tint on the glass and the spill off
        # it must die together; a screen that stops glowing while its tint stays
        # painted (or the reverse) is worse than neither being gated.
        self.assertRegex(PLUGIN_CPP, r"kDcBusLiveVolts\s*=\s*24\.0f")
        self.assertIn("gRectBus[i].floorF = kDcBusLiveVolts;", PLUGIN_CPP)
        start = PLUGIN_CPP.index("const bool busLive = DcBusLive();")
        end = PLUGIN_CPP.index("\n    }", start)
        self.assertIn("busLive &&", PLUGIN_CPP[start:end],
                      "the spill loop must gate on the bus")

    def test_the_bus_handle_is_retried_on_the_1_hz_loop(self):
        """⚠ THIS WAS THE BUG. ToLiss's datarefs appear when ITS plugin starts,
        which can be after our aircraft-loaded hook runs. ResolveDcBus was called
        only from that hook, so on any start where ToLiss lost the race the handle
        stayed null, DcBusLive() answered "unknown, therefore live" for the whole
        session, and the gate never fired once — with nothing in the log.

        Every cached ToLiss handle belongs in AutoLoop's retry list."""
        start = PLUGIN_CPP.index("static float AutoLoop(")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
        for handle, resolver in (("gMapRheostat", "ResolveMapRheostat"),
                                 ("gDUBrightness", "ResolveDUBrightness"),
                                 ("gDcBusRef", "ResolveDcBus"),
                                 ("gDisplayFallbackRef", "ResolveDisplayFallback")):
            self.assertRegex(body, r"if\s*\(!%s\)\s*%s\(\);" % (handle, resolver),
                             "%s is not retried in AutoLoop" % handle)

    def test_the_du_rect_set_handle_is_rebound_with_the_aircraft(self):
        """⚠ THIS WAS THE BUG, a THIRD time, and it hid better than the first two.

        AirbusFBW/DisplayResolutionFallback picks which of the two DU rect sets
        the compositor paints. It was resolved once in StartPanelPass — that is
        XPluginEnable — and never again: not dropped when an aircraft went away,
        not rebound when one arrived, not retried. So after an aircraft reload
        the handle pointed into a ToLiss plugin that had been UNLOADED, and
        reading a destroyed dataref is undefined rather than merely stale.

        What makes it hard to recognise is the blast radius. ONLY the six DUs are
        resolution-switched; every other rect is byte-identical in the two
        tables. So the entire rest of the cockpit keeps tinting correctly and
        just the displays go wrong — reported in-sim as "the FX works on the
        MCDUs and DCDUs but not the PFDs", which reads like a rect-table problem
        and not like a dangling handle.

        Null is the SAFE value here (PanelRects falls back to full-res), which is
        why dropping it on the way out is free."""
        # ⚠ The DEFINITION, not the forward declaration ten pages earlier —
        # `index` finds the decl first and returns a body full of dataref
        # accessors that of course mentions neither symbol.
        start = PLUGIN_CPP.index("static void UpdateMenuVisibility() {")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}\n", start)]
        # Bound on the way in...
        self.assertIn("ResolveDisplayFallback();", body,
                      "the DU rect-set handle is not rebound on aircraft load")
        # ...and dropped on the way out, with the others.
        self.assertIn("gDisplayFallbackRef = nullptr;", body,
                      "the DU rect-set handle is not dropped when a ToLiss goes "
                      "away - it then points into an unloaded plugin")

    def test_the_bus_is_read_with_the_getter_matching_its_type(self):
        """⚠ XPLMGetDatavf on an INT array returns 0 with no error and no log
        line, so assuming float would leave the count at 0 and the gate silently
        dead — the same failure as the missing retry, by a different road. This
        is the project's "read a dataref with the matching getter" tripwire."""
        for fn in ("static void ResolveDcBus()", "static bool DcBusLive()"):
            start = PLUGIN_CPP.index(fn)
            body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
            self.assertIn("xplmType_FloatArray", body, fn)
            self.assertIn("xplmType_IntArray", body, fn)

    def test_an_unreadable_bus_means_live(self):
        """A missing dataref must not black out the screens — the same rule the
        FX drivers already follow. Every early return in DcBusLive is `true`."""
        start = PLUGIN_CPP.index("static bool DcBusLive()")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
        for line in body.splitlines():
            if "return" in line and "v >" not in line:
                self.assertIn("return true", line,
                              "DcBusLive must fail OPEN: %s" % line.strip())

    def test_the_breaker_table_holds_no_guessed_indices(self):
        """⚠ Nothing names CBArray's 363 elements. A row here is only legitimate
        once it has been confirmed with the Dev > Probe CB watch; until then an
        empty table is the honest state, and a plausible-looking guess is the
        worst outcome because it looks right until that breaker is pulled."""
        start = PLUGIN_CPP.index("static const PanelBreaker kPanelBreakers[] = {")
        end = PLUGIN_CPP.index("};", start)
        rows = re.findall(r'\{\s*("?[^"{,]+"?)\s*,\s*(-?\d+)\s*\}', PLUGIN_CPP[start:end])
        rects = {r[0] for r in self.rects}
        for name, _idx in rows:
            if name.strip() == "nullptr":
                continue
            self.assertIn(name.strip('"'), rects,
                          "kPanelBreakers names %r, which is not a rect" % name)

    def test_the_gates_are_never_a_stack_override(self):
        # `power` is part of the authored look and a stack may override it. Bus
        # and breaker are facts about the aircraft; a look able to opt out of
        # them would eventually be authored by accident.
        start = PLUGIN_CPP.index("static float FxRectFactor(int rectIndex,")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
        self.assertIn("gRectBus[rectIndex]", body)
        self.assertIn("gRectCb[rectIndex]", body)
        for token in ("gFxDrawStack->bus", "gFxDrawStack->cb"):
            self.assertNotIn(token, body)


class DcduBrightnessTests(unittest.TestCase):
    """DCDU brightness is INFERRED from commands, because no level dataref exists.

    Two things here are load-bearing and silent: a handler that returns 0 eats
    the command, so ToLiss never dims and the tint tracks a level the real screen
    never reached; and stepping on any phase but Begin runs the whole range in a
    fraction of a second while the button is held.
    """

    def setUp(self):
        start = PLUGIN_CPP.index("static const DcduCommand kDcduCommands[] = {")
        end = PLUGIN_CPP.index("};", start)
        self.rows = re.findall(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*([+-]?\d+)\s*\}',
                               PLUGIN_CPP[start:end])

    def test_all_four_commands_are_watched(self):
        self.assertEqual(sorted(r[0] for r in self.rows), [
            "AirbusFBW/CPDLC1/BrightnessDown",
            "AirbusFBW/CPDLC1/BrightnessUp",
            "AirbusFBW/CPDLC2/BrightnessDown",
            "AirbusFBW/CPDLC2/BrightnessUp",
        ])

    def test_each_unit_gets_one_up_and_one_down(self):
        by_unit = {}
        for name, unit, step in self.rows:
            by_unit.setdefault(int(unit), []).append(int(step))
        self.assertEqual(sorted(by_unit), [0, 1])
        for unit, steps in by_unit.items():
            self.assertEqual(sorted(steps), [-1, 1],
                             "DCDU %d needs exactly one up and one down" % unit)

    def test_cpdlc1_is_the_captain_and_cpdlc2_the_first_officer(self):
        for name, unit, _step in self.rows:
            self.assertEqual(int(unit), 0 if "CPDLC1" in name else 1)

    def test_the_handler_passes_the_command_on(self):
        start = PLUGIN_CPP.index("static int DcduBrightnessHandler(")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
        self.assertIn("return 1;", body)
        self.assertNotIn("return 0;", body)

    def test_the_handler_steps_on_begin_only(self):
        start = PLUGIN_CPP.index("static int DcduBrightnessHandler(")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
        self.assertIn("phase == xplm_CommandBegin", body)
        self.assertNotIn("xplm_CommandContinue", body)

    def test_the_handler_is_registered_before_the_aircrafts_own(self):
        """⚠ inBefore = 1, and it must stay 1.

        It was 0, on the reasoning that a monitor should stay out of the way.
        But a BEFORE handler that returns 0 stops the command dead and later
        handlers are never called at all — so with inBefore = 0, whether we ever
        hear a press rested on the return value of ToLiss's own handler: a fact
        about someone else's plugin that we cannot see and that any update can
        change. Running first removes the dependency.

        Safe only in company with test_the_handler_passes_the_command_on: first
        AND returning 0 would eat the press, and the tint would then track a
        brightness the real DCDU never reached."""
        # ⚠ Index on the "{" — both of these have a forward declaration above,
        # and matching that gives you the next unrelated block of the file.
        for fn in ("static void RetryDcduCommands() {",
                   "static void UnbindDcduCommands() {"):
            start = PLUGIN_CPP.index(fn)
            body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
            self.assertRegex(
                body,
                r"XPLM(?:Unregister|Register)CommandHandler\([^;]*DcduBrightnessHandler,\s*"
                r"kDcduHandlerBefore,",
                "%s must register/unregister with the shared kDcduHandlerBefore" % fn)
        self.assertRegex(PLUGIN_CPP, r"kDcduHandlerBefore\s*=\s*1;")

    def test_the_commands_are_retried_on_the_1_hz_loop(self):
        """⚠ THIS WAS THE BUG, and it is the bus-gate bug a second time.

        XPLMFindCommand at aircraft-load time found none of the four: ToLiss's
        plugin starts AFTER our hook (Log.txt, our line at 3882 and "ToLiss
        aircraft systems plugin starting up" at 3900). A command is worse than a
        dataref here — no later read can heal it, so the buttons went unwatched
        for the whole session."""
        start = PLUGIN_CPP.index("static float AutoLoop(")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
        self.assertRegex(body, r"gDcduBound\s*<\s*kDcduCommandCount\)\s*RetryDcduCommands\(\);")

    def test_the_retry_cannot_double_register_a_command(self):
        """Called once a second forever, so it must skip slots it already holds.
        Two registrations on one command would count every press twice — which
        looks like the level being over-sensitive, not like a binding bug."""
        start = PLUGIN_CPP.index("static void RetryDcduCommands()")
        body = PLUGIN_CPP[start:PLUGIN_CPP.index("\n}", start)]
        self.assertIn("if (gDcduCmdRefs[i]) continue;", body)

    def test_binding_resets_the_level_but_retrying_does_not(self):
        """The default is only as good as a guess gets, and a load is the one
        moment it is that good. Resetting on the 1 Hz retry instead would snap a
        level the user had just set back to 8 a second later."""
        bind = PLUGIN_CPP.index("static void BindDcduCommands()")
        bind_body = PLUGIN_CPP[bind:PLUGIN_CPP.index("\n}", bind)]
        self.assertIn("kDcduLevelDefault", bind_body)
        retry = PLUGIN_CPP.index("static void RetryDcduCommands()")
        retry_body = PLUGIN_CPP[retry:PLUGIN_CPP.index("\n}", retry)]
        self.assertNotIn("gDcduLevel", retry_body)

    def test_the_source_row_reuses_the_level_constants(self):
        """⚠ kDcduLevelMax is an ASSUMPTION awaiting an in-sim check. A literal
        10 in kPanelSources would be a second place to remember when it moves,
        and the symptom of forgetting is a tint that saturates early."""
        table = PLUGIN_CPP.index("static const PanelSource kPanelSources[] = {")
        start = PLUGIN_CPP.index('{ "DCDU capt",', table)
        row = PLUGIN_CPP[start:PLUGIN_CPP.index("},", start)]
        self.assertIn("kDcduBrightnessRef", row)
        self.assertIn("kDcduLevelMax", row)

    def test_the_default_sits_inside_the_range(self):
        def const(name):
            return int(re.search(r"%s\s*=\s*(-?\d+)" % name, PLUGIN_CPP).group(1))
        lo, hi, dflt = const("kDcduLevelMin"), const("kDcduLevelMax"), const("kDcduLevelDefault")
        self.assertLess(lo, hi)
        self.assertTrue(lo <= dflt <= hi)


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
