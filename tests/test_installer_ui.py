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
    """Run must have no Back WHILE IT IS RUNNING, and that is the safety property.

    Dropping `!running` lets a run in progress be abandoned half-written, which is
    the one movement in this wizard that cannot be undone by moving again.

    ⚠ COMPLETE LOST ITS BACK ON 2026-08-14 — to a RELABEL, not to a removal. Back
    was added there (2026-08-12) because the installer log vanished when a run
    finished and stepping back to Run was the only way to read it again. **View
    log** now occupies that slot and makes exactly that move, by the same plain
    `step` assignment; what changed is that the label says what the movement is for
    on a screen where "Back" begged the question of back to what.

    ⚠ IT BRIEFLY OPENED THE LOG FILE INSTEAD, and that is the version this class
    tests is gone — see `test_view_log_goes_back_to_the_run_screen`.
    """

    def setUp(self):
        self.cpp = (REPO / "src" / "native" / "src" / "installer"
                    / "gui.cpp").read_text(encoding="utf-8")

    def test_back_is_refused_while_a_run_is_in_progress(self):
        text = strip_comments((UI / "app.slint").read_text(encoding="utf-8"))
        match = re.search(r"can-go-back\s*:\s*([^;]+);", text)
        self.assertIsNotNone(match, "app.slint lost its can-go-back condition")
        self.assertIn(
            "!root.running", match.group(1),
            "Back is now reachable during a run — it can abandon a half-written "
            "aircraft")

    def test_complete_swaps_back_for_view_log(self):
        """⚠ NEVER BOTH, AND NEVER NEITHER. Both would be three buttons in a bar
        the design gives two; neither would strand the log behind a screen the
        user cannot get back to."""
        text = strip_comments((UI / "app.slint").read_text(encoding="utf-8"))
        back = re.search(r"back-visible\s*:\s*([^;]+);", text)
        log = re.search(r"view-log-visible\s*:\s*([^;]+);", text)
        self.assertIsNotNone(back, "app.slint lost its back-visible condition")
        self.assertIsNotNone(log, "Complete lost its View log button")
        self.assertIn(
            "!root.done", back.group(1),
            "Back is visible on Complete again — it now shares the slot with "
            "View log, so the two would overlap or push the bar wide")
        self.assertIn(
            "root.done", log.group(1),
            "View log is no longer scoped to Complete")

    def test_view_log_goes_back_to_the_run_screen(self):
        """⚠ IT IS NAVIGATION, NOT A FILE (2026-08-14, the same day the button
        landed opening one).

        The Run screen is still holding that log — colored, with its markers and
        its red lines — so handing the user the OS text viewer instead swaps a
        designed screen for a wall of timestamps. The FILE is still written and
        still flushed per line; it is the record for afterwards, which is why the
        `FileLog` is untouched and only the button's target moved.

        ⚠ THE ROUND TRIP IS WHAT MAKES THE Run SCREEN'S AUTO-ADVANCE ACCEPTABLE
        (`AutoAdvanceTests`): the wizard skips past that screen on the user's
        behalf, so there has to be a way back to it that is not a re-run.
        """
        start = self.cpp.find("on_view_log")
        self.assertNotEqual(-1, start, "the View log button lost its handler")
        body = self.cpp[start:self.cpp.find("});", start)]
        self.assertIn(
            "set_step(kRun)", body,
            "View log no longer returns to the Run screen")
        self.assertNotIn(
            "OpenInEditor", body,
            "View log opens the log FILE again — the Run screen is already "
            "showing that log, and better than a text editor can")
        self.assertIn(
            "kComplete", body,
            "the handler is unscoped: the callback is reachable from any screen "
            "the bar is on, and a stray one would jump the user into a run "
            "screen for a run that never happened")

    def test_the_log_file_is_still_written(self):
        """⚠ THE BUTTON STOPPED OPENING IT; NOTHING STOPPED WRITING IT. It is the
        record for afterwards — the only artifact that outlives the process, and
        the only one carrying levels and timestamps at all."""
        self.assertIn(
            "std::make_unique<FileLog>", self.cpp,
            "the GUI no longer writes an installer log file")
        self.assertIn(
            "ctx.file = g.fileLog.get()", self.cpp,
            "the run no longer logs to the file — the UI list would be the only "
            "copy, and it dies with the window")


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


class ReviewLayoutTests(unittest.TestCase):
    """⚠ THE REVIEW PAGE HAS NO VERTICAL SLACK, AND OVERFLOWING IT DOES NOT CLIP —
    IT PUSHES THE BOTTOM BAR OFF THE WINDOW.

    The screen is the tallest in the installer and the design fills the content
    area to the pixel: 24 + 40 + 4x(10 + 40) + 10 + 24 + 94 + 24 = 416, which is
    480 minus the bar. The column is a VerticalLayout inside another one that also
    holds the bar, so ten pixels too many do not overflow a scroll region or get
    cut off — the bar, which is a fixed piece of the frame on every other screen,
    slides down out of the window.

    ⚠ THAT IS EXACTLY WHAT HAPPENED (2026-08-14) and it did not read as an overflow.
    The Review rows grew from 18 px to 22 in the restyle, and the "Selected Options"
    head was a sibling of the attribute list in the `content-gap` column — so the
    head and its own list were pushed 10 px apart, and the report was "too much
    padding under the Features heading", which is where the ten pixels were sitting.
    The design has them as ONE frame (`212:1642`) with the list flush at y = 24.
    """

    def token(self, name):
        text = (UI / "tokens.slint").read_text(encoding="utf-8")
        match = re.search(rf"{name}\s*:\s*(\d+)px\s*;", text)
        self.assertIsNotNone(match, f"tokens.slint lost `{name}`")
        return int(match.group(1))

    def setUp(self):
        self.review = strip_comments(
            (UI / "screens" / "review.slint").read_text(encoding="utf-8"))
        self.widgets = strip_comments(
            (UI / "widgets.slint").read_text(encoding="utf-8"))

    def groups(self, marker):
        """The SMALLEST VerticalLayout containing `marker` — i.e. the innermost,
        since `blocks` returns every nesting level that encloses it."""
        found = [b for b in blocks(self.review, "VerticalLayout") if marker in b]
        self.assertTrue(found, f"review.slint no longer lays out `{marker}`")
        return min(found, key=len)

    def test_the_head_and_its_list_are_one_block(self):
        """⚠ NOT COSMETIC GROUPING. Every other child of the content column is a
        `content-gap` apart, and the head taking that gap too is both wrong against
        the design and the ten pixels that push the bar off the window."""
        both = [b for b in blocks(self.review, "VerticalLayout")
                if "Selected Options" in b and "for attribute in" in b]
        self.assertTrue(
            both, "review.slint no longer lays out the head and its list at all")
        # ⚠ THE SMALLEST ONE, and if the group is ever removed that is the CONTENT
        # COLUMN — which carries `content-gap` and fails the next assertion. That
        # is the regression, caught by the same two lines.
        self.assertIsNotNone(
            re.search(r"spacing\s*:\s*0(px)?\s*;", min(both, key=len)),
            "the Selected Options head and its attribute list are separated by a "
            "spacing again. If it is `content-gap` they are back to being "
            "siblings in the content column — 10 px the page does not have")

    def test_the_page_fits_between_the_top_and_the_bottom_bar(self):
        """The arithmetic, end to end, from the four files that own a term. ⚠ It
        is an EQUALITY in the design and this asserts `<=`, so there is room to
        take something out and none to put anything in."""
        cpp = (REPO / "src" / "native" / "src" / "installer"
               / "gui.cpp").read_text(encoding="utf-8")
        start = cpp.find("void BuildReview")
        self.assertNotEqual(-1, start, "BuildReview is gone")
        # ⚠ THE MOST ROWS THE LIST CAN HOLD, not the usual number: two are gated
        # per airframe (`ReviewRowTests`), and the page has to fit the airframe
        # that shows all four.
        rows = len(re.findall(r"\n\s*add\(", cpp[start:cpp.find("\n}", start)]))
        self.assertEqual(4, rows, "the attribute list changed length")

        head = int(re.search(r"Header1 \{\s*height:\s*(\d+)px",
                             self.review).group(1))
        sub = int(re.search(r"Header2 \{\s*height:\s*(\d+)px",
                            self.review).group(1))
        row_h = int(re.search(
            r"component AttributeRow.*?height:\s*(\d+)px",
            self.review, re.S).group(1))
        row_gap = int(re.search(
            r"spacing:\s*(\d+)px", self.groups("for attribute in")).group(1))
        prop_h = int(re.search(
            r"height:\s*(\d+)px",
            blocks(self.widgets, "export component PropertyRow")[0]).group(1))
        props = len(re.findall(r"\bPropertyRow \{", self.review))

        pad = self.token("content-pad-y")
        gap = self.token("content-gap")
        children = 1 + props + 1          # head, the property rows, the group
        total = (2 * pad + head + props * prop_h
                 + sub + rows * row_h + (rows - 1) * row_gap
                 + (children - 1) * gap)
        available = self.token("window-h") - self.token("bar-h")
        self.assertLessEqual(
            total, available,
            f"the Review page needs {total} px of the {available} px between the "
            f"top of the window and the bottom bar. It does not clip — the bar "
            f"is pushed down out of the window by the difference")


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


