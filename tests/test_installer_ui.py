"""Rules that hold across the GUI installer's .slint files and gui.cpp.

These are the Slint counterpart to test_ui.py's Dear ImGui rules, and they are
here for the same reason: each one fails SILENTLY. Slint's compiler is happy
with all of them, nothing is logged, and what you get is a window that looks
right in a screenshot and is broken to use — dead to the mouse, or with the Tab
key trapped on one control, or with a focus ring that never appears.

None of these can be caught by building. Two of the three were live bugs during
the focus work (2026-08-12) and are one careless line away at all times.
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
UI = REPO / "src" / "native" / "ui"

SLINT_FILES = sorted(UI.rglob("*.slint"))


def strip_comments(text):
    """Slint's `//` and `/* */`, so a hex code in prose is not a violation."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def blocks(text, opener):
    """Every brace-balanced block introduced by `opener`, as source strings."""
    out = []
    for match in re.finditer(re.escape(opener), text):
        start = text.find("{", match.end() - 1)
        if start < 0:
            continue
        depth, i = 0, start
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out.append(text[start:i + 1])
    return out


class FocusScopeTests(unittest.TestCase):
    """The trap that makes the whole UI unclickable.

    `FocusScope::input_event` returns EventAccepted for a press when `enabled`
    and `focus-on-click` are both true — it SWALLOWS the click rather than
    merely taking focus. A FocusScope defaults to its parent's geometry, so a
    plainly-written one covers the control it was meant to serve and every
    TouchArea under it stops receiving anything. Slint's own widgets pin theirs
    to `width: 0` for exactly this reason.

    Two ways out, and the test accepts either: make it unclickable (`width: 0`)
    or make its click harmless (`focus-on-click: false`). The second is what the
    App's window-sized fallback scope uses.
    """

    def test_every_focus_scope_is_zero_width_or_declines_clicks(self):
        offenders = []
        for path in SLINT_FILES:
            text = strip_comments(path.read_text(encoding="utf-8"))
            for block in blocks(text, "FocusScope"):
                zero_width = re.search(r"\bwidth\s*:\s*0(px)?\s*;", block)
                declines = re.search(
                    r"focus-on-click\s*:\s*false\s*;", block)
                if not (zero_width or declines):
                    offenders.append(
                        f"{path.relative_to(REPO)}: a FocusScope is neither "
                        f"`width: 0` nor `focus-on-click: false`, so it will "
                        f"swallow every click over its area")
        self.assertEqual([], offenders, "\n".join(offenders))

    def test_every_key_handler_rejects_what_it_does_not_handle(self):
        """Or Tab stops moving focus.

        Slint runs its Tab/Backtab traversal in the window's key handler only
        AFTER the focused item and its parents have all rejected the event. A
        `key-pressed` that falls off the end without `return reject` therefore
        traps focus on whatever control it belongs to, permanently, with no
        other symptom.
        """
        offenders = []
        for path in SLINT_FILES:
            text = strip_comments(path.read_text(encoding="utf-8"))
            for block in blocks(text, "key-pressed"):
                if "return reject" not in block:
                    offenders.append(
                        f"{path.relative_to(REPO)}: a key-pressed handler "
                        f"never returns `reject`, so Tab cannot leave it")
        self.assertEqual([], offenders, "\n".join(offenders))

    def test_something_actually_handles_the_keyboard(self):
        """A guard on the guards: the two rules above pass vacuously on a UI
        with no FocusScope at all, which is exactly what this file existed to
        stop happening again."""
        total = sum(len(blocks(strip_comments(p.read_text(encoding="utf-8")),
                               "FocusScope"))
                    for p in SLINT_FILES)
        self.assertGreaterEqual(total, 4, "the installer lost its key handling")


class SizedItemTests(unittest.TestCase):
    """A sized child with no stated position is CENTERED, not placed at 0.

    In a non-layout parent Slint centers any child that has an explicit size and
    no explicit x/y. On a `Rectangle` that is at least visible; on a `TouchArea`
    it is invisible and total — the control it was meant to cover goes dead to
    the mouse and a band of empty space somewhere else goes live. It cost a build
    cycle on `SmallRadio` (2026-08-13), because "sized but unplaced" reads as the
    most ordinary line in the file.

    A TouchArea that states no size at all fills its parent and needs nothing,
    which is why most of them here say neither.
    """

    FULL = ("parent.width", "parent.height", "100%")

    def test_a_sized_touch_area_states_where_it_sits(self):
        offenders = []
        for path in SLINT_FILES:
            text = strip_comments(path.read_text(encoding="utf-8"))
            for block in blocks(text, "TouchArea"):
                for extent, pos in (("width", "x"), ("height", "y")):
                    match = re.search(rf"(?<![\w-]){extent}\s*:\s*([^;]+);", block)
                    if match is None or match.group(1).strip() in self.FULL:
                        continue          # unsized, or exactly filling: centering is a no-op
                    if re.search(rf"(?<![\w-]){pos}\s*:", block) is None:
                        offenders.append(
                            f"{path.relative_to(REPO)}: a TouchArea sets "
                            f"{extent} ({match.group(1).strip()}) but no {pos} — "
                            f"Slint will CENTER it and the control goes dead")
        self.assertEqual([], offenders, "\n".join(offenders))


