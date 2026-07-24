"""Terminal UI engine: ANSI color, cross-platform single-keystroke input, and a
compose-then-blit layout engine (`render`) with a fixed header/footer and a
scrollable body. Ported unchanged from the .scratch/photon_installer_mockup.py
prototype — see that file's module docstring for the design rationale.

Zero third-party deps: msvcrt (Windows) / termios+tty (POSIX) only.
"""
from __future__ import annotations

import os
import re
import shutil
import sys

from installer.constants import VERSION

# The layout spans the full detected terminal width (there is no upper cap):
# the header/footer rules reach the window edge. MIN_WIDTH is only an absolute
# sanity floor for absurdly narrow terminals.
MIN_WIDTH = 40

# Body content is inset this many columns from the left; the header/footer
# rules are NOT inset, so they still span the full width.
BODY_INDENT = 1

# Breadcrumb / progress stages, left->right.
STAGES = ["Root", "Aircraft", "Configure", "Run", "Complete"]


# ─── ANSI color + terminal control ────────────────────────────────────────────
class C:
    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    UNDER = "\033[4m"
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    WHITE = "\033[37m"     # light gray
    CYAN = "\033[36m"
    BR_CYAN = "\033[96m"
    BR_GREEN = "\033[92m"
    BR_YELLOW = "\033[93m"
    BR_WHITE = "\033[97m"
    BR_BLACK = "\033[90m"  # grey


def enable_vt():
    """Turn on ANSI processing and UTF-8 output (for the box-drawing glyphs)."""
    try:
        sys.stdout.reconfigure(encoding="utf-8")  # box chars ▁▔►✈ on Windows
    except Exception:
        pass
    if os.name == "nt":
        try:
            import ctypes

            k = ctypes.windll.kernel32
            k.SetConsoleMode(k.GetStdHandle(-11), 7)  # ENABLE_VIRTUAL_TERMINAL
        except Exception:
            pass


def enter_alt_screen():
    sys.stdout.write("\033[?1049h")
    sys.stdout.flush()


def leave_alt_screen():
    sys.stdout.write("\033[?1049l")
    sys.stdout.flush()


def hide_cursor():
    sys.stdout.write("\033[?25l")


def show_cursor():
    sys.stdout.write("\033[?25h")


# ─── single-keystroke input (cross-platform, no deps) ─────────────────────────
KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT = "UP", "DOWN", "LEFT", "RIGHT"
KEY_ENTER, KEY_ESC, KEY_BACKSPACE = "ENTER", "ESC", "BACKSPACE"
KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END = "PGUP", "PGDN", "HOME", "END"


def read_key():
    """Block for one keypress; return a normalized token or a literal character.

    A plain module-level function (not a class method), so tests can swap it
    out with `installer.tui.read_key = fake` — callers inside this module
    resolve the bare name against the module dict at call time, so the
    monkeypatch takes effect without any refactor."""
    if os.name == "nt":
        import msvcrt

        ch = msvcrt.getwch()
        if ch in ("\x00", "\xe0"):
            ch2 = msvcrt.getwch()
            return {"H": KEY_UP, "P": KEY_DOWN, "K": KEY_LEFT, "M": KEY_RIGHT,
                    "I": KEY_PGUP, "Q": KEY_PGDN, "G": KEY_HOME, "O": KEY_END}.get(
                ch2, "")
        if ch in ("\r", "\n"):
            return KEY_ENTER
        if ch == "\x1b":
            return KEY_ESC
        if ch in ("\x08", "\x7f"):
            return KEY_BACKSPACE
        if ch == "\x03":
            raise KeyboardInterrupt
        return ch
    import termios
    import tty

    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        ch = sys.stdin.read(1)
        if ch == "\x1b":
            import select

            if select.select([sys.stdin], [], [], 0.0005)[0]:
                seq = sys.stdin.read(1)
                if seq == "[":
                    code = sys.stdin.read(1)
                    if code in "0123456789":  # e.g. ESC[5~ = PgUp
                        rest = sys.stdin.read(1)
                        return {"5": KEY_PGUP, "6": KEY_PGDN, "1": KEY_HOME,
                                "4": KEY_END}.get(code, "")
                    return {"A": KEY_UP, "B": KEY_DOWN, "C": KEY_RIGHT,
                            "D": KEY_LEFT, "H": KEY_HOME, "F": KEY_END}.get(code, "")
                return ""
            return KEY_ESC
        if ch in ("\r", "\n"):
            return KEY_ENTER
        if ch in ("\x7f", "\x08"):
            return KEY_BACKSPACE
        if ch == "\x03":
            raise KeyboardInterrupt
        return ch
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