class ContextMenuTests(unittest.TestCase):
    """The right-click menus (2026-08-14). Every rule here fails silently, and
    three of them fail by breaking something ELSE than the menu.

    The menu is a `PopupWindow` we draw ourselves rather than Slint's built-in
    `ContextMenuArea`/`Menu`/`MenuItem`, for the reason `StdWidgetsTests` gives:
    those render fluent's menu, whose colors are literals inside the style and
    whose light/dark branch follows the user's OS setting. A floating menu is the
    worst place to accept that — there is nothing beside it to compare against.
    """

    def components(self, path):
        """(name, body) for every component declared in a .slint file."""
        text = strip_comments(path.read_text(encoding="utf-8"))
        out = []
        for match in re.finditer(r"\bcomponent\s+([\w-]+)", text):
            body = blocks(text[match.start():], "component")
            if body:
                out.append((match.group(1), body[0]))
        return out

    def test_a_menu_touch_area_sits_behind_the_text_input_it_serves(self):
        """⚠ THE ORDER IS THE WHOLE CONTRACT, and getting it wrong kills the text
        field rather than the menu.

        Slint hit-tests the LAST child first. A TouchArea declared after a
        TextInput therefore takes every LEFT click too — no caret, no
        drag-selection, no editing — while still looking perfectly normal.
        Declared before it, the TextInput gets first refusal and returns
        EventIgnored for a right press (its `input_event` handles Left and Middle
        and ignores the rest), so exactly one kind of click reaches the menu.

        Same rule as `DeadSpaceTests`, reached from the other direction.
        """
        offenders = []
        for path in SLINT_FILES:
            for name, body in self.components(path):
                inputs = [m.start() for m in re.finditer(r"\bTextInput\b", body)]
                if not inputs:
                    continue
                for match in re.finditer(r"\bTouchArea\b", body):
                    area = blocks(body[match.start():], "TouchArea")
                    if not area or "show-at(" not in area[0]:
                        continue
                    if match.start() > min(inputs):
                        offenders.append(
                            f"{path.relative_to(REPO)}: {name}'s menu TouchArea "
                            f"is declared AFTER its TextInput, so it now "
                            f"swallows every left click and the field is dead")
        self.assertEqual([], offenders, "\n".join(offenders))

    def test_every_pointer_event_handler_names_a_button(self):
        """`pointer-event` fires for EVERY button, unlike `clicked`, which is
        left-only. A handler that opens a menu without testing the button opens
        it on an ordinary click as well — which reads as the menu being broken,
        not as the handler being ungated."""
        offenders = []
        for path in SLINT_FILES:
            text = strip_comments(path.read_text(encoding="utf-8"))
            for block in blocks(text, "pointer-event"):
                if "PointerEventButton." not in block:
                    offenders.append(
                        f"{path.relative_to(REPO)}: a pointer-event handler "
                        f"tests no PointerEventButton, so it runs for every "
                        f"button on the mouse")
        self.assertEqual([], offenders, "\n".join(offenders))

    def test_the_menu_pins_itself_to_its_parents_origin(self):
        """A popup's x/y are measured from the element that holds it, and a sized
        child with no stated position is CENTERED by Slint — the same trap
        `SizedItemTests` guards on TouchAreas. Drop either line and every menu
        opens offset by half its parent, which looks like the CALLER computing
        the wrong coordinates."""
        text = strip_comments((UI / "widgets.slint").read_text(encoding="utf-8"))
        menu = blocks(text, "export component PhotonContextMenu")
        self.assertEqual(1, len(menu), "PhotonContextMenu moved or was renamed")
        for prop in ("x", "y"):
            self.assertIsNotNone(
                re.search(rf"^\s*{prop}\s*:\s*0(px)?\s*;", menu[0], re.M),
                f"PhotonContextMenu no longer states `{prop}: 0`, so Slint "
                f"centers it and every menu opens half a parent away")

    def test_no_slint_file_uses_the_built_in_menu_elements(self):
        offenders = []
        for path in SLINT_FILES:
            text = strip_comments(path.read_text(encoding="utf-8"))
            for element in ("ContextMenuArea", "MenuItem", "MenuBar"):
                if re.search(rf"\b{element}\b", text):
                    offenders.append(
                        f"{path.relative_to(REPO)}: uses {element} — it draws "
                        f"fluent's menu, which is unreachable from tokens.slint "
                        f"and follows the user's OS light/dark setting")
        self.assertEqual([], offenders, "\n".join(offenders))

    def test_every_menu_dispatches_exactly_as_many_rows_as_it_lists(self):
        """⚠ THE ENTRY LIST AND THE INDEX LADDER ARE ONE LIST WRITTEN TWICE.
        `activated` carries a position into a string array the same call site
        wrote, so adding a row without its branch is a menu row that does
        nothing, and removing one silently shifts every action below it onto the
        wrong label.
        """
        offenders = []
        instances = 0
        for path in SLINT_FILES:
            text = strip_comments(path.read_text(encoding="utf-8"))
            # ⚠ Matched on the `:=`, so this sees INSTANCES and not the import
            # line — whose next brace is the whole screen's body, which would
            # merge two menus into one count and hide a mismatch between them.
            # Every instance is named anyway; `show-at` has to be called on one.
            for block in blocks(text, ":= PhotonContextMenu"):
                entries = re.search(r"entries\s*:\s*\[(.*?)\]\s*;", block, re.S)
                if entries is None:
                    continue
                instances += 1
                listed = len(re.findall(r'"', entries.group(1))) // 2
                handled = len(set(re.findall(
                    r"index\s*==\s*(\d+)", block)))
                if listed != handled:
                    offenders.append(
                        f"{path.relative_to(REPO)}: a context menu lists "
                        f"{listed} rows but dispatches {handled} — the labels "
                        f"and the actions have drifted apart")
        self.assertEqual([], offenders, "\n".join(offenders))
        # A guard on the guard: the scan passes vacuously on a UI with no menus,
        # which is exactly the state this test was written to leave behind. Four
        # ship — the path field, the installer log, an aircraft card and the
        # Complete screen's outcome row.
        self.assertGreaterEqual(
            instances, 4, "the installer lost a context menu")

    def test_the_measured_width_and_the_drawn_row_share_one_type(self):
        """⚠ THE MENU MEASURES ITSELF WITH A HIDDEN COPY OF ITS OWN LABELS, so the
        measuring Text and the drawn Text must be the same font, size and weight.
        Written twice they drift, and the menu comes out a few pixels short of its
        own longest label — which shows as an elided row, with nothing anywhere
        saying the two fonts disagree. One component, used by both.
        """
        text = strip_comments((UI / "widgets.slint").read_text(encoding="utf-8"))
        menu = blocks(text, "export component PhotonContextMenu")[0]
        row = blocks(text, "component ContextMenuOption")[0]
        for where, body in (("the measuring stack", menu), ("the row", row)):
            self.assertIn(
                "MenuLabel", body,
                f"{where} no longer uses MenuLabel — the two can now disagree "
                f"about the type they measure and draw")
        # And nothing else may restate the type on top of it.
        for where, body in (("the measuring stack", menu), ("the row", row)):
            self.assertNotIn(
                "font-family", body,
                f"{where} states a font of its own again, which is exactly the "
                f"drift MenuLabel exists to prevent")

    def test_the_menu_clips_its_measuring_stack(self):
        """⚠ Slint does NOT clip by default, and the measuring stack is a real
        column of Texts sitting at the menu element's origin. Without `clip` they
        are painted onto the screen behind it — permanently, and nowhere near a
        menu — which reads as stray artwork rather than as a missing property."""
        text = strip_comments((UI / "widgets.slint").read_text(encoding="utf-8"))
        menu = blocks(text, "export component PhotonContextMenu")[0]
        self.assertIn("measure := VerticalLayout", menu,
                      "the menu lost the stack its width is measured from")
        self.assertIsNotNone(
            re.search(r"^\s*clip\s*:\s*true\s*;", menu, re.M),
            "the menu root no longer clips, so its measuring labels are drawn "
            "on the screen behind it")

    def test_the_menu_swallows_enter_even_with_no_row_highlighted(self):
        """⚠ Rejecting it would send Enter up to the App's root FocusScope, which
        is what advances the wizard — so opening a menu and pressing Enter would
        step to the next screen with the menu still on top of it."""
        text = strip_comments((UI / "widgets.slint").read_text(encoding="utf-8"))
        menu = blocks(text, "export component PhotonContextMenu")[0]
        handler = blocks(menu, "key-pressed")
        self.assertEqual(1, len(handler), "the menu lost its key handler")
        enter = blocks(handler[0], "if (event.text == Key.Return")
        self.assertEqual(1, len(enter), "the menu no longer handles Enter")
        self.assertIn(
            "return accept", enter[0],
            "the menu's Enter branch can fall through to `reject`, which hands "
            "the key to the wizard's own Enter handler")

    def test_the_arrow_keys_and_the_pointer_share_one_highlight(self):
        """The design has only Default and Hover, so the keyboard reuses the
        Hover look — safe only while there is exactly ONE highlighted row. A row
        reading its own TouchArea instead of the menu's flag would light up
        alongside whatever the arrows had reached."""
        text = strip_comments((UI / "widgets.slint").read_text(encoding="utf-8"))
        row = blocks(text, "component ContextMenuOption")[0]
        fill = re.search(r"background\s*:\s*([^;]+);", row)
        self.assertIsNotNone(fill, "the menu row lost its fill")
        self.assertIn(
            "root.active", fill.group(1),
            "the row paints itself from something other than the menu's single "
            "`active` flag, so hover and the arrows can both show at once")
        self.assertNotIn(
            "has-hover", fill.group(1),
            "the row reads its own hover again — two rows can now look chosen")

    def test_the_menu_keyline_cannot_be_mistaken_for_a_focus_ring(self):
        """⚠ A DELIBERATE DEVIATION FROM THE DESIGN, which says 2 px — and the
        exact thing a later transcription pass would "fix" back.

        The 2 px white keyline means FOCUS in this UI and nothing else
        (`FocusLookTests`, and the note on `focus-ring-w`): a decoration that is
        always on cannot also be a state. A menu wearing one permanently spends
        that signal on a panel that is never focused, and leaves every button's
        Focus variant looking like a menu border.
        """
        text = strip_comments((UI / "tokens.slint").read_text(encoding="utf-8"))
        widths = dict(re.findall(
            r"out property <length>\s*(menu-ring-w|focus-ring-w)\s*:\s*(\d+)px",
            text))
        self.assertEqual({"menu-ring-w", "focus-ring-w"}, set(widths),
                         "one of the two ring widths is gone")
        self.assertNotEqual(
            widths["menu-ring-w"], widths["focus-ring-w"],
            f"the menu's edge keyline is now the focus ring's width "
            f"({widths['menu-ring-w']}px) — the one thing in this UI that means "
            f"focus is being drawn permanently on something that never has it")

    def test_leaving_the_menu_drops_a_POINTER_highlight_but_not_a_KEY_one(self):
        """⚠ TWO HALVES, AND EACH ONE ALONE IS A BUG (2026-08-14).

        A lit row under no cursor is a menu claiming a choice nobody is making,
        and the next Enter takes it — so the pointer leaving has to clear it. A row
        the ARROWS chose has to survive exactly that, because the pointer is not
        what is driving the menu and moving it out of the way is not a decision.
        One flag written at the only two places that can set `active`.

        ⚠ THE DETECTOR IS AN ANCESTOR TouchArea, NOT A SIBLING. Slint sets
        `has-hover` from `input_event_filter_before_children`, which runs on every
        ancestor of the item under the pointer — a sibling behind the rows would go
        un-hovered the moment the pointer touched one, and the highlight would
        clear on the way IN.
        """
        text = strip_comments((UI / "widgets.slint").read_text(encoding="utf-8"))
        menu = blocks(text, "export component PhotonContextMenu")[0]
        self.assertIn(
            "active-by-key", menu,
            "the menu no longer records HOW its row was highlighted, so leaving "
            "with the pointer clears an arrow-key selection too (or nothing)")
        hover = blocks(menu, "hover := TouchArea")
        self.assertEqual(
            1, len(hover),
            "the menu lost the TouchArea that notices the pointer leaving it")
        self.assertIn(
            "!root.active-by-key", hover[0],
            "leaving the menu clears the highlight unconditionally — an arrow-key "
            "selection now vanishes when the mouse moves off, which is the one "
            "case that must survive")
        # The wrapper has to CONTAIN the rows, or it stops being an ancestor.
        self.assertIn(
            "ContextMenuOption", hover[0],
            "the hover detector no longer wraps the rows. As a sibling it is not "
            "an ancestor, so Slint stops reporting has-hover the moment the "
            "pointer is over a row — and the highlight clears on the way in")

    def test_the_menu_draws_no_focus_ring(self):
        """The other half of the same decision. A menu is modal for as long as it
        is open, so a ring saying "this has focus" is true and useless; what the
        keyboard is ON is the highlighted row, which the accent fill shows."""
        text = strip_comments((UI / "widgets.slint").read_text(encoding="utf-8"))
        menu = blocks(text, "export component PhotonContextMenu")[0]
        self.assertNotIn(
            "FocusRing", menu,
            "the context menu grew a focus ring — it is exempt on purpose, and "
            "the highlighted row is what says where the keyboard is")

    def test_the_folder_row_opens_the_row_that_was_right_clicked(self):
        """⚠ A right-click deliberately does NOT move the selection, so the row
        the menu is about and the row that is about to be installed to are
        allowed to differ. Reading `Selected()` in this handler would open the
        wrong folder exactly when they do — and be right often enough that
        nobody notices."""
        text = (REPO / "src" / "native" / "src" / "installer"
                / "gui.cpp").read_text(encoding="utf-8")
        start = text.find("on_open_aircraft_folder")
        self.assertNotEqual(-1, start, "the folder context-menu row is gone")
        body = text[start:text.find("});", start)]
        self.assertIn(
            "index", body, "the handler ignores the index the menu handed it")
        self.assertNotIn(
            "Selected()", body,
            "the handler reads the SELECTED aircraft rather than the "
            "right-clicked one — it opens the wrong folder whenever they differ")


