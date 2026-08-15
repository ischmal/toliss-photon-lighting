// Terminal UI engine: ANSI color, cross-platform single-keystroke input, and a
// compose-then-blit layout engine (`Render`) with a fixed header/footer and a
// scrollable body.
//
// A 1:1 port of `installer/tui.py` (docs/installer_cpp_plan.md §4). The decision
// not to adopt FTXUI or notcurses was deliberate: every screen is written against
// this exact API, and a library would mean redesigning and re-QA-ing the look for
// no user-visible gain. Zero third-party dependencies — Win32 console API on
// Windows, `termios` on POSIX.
//
// ⚠ EVERY LENGTH HERE IS IN CODEPOINTS, NOT BYTES. The rules and arrows
// (`▁ ▔ ► ▲ ▼ ✓ ← →`) are multi-byte UTF-8 literals in this source, so a
// byte-counting `VisLen` would make the header rule three times too long and clip
// every row to a third of the window. (Like the Python original this still assumes
// one column per codepoint, so a full-width CJK glyph in an aircraft folder name
// measures narrow — the same behaviour, ported rather than fixed.)
#pragma once

#include <set>
#include <string>
#include <vector>

namespace photon {
namespace installer {
namespace tui {

// ─── layout constants ────────────────────────────────────────────────────────
// The layout spans the full detected terminal width — the header/footer rules
// reach the window edge. This is only an absolute floor for absurd terminals.
constexpr int kMinWidth = 40;

// Body content is inset this many columns from the left; the rules are NOT inset.
constexpr int kBodyIndent = 1;

// "  ► 1. " — options and table headers align to this column.
constexpr int kPrefix = 7;

// Breadcrumb stages, left to right.
const std::vector<std::string>& Stages();

// ─── ANSI color ─────────────────────────────────────────────────────────────
namespace C {
constexpr char kReset[] = "\033[0m";
constexpr char kBold[] = "\033[1m";
constexpr char kDim[] = "\033[2m";
constexpr char kUnder[] = "\033[4m";
constexpr char kRed[] = "\033[31m";
constexpr char kGreen[] = "\033[32m";
constexpr char kYellow[] = "\033[33m";
constexpr char kWhite[] = "\033[37m";     // light gray
constexpr char kCyan[] = "\033[36m";
constexpr char kBrCyan[] = "\033[96m";
constexpr char kBrGreen[] = "\033[92m";
constexpr char kBrYellow[] = "\033[93m";
constexpr char kBrWhite[] = "\033[97m";
constexpr char kBrBlack[] = "\033[90m";   // gray
}  // namespace C

// ─── terminal lifecycle ──────────────────────────────────────────────────────
void EnterAltScreen();
void LeaveAltScreen();
void HideCursor();
void ShowCursor();

// Cursor back on, alt screen left — the exact ordering `_quit` uses, and what a
// Ctrl-C must also perform. Written with raw `write()`/`WriteFile` so it is safe
// from a signal handler.
void RestoreTerminal();

// ⚠ CTRL-C MUST LEAVE THE TERMINAL AS AN ESC-QUIT DOES. On POSIX raw mode
// disables ISIG so `\x03` arrives as a keystroke (see kKeyInterrupt); between
// reads it is a real SIGINT, and on Windows `_getwch` cannot see Ctrl-C at all —
// it goes to the console control handler. All three routes end here.
using QuitHook = void (*)();
void SetQuitHook(QuitHook hook);
void InstallInterruptHandler();

// ─── keys ────────────────────────────────────────────────────────────────────
// A key is either a normalized token or the literal (UTF-8) character typed —
// the same representation the Python original returns, which is what lets the
// screens read `k == kKeyEnter` and `k.size() == 1` side by side.
extern const char kKeyUp[];
extern const char kKeyDown[];
extern const char kKeyLeft[];
extern const char kKeyRight[];
extern const char kKeyEnter[];
extern const char kKeyEsc[];
extern const char kKeyBackspace[];
extern const char kKeyPgUp[];
extern const char kKeyPgDn[];
extern const char kKeyHome[];
extern const char kKeyEnd[];
extern const char kKeyInterrupt[];   // Ctrl-C seen as a keystroke (POSIX raw mode)

// Block for one keypress. An unrecognized escape sequence returns "".
std::string ReadKey();

// Like ReadKey() but waits at most `seconds`, returning "" if nothing arrives —
// which lets a caller poll something else (is X-Plane still running?) and
// re-render between checks while still responding instantly to a keypress.
//
// ⚠ The decode itself is DELEGATED to ReadKey once input is known to be waiting,
// so key handling stays in exactly one place.
std::string ReadKeyTimeout(double seconds);

// ─── ANSI-aware string helpers ───────────────────────────────────────────────
// Printable length in codepoints, ignoring ANSI escape sequences.
int VisLen(const std::string& s);

// Truncate to `width` printable columns, preserving ANSI codes; always terminates
// with RESET so a cut mid-color cannot bleed into the next row.
std::string Clip(const std::string& s, int width);

// The SGR codes still in effect at the end of `s`; a RESET clears the set. Used to
// re-open the live color on a wrapped continuation line.
std::string ActiveSgr(const std::string& s);

// Wrap to `width` visible columns: break at the last space that fits, else
// hard-break an over-long run. Escapes are zero-width and never split, and each
// continuation line is re-opened with whatever color was active at the break.
std::vector<std::string> WrapAnsi(const std::string& s, int width);

// Left- and right-justify two fragments within `width`. Not enough room drops the
// right fragment rather than overflowing the line.
std::string Row(const std::string& left, const std::string& right, int width);

// The shared "Press <KEY> to <action>" coloring — dim "Press", bold key, dim
// action. Reused in the launch body so the scheme is learned on the first screen.
std::string PressHint(const std::string& key, const std::string& action);

// A step heading, and one option row.
std::string Step(const std::string& text);
std::string Option(int i, const std::string& text, bool selected);

// ─── the layout engine ───────────────────────────────────────────────────────
// (columns, rows) for the frame. ⚠ The width is `cols - 1`: writing the last
// column of a line makes most terminals wrap to the next row, which would push
// the whole frame down by one line per row drawn.
void Dims(int& width, int& height);

constexpr int kNoStage = -1;
constexpr int kNoFocus = -1;

struct RenderOpts {
    std::string back;             // "" = no back hint
    std::string cont;             // "" = no continue hint
    std::string contKey = "ENTER";
    int focus = kNoFocus;         // index into `body` (pre-wrap) to keep on screen
    bool center = false;
};

// Compose one full frame — header pinned top, footer pinned bottom, body
// (scrolled if needed) between — and blit it with a SINGLE write. Composing then
// blitting is what keeps the frame from visibly tearing as it is drawn.
void Render(int stage, const std::vector<std::string>& body,
            const RenderOpts& opts = RenderOpts());

// The frame Render blits, as a string.
//
// ⚠ SPLIT OUT SO THE LAYOUT IS TESTABLE WITHOUT A TERMINAL. The composition — the
// scroll window, the focus remap through wrapping, the per-row clip — is where a
// port goes wrong in ways no individual helper test catches, and capturing stdout
// to check it would be worse than not checking it.
std::string ComposeFrame(int stage, const std::vector<std::string>& body,
                         const RenderOpts& opts = RenderOpts());

// ─── screens' building blocks ────────────────────────────────────────────────
// Thrown by a screen on ESC: return to the previous screen. A distinct type
// rather than a sentinel return because it has to unwind through the nested
// prompts the Perform screen opens.
struct Back {};

struct MenuOpts {
    std::string back = "go back";
    std::string cont = "continue";
    std::vector<std::string> headerLines;   // static rows above the options
    std::vector<std::string> footerLines;   // static rows below the options
    bool allowBack = true;
    std::set<int> disabled;
    int index = 0;                          // initially-highlighted option
};

// Arrow/number-navigable menu. Returns the chosen index; throws Back on ESC.
int Menu(int stage, const std::string& step, const std::string& prompt,
         const std::vector<std::string>& options, const MenuOpts& opts = MenuOpts());

// A single-line text field. `error`, if given, renders as a warning above the
// field and CLEARS ITSELF the moment the user edits the text, so it never lingers
// once they have started fixing it. Throws Back on ESC.
std::string TextPrompt(int stage, const std::string& step, const std::string& prompt,
                       const std::string& initial = "",
                       const std::string& error = "");

void Flash(int stage, const std::vector<std::string>& lines);

}  // namespace tui
}  // namespace installer
}  // namespace photon