def read_key_timeout(timeout: float):
    """Like read_key(), but wait at most `timeout` seconds for a keypress and
    return None if none arrives — letting a caller poll something else (e.g. is
    X-Plane still running?) and re-render between checks while still responding
    instantly to a keypress. Delegates the actual decode to read_key() once input
    is known to be waiting, so key handling stays in one place.

    A module-level function like read_key, so tests can monkeypatch it."""
    import time as _time

    if os.name == "nt":
        import msvcrt

        deadline = _time.monotonic() + timeout
        while _time.monotonic() < deadline:
            if msvcrt.kbhit():
                return read_key()
            _time.sleep(0.02)
        return None

    import select
    import termios
    import tty

    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:  # wait for input in raw mode so a single keystroke (not a full line)
        tty.setraw(fd)  # wakes select; the char stays buffered for read_key()
        if not select.select([sys.stdin], [], [], timeout)[0]:
            return None
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
    return read_key()


# ─── low-level string helpers (ANSI-aware) ────────────────────────────────────
def _vis_len(s: str) -> int:
    """Printable length, ignoring ANSI escape sequences."""
    out, i = 0, 0
    while i < len(s):
        if s[i] == "\033":
            while i < len(s) and s[i] != "m":
                i += 1
            i += 1
            continue
        out += 1
        i += 1
    return out


def _clip(s: str, width: int) -> str:
    """Truncate to `width` printable columns, preserving ANSI codes; always
    terminates with RESET so a cut mid-color can't bleed into the next row."""
    out, vis, i = [], 0, 0
    while i < len(s):
        if s[i] == "\033":
            j = i
            while j < len(s) and s[j] != "m":
                j += 1
            out.append(s[i:j + 1])
            i = j + 1
            continue
        if vis >= width:
            break
        out.append(s[i])
        vis += 1
        i += 1
    return "".join(out) + C.RESET


def _row(left: str, right: str, width: int) -> str:
    """Left- and right-justify two fragments within width (ANSI-aware)."""
    gap = width - _vis_len(left) - _vis_len(right)
    if gap < 1:
        return left  # not enough room; drop the right fragment
    return left + " " * gap + right


_ANSI_RE = re.compile(r"\033\[[0-9;]*m")


def _active_sgr(s: str) -> str:
    """The SGR (color) codes still in effect at the end of `s`. Used to re-open
    the active color on a wrapped continuation line so styling survives the
    break; a RESET clears the set."""
    codes: list[str] = []
    for m in _ANSI_RE.finditer(s):
        seq = m.group(0)
        if seq in ("\033[0m", "\033[m"):
            codes = []
        else:
            codes.append(seq)
    return "".join(codes)


def _wrap_ansi(s: str, width: int) -> list[str]:
    """Wrap `s` to `width` visible columns, ANSI-aware: break at the last space
    that fits, else hard-break an over-long run. ANSI escapes are zero-width and
    never split; each continuation line is re-opened with whatever color was
    active at the break so a wrapped colored line keeps its color."""
    if width <= 0 or _vis_len(s) <= width:
        return [s]
    slices: list[tuple[int, int]] = []
    line_start = 0
    vis = 0
    last_space = -1
    i, n = 0, len(s)
    while i < n:
        m = _ANSI_RE.match(s, i)
        if m:
            i = m.end()
            continue
        if vis >= width:  # current char is the (width+1)-th visible column
            if s[i] == " ":
                # the line is exactly full and this space is the separator —
                # break here (keeping the full word that just fit) and drop it
                slices.append((line_start, i))
                line_start = i + 1
            elif last_space > line_start:
                slices.append((line_start, last_space))  # break at the word gap
                line_start = last_space + 1
            else:
                slices.append((line_start, i))            # hard-break a long run
                line_start = i
            vis, last_space, i = 0, -1, line_start
            continue
        if s[i] == " ":
            last_space = i
        vis += 1
        i += 1
    if line_start < n or not slices:
        slices.append((line_start, n))
    return [s[a:b] if idx == 0 else _active_sgr(s[:a]) + s[a:b]
            for idx, (a, b) in enumerate(slices)]