class RailNavigationTests(unittest.TestCase):
    """A completed rail step is a way back to it (2026-08-14) — mouse only, and
    only BACKWARD.

    ⚠ THE THREE REFUSALS ARE WHAT KEEP IT FROM BEING A BUG: no forward jump, never
    to Run, and never out of a run in progress. Each is written twice on purpose —
    gui.cpp owns the rule and app.slint owns what LOOKS clickable — so both halves
    are asserted here.

    ⚠ IT WAS DEAD ON Complete FOR TWO DAYS, on the argument that a finished install
    must not be walkable back into Review. That aimed at the wrong control. Looking
    at a previous screen restores a set of answers; what commits anything is
    pressing Next, and Next from there walks the wizard forward again, rebuilding
    everything after it exactly as a first pass would. The lock belongs on the
    forward move, where it already was.
    """

    def setUp(self):
        self.cpp = (REPO / "src" / "native" / "src" / "installer"
                    / "gui.cpp").read_text(encoding="utf-8")
        self.app = strip_comments(
            (UI / "app.slint").read_text(encoding="utf-8"))

    def test_the_rail_asks_cpp_rather_than_setting_the_step(self):
        """app.slint decides nothing — the same rule `next()` and `back()` follow,
        and the reason all three front-ends stay in agreement."""
        # ⚠ Matched with the brace, so this is the INSTANTIATION and not the
        # import line — whose next brace is the whole App body.
        rail = blocks(self.app, "StepRail {")
        self.assertEqual(1, len(rail), "the rail moved or was renamed")
        self.assertIn("root.go-to-step(", rail[0],
                      "the rail no longer raises go-to-step")
        self.assertNotIn(
            "root.step =", rail[0],
            "the rail writes `step` directly, so the state machine in gui.cpp is "
            "no longer the only thing that decides where the wizard goes")

    def test_a_rail_jump_is_refused_forward(self):
        """⚠ Walking FORWARD is what rebuilds each screen: `GoNext` re-detects
        aircraft, publishes per-airframe availability and assembles the review
        list. A jump past a screen arrives with the last run's answers — a Review
        built for the aircraft selected before the X-Plane root changed."""
        start = self.cpp.find("void GoTo(")
        self.assertNotEqual(-1, start, "the rail's state-machine entry is gone")
        body = self.cpp[start:self.cpp.find("\n}", start)]
        self.assertIn(
            "target >= step", body,
            "GoTo no longer refuses a forward target, so a rail click can skip "
            "the step that builds the screen it lands on")
        self.assertNotIn(
            "Refresh", body,
            "GoTo grew a side effect — it must stay a plain assignment like "
            "GoBack, or going back rebuilds state the user did not ask to lose")

    def test_a_rail_jump_is_refused_while_a_run_is_in_progress(self):
        """⚠ `run_finished`, NOT `step != kRun`. Rewinding out of a run that is
        still writing files walks away from a half-written aircraft — the same
        condition `can-go-back` carries, said again for the other control.

        ⚠ IT MUST NOT BE THE STRONGER TEST, because the Run screen is reachable
        after the fact via Complete's "View log", and a rail dead on it would make
        that a trap: the only way out would be Next, back to where you came from.
        """
        start = self.cpp.find("void GoTo(")
        body = self.cpp[start:self.cpp.find("\n}", start)]
        self.assertIn(
            "get_run_finished()", body,
            "GoTo no longer refuses a jump out of a run in progress")
        match = re.search(r"rail-navigable\s*:\s*([^;]+);", self.app)
        self.assertIsNotNone(match, "app.slint lost its rail-navigable condition")
        self.assertIn(
            "run-finished", match.group(1),
            "the rail looks clickable during a run — the UI now offers a row "
            "that gui.cpp will silently refuse")
        self.assertNotIn(
            "!root.done", match.group(1),
            "the rail is dead on Complete again. Going back to LOOK at a screen "
            "costs nothing — what commits anything is pressing Next, which walks "
            "the wizard forward from there and rebuilds everything after it")

    def test_the_run_step_is_never_a_rail_destination(self):
        """⚠ THE ONE BLOCKED ROW, AND THE ONLY ONE (2026-08-14). Every other screen
        is a set of ANSWERS and re-showing it costs nothing. Run is not a state to
        restore but an action that has already happened: a rail row landing on a
        finished log with an enabled Next under it gives no sign of which run it
        belongs to. "View log" goes there and says what it is for.

        Both halves are asserted because they are written separately — gui.cpp
        owns the rule, app.slint owns which row LOOKS clickable, and a rail that
        offers a row C++ refuses reads as the rail being broken.
        """
        start = self.cpp.find("void GoTo(")
        body = self.cpp[start:self.cpp.find("\n}", start)]
        self.assertIn(
            "target == kRun", body,
            "GoTo accepts Run as a rail destination again")
        rail = blocks(self.app, "StepRail {")[0]
        match = re.search(r"blocked\s*:\s*(\d+)\s*;", rail)
        self.assertIsNotNone(
            match, "the rail no longer blocks a row, so Run is offered on "
                   "Complete — where every row before it is legitimately live")
        # Step 5 is Run and rail index is `step - 1`; app.slint owns that mapping,
        # which is why the number is written there and checked here.
        self.assertEqual(
            "4", match.group(1),
            "the blocked rail row is no longer Run's — this is the one place the "
            "step ⇄ rail-index mapping is spelled out, and off by one it blocks "
            "Review instead")

    def test_the_current_step_is_not_a_destination(self):
        """`idx < root.current`, never `<=`: the step you are on is not somewhere
        to go, and a row that highlights on hover and then does nothing reads as
        the rail being broken."""
        rail = blocks(strip_comments(
            (UI / "widgets.slint").read_text(encoding="utf-8")),
            "export component StepRail")[0]
        match = re.search(r"reachable\s*:\s*([^;]+);", rail)
        self.assertIsNotNone(match, "the rail row lost its `reachable` property")
        self.assertIn("idx < root.current", match.group(1),
                      "a rail row is reachable at or past the current step")
        self.assertIn("root.navigable", match.group(1),
                      "a rail row ignores whether the screen allows navigation")
        self.assertIn("idx != root.blocked", match.group(1),
                      "the rail row ignores `blocked`, so the Run row is "
                      "clickable on Complete")


