#include "installer/tui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#include "core/version.h"
#include "installer/platform.h"

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace photon {
namespace installer {
namespace tui {

const char kKeyUp[] = "UP";
const char kKeyDown[] = "DOWN";
const char kKeyLeft[] = "LEFT";
const char kKeyRight[] = "RIGHT";
const char kKeyEnter[] = "ENTER";
const char kKeyEsc[] = "ESC";
const char kKeyBackspace[] = "BACKSPACE";
const char kKeyPgUp[] = "PGUP";
const char kKeyPgDn[] = "PGDN";
const char kKeyHome[] = "HOME";
const char kKeyEnd[] = "END";
const char kKeyInterrupt[] = "INTERRUPT";

namespace {

QuitHook gQuitHook = nullptr;

void RawWrite(const char* s) {
#ifdef _WIN32
    DWORD written = 0;
    ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), s,
                static_cast<DWORD>(std::strlen(s)), &written, nullptr);
#else
    const ssize_t n = ::write(STDOUT_FILENO, s, std::strlen(s));
    (void)n;
#endif
}

void Emit(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

// ─── UTF-8 ───────────────────────────────────────────────────────────────────
// Index just past the codepoint starting at `i`. A malformed lead byte advances
// by one, so a corrupt name can never spin this forever.
std::size_t Utf8Next(const std::string& s, std::size_t i) {
    if (i >= s.size()) return s.size();
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    return std::min(s.size(), i + len);
}

// An ANSI SGR sequence starting at `i` ("\033[…m"), or 0 if there is not one.
// Matches the Python `\033\[[0-9;]*m`.
std::size_t SgrLen(const std::string& s, std::size_t i) {
    if (i + 1 >= s.size() || s[i] != '\033' || s[i + 1] != '[') return 0;
    std::size_t j = i + 2;
    while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) != 0 ||
                            s[j] == ';')) {
        ++j;
    }
    if (j < s.size() && s[j] == 'm') return j + 1 - i;
    return 0;
}

std::string Utf8FromCodepoint(unsigned int cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// `glyph` repeated `n` times — the rules are multi-byte, so this cannot be
// std::string(n, ch).
std::string Repeat(const char* glyph, int n) {
    std::string out;
    if (n <= 0) return out;
    out.reserve(std::strlen(glyph) * static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out += glyph;
    return out;
}

// ─── key decoding ────────────────────────────────────────────────────────────
#ifndef _WIN32
class RawMode {
public:
    RawMode() : ok_(::tcgetattr(STDIN_FILENO, &old_) == 0) {
        if (!ok_) return;
        struct termios raw = old_;
        // ⚠ SPELLED OUT RATHER THAN `cfmakeraw`, for two reasons. It is these exact
        // flags that Python's `tty.setraw` sets, so this stays the 1:1 port it
        // claims to be; and `cfmakeraw` is a BSD extension that glibc hides under
        // feature-test macros, so calling it makes the build depend on which
        // `-std=` the toolchain happened to pick.
        //
        // ⚠ Clearing ISIG is what turns Ctrl-C into the `\x03` byte ReadKey decodes
        // — not an oversight, and the reason the interrupt handling has three
        // routes rather than one.
        raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
        raw.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB);
        raw.c_cflag |= static_cast<tcflag_t>(CS8);
        raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 1;    // block until one byte is available…
        raw.c_cc[VTIME] = 0;   // …with no inter-byte timer
        ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    ~RawMode() {
        if (ok_) ::tcsetattr(STDIN_FILENO, TCSADRAIN, &old_);
    }
    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;

private:
    struct termios old_ {};
    bool ok_ = false;
};

// One byte, blocking. Returns false at EOF.
bool ReadByte(char& out) {
    const ssize_t n = ::read(STDIN_FILENO, &out, 1);
    return n == 1;
}

// Is a byte already waiting? Used to tell a lone ESC from the head of a sequence.
bool ByteWaiting(double seconds) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(seconds);
    tv.tv_usec = static_cast<suseconds_t>((seconds - tv.tv_sec) * 1e6);
    return ::select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
}
#endif

}  // namespace

const std::vector<std::string>& Stages() {
    static const std::vector<std::string> kStages = {"Root", "Aircraft",
                                                     "Configure", "Run", "Complete"};
    return kStages;
}