class DeadSpaceTests(unittest.TestCase):
    """Clicking the background dismisses the focus ring.

    ⚠ The TouchArea that does it must be declared BEFORE the layout. Slint
    hit-tests the last child first, so a window-sized TouchArea placed after the
    content sits on top of the entire UI and swallows every click — the same
    failure as a full-size FocusScope, reached a different way, and just as
    invisible: the window still draws perfectly and simply stops responding.
    """

    def test_the_background_touch_area_is_declared_before_the_content(self):
        text = strip_comments((UI / "app.slint").read_text(encoding="utf-8"))
        touch = text.find("TouchArea")
        layout = text.find("VerticalLayout")
        self.assertNotEqual(-1, touch, "app.slint lost its dead-space TouchArea")
        self.assertLess(
            touch, layout,
            "the dead-space TouchArea is declared after the content, so it now "
            "covers the whole window and eats every click")

    def test_dead_space_returns_focus_rather_than_dropping_it(self):
        """`clear-focus()` would hide the ring and take the Enter key with it."""
        text = strip_comments((UI / "app.slint").read_text(encoding="utf-8"))
        self.assertIn("rootKeys.focus()", text)
        self.assertNotIn(
            "clear-focus()", text,
            "dead space drops focus entirely — Enter stops working until Tab")


class BackButtonTests(unittest.TestCase):
    """Complete has a Back button so the log stays readable in the program; Run
    must not, and that asymmetry is the safety property.

    `GoBack` is a plain `step - 1`, so Complete steps to Run. Run having no Back
    of its own is what closes the loop: the only way out of the pair is forward,
    and no finished install can be rewound into Review and started a second time.
    Dropping `!running` from the condition opens that path AND lets a run in
    progress be abandoned half-written.
    """

    def test_back_is_refused_while_a_run_is_in_progress(self):
        text = strip_comments((UI / "app.slint").read_text(encoding="utf-8"))
        match = re.search(r"can-go-back\s*:\s*([^;]+);", text)
        self.assertIsNotNone(match, "app.slint lost its can-go-back condition")
        self.assertIn(
            "!root.running", match.group(1),
            "Back is now reachable during a run — it can abandon a half-written "
            "aircraft, and Complete's Back is no longer a closed loop")

    def test_complete_can_reach_the_log_again(self):
        """The whole point of the button. `done` in the condition would send
        Complete back to having no Back at all."""
        text = strip_comments((UI / "app.slint").read_text(encoding="utf-8"))
        match = re.search(r"can-go-back\s*:\s*([^;]+);", text)
        self.assertNotIn(
            "done", match.group(1),
            "Complete lost its Back button — the installer log is no longer "
            "readable inside the program once a run finishes")


class ReviewRowTests(unittest.TestCase):
    """The Review list must not offer a row the Features screen hid.

    ⚠ `gui.cpp` HAS NO C++ TESTS — it is in the photon-installer target, not
    photoncore — so this source-level scan is the only automated guard on it. The
    same shape as test_ui.py scanning plugin.cpp, and for the same reason: the
    failure is silent. On an airframe with no cockpit mod the A330 is the case
    today) "Cockpit lighting: No" reports a choice the user was never offered and
    could not have made, and it reads as a real answer rather than as a bug.
    """

    # (row label, the predicate that must guard it). Both are the SAME predicate
    # the Features screen hides its control with — `interior_available` and
    # `wings_available` — and they have to be, or the two screens disagree about
    # whether the feature exists at all.
    GATED_ROWS = (
        ("Cockpit lighting", "AirframeHasInterior"),
        ("Wing mod", "WingsFor"),
    )

    def test_optional_rows_are_gated_on_the_features_screen_predicate(self):
        text = (REPO / "src" / "native" / "src" / "installer"
                / "gui.cpp").read_text(encoding="utf-8")
        start = text.find("void BuildReview")
        self.assertNotEqual(-1, start, "BuildReview is gone")
        body = text[start:text.find("\n}", start)]

        for label, predicate in self.GATED_ROWS:
            row = body.find(f'add("{label}"')
            self.assertNotEqual(-1, row, f"the {label} row is gone entirely")
            guard = body.find(predicate)
            self.assertNotEqual(
                -1, guard,
                f"the {label} row is no longer gated — an airframe that cannot "
                f"use it now reports it as a declined option")
            self.assertLess(
                guard, row, f"{predicate} no longer guards the {label} row")


