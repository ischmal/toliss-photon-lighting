"""Dear ImGui usage rules that hold across every window in plugin.cpp.

Both of the things checked here fail in the same annoying way: the window still
builds, nothing is logged, and what you get is a tooltip with its end missing or
a slider that moves a different slider. Neither reads as "the UI code is wrong"
from inside the simulator, and both are one careless line away at all times.
"""
import collections
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
SRC = REPO / "src" / "native" / "src" / "plugin.cpp"
PLUGIN_CPP = SRC.read_text(encoding="utf-8", errors="replace")
LINES = PLUGIN_CPP.splitlines()

# Widgets whose ID comes from their label. Text, TextUnformatted, Separator,
# SeparatorText, PlotLines and the rest create no ID and cannot collide.
ID_WIDGETS = """Button SmallButton InvisibleButton ArrowButton Checkbox CheckboxFlags
RadioButton SliderFloat SliderFloat2 SliderFloat3 SliderInt DragFloat DragInt
InputText InputTextMultiline InputFloat InputInt InputDouble ColorEdit3 ColorEdit4
ColorPicker3 ColorPicker4 ColorButton Selectable TreeNode TreeNodeEx
CollapsingHeader BeginCombo Combo ListBox BeginChild BeginTabItem BeginMenu
MenuItem BeginTable TabItemButton SliderAngle VSliderFloat""".split()

WIDGET_CALL = re.compile(r"ImGui::(%s)\s*\(" % "|".join(ID_WIDGETS))
WIDGET_LITERAL = re.compile(r'ImGui::(?:%s)\s*\(\s*"' % "|".join(ID_WIDGETS))
PUSH_ID = re.compile(r"ImGui::PushID\s*\(")
LOOP = re.compile(r"\b(?:for|while)\s*\(")
FUNC_DEF = re.compile(r"^(?:static\s+)?[A-Za-z_][\w:<>,\s\*&]*[\s\*&](\w+)\s*\(")


def _code(line):
    """The line with its trailing // comment removed. Crude — a // inside a
    string literal would be cut too — but no tooltip text in this file contains
    one, and the alternative is a C++ lexer."""
    return line.split("//")[0]


def _owner_by_line():
    """line number (1-based) -> name of the function it sits in."""
    owner, cur, brace = {}, None, 0
    for n, raw in enumerate(LINES, 1):
        if brace == 0:
            m = FUNC_DEF.match(raw)
            if m and not raw.lstrip().startswith(("return", "if", "for", "while")):
                cur = m.group(1)
        owner[n] = cur
        brace = max(0, brace + _code(raw).count("{") - _code(raw).count("}"))
    return owner