// ─── terminal lifecycle ──────────────────────────────────────────────────────
void EnterAltScreen() { Emit("\033[?1049h"); }
void LeaveAltScreen() { Emit("\033[?1049l"); }
void HideCursor() { Emit("\033[?25l"); }
void ShowCursor() { Emit("\033[?25h"); }

void RestoreTerminal() {
    RawWrite("\033[?25h");     // cursor first, then leave — `_quit`'s ordering
    RawWrite("\033[?1049l");
}

void SetQuitHook(QuitHook hook) { gQuitHook = hook; }

namespace {

void HandleInterrupt() {
    if (gQuitHook != nullptr) {
        gQuitHook();
    } else {
        RestoreTerminal();
    }
}

#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        HandleInterrupt();
        ::ExitProcess(0);
    }
    return FALSE;
}
#else
extern "C" void SigIntHandler(int) {
    HandleInterrupt();
    ::_exit(0);
}
#endif

}  // namespace

void InstallInterruptHandler() {
#ifdef _WIN32
    // ⚠ REQUIRED ON WINDOWS, not merely nice: `_getwch` cannot see Ctrl-C at all
    // (it goes to the console control handler), so without this the alt screen is
    // never left and the user's terminal is wrecked.
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
    // Raw mode clears ISIG, so during a key read Ctrl-C arrives as `\x03` and is
    // decoded below. This covers the gaps in between — rendering, the filesystem
    // work — where it is still a real signal.
    std::signal(SIGINT, SigIntHandler);
#endif
}

// ─── keys ────────────────────────────────────────────────────────────────────
std::string ReadKey() {
#ifdef _WIN32
    const wint_t ch = static_cast<wint_t>(::_getwch());
    if (ch == 0x00 || ch == 0xE0) {
        // ⚠ THE EXTENDED-KEY PREFIX PAIR. An arrow or navigation key arrives as
        // TWO calls: a 0x00/0xE0 lead followed by the scan code. Reading only the
        // first leaves the second in the buffer, where it reads back as a stray
        // literal character on the next keypress.
        const wint_t ch2 = static_cast<wint_t>(::_getwch());
        switch (ch2) {
            case L'H': return kKeyUp;
            case L'P': return kKeyDown;
            case L'K': return kKeyLeft;
            case L'M': return kKeyRight;
            case L'I': return kKeyPgUp;
            case L'Q': return kKeyPgDn;
            case L'G': return kKeyHome;
            case L'O': return kKeyEnd;
            default: return std::string();
        }
    }
    if (ch == L'\r' || ch == L'\n') return kKeyEnter;
    if (ch == 0x1B) return kKeyEsc;
    if (ch == 0x08 || ch == 0x7F) return kKeyBackspace;
    if (ch == 0x03) return kKeyInterrupt;
    return Utf8FromCodepoint(static_cast<unsigned int>(ch));
#else
    RawMode raw;
    char c = 0;
    if (!ReadByte(c)) return std::string();
    if (c == '\033') {
        // ⚠ THE LONE-ESC PROBE. ESC is both a key and the head of every arrow
        // sequence; the only way to tell them apart is whether more bytes are
        // already waiting. The window is deliberately short — long enough for a
        // terminal that has already queued the whole sequence, too short to add a
        // felt delay to pressing ESC.
        if (!ByteWaiting(0.0005)) return kKeyEsc;
        char seq = 0;
        if (!ReadByte(seq)) return kKeyEsc;
        if (seq != '[') return std::string();
        char code = 0;
        if (!ReadByte(code)) return std::string();
        if (code >= '0' && code <= '9') {   // e.g. ESC[5~ = PgUp
            char tilde = 0;
            ReadByte(tilde);
            switch (code) {
                case '5': return kKeyPgUp;
                case '6': return kKeyPgDn;
                case '1': return kKeyHome;
                case '4': return kKeyEnd;
                default: return std::string();
            }
        }
        switch (code) {
            case 'A': return kKeyUp;
            case 'B': return kKeyDown;
            case 'C': return kKeyRight;
            case 'D': return kKeyLeft;
            case 'H': return kKeyHome;
            case 'F': return kKeyEnd;
            default: return std::string();
        }
    }
    if (c == '\r' || c == '\n') return kKeyEnter;
    if (c == 0x7F || c == 0x08) return kKeyBackspace;
    if (c == 0x03) return kKeyInterrupt;
    // A typed character may be multi-byte: read the continuation bytes so a
    // non-ASCII path typed into the manual-path prompt arrives whole.
    std::string out(1, c);
    const unsigned char lead = static_cast<unsigned char>(c);
    int extra = 0;
    if ((lead & 0xE0) == 0xC0) extra = 1;
    else if ((lead & 0xF0) == 0xE0) extra = 2;
    else if ((lead & 0xF8) == 0xF0) extra = 3;
    for (int i = 0; i < extra; ++i) {
        char cont = 0;
        if (!ReadByte(cont)) break;
        out += cont;
    }
    return out;
#endif
}