def _press_hint(key: str, action: str) -> str:
    """The shared 'Press <KEY> to <action>' coloring used by the footer — dim
    'Press', bold key, dim action. Reused in the launch body so the user learns
    the scheme from the first screen."""
    return f"{C.DIM}Press{C.RESET} {C.BOLD}{key}{C.RESET}{C.DIM} to {action}{C.RESET}"


# ─── layout engine: compose a full grid, blit once ────────────────────────────
def _dims():
    cols, rows = shutil.get_terminal_size((90, 30))
    width = max(MIN_WIDTH, cols - 1)  # full width; -1 avoids last-column wrap
    return width, max(rows, 10)


def _breadcrumb(active: int | None, width: int) -> str:
    """The progress indicator: done stages green, current bright+bold+underline,
    upcoming grey."""
    parts = []
    for i, name in enumerate(STAGES):
        if active is not None and i < active:
            parts.append(f"{C.GREEN}{name}{C.RESET}")
        elif active is not None and i == active:
            parts.append(f"{C.BOLD}{C.UNDER}{C.BR_CYAN}{name}{C.RESET}")
        else:
            parts.append(f"{C.BR_BLACK}{name}{C.RESET}")
    return f"{C.BR_BLACK} / {C.RESET}".join(parts)


def _header_rows(active: int | None, width: int) -> list[str]:
    """Header: title + breadcrumb, then the ▁ rule."""
    title = f"{C.BOLD}{C.BR_WHITE}ToLiss Photon {VERSION}{C.RESET}{C.DIM} - Installer{C.RESET}"
    crumb = _breadcrumb(active, width)
    return [_row(title, crumb, width), f"{C.BR_BLACK}{'▁' * width}{C.RESET}"]


def _footer_rows(width: int, back: str | None, cont: str | None,
                 cont_key: str) -> list[str]:
    """Footer pinned to the bottom, uniform on every screen: a ▔ rule, the ESC /
    <cont_key> key hints, then an always-present blank line so the hints never sit
    flush against the terminal's bottom edge. Both hints carry a one-space margin
    (the continue hint is offset one space in from the right edge to mirror it)."""
    left = (f"{C.BR_YELLOW}←{C.RESET} " + _press_hint("ESC", back)) if back else ""
    right = (_press_hint(cont_key, cont) + f" {C.BR_GREEN}→{C.RESET}") if cont else ""
    keys = " " + _row(left, right, width - 2) + " "  # 1-space margin each side
    return [f"{C.BR_BLACK}{'▔' * width}{C.RESET}", keys, ""]