class StdWidgetsTests(unittest.TestCase):
    """Nothing imports std-widgets any more, and nothing should again.

    `ScrollView` was the last one and became `PhotonScrollView` on 2026-08-13.
    Re-importing it looks harmless and brings back two things at once: fluent's
    dark rounded hover track, which belongs to a different design language than
    any panel it lands in, and — worse because it is invisible on the machine
    that writes the code — a dependence on the USER'S OS THEME. Both fluent
    colors involved are written `dark-color-scheme ? <dark> : <light>` and are
    literals inside the style, unreachable through the public `Palette`. Nothing
    else in this UI reacts to the system theme, so that is one control
    disagreeing with the whole window on half of all machines.
    """

    def test_no_slint_file_imports_std_widgets(self):
        offenders = []
        for path in SLINT_FILES:
            text = strip_comments(path.read_text(encoding="utf-8"))
            if "std-widgets" in text:
                offenders.append(
                    f"{path.relative_to(REPO)}: imports std-widgets — its "
                    f"theming is not reachable from tokens.slint and follows "
                    f"the user's OS light/dark setting")
        self.assertEqual([], offenders, "\n".join(offenders))

    def test_the_scroll_track_has_no_color_to_paint(self):
        """The user-visible half of the same rule: a track color would have to be
        invented here before it could be painted, which is the moment to stop.

        ⚠ Matched on COLOR AND BRUSH DECLARATIONS ONLY, not on the token names as
        text. A plain substring test also caught `scroll-track-inset`, which is a
        length — the track's geometry is fine and used; it is only a fill that
        must not come back.
        """
        text = strip_comments((UI / "tokens.slint").read_text(encoding="utf-8"))
        self.assertIn("scroll-thumb-active", text)
        painted = [name
                   for name in re.findall(
                       r"out property <(?:color|brush)>\s*([\w-]+)", text)
                   if "track" in name]
        self.assertEqual(
            [], painted,
            f"a scrollbar track fill exists again ({painted}) — the transparent "
            f"track is the whole reason PhotonScrollView was written")


class PaletteTests(unittest.TestCase):
    """tokens.slint's own header states the rule; nothing enforced it until now.

    The mockup is still moving, so a hex written into a widget is a retune that
    gets missed — it keeps rendering the old palette and no grep for the token
    finds it.
    """

    def test_no_slint_file_but_tokens_names_a_literal_color(self):
        offenders = []
        for path in SLINT_FILES:
            if path.name == "tokens.slint":
                continue
            text = strip_comments(path.read_text(encoding="utf-8"))
            for line_no, line in enumerate(text.splitlines(), 1):
                if re.search(r"#[0-9a-fA-F]{3,8}\b", line):
                    offenders.append(
                        f"{path.relative_to(REPO)}:{line_no}: literal color — "
                        f"put it in tokens.slint ({line.strip()})")
        self.assertEqual([], offenders, "\n".join(offenders))


class FocusLookTests(unittest.TestCase):
    """The 2 px white keyline means focus, and only focus (2026-08-12).

    It used to be Primary's permanent identity. The design removed it from every
    resting state and made it the Focus variant of all three styles, so putting
    `primary` back into this binding does not merely restore a decoration — it
    makes the focus ring invisible on the one button every screen leads with.
    """

    def test_the_button_keyline_is_bound_to_focus_and_nothing_else(self):
        text = strip_comments((UI / "widgets.slint").read_text(encoding="utf-8"))
        match = re.search(r"border-width\s*:\s*root\.focused[^;]*;", text)
        self.assertIsNotNone(
            match, "PhotonButton's border-width is no longer driven by focus")
        self.assertNotIn(
            "primary", match.group(0),
            "the focus keyline is style-dependent again — Primary's focus state "
            "is now invisible")