std::string ReadKeyTimeout(double seconds) {
#ifdef _WIN32
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        if (::_kbhit() != 0) return ReadKey();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::string();
#else
    {
        // ⚠ Wait for input IN RAW MODE so a single keystroke wakes select — in
        // cooked mode nothing arrives until the user presses Enter, and the
        // "close X-Plane and I will continue automatically" screen would hang on
        // the poll instead of ticking. The byte stays buffered for ReadKey().
        RawMode raw;
        if (!ByteWaiting(seconds)) return std::string();
    }
    return ReadKey();
#endif
}

// ─── ANSI-aware string helpers ───────────────────────────────────────────────
int VisLen(const std::string& s) {
    int out = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\033') {
            // Skip to the sequence terminator, exactly as the Python does — any
            // escape, not only SGR.
            while (i < s.size() && s[i] != 'm') ++i;
            if (i < s.size()) ++i;
            continue;
        }
        i = Utf8Next(s, i);
        ++out;
    }
    return out;
}

std::string Clip(const std::string& s, int width) {
    std::string out;
    int vis = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\033') {
            std::size_t j = i;
            while (j < s.size() && s[j] != 'm') ++j;
            if (j < s.size()) ++j;
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        if (vis >= width) break;
        const std::size_t next = Utf8Next(s, i);
        out.append(s, i, next - i);
        ++vis;
        i = next;
    }
    return out + C::kReset;
}

std::string ActiveSgr(const std::string& s) {
    std::string codes;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t len = SgrLen(s, i);
        if (len == 0) {
            i = Utf8Next(s, i);
            continue;
        }
        const std::string seq = s.substr(i, len);
        if (seq == "\033[0m" || seq == "\033[m") {
            codes.clear();
        } else {
            codes += seq;
        }
        i += len;
    }
    return codes;
}

std::vector<std::string> WrapAnsi(const std::string& s, int width) {
    if (width <= 0 || VisLen(s) <= width) return {s};

    std::vector<std::pair<std::size_t, std::size_t>> slices;
    std::size_t lineStart = 0;
    int vis = 0;
    std::ptrdiff_t lastSpace = -1;
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const std::size_t sgr = SgrLen(s, i);
        if (sgr != 0) {
            i += sgr;
            continue;
        }
        if (vis >= width) {  // this codepoint is the (width+1)-th visible column
            if (s[i] == ' ') {
                // The line is exactly full and this space is the separator — break
                // here (keeping the word that just fit) and drop the space.
                slices.emplace_back(lineStart, i);
                lineStart = i + 1;
            } else if (lastSpace > static_cast<std::ptrdiff_t>(lineStart)) {
                slices.emplace_back(lineStart, static_cast<std::size_t>(lastSpace));
                lineStart = static_cast<std::size_t>(lastSpace) + 1;
            } else {
                slices.emplace_back(lineStart, i);   // hard-break a long run
                lineStart = i;
            }
            vis = 0;
            lastSpace = -1;
            i = lineStart;
            continue;
        }
        if (s[i] == ' ') lastSpace = static_cast<std::ptrdiff_t>(i);
        ++vis;
        i = Utf8Next(s, i);
    }
    if (lineStart < n || slices.empty()) slices.emplace_back(lineStart, n);

    std::vector<std::string> out;
    out.reserve(slices.size());
    for (std::size_t k = 0; k < slices.size(); ++k) {
        const std::size_t a = slices[k].first;
        const std::size_t b = slices[k].second;
        if (k == 0) {
            out.push_back(s.substr(a, b - a));
        } else {
            // ⚠ RE-OPEN THE LIVE COLOR. A wrapped colored paragraph otherwise
            // loses its color from the second line down, because the SGR that
            // opened it is back on line one.
            out.push_back(ActiveSgr(s.substr(0, a)) + s.substr(a, b - a));
        }
    }
    return out;
}