class LogRenderingTests(unittest.TestCase):
    """The installer log is colored rows, not one string (2026-08-14).

    ⚠ THE MARKER IS AN SVG BECAUSE NEITHER EMBEDDED FONT HAS U+2713. Checked
    against `Roboto-Medium.ttf` and `RobotoMono-Medium.ttf` cmap tables, not
    assumed: the `✓` that shipped for months was drawn by a SYSTEM FALLBACK, which
    is the precise failure the fonts are embedded to prevent — fine here, a
    missing-glyph box on a machine whose fallback does not cover it.
    """

    CHECK = "✓"

    def test_no_ui_string_carries_a_checkmark_character(self):
        offenders = []
        targets = list(SLINT_FILES) + [
            REPO / "src" / "native" / "src" / "installer" / "gui.cpp"]
        for path in targets:
            text = strip_comments(path.read_text(encoding="utf-8"))
            if self.CHECK in text:
                offenders.append(
                    f"{path.relative_to(REPO)}: contains U+2713 — neither Roboto "
                    f"nor Roboto Mono has that glyph, so it renders through a "
                    f"system fallback font or as a box")
        self.assertEqual([], offenders, "\n".join(offenders))

    def test_the_embedded_fonts_really_do_lack_it(self):
        """A guard on the guard above: if a future font drop DID include the
        glyph, the rule stops being true and this says so instead of leaving a
        ban nobody can justify."""
        fonts = UI / "assets" / "fonts"
        for name in ("Roboto-Medium.ttf", "RobotoMono-Medium.ttf"):
            path = fonts / name
            self.assertTrue(path.exists(), f"{name} is gone")
            self.assertNotIn(
                ord(self.CHECK), _cmap(path),
                f"{name} now contains U+2713 — the SVG marker rule above can be "
                f"revisited")

    def test_the_red_line_and_the_stalled_advance_are_one_rule(self):
        """⚠ `bad` IS ERROR *OR* WARN, the same predicate `RunLog::Clean` uses, so
        a line painted red and a line that stops the Run screen advancing itself
        are by construction the same set. Two spellings drift the first time a
        level is added."""
        text = (REPO / "src" / "native" / "src" / "installer"
                / "gui.cpp").read_text(encoding="utf-8")
        start = text.find("class RunLog")
        body = text[start:text.find("};", start)]
        match = re.search(r"const bool bad\s*=\s*([^;]+);", body)
        self.assertIsNotNone(match, "RunLog no longer classifies its lines")
        self.assertIn("ERROR", match.group(1))
        self.assertIn("WARN", match.group(1))
        self.assertIn(
            "if (bad) clean_ = false;", body,
            "the red-line flag and the clean-run flag are computed separately")

    def test_copy_log_joins_what_the_screen_draws(self):
        """The colored rows cost drag-selection; this replaced it. It must read
        the same vector the model is built from, or it copies a second rendering
        of the log that can disagree with the one on screen."""
        text = (REPO / "src" / "native" / "src" / "installer"
                / "gui.cpp").read_text(encoding="utf-8")
        start = text.find("on_copy_log")
        self.assertNotEqual(-1, start, "the log can no longer be copied at all")
        body = text[start:text.find("});", start)]
        self.assertIn("*g.log", body,
                      "copy-log no longer reads the lines the screen is drawing")
        self.assertIn("SetClipboardText", body)


