"""Minimal proof that installer/tui.py's menu()/text_prompt() are drivable by
a scripted key sequence. `read_key` is a plain module-level function (not a
class method), so a test can monkeypatch `tui.read_key` directly — callers in
the same module resolve the bare name against the module dict at call time,
so no production refactor is needed to make it swappable.

Run: python -m unittest discover -s tests -v
"""
from __future__ import annotations

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from installer import tui


class ScriptedKeys:
    """Callable stand-in for tui.read_key: pops one scripted key per call."""

    def __init__(self, keys):
        self.keys = list(keys)

    def __call__(self):
        return self.keys.pop(0)


class _SwapReadKey:
    def __init__(self, keys):
        self.keys = keys

    def __enter__(self):
        self._old = tui.read_key
        tui.read_key = ScriptedKeys(self.keys)
        return self

    def __exit__(self, *exc):
        tui.read_key = self._old


class MenuTests(unittest.TestCase):
    def test_arrow_down_then_enter_selects_second_option(self):
        with _SwapReadKey([tui.KEY_DOWN, tui.KEY_ENTER]):
            with redirect_stdout(io.StringIO()):
                choice = tui.menu(0, "step", "prompt", ["one", "two", "three"])
        self.assertEqual(choice, 1)

    def test_number_key_selects_immediately(self):
        with _SwapReadKey(["3"]):
            with redirect_stdout(io.StringIO()):
                choice = tui.menu(0, "step", "prompt", ["one", "two", "three"])
        self.assertEqual(choice, 2)

    def test_esc_raises_back(self):
        with _SwapReadKey([tui.KEY_ESC]):
            with redirect_stdout(io.StringIO()):
                with self.assertRaises(tui.Back):
                    tui.menu(0, "step", "prompt", ["one", "two"])

    def test_disabled_option_is_skipped_by_arrow_navigation(self):
        # start on 0, but 0 is disabled -> initial idx snaps to 1; DOWN then
        # skips disabled index 2, wrapping to 0... rather than depend on wrap
        # behavior, just confirm ENTER never returns a disabled index.
        with _SwapReadKey([tui.KEY_DOWN, tui.KEY_ENTER]):
            with redirect_stdout(io.StringIO()):
                choice = tui.menu(0, "step", "prompt", ["a", "b", "c"], disabled={0, 2})
        self.assertEqual(choice, 1)


class WrapTests(unittest.TestCase):
    def test_short_line_is_not_wrapped(self):
        self.assertEqual(tui._wrap_ansi("hello", 20), ["hello"])

    def test_empty_line_stays_empty(self):
        self.assertEqual(tui._wrap_ansi("", 10), [""])

    def test_wraps_on_word_boundary(self):
        # greedy: "world foo" is 9 cols, over width 8, so each word gets its own line
        self.assertEqual(tui._wrap_ansi("hello world foo", 8),
                         ["hello", "world", "foo"])
        # widen enough for two words to share a line
        self.assertEqual(tui._wrap_ansi("hello world foo", 11),
                         ["hello world", "foo"])

    def test_hard_breaks_an_overlong_token(self):
        parts = tui._wrap_ansi("verylongunbreakabletoken", 10)
        self.assertTrue(all(tui._vis_len(p) <= 10 for p in parts))
        self.assertEqual("".join(parts), "verylongunbreakabletoken")

    def test_every_wrapped_line_fits_width(self):
        s = "the quick brown fox jumps over the lazy dog again and again"
        for w in (5, 8, 12, 20, 40):
            for line in tui._wrap_ansi(s, w):
                self.assertLessEqual(tui._vis_len(line), w)

    def test_ansi_codes_are_zero_width_and_color_continues(self):
        colored = f"{tui.C.BR_WHITE}alpha beta gamma delta{tui.C.RESET}"
        parts = tui._wrap_ansi(colored, 10)
        self.assertGreater(len(parts), 1)
        for p in parts:
            self.assertLessEqual(tui._vis_len(p), 10)
            # each continuation re-opens the active color
            self.assertIn(tui.C.BR_WHITE, p)


class TextPromptTests(unittest.TestCase):
    def test_typed_text_then_enter(self):
        with _SwapReadKey(list("hi") + [tui.KEY_ENTER]):
            with redirect_stdout(io.StringIO()):
                result = tui.text_prompt(0, "step", "prompt")
        self.assertEqual(result, "hi")

    def test_backspace_removes_last_char(self):
        with _SwapReadKey(list("hit") + [tui.KEY_BACKSPACE, tui.KEY_ENTER]):
            with redirect_stdout(io.StringIO()):
                result = tui.text_prompt(0, "step", "prompt")
        self.assertEqual(result, "hi")


if __name__ == "__main__":
    unittest.main()