std::string Row(const std::string& left, const std::string& right, int width) {
    const int gap = width - VisLen(left) - VisLen(right);
    if (gap < 1) return left;   // not enough room; drop the right fragment
    return left + std::string(static_cast<std::size_t>(gap), ' ') + right;
}

std::string PressHint(const std::string& key, const std::string& action) {
    return std::string(C::kDim) + "Press" + C::kReset + " " + C::kBold + key +
           C::kReset + C::kDim + " to " + action + C::kReset;
}

std::string Step(const std::string& text) {
    return std::string(C::kBold) + C::kBrCyan + text + C::kReset;
}

std::string Option(int i, const std::string& text, bool selected) {
    const std::string num = std::to_string(i + 1);
    if (selected) {
        return std::string("  ") + C::kBrGreen + "►" + C::kReset + " " + C::kBrWhite +
               C::kBold + num + "." + C::kReset + " " + C::kBrWhite + text + C::kReset;
    }
    return std::string("    ") + C::kDim + num + "." + C::kReset + " " + text;
}

// ─── the layout engine ───────────────────────────────────────────────────────
void Dims(int& width, int& height) {
    int cols = 90;
    int rows = 30;
    platform::TerminalSize(cols, rows);
    // ⚠ `cols - 1`: writing the last column wraps on most terminals, which would
    // push every subsequent row down one line and shear the frame.
    width = std::max(kMinWidth, cols - 1);
    height = std::max(rows, 10);
}

namespace {

std::string Breadcrumb(int active) {
    const std::vector<std::string>& stages = Stages();
    std::string out;
    const std::string sep = std::string(C::kBrBlack) + " / " + C::kReset;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (!out.empty()) out += sep;
        const int idx = static_cast<int>(i);
        if (active >= 0 && idx < active) {
            out += std::string(C::kGreen) + stages[i] + C::kReset;
        } else if (active >= 0 && idx == active) {
            out += std::string(C::kBold) + C::kUnder + C::kBrCyan + stages[i] +
                   C::kReset;
        } else {
            out += std::string(C::kBrBlack) + stages[i] + C::kReset;
        }
    }
    return out;
}

std::vector<std::string> HeaderRows(int active, int width) {
    const std::string title = std::string(C::kBold) + C::kBrWhite + "ToLiss Photon " +
                              kPhotonVersion + C::kReset + C::kDim + " - Installer" +
                              C::kReset;
    return {Row(title, Breadcrumb(active), width),
            std::string(C::kBrBlack) + Repeat("▁", width) + C::kReset};
}

std::vector<std::string> FooterRows(int width, const std::string& back,
                                    const std::string& cont,
                                    const std::string& contKey) {
    const std::string left =
        back.empty() ? std::string()
                     : std::string(C::kBrYellow) + "←" + C::kReset + " " +
                           PressHint("ESC", back);
    const std::string right =
        cont.empty() ? std::string()
                     : PressHint(contKey, cont) + " " + C::kBrGreen + "→" + C::kReset;
    // A one-space margin each side; the continue hint is offset one space in from
    // the right edge to mirror the back hint.
    const std::string keys = " " + Row(left, right, width - 2) + " ";
    // The trailing blank line is always present so the hints never sit flush
    // against the terminal's bottom edge.
    return {std::string(C::kBrBlack) + Repeat("▔", width) + C::kReset, keys, ""};
}