def _cmap(path):
    """Every code point in a TrueType file's format-4 unicode cmap."""
    import struct
    d = path.read_bytes()
    num = struct.unpack('>H', d[4:6])[0]
    tabs = {}
    for i in range(num):
        off = 12 + 16 * i
        tag = d[off:off + 4].decode('latin-1')
        o, _l = struct.unpack('>II', d[off + 8:off + 16])
        tabs[tag] = o
    co = tabs['cmap']
    n = struct.unpack('>H', d[co + 2:co + 4])[0]
    best = None
    for i in range(n):
        p = co + 4 + 8 * i
        pid, eid, so = struct.unpack('>HHI', d[p:p + 8])
        if (pid, eid) in ((3, 1), (3, 10), (0, 3), (0, 4)):
            best = co + so
    chars = set()
    if best is not None and struct.unpack('>H', d[best:best + 2])[0] == 4:
        segx2 = struct.unpack('>H', d[best + 6:best + 8])[0]
        seg = segx2 // 2
        ends = struct.unpack('>%dH' % seg, d[best + 14:best + 14 + segx2])
        starts = struct.unpack('>%dH' % seg,
                               d[best + 16 + segx2:best + 16 + 2 * segx2])
        for s, e in zip(starts, ends):
            if s == 0xFFFF and e == 0xFFFF:
                continue
            chars.update(range(s, e + 1))
    return chars


class OutcomeTests(unittest.TestCase):
    """The progress bar's red state and the Complete screen's glyph both report
    the run's outcome without being read — so both must agree with the sentences
    C++ renders, which means all three read the same `ok`."""

    def setUp(self):
        self.cpp = (REPO / "src" / "native" / "src" / "installer"
                    / "gui.cpp").read_text(encoding="utf-8")

    def test_the_failed_flag_comes_from_the_same_ok_as_the_words(self):
        self.assertIn(
            "set_run_failed(!ok)", self.cpp,
            "the red bar and the failure glyph are driven by something other "
            "than the `ok` the headline and summary are rendered from")

    def test_a_failed_run_is_not_forced_to_a_hundred_percent(self):
        """⚠ A full red bar reading "100%" contradicts "Something went wrong".
        Left where it stopped, the bar also reports how FAR it got, which is the
        first thing anyone reading the log wants to know."""
        self.assertIn(
            "if (ok) (*ui)->set_progress(1.0f);", self.cpp,
            "a failed run still fills its bar to 100%")

    def test_a_new_run_clears_the_previous_outcome(self):
        start = self.cpp.find("void StartRun")
        body = self.cpp[start:self.cpp.find("\n}", start)]
        self.assertIn(
            "set_run_failed(false)", body,
            "a run started after a failed one opens on a red bar and a failure "
            "glyph before it has done anything")


class LogTailTests(unittest.TestCase):
    """The log follows its own bottom, and repeats its problems there.

    The two landed together and are one feature: the run stops, the view is
    already parked on the end of the log, and what is under the rule is why it
    stopped.
    """

    def setUp(self):
        self.run = strip_comments(
            (UI / "screens" / "run.slint").read_text(encoding="utf-8"))
        self.widgets = strip_comments(
            (UI / "widgets.slint").read_text(encoding="utf-8"))
        self.cpp = (REPO / "src" / "native" / "src" / "installer"
                    / "gui.cpp").read_text(encoding="utf-8")

    def test_the_tail_follows_the_content_height_not_the_model(self):
        """⚠ THE DIFFERENCE IS ONE LINE, EVERY TIME. A `changed` on the model fires
        before the repeater has instantiated the new row, so `viewport-height` is
        still the OLD height and the view scrolls to the PREVIOUS bottom — which
        reads as an off-by-one in the scroll arithmetic rather than a sequencing
        bug. `viewport-height` changes only once the row has been measured.

        ⚠ It is also what stops it fighting a manual scroll: it fires when the
        CONTENT changes and never otherwise, so once the run is over there is
        nothing to leave and nothing to detect a user scroll against.
        """
        view = blocks(self.widgets, "export component PhotonScrollView")[0]
        tracker = re.search(
            r"changed\s+viewport-height\s*=>\s*\{([^}]*\}?[^}]*)\}", view)
        self.assertIsNotNone(
            tracker,
            "PhotonScrollView no longer follows its content height — whatever "
            "replaced it must not key off the MODEL, which changes one layout too "
            "early")
        self.assertIn("scroll-to-bottom()", tracker.group(1))
        self.assertNotIn(
            "changed log-lines", self.run,
            "the Run screen scrolls on the model changing, which lands one line "
            "short of the bottom every time")

    def test_the_jump_is_clamped_by_construction(self):
        """Flickable clamps its own dragging and wheel handling, but a direct write
        to `viewport-y` is taken literally — a large negative number scrolls the
        log into empty space."""
        view = blocks(self.widgets, "export component PhotonScrollView")[0]
        body = blocks(view, "public function scroll-to-bottom")
        self.assertTrue(body, "PhotonScrollView lost scroll-to-bottom()")
        self.assertIn(
            "-root.maximum", body[0],
            "scroll-to-bottom() no longer scrolls to the computed maximum")

    def test_the_summary_is_the_same_lines_the_log_drew(self):
        """⚠ FILTERED IN C++ FROM THE ONE VECTOR, on the same `bad` flag the rows
        are colored by — so a line cannot be red in the log and absent from the
        summary, or worded differently in the two places. Slint has no filtered
        model, and the alternative (a repeater over every line with the good ones
        hidden) leaves invisible elements occupying their layout cells."""
        start = self.cpp.find("void PublishLog")
        self.assertNotEqual(-1, start, "PublishLog is gone")
        body = self.cpp[start:self.cpp.find("\n}", start)]
        self.assertIn("line.bad", body,
                      "the problem summary is no longer filtered on the same flag "
                      "that colors a log row red")
        self.assertIn("set_problems", body,
                      "the problem summary is no longer published beside the log, "
                      "so the two can be published at different moments")

    def test_the_summary_waits_for_the_run_to_stop(self):
        """⚠ ITS CONDITION AND THE AUTO-ADVANCE'S ARE THE SAME ONE, by construction.
        The wizard advances on `ok && Clean()`; `problems` is exactly the lines
        `Clean()` counts, and a failed run always writes an ERROR — so "finished
        with a non-empty summary" and "finished and did not advance itself" are the
        same state. That is what the section answers: why am I still here.

        Without `finished` a WARN two seconds in would open a section that grows
        while it is being read.
        """
        match = re.search(r"show-problems\s*:\s*([^;]+);", self.run)
        self.assertIsNotNone(match, "the Run screen lost its problem summary")
        self.assertIn("finished", match.group(1),
                      "the problem summary appears mid-run")
        self.assertIn("problems.length", match.group(1),
                      "the problem summary is not gated on there being any")

    def test_the_summary_is_one_conditional_around_the_group(self):
        """⚠ NOT `visible` PER ROW. An invisible element still occupies its cell in
        a Slint layout, so per-row visibility leaves the log ending in a column of
        blank rows with a scrollbar for them."""
        group = [b for b in blocks(self.run, "if root.show-problems")]
        self.assertTrue(group, "the summary is no longer behind one conditional")
        self.assertIn("LogDivider", group[0],
                      "the rule is outside the conditional, so an ordinary clean "
                      "run ends on a divider with nothing under it")
        self.assertIn("for line in root.problems", group[0],
                      "the summary rows are outside the conditional")
        self.assertNotIn(
            "visible: root.show-problems", self.run,
            "the summary rows are hidden rather than removed, so they still take "
            "their space in the layout")