class TooltipTests(unittest.TestCase):
    """⚠ A tooltip window AUTO-SIZES to its content and a paragraph with no wrap
    position is one line however long it is. ImGui then CLAMPS that window to the
    X-Plane window rather than re-flowing it, so the sentence is cut off — and
    only near an edge, so the same tooltip reads fine somewhere else on screen.
    Every explanatory string in this plugin is a paragraph, so this is the normal
    case and not an occasional one."""

    def test_nothing_calls_set_tooltip_directly(self):
        offenders = [
            "plugin.cpp:%d" % n
            for n, raw in enumerate(LINES, 1)
            if "ImGui::SetTooltip(" in _code(raw) or "ImGui::SetItemTooltip(" in _code(raw)
        ]
        self.assertEqual(
            offenders, [],
            "use UiTooltip / UiItemTooltip instead — they wrap, SetTooltip does "
            "not: %s" % ", ".join(offenders))

    def test_the_helper_actually_wraps(self):
        m = re.search(r"static void UiTooltipV\(.*?\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "UiTooltipV moved or was renamed")
        body = m.group(0)
        self.assertIn("ImGui::PushTextWrapPos(ImGui::GetFontSize() * kUiTooltipEm)", body)
        self.assertIn("ImGui::PopTextWrapPos()", body)
        # ⚠ BeginTooltip returns a bool in 1.92 and EndTooltip may only be called
        # when it returned true. An unconditional EndTooltip unbalances the
        # window stack, which asserts far away from here.
        self.assertRegex(body, r"if \(!ImGui::BeginTooltip\(\)\) return;")

    def test_the_wrap_width_is_a_named_constant(self):
        self.assertRegex(PLUGIN_CPP, r"static const float kUiTooltipEm = [\d.]+f;")


class RadioButtonTests(unittest.TestCase):
    """⚠ The radio DOT and the checkbox TICK share one style color,
    ImGuiCol_CheckMark — ImGui has no separate slot for the dot. The tick is
    white (it sits on the accent-filled ImGuiCol_CheckboxSelectedBg, where an
    accent-derived mark is one blue on another); the dot sits on an ordinary
    FrameBg and wants the accent. One theme value cannot be both, so the dot's is
    pushed per call — which only works if every call goes through the helper. A
    raw ImGui::RadioButton silently gets the white dot instead."""

    def test_nothing_calls_radio_button_directly(self):
        # UiRadioButton itself is the one place that may — it is the wrapper.
        owner = _owner_by_line()
        offenders = [
            "plugin.cpp:%d" % n
            for n, raw in enumerate(LINES, 1)
            if "ImGui::RadioButton(" in _code(raw)
            and owner.get(n) != "UiRadioButton"
        ]
        self.assertEqual(
            offenders, [],
            "use UiRadioButton — it pushes the dot color: %s" % ", ".join(offenders))

    def test_the_helper_pushes_and_pops_the_check_mark_color(self):
        m = re.search(r"static bool UiRadioButton\(.*?\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m, "UiRadioButton moved or was renamed")
        body = m.group(0)
        self.assertIn("ImGui::PushStyleColor(ImGuiCol_CheckMark,", body)
        self.assertIn("PhotonImgui::AccentLight()", body)
        self.assertIn("ImGui::PopStyleColor()", body)

    def test_the_dot_color_is_not_a_second_copy_of_the_accent(self):
        """It comes from the theme's own expression, so retuning kUiBlue moves
        the dot too. A literal here is how a derived palette acquires a
        hand-picked outlier that nothing updates."""
        body = re.search(r"static bool UiRadioButton\(.*?\n\}", PLUGIN_CPP, re.S).group(0)
        self.assertNotRegex(body, r"ImVec4\s*\(")


class WidgetIdTests(unittest.TestCase):
    """⚠ An ImGui item's ID is its LABEL TEXT hashed with the current ID stack.
    Two visible items that hash the same raise "2 visible items with conflicting
    ID" and then share state: dragging one slider moves the other, and clicking
    one button lights up its twin."""

    def test_no_literal_labelled_widget_sits_unscoped_in_a_loop(self):
        """A widget whose label is a string LITERAL, inside a loop, with no
        PushID in that loop body, is N items with one ID — every time, not just
        sometimes. (A label read from a table or a vector can be distinct per
        iteration, so those are not flagged here; scoping them anyway is the
        house habit, and the reason DbgOptionRow and the repo-command row carry
        a PushID they do not strictly need today.)"""
        owner = _owner_by_line()
        offenders, stack, brace = [], [], 0
        for n, raw in enumerate(LINES, 1):
            code = _code(raw)
            if LOOP.search(code) and "{" in code:
                stack.append([brace, n, False, []])
            if stack and PUSH_ID.search(code):
                stack[-1][2] = True
            if stack and WIDGET_LITERAL.search(code):
                stack[-1][3].append(n)
            brace += code.count("{") - code.count("}")
            while stack and brace <= stack[-1][0]:
                _, ln, scoped, hits = stack.pop()
                if hits and not scoped:
                    offenders.append(
                        "%s() loop at plugin.cpp:%d -> widget(s) at %s"
                        % (owner.get(ln), ln, ", ".join(str(h) for h in hits)))
            brace = max(brace, 0)
        self.assertEqual(offenders, [],
                         "PushID the loop body: " + "; ".join(offenders))

    def test_a_helper_that_emits_ids_scopes_itself(self):
        """A row helper called twice from one pane submits its widgets into the
        SAME ID scope, so identical labels in the two calls collide. Either the
        helper pushes an ID of its own, or every widget label it emits comes
        from one of its parameters (which is what makes FxTriCombo safe)."""
        owner = _owner_by_line()

        emits, scopes, param_labelled = set(), set(), collections.defaultdict(bool)
        for n, raw in enumerate(LINES, 1):
            f = owner.get(n)
            if not f:
                continue
            code = _code(raw)
            if WIDGET_CALL.search(code):
                emits.add(f)
                if not WIDGET_LITERAL.search(code):
                    param_labelled[f] = True
            if PUSH_ID.search(code):
                scopes.add(f)

        calls = collections.defaultdict(list)
        for n, raw in enumerate(LINES, 1):
            code = _code(raw)
            for f in emits:
                if re.search(r"(?<![\w:])%s\s*\(" % f, code) and owner.get(n) != f:
                    calls[f].append(n)

        offenders = [
            "%s() (called from %d places, no PushID, emits literal labels)"
            % (f, len(calls[f]))
            for f in sorted(emits - scopes)
            if len(calls.get(f, [])) > 1 and not param_labelled[f]
        ]
        self.assertEqual(offenders, [],
                         "add ImGui::PushID: " + "; ".join(offenders))

    def test_the_strength_sliders_are_still_scoped(self):
        """⚠ The one collision that actually shipped and was found in-sim: three
        Displays rows whose slider is called "Strength" in all three. Named here
        so the fix cannot be undone by tidying the helper away."""
        m = re.search(r"static bool DisplayToggleRow\(.*?\n\}", PLUGIN_CPP, re.S)
        self.assertIsNotNone(m)
        body = m.group(0)
        self.assertIn("ImGui::PushID(label)", body)
        self.assertLess(body.index("ImGui::PushID(label)"),
                        body.index('SliderFloat("Strength"'))


if __name__ == "__main__":
    unittest.main()