// The visible slice of `lines`, scrolled so `focus` stays on screen, plus whether
// content was cut off above and below.
void Window(const std::vector<std::string>& lines, int height, int focus,
            std::vector<std::string>& out, bool& moreAbove, bool& moreBelow) {
    const int n = static_cast<int>(lines.size());
    if (n <= height) {
        out = lines;
        moreAbove = false;
        moreBelow = false;
        return;
    }
    const int f = focus < 0 ? 0 : focus;
    const int off = std::max(0, std::min(f - height / 2, n - height));
    out.assign(lines.begin() + off, lines.begin() + off + height);
    moreAbove = off > 0;
    moreBelow = off + height < n;
}

}  // namespace

void Render(int stage, const std::vector<std::string>& body, const RenderOpts& o) {
    Emit(ComposeFrame(stage, body, o));
}

std::string ComposeFrame(int stage, const std::vector<std::string>& body,
                         const RenderOpts& o) {
    int width = 0;
    int height = 0;
    Dims(width, height);
    std::vector<std::string> grid(static_cast<std::size_t>(height));

    const std::vector<std::string> head =
        stage == kNoStage ? std::vector<std::string>() : HeaderRows(stage, width);
    const std::vector<std::string> foot =
        FooterRows(width, o.back, o.cont, o.contKey);
    for (std::size_t i = 0; i < head.size() && i < grid.size(); ++i) grid[i] = head[i];
    for (std::size_t i = 0; i < foot.size(); ++i) {
        const std::size_t r = grid.size() - foot.size() + i;
        if (r < grid.size()) grid[r] = foot[i];
    }

    const int bodyTop = static_cast<int>(head.size()) + 1;       // blank under head
    const int bodyBottom = height - static_cast<int>(foot.size()) - 1;  // blank above
    const int regionH = std::max(1, bodyBottom - bodyTop + 1);

    // Wrap the body to the indented content width, remapping `focus` (an index into
    // the PRE-WRAP body) onto the physical line it now begins at, so the scroll
    // window still keeps the focused option on screen.
    const int contentW = std::max(1, width - kBodyIndent);
    std::vector<std::string> phys;
    int physFocus = kNoFocus;
    for (std::size_t origI = 0; origI < body.size(); ++origI) {
        if (o.focus != kNoFocus && static_cast<int>(origI) == o.focus) {
            physFocus = static_cast<int>(phys.size());
        }
        for (std::string& w : WrapAnsi(body[origI], contentW)) {
            phys.push_back(std::move(w));
        }
    }

    std::vector<std::string> win;
    bool up = false;
    bool down = false;
    Window(phys, regionH, physFocus, win, up, down);
    int start = bodyTop;
    if (o.center && static_cast<int>(win.size()) < regionH) {
        start += (regionH - static_cast<int>(win.size())) / 2;
    }
    if (up && !win.empty()) win.front() = std::string(C::kBrBlack) + "▲  more above" + C::kReset;
    if (down && !win.empty()) win.back() = std::string(C::kBrBlack) + "▼  more below" + C::kReset;

    const std::string indent(kBodyIndent, ' ');
    for (std::size_t i = 0; i < win.size(); ++i) {
        const int r = start + static_cast<int>(i);
        if (r >= bodyTop && r <= bodyBottom && r >= 0 &&
            r < static_cast<int>(grid.size())) {
            grid[static_cast<std::size_t>(r)] = indent + win[i];
        }
    }

    std::string out = "\033[H";
    for (std::size_t i = 0; i < grid.size(); ++i) {
        out += "\033[" + std::to_string(i + 1) + ";1H";
        out += Clip(grid[i], width);
        out += "\033[K";
    }
    out += "\033[J";
    return out;
}