class WingSeedTests(unittest.TestCase):
    """The Features screen opens with the detected wing mod already checked
    (2026-08-14).

    ⚠ THIS IS NOT THE SAME ACT AS `actions::Install`'s WING CHECK, and it does not
    break that one's rule ("it reports, and that is all"). That check runs AFTER
    the user has chosen, where silently substituting their choice would be
    unexplainable from the screen they clicked through. This one runs BEFORE
    anyone has chosen: it fills in a default, on screen, where it can be seen and
    changed. Not overriding an answer and not offering one are different rules.
    """

    def setUp(self):
        self.cpp = (REPO / "src" / "native" / "src" / "installer"
                    / "gui.cpp").read_text(encoding="utf-8")
        self.wingmod = (REPO / "src" / "native" / "src" / "core"
                        / "wingmod.h").read_text(encoding="utf-8")

    def body(self, signature):
        start = self.cpp.find(signature)
        self.assertNotEqual(-1, start, f"{signature} is gone")
        return self.cpp[start:self.cpp.find("\n}", start)]

    def test_the_wing_token_table_round_trips(self):
        """⚠ ONE TABLE WRITTEN TWICE. `WingName` turns a radio into `--wing`;
        `WingIndexFor` turns a detector verdict into a radio. Off by one it
        preselects RealWings on a Durantula aircraft — which installs cleanly,
        says so, and leaves the wingtip lights holding still while the wing
        flexes, needing the aircraft loaded and airborne to notice.
        """
        names = {int(n): token for n, token in re.findall(
            r'case (\d+): return "(\w+)";', self.body("const char* WingName"))}
        consts = dict((name, value) for name, value in re.findall(
            r'constexpr char (k\w+)\[\] =\s*"(\w+)";', self.wingmod))
        back = {int(n): consts[const] for const, n in re.findall(
            r"token == wingmod::(k\w+)\) return (\d+);",
            self.body("int WingIndexFor"))}
        self.assertTrue(names, "WingName's table could not be read")
        self.assertEqual(
            names, back,
            "WingName and WingIndexFor disagree about which radio is which wing "
            "mod, so a detected mod preselects the wrong build")

    def test_the_seed_is_clamped_to_what_the_airframe_offers(self):
        """`wingmod::Detect` answers about the AEROPLANE and `WingsFor` about what
        Photon can build. `resolve_mount()` falls back to whatever mount exists, so
        a variant with no build is how a stock OBJ ships mislabeled as a mod."""
        body = self.body("int SeedWing")
        self.assertIn("WingsFor", body,
                      "the wing seed is no longer clamped to the airframe's own "
                      "list of buildable variants")
        self.assertIn("wm.determined", body,
                      "an undetermined detection now preselects something")

    def test_the_seed_happens_once_per_aircraft_not_once_per_visit(self):
        """⚠ WHAT KEEPS IT A DEFAULT RATHER THAN AN OVERRIDE. Availability is
        re-applied on every walk from Aircraft to Features, so without this a user
        who corrected the radio, stepped back to check the aircraft and came
        forward again would find their correction silently undone — the one
        behavior a preselection must never have, since the reason to touch that
        radio at all is believing the detector is wrong.
        """
        body = self.body("void ApplyAvailability")
        self.assertIn("wingSeededFor", body,
                      "the wing is re-seeded on every visit to Features, so a "
                      "user's correction is undone by stepping back and forward")
        self.assertIn(
            "ac->folder", body,
            "the seed is keyed on something other than the folder — the index is "
            "a position in a list that detection rebuilds")

    def test_the_install_time_check_still_only_reports(self):
        """⚠ THE SEED DOES NOT LICENSE THE OTHER ONE TO ACT. A wrong seed has to
        produce the same WARN a wrong manual choice would, or the preselection
        becomes a silent substitution with nothing anywhere saying so."""
        actions = (REPO / "src" / "native" / "src" / "core"
                   / "actions.cpp").read_text(encoding="utf-8")
        start = actions.find("wingmod::Detect")
        self.assertNotEqual(-1, start, "the install-time wing check is gone")
        body = actions[start:start + 2000]
        self.assertNotIn(
            "opts.wing =", body,
            "the install-time check now CHANGES what is installed. It must only "
            "report: a substitution is unexplainable from the screen the user "
            "clicked through, and it is wrong exactly when detection is")
        self.assertIn('"WARN"', body,
                      "the wing mismatch no longer warns, so a wrong preselection "
                      "would install silently")

    def test_the_preselection_is_recorded(self):
        """A checked box explains itself to nobody. The log is the only place that
        can say why — and the only place a wrong detection leaves a trace."""
        self.assertIn(
            "preselecting", self.body("int SeedWing"),
            "the wing preselection is no longer written to the installer log")


class WingProbeTests(unittest.TestCase):
    """The detection behind that preselection runs on its own thread, and it is
    started when the aircraft is SELECTED rather than when Next is pressed
    (2026-08-14).

    ⚠ THE COST IT MOVES IS I/O, NOT ARITHMETIC. `wingmod::Detect` reads all four
    `.acf` variants (9.2 MB on the A320/A321) and both wing OBJs (~4 MB more).
    Warm that is a few ms; cold — which is what it always is the first time an
    aircraft is looked at in a session — it is 13 MB off disk, and it was being
    paid inside the Next click, where a GUI-subsystem window does not repaint. It
    read as the button sticking.

    ⚠ THREADING ALONE WOULD ONLY MOVE THE WAIT: Features cannot open on the right
    radio until the answer exists, so a probe started at Next would still be waited
    on. Starting it at selection overlaps the read with the user reading the screen
    and travelling to the button. Both halves are pinned below, because dropping
    either one silently restores the freeze.
    """

    def setUp(self):
        self.cpp = (REPO / "src" / "native" / "src" / "installer"
                    / "gui.cpp").read_text(encoding="utf-8")

    def body(self, signature):
        start = self.cpp.find(signature)
        self.assertNotEqual(-1, start, f"{signature} is gone")
        return self.cpp[start:self.cpp.find("\n}", start)]

    def block(self, opener):
        found = blocks(self.cpp, opener)
        self.assertTrue(found, f"{opener} is gone")
        return found[0]

    def test_the_read_happens_only_on_the_probe_thread(self):
        """⚠ ONE CALL SITE, INSIDE THE WORKER LOOP. `wingmod::Detect` anywhere the
        UI thread reaches is 13 MB of file I/O in a click handler — the original
        bug, and it comes back the moment a caller finds it easier to just ask."""
        # ⚠ Counted in the CODE. The comments name it repeatedly and have to —
        # this is the rule they exist to state.
        self.assertEqual(
            1, strip_comments(self.cpp).count("wingmod::Detect"),
            "gui.cpp calls wingmod::Detect from more than one place — the only "
            "one that may is WingProbe's worker loop")
        self.assertIn("wingmod::Detect", self.block("class WingProbe"),
                      "the wing read has left the probe thread")
        for caller in ("int SeedWing", "void ApplyAvailability"):
            self.assertNotIn(
                "wingmod::Detect", self.body(caller),
                f"{caller} reads the aircraft's files again, on the UI thread")

    def test_the_probe_is_started_from_the_selection(self):
        """⚠ THE HALF THAT IS ACTUALLY THE OPTIMIZATION. A probe first asked for at
        Next is a probe that is waited on."""
        self.assertIn(
            "wingProbe.Request", self.block("on_select_aircraft"),
            "choosing an aircraft no longer starts the wing read, so the whole "
            "cost lands on Next again")

    def test_next_never_waits_for_the_probe(self):
        """Selecting a row and pressing Enter is one keystroke apart and beats any
        read. Features opens on `stock` and the poll ticks the radio over when the
        answer lands; a wait here — even a bounded one — is the freeze back with a
        shorter fuse."""
        body = self.body("void ApplyAvailability")
        self.assertIn("wingProbe.Ready", body,
                      "the seed no longer collects a verdict the probe already has")
        self.assertIn("wingSeedPending", body,
                      "a verdict that has not arrived yet is no longer waited for "
                      "on the poll — so either it blocks, or it is lost")
        for waiting in ("blocking_invoke_from_event_loop", ".join()", "wait_for"):
            self.assertNotIn(
                waiting, body,
                f"ApplyAvailability blocks on the probe ({waiting})")

    def test_the_probe_never_posts_to_the_event_loop(self):
        """⚠ `slint::invoke_from_event_loop` IS `slint_post_event`, WHICH
        `.unwrap()`s. Posting after the loop has terminated aborts the process, and
        a worker cannot check "is the loop still running" without racing the answer
        — so the hand-off is a `slint::Timer` poll, which runs on the UI thread by
        construction and cannot outlive the loop.
        """
        self.assertNotIn(
            "invoke_from_event_loop", self.block("class WingProbe"),
            "the probe posts to the Slint event loop; closing the window while it "
            "is reading would abort the process")
        self.assertIn("wingPoll.start", self.cpp,
                      "the poll timer that collects a late verdict is gone")

    def test_a_late_verdict_cannot_overrule_the_user(self):
        """⚠ `wingSeededFor`'s RULE, EXTENDED ACROSS THE WAIT. A hand-made choice
        ends the seed's claim on that radio — the reason to touch it at all is
        believing the detector is wrong — and choosing a different aircraft
        abandons a verdict that would otherwise preselect one aeroplane's wing mod
        on another."""
        for opener, why in (("on_set_wing", "the user's own correction"),
                            ("on_select_aircraft", "a different aircraft")):
            self.assertIn(
                "wingSeedPending.clear()", self.block(opener),
                f"a verdict still in flight can land on top of {why}")
        poll = self.body("void PollWingSeed")
        self.assertIn(
            "kFeatures", poll,
            "a verdict arriving after Review still moves the wing radio — which is "
            "the silent substitution the whole preselection is written not to be")
        self.assertIn(
            "folder != g.wingSeedPending", poll,
            "the poll no longer checks the verdict belongs to the aircraft that is "
            "currently selected")

    def test_the_probe_thread_is_stopped(self):
        """Both threads this file starts are joined before `Gui` goes out of
        scope. A detached one reading an aircraft folder through process teardown
        is a crash report nobody could explain."""
        self.assertIn("thread_.join()", self.block("class WingProbe"),
                      "the probe thread is no longer joined")
        self.assertIn("wingProbe.Stop()", self.body("int RunGui"),
                      "RunGui no longer stops the probe thread")