class CheckboxStateTests(unittest.TestCase):
    """The Hover and Down variants of the two tick controls (2026-08-13).

    They fail the same silent way: the box keeps drawing its Normal fill, which
    is a perfectly good-looking checkbox, so nothing about the window says the
    two states are unreachable. Only a side-by-side with the design does.
    """

    def setUp(self):
        self.text = strip_comments(
            (UI / "widgets.slint").read_text(encoding="utf-8"))

    def test_the_row_reports_its_pointer_state_to_the_box(self):
        """`OptionRow`'s TouchArea spans all 442 px and sits ABOVE the 20 px box,
        so the box's own TouchArea sees neither hover nor press. Drop these two
        bindings and the design's Hover and Down variants become unreachable
        everywhere except over the glyph itself."""
        row = blocks(self.text, "export component OptionRow")
        self.assertEqual(1, len(row), "OptionRow moved or was renamed")
        box = blocks(row[0], "LargeCheckbox")
        self.assertEqual(1, len(box), "OptionRow no longer holds one checkbox")
        for prop in ("hovered", "down"):
            match = re.search(rf"\b{prop}\s*:\s*([^;]+);", box[0])
            self.assertIsNotNone(
                match, f"OptionRow stopped passing `{prop}` to its checkbox — "
                       f"the row's pointer state no longer reaches the box")
            self.assertIn(
                "touch.", match.group(1),
                f"`{prop}` is no longer bound to the ROW's TouchArea, so it "
                f"reports the 20 px box instead of the 442 px control")

    def test_pressed_beats_hovered_on_both_controls(self):
        """A press keeps the pointer inside the control, so `has-hover` stays
        true for its whole duration. Without the exclusion the hover fill and
        the pressed inset are both on at once and Down never renders — while
        the control still looks entirely reasonable."""
        for name in ("LargeCheckbox", "SmallRadio"):
            with self.subTest(component=name):
                box = blocks(self.text, f"export component {name}")
                self.assertEqual(1, len(box), f"{name} moved or was renamed")
                hot = re.search(r"property\s*<bool>\s*hot\s*:\s*([^;]+);", box[0])
                self.assertIsNotNone(hot, f"{name} lost its `hot` property")
                self.assertIn(
                    "!root.pressed", hot.group(1),
                    f"{name}'s `hot` no longer excludes a press, so its Down "
                    f"variant is dead")

    def test_the_checkbox_press_reaches_it_from_the_row(self):
        """`LargeCheckbox` alone takes its press from the caller, because
        `OptionRow` owns the click. `SmallRadio` is its own row and does not."""
        box = blocks(self.text, "export component LargeCheckbox")
        pressed = re.search(
            r"property\s*<bool>\s*pressed\s*:\s*([^;]+);", box[0])
        self.assertIsNotNone(pressed, "LargeCheckbox lost its `pressed` property")
        self.assertIn(
            "root.down", pressed.group(1),
            "`pressed` ignores the caller's `down`, so a row press no longer "
            "reaches the box")

    def test_the_radio_ring_and_hit_zone_share_one_extent(self):
        """`SmallRadio` stretches to the whole content column but only its box
        and label are the control. The hit zone was narrowed to match when the
        hover state arrived; a focus ring still drawn to `parent.width` would
        then outline 300 px of dead space the pointer cannot use."""
        box = blocks(self.text, "export component SmallRadio")
        self.assertEqual(1, len(box), "SmallRadio moved or was renamed")
        self.assertIn("property <length> hot-w", box[0],
                      "SmallRadio lost its `hot-w` extent")
        touch = blocks(box[0], "touch := TouchArea")
        self.assertEqual(1, len(touch), "SmallRadio's TouchArea lost its name")
        self.assertIn(
            "width: root.hot-w;", touch[0],
            "the hit zone is no longer bound to `hot-w`, so it can disagree "
            "with the focus ring and with what lights up on hover")
        ring = blocks(box[0], "FocusRing")
        self.assertEqual(1, len(ring), "SmallRadio's focus ring moved")
        self.assertIn(
            "root.hot-w", ring[0],
            "the focus ring is no longer bound to `hot-w`, so it outlines more "
            "than the pointer can hit")


class AircraftListTests(unittest.TestCase):
    """`reveal()` does arithmetic with the row pitch, and the layout lays rows
    out with it. Written twice they drift, and the scroll lands a few pixels off
    per row — which reads as the list being sloppy rather than as two numbers
    disagreeing."""

    def test_the_scroll_math_and_the_layout_share_their_metrics(self):
        text = strip_comments(
            (UI / "screens" / "aircraft.slint").read_text(encoding="utf-8"))
        for prop, use in (("row-spacing", "spacing: root.row-spacing;"),
                          ("well-pad", "padding-top: root.well-pad;")):
            self.assertIn(f"property <length> {prop}", text)
            self.assertIn(use, text,
                          f"the card layout stopped using root.{prop}, so "
                          f"reveal()'s arithmetic can drift from it")


if __name__ == "__main__":
    unittest.main()