// ─── screens' building blocks ────────────────────────────────────────────────
int Menu(int stage, const std::string& step, const std::string& prompt,
         const std::vector<std::string>& options, const MenuOpts& o) {
    if (options.empty()) return -1;
    const int n = static_cast<int>(options.size());
    if (static_cast<int>(o.disabled.size()) >= n) return -1;  // nothing selectable

    int idx = (o.index >= 0 && o.index < n) ? o.index : 0;
    while (o.disabled.count(idx) > 0) idx = (idx + 1) % n;

    while (true) {
        std::vector<std::string> body{""};
        if (!step.empty()) {
            body.push_back(Step(step));
            body.push_back("");
        }
        if (!prompt.empty()) {
            body.push_back(prompt);
            body.push_back("");
        }
        for (const std::string& l : o.headerLines) body.push_back(l);
        const int optStart = static_cast<int>(body.size());
        for (int i = 0; i < n; ++i) {
            if (o.disabled.count(i) > 0) {
                body.push_back(std::string("    ") + C::kBrBlack +
                               std::to_string(i + 1) + ". " + options[i] + C::kReset);
            } else {
                body.push_back(Option(i, options[i], i == idx));
            }
        }
        for (const std::string& l : o.footerLines) body.push_back(l);

        RenderOpts ro;
        ro.back = o.allowBack ? o.back : std::string();
        ro.cont = o.cont;
        ro.focus = optStart + idx;
        Render(stage, body, ro);

        const std::string k = ReadKey();
        if (k == kKeyUp) {
            do {
                idx = (idx - 1 + n) % n;
            } while (o.disabled.count(idx) > 0);
        } else if (k == kKeyDown) {
            do {
                idx = (idx + 1) % n;
            } while (o.disabled.count(idx) > 0);
        } else if (k == kKeyHome) {
            for (int i = 0; i < n; ++i) {
                if (o.disabled.count(i) == 0) { idx = i; break; }
            }
        } else if (k == kKeyEnd) {
            for (int i = n - 1; i >= 0; --i) {
                if (o.disabled.count(i) == 0) { idx = i; break; }
            }
        } else if (k.size() == 1 && k[0] >= '1' && k[0] <= '9') {
            const int pick = k[0] - '1';
            if (pick < n && o.disabled.count(pick) == 0) return pick;
        } else if (k == kKeyEnter && o.disabled.count(idx) == 0) {
            return idx;
        } else if (k == kKeyEsc && o.allowBack) {
            throw Back{};
        } else if (k == kKeyInterrupt) {
            HandleInterrupt();
            std::exit(0);
        }
    }
}

std::string TextPrompt(int stage, const std::string& step, const std::string& prompt,
                       const std::string& initial, const std::string& error) {
    std::string buf = initial;
    std::string err = error;
    while (true) {
        const std::string field = std::string("  ") + C::kBrGreen + "►" + C::kReset +
                                  " " + C::kBrWhite + buf + C::kReset +
                                  "\033[7m \033[0m";   // the block cursor
        std::vector<std::string> body{"", Step(step), "", prompt};
        if (!err.empty()) {
            body.push_back("");
            body.push_back(std::string(C::kRed) + C::kBold + "⚠ " + err + C::kReset);
        }
        body.push_back("");
        body.push_back(field);

        RenderOpts ro;
        ro.back = "go back";
        ro.cont = "continue";
        ro.focus = static_cast<int>(body.size()) - 1;
        Render(stage, body, ro);

        const std::string k = ReadKey();
        if (k == kKeyEnter) {
            // Trim, so a trailing space picked up from a drag-and-drop does not
            // become part of the path.
            const std::size_t a = buf.find_first_not_of(" \t");
            const std::size_t b = buf.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : buf.substr(a, b - a + 1);
        }
        if (k == kKeyEsc) throw Back{};
        if (k == kKeyInterrupt) {
            HandleInterrupt();
            std::exit(0);
        }
        if (k == kKeyBackspace) {
            err.clear();
            if (!buf.empty()) {
                // Back over a whole codepoint, not a byte.
                std::size_t i = buf.size() - 1;
                while (i > 0 && (static_cast<unsigned char>(buf[i]) & 0xC0) == 0x80) --i;
                buf.erase(i);
            }
            continue;
        }
        // A printable literal: anything that is not a control byte and not one of
        // the tokens above.
        if (!k.empty() && static_cast<unsigned char>(k[0]) >= 0x20 &&
            static_cast<unsigned char>(k[0]) != 0x7F && VisLen(k) == 1) {
            err.clear();
            buf += k;
        }
    }
}

void Flash(int stage, const std::vector<std::string>& lines) {
    std::vector<std::string> body{""};
    for (const std::string& l : lines) body.push_back(l);
    body.push_back("");
    body.push_back(std::string(C::kDim) + "Press any key to continue…" + C::kReset);
    Render(stage, body);
    ReadKey();
}

}  // namespace tui
}  // namespace installer
}  // namespace photon