class BackspaceTests(unittest.TestCase):
    """Backspace is Back (2026-08-14), the browser's reading of the key.

    ⚠ IT IS SAFE ONLY BECAUSE THE ROOT SCOPE IS A FALLBACK. Slint offers a key to
    the focused item first, and an editable `TextInput` accepts Backspace
    unconditionally — even on empty text, since only `read-only` makes it decline
    (`TextInput::key_event` in Slint's core). So the path field keeps deleting
    characters and never navigates.
    """

    def setUp(self):
        self.app = strip_comments(
            (UI / "app.slint").read_text(encoding="utf-8"))
        self.widgets = strip_comments(
            (UI / "widgets.slint").read_text(encoding="utf-8"))

    def test_it_shares_escape_s_handler_and_its_guard(self):
        handler = re.search(
            r"key-pressed\(event\)\s*=>\s*\{(.*?)\n        \}", self.app, re.S)
        self.assertIsNotNone(handler, "the root key handler is gone")
        # ⚠ Matched to the `) {` that opens the body, not to the first `)` — the
        # condition contains a parenthesised `||` and stopping there reads only
        # half of it, which is exactly the half the guard is NOT in.
        branch = re.search(
            r"if \((.*?Key\.Backspace.*?)\)\s*\{", handler.group(1), re.S)
        self.assertIsNotNone(
            branch, "Backspace no longer goes back")
        self.assertIn(
            "can-go-back", branch.group(1),
            "Backspace bypasses the guard Escape uses, so it can rewind out of a "
            "run in progress and abandon a half-written aircraft")

    def test_no_read_only_text_input_can_leak_it(self):
        """⚠ THE ONE SHAPE THAT BREAKS IT. An editable TextInput swallows
        Backspace; a `read-only` one returns EventIgnored, so the key would reach
        the root scope and navigate while the user is trying to edit — or, worse,
        while they are dragging a selection they cannot change anyway."""
        offenders = [
            str(p.relative_to(REPO)) for p in SLINT_FILES
            if "read-only" in strip_comments(p.read_text(encoding="utf-8"))]
        self.assertEqual(
            [], offenders,
            f"a read-only TextInput is back in {offenders} — Backspace passes "
            f"through it to the wizard's Back. It needs its own key handler")

    def test_an_open_menu_swallows_it(self):
        """⚠ THE SAME RULE ENTER ALREADY HAD. A menu is modal for as long as it is
        open, so a key that moves the wizard must stop at it — otherwise
        right-clicking and pressing Backspace steps BACKWARD with the menu still
        on screen. Escape is the deliberate exception: rejecting it is how Slint's
        own popup handling gets to close the menu."""
        menu = blocks(self.widgets, "export component PhotonContextMenu")[0]
        keys = blocks(menu, "keys := FocusScope")[0]
        self.assertIn(
            "Key.Backspace", keys,
            "the context menu passes Backspace through to the App's root scope, "
            "which now steps the wizard back underneath the open menu")