def _window(lines: list[str], height: int, focus: int | None):
    """Return (visible_slice, more_above, more_below) scrolled so `focus` shows."""
    lines = list(lines)
    n = len(lines)
    if n <= height:
        return lines, False, False
    f = 0 if focus is None else focus
    off = max(0, min(f - height // 2, n - height))
    return lines[off:off + height], off > 0, off + height < n


def render(stage: int | None, body: list[str], *, back: str | None = None,
           cont: str | None = None, cont_key: str = "ENTER",
           focus: int | None = None, center: bool = False):
    """Compose one full frame — header pinned top, footer pinned bottom, body
    (scrolled if needed) in between — and blit it with a single write."""
    width, height = _dims()
    grid = [""] * height

    head = _header_rows(stage, width) if stage is not None else []
    foot = _footer_rows(width, back, cont, cont_key)
    for i, ln in enumerate(head):
        grid[i] = ln
    for i, ln in enumerate(foot):
        grid[height - len(foot) + i] = ln

    body_top = len(head) + 1               # one blank line under the header
    body_bottom = height - len(foot) - 1   # one blank line above the footer
    region_h = max(1, body_bottom - body_top + 1)

    # Wrap the body to the indented content width, remapping `focus` (an index
    # into the pre-wrap body) onto the physical line it now begins at, so the
    # scroll window still keeps the focused option on screen.
    content_w = max(1, width - BODY_INDENT)
    phys: list[str] = []
    phys_focus = None
    for orig_i, ln in enumerate(body):
        if focus is not None and orig_i == focus:
            phys_focus = len(phys)
        phys.extend(_wrap_ansi(ln, content_w))

    win, up, down = _window(phys, region_h, phys_focus)
    start = body_top
    if center and len(win) < region_h:
        start += (region_h - len(win)) // 2
    if up:
        win[0] = f"{C.BR_BLACK}▲  more above{C.RESET}"
    if down:
        win[-1] = f"{C.BR_BLACK}▼  more below{C.RESET}"
    indent = " " * BODY_INDENT
    for i, ln in enumerate(win):
        r = start + i
        if body_top <= r <= body_bottom:
            grid[r] = indent + ln

    out = ["\033[H"]
    for i, ln in enumerate(grid, 1):
        out.append(f"\033[{i};1H{_clip(ln, width)}\033[K")
    out.append("\033[J")
    sys.stdout.write("".join(out))
    sys.stdout.flush()


# ─── shared body/option formatting ────────────────────────────────────────────
PREFIX = 7  # "  ► 1. " — options and table headers align to this column


def _step(step: str) -> str:
    return f"{C.BOLD}{C.BR_CYAN}{step}{C.RESET}"


def _option(i: int, text: str, selected: bool) -> str:
    if selected:
        return (f"  {C.BR_GREEN}►{C.RESET} {C.BR_WHITE}{C.BOLD}{i + 1}.{C.RESET} "
                f"{C.BR_WHITE}{text}{C.RESET}")
    return f"    {C.DIM}{i + 1}.{C.RESET} {text}"


class Back(Exception):
    """Raised by a screen on ESC -> return to the previous screen."""


def menu(stage, step, prompt, options, *, back="go back", cont="continue",
         header_lines=None, footer_lines=None, allow_back=True, disabled=None,
         index=0):
    """Arrow/number-navigable menu rendered through the layout engine. Returns the
    chosen index; raises Back on ESC. `header_lines` are static (non-selectable)
    rows drawn above the options (used for the aircraft table columns).
    `footer_lines` are static rows drawn below the options (used for a status
    note after an action, so it appears on the same screen instead of a
    separate flash). `index` is the initially-highlighted option (clamped, and
    advanced off a disabled row)."""
    disabled = disabled or set()
    idx = index if 0 <= index < len(options) else 0
    while idx in disabled:
        idx = (idx + 1) % len(options)
    while True:
        body: list[str] = [""]
        if step:
            body += [_step(step), ""]
        if prompt:
            body += [prompt, ""]
        if header_lines:
            body += list(header_lines)
        opt_start = len(body)
        for i, opt in enumerate(options):
            if i in disabled:
                body.append(f"    {C.BR_BLACK}{i + 1}. {opt}{C.RESET}")
            else:
                body.append(_option(i, opt, i == idx))
        if footer_lines:
            body += list(footer_lines)
        render(stage, body, back=back if allow_back else None, cont=cont,
               focus=opt_start + idx)
        k = read_key()
        if k == KEY_UP:
            idx = (idx - 1) % len(options)
            while idx in disabled:
                idx = (idx - 1) % len(options)
        elif k in (KEY_DOWN,):
            idx = (idx + 1) % len(options)
            while idx in disabled:
                idx = (idx + 1) % len(options)
        elif k in (KEY_HOME,):
            idx = next(i for i in range(len(options)) if i not in disabled)
        elif k in (KEY_END,):
            idx = next(i for i in reversed(range(len(options))) if i not in disabled)
        elif k.isdigit() and 1 <= int(k) <= len(options) and int(k) - 1 not in disabled:
            return int(k) - 1
        elif k == KEY_ENTER and idx not in disabled:
            return idx
        elif k == KEY_ESC and allow_back:
            raise Back


def text_prompt(stage, step, prompt, initial="", error: str | None = None):
    """A single-line text field. `error`, if given, renders as a warning line
    above the field on the first frame (e.g. 'that path doesn't look right') —
    it clears itself the moment the user edits the text, so it never lingers
    once they've started fixing it."""
    buf = list(initial)
    while True:
        shown = "".join(buf)
        field = f"  {C.BR_GREEN}►{C.RESET} {C.BR_WHITE}{shown}{C.RESET}\033[7m \033[0m"
        body = ["", _step(step), "", prompt]
        if error:
            body += ["", f"{C.RED}{C.BOLD}⚠ {error}{C.RESET}"]
        body += ["", field]
        render(stage, body, back="go back", cont="continue", focus=len(body) - 1)
        k = read_key()
        if k == KEY_ENTER:
            return "".join(buf).strip()
        elif k == KEY_ESC:
            raise Back
        elif k == KEY_BACKSPACE:
            error = None
            if buf:
                buf.pop()
        elif len(k) == 1 and k.isprintable():
            error = None
            buf.append(k)


def flash(stage, msg_lines):
    body = ["", *msg_lines, "", f"{C.DIM}Press any key to continue…{C.RESET}"]
    render(stage, body)
    read_key()