class CompletePrimaryTests(unittest.TestCase):
    """Complete's two buttons, and which one is blue (2026-08-14).

    If every supported aircraft under this root is now current there is nothing
    left to modify and the likely next move is to leave, so **Exit** takes Primary
    and the focus; otherwise **Modify another aircraft** takes both.

    ⚠ THE FILL AND THE RING ARE ONE DECISION. A blue button that Enter does not
    press is worse than no recommendation at all, so one flag drives both — and
    the two focus properties must stay mutually exclusive, because `apply-focus`
    reads them in order and a state where both are true would focus the bar AND
    leave the screen calling `.focus()` on its own button a frame later.
    """

    def setUp(self):
        self.app = strip_comments(
            (UI / "app.slint").read_text(encoding="utf-8"))
        self.complete = strip_comments(
            (UI / "screens" / "complete.slint").read_text(encoding="utf-8"))
        self.cpp = (REPO / "src" / "native" / "src" / "installer"
                    / "gui.cpp").read_text(encoding="utf-8")

    def test_exactly_one_button_is_primary(self):
        bar = re.search(r"next-primary\s*:\s*([^;]+);", self.app)
        self.assertIsNotNone(bar, "the bottom bar's Next is unconditionally blue "
                                  "again, so Complete can show two primaries")
        self.assertIn("all-up-to-date", bar.group(1))
        modify = re.search(r"primary\s*:\s*([^;]+);", self.complete)
        self.assertIsNotNone(modify, "Modify another aircraft lost its fill rule")
        self.assertIn(
            "!root.all-up-to-date", modify.group(1),
            "the two buttons no longer read the same flag in opposite senses — "
            "they can now be blue at the same time, or neither of them")

    def test_the_ring_follows_the_fill(self):
        bar = re.search(r"focus-bar-next\s*:\s*((?:[^;]|\n)+);", self.app)
        screen = re.search(r"screen-owns-focus\s*:\s*([^;]+);", self.app)
        self.assertIn(
            "root.done && root.all-up-to-date", bar.group(1),
            "Exit is styled primary on a finished, fully-current install but does "
            "not take the focus, so the ring is on a button that is not blue")
        self.assertIn(
            "!root.all-up-to-date", screen.group(1),
            "the Complete screen focuses its own button even when the bar's Exit "
            "is the primary one — both would then race for the keyboard")

    def test_it_is_re_detected_on_the_way_to_complete(self):
        """⚠ THE RUN JUST CHANGED THE ANSWER. Reading the pre-run list is wrong in
        exactly the case the flag exists for: install the last out-of-date
        aircraft and the screen would still push "Modify another"."""
        start = self.cpp.find("void EnterComplete")
        self.assertNotEqual(-1, start, "the Complete-arrival hook is gone")
        body = self.cpp[start:self.cpp.find("\n}", start)]
        self.assertIn("Refresh", body, "Complete is reached without re-detecting")
        self.assertIn("set_all_up_to_date", body)

    def test_both_routes_into_complete_go_through_it(self):
        """⚠ THE AUTO-ADVANCE IS THE ONE THAT MATTERS. A clean install is exactly
        the run that reaches Complete without anyone pressing anything, so a timer
        that assigned the step directly would skip the re-detection in precisely
        the case the flag is for."""
        start = self.cpp.find("void EnterComplete")
        inside = self.cpp[start:self.cpp.find("\n}", start)]
        self.assertEqual(
            1, self.cpp.count("set_step(kComplete)"),
            "Complete is reached from more than one place. Only EnterComplete may "
            "set that step, or a route exists that skips the re-detection")
        self.assertIn(
            "set_step(kComplete)", inside,
            "the one place that sets the Complete step is no longer EnterComplete")
        self.assertIn(
            "invoke_next()", self.cpp,
            "the auto-advance no longer presses Next, so the two routes into "
            "Complete can diverge — and the clean install, which is exactly the "
            "run nobody clicks through, is the one that would take the bad route")

    def test_an_empty_list_is_not_all_up_to_date(self):
        """The sentence "everything is current" is false about nothing, and of the
        two defaults the safe one offers to go back to the list."""
        start = self.cpp.find("bool AllUpToDate")
        self.assertNotEqual(-1, start, "AllUpToDate is gone")
        body = self.cpp[start:self.cpp.find("\n}", start)]
        self.assertIn("empty()", body,
                      "AllUpToDate reports true for an empty aircraft list")
        self.assertIn(
            "EntryFor", body,
            "AllUpToDate re-derives 'green' instead of reading the badge the "
            "aircraft list already draws — the two can now disagree, and the "
            "Complete screen would call an aircraft current that the previous "
            "screen badged REPAIR")

    def test_the_selection_survives_the_re_detection(self):
        """⚠ `RefreshAircraft` CLEARS IT, AND THE RAIL CAN NOW WALK BACK. From
        Complete to Review with no selection, Install finds `Selected()` null and
        returns — a button that silently does nothing."""
        start = self.cpp.find("void RefreshAircraftKeepingSelection")
        self.assertNotEqual(-1, start, "the selection-preserving refresh is gone")
        body = self.cpp[start:self.cpp.find("\n}", start)]
        self.assertIn(
            "folder", body,
            "the selection is restored by something other than the folder — the "
            "index is a position in a list that was just rebuilt")


class AutoFocusTests(unittest.TestCase):
    """Four screens open with their primary control focused (2026-08-14).

    Enter already did the same thing through the root scope's fallback handler,
    so what this buys is the FOCUS RING — the screen says which control Enter
    operates instead of leaving it to be guessed.
    """

    def setUp(self):
        self.app = strip_comments(
            (UI / "app.slint").read_text(encoding="utf-8"))

    def test_the_screens_that_need_an_answer_are_excluded(self):
        """⚠ Aircraft REQUIRES a choice and Review exists to be read. Pre-arming
        the button that writes to someone's X-Plane install is the one place this
        would be actively wrong."""
        match = re.search(r"focus-bar-next\s*:\s*((?:[^;]|\n)+);", self.app)
        self.assertIsNotNone(match, "the auto-focus condition is gone")
        condition = match.group(1)
        for step, name in ((2, "Select Aircraft"), (4, "Review")):
            self.assertNotIn(
                f"step == {step}", condition,
                f"{name} now opens with its primary button focused — that screen "
                f"is waiting for the user, not for agreement")

    def test_the_directory_screen_waits_for_a_usable_path(self):
        match = re.search(r"focus-bar-next\s*:\s*((?:[^;]|\n)+);", self.app)
        self.assertIn(
            "root-status-ok", match.group(1),
            "the path screen arms Next even when the path is unusable, so the "
            "keyboard skips past work the user still has to do")

    def test_the_splash_gets_its_focus_without_a_step_change(self):
        """`changed step` does not fire for the step the window opens on, so the
        splash — the one screen never arrived at — needs the `init` too."""
        self.assertIn("init => { root.apply-focus(); }", self.app)
        self.assertIn("changed step => { root.apply-focus(); }", self.app)

    def test_complete_focuses_itself_and_the_app_stands_back(self):
        """Slint refuses to reach an element inside an `if` from outside it, so
        the screen owns this one. Without the exclusion in `apply-focus` the two
        would race for the keyboard on arrival."""
        match = re.search(r"screen-owns-focus\s*:\s*([^;]+);", self.app)
        self.assertIsNotNone(match, "app.slint lost its self-focus exclusion")
        self.assertIn("root.done", match.group(1))
        self.assertIn(
            "!root.screen-owns-focus", self.app,
            "apply-focus no longer stands back for a screen that focuses itself")
        complete = strip_comments(
            (UI / "screens" / "complete.slint").read_text(encoding="utf-8"))
        self.assertIn("modifyBtn.focus()", complete,
                      "Complete no longer focuses its own primary button")


class AutoAdvanceTests(unittest.TestCase):
    """A finished, CLEAN run walks itself from Run to Complete (2026-08-14).

    ⚠ gui.cpp has no C++ tests — it is in the photon-installer target, not
    photoncore — so this source-level scan is the only automated guard, the same
    shape as `ReviewRowTests`.

    The two gates are what stop it becoming a bug. Without the first, a failed or
    noisy install skips past the one screen showing 288 px of log explaining what
    happened. Without the second, someone who pressed Next (or Escape) inside the
    delay is yanked off the screen they just chose.
    """

    def setUp(self):
        text = (REPO / "src" / "native" / "src" / "installer"
                / "gui.cpp").read_text(encoding="utf-8")
        start = text.find("const bool advance")
        self.assertNotEqual(-1, start, "the Run screen's auto-advance is gone")
        self.text = text
        self.body = text[start:text.find("\n    });", start)]

    def test_only_a_clean_run_advances_itself(self):
        """⚠ `Clean()` COUNTS A WARN, not just an ERROR. A warning is a problem
        the user is meant to read — the wing-mod mismatch is the live example,
        where the installer proceeds with what was asked and says so — and
        skipping the only screen that shows it would make warning pointless."""
        gate = re.search(r"const bool advance\s*=\s*([^;]+);", self.body)
        self.assertIsNotNone(gate, "the auto-advance lost its condition")
        for token in ("ok", "Clean()"):
            self.assertIn(
                token, gate.group(1),
                f"the auto-advance no longer consults `{token}` — a run worth "
                f"reading now skips its own log")
        self.assertIn(
            "clean_ = false",
            self.text[self.text.find("class RunLog"):],
            "RunLog stopped recording that a run said something, so `Clean()` "
            "can only ever be true")

    def test_it_gives_up_if_the_user_has_already_moved(self):
        self.assertIn(
            "get_step() == kRun", self.body,
            "the auto-advance fires without checking the user is still on Run — "
            "it can now drag them off a screen they chose during the delay")

    def test_it_waits_long_enough_to_paint_the_finished_bar(self):
        """⚠ NOT ZERO. `set_progress(1.0f)` and the step change arrive in one
        event-loop callback, so with no delay the last frame the Run screen ever
        paints is the one before the bar filled — the install appears to end on a
        part-full bar and a screen that vanished."""
        match = re.search(r"kAutoAdvanceMs\s*=\s*(\d+)\s*;", self.text)
        self.assertIsNotNone(match, "kAutoAdvanceMs is gone")
        self.assertGreaterEqual(
            int(match.group(1)), 250,
            "the auto-advance delay is too short for the 100% bar to be seen")
        self.assertIn(
            "Timer::single_shot", self.body,
            "the auto-advance no longer goes through a timer, so it changes the "
            "screen in the same frame that fills the bar")


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
