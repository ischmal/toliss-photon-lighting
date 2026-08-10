// The OS-specific glue the TUI and the screens need — everything in the port that
// is a different system call per platform, and nothing else.
//
// Each function is the C++ half of one row in `docs/installer_cpp_plan.md` §4's
// platform table. Keeping them here rather than scattered behind `#ifdef`s through
// tui.cpp and screens.cpp is what makes that table checkable against the code.
//
// ⚠ NOTHING HERE MAY FAIL AN INSTALL. Opening a browser, opening the log, and
// asking whether X-Plane is running are all conveniences: every one of them
// returns a best-effort answer and none of them throws.
#pragma once

#include <string>

namespace photon {
namespace installer {
namespace platform {

// ─── console setup ───────────────────────────────────────────────────────────
// ANSI escape processing plus UTF-8 output. On Windows both halves are needed and
// both are easy to forget:
//
// ⚠ Without ENABLE_VIRTUAL_TERMINAL_PROCESSING every escape sequence prints as
// literal garbage — the screen fills with `←[1m`. Without CP_UTF8 the box-drawing
// rules and arrows (`▁ ▔ ► ▲ ▼ ✓ ←`) come out as mojibake, because the source
// holds them as multi-byte UTF-8 literals.
void EnableVt();

// The window, as (columns, rows). Falls back to 90x30 when it cannot be
// determined — a pipe, a CI runner, a terminal that does not answer.
void TerminalSize(int& cols, int& rows);

// ─── opening things ──────────────────────────────────────────────────────────
// Best effort, non-blocking, never throws. Returns false when the platform call
// itself failed; a browser that opens and then closes is still `true`.
bool OpenUrl(const std::string& url);

// ⚠ WINDOWS DELIBERATELY USES notepad.exe, NOT THE SHELL ASSOCIATION. A `.log`
// file frequently has no registered association, and ShellExecute then pops the
// "How do you want to open this file?" chooser: the log never opens, and that
// shell interaction disturbs the console — the reported "opening the log scrolls
// the header off the top" bug. notepad.exe is always present, opens any text file
// regardless of association, and is a GUI app so it never writes to our console.
bool OpenInEditor(const std::string& pathUtf8);

// ─── misc ────────────────────────────────────────────────────────────────────
// The OS temp directory, UTF-8. Where the installer log lands.
std::string TempDir();

// `YYYYmmdd_HHMMSS` and `HH:MM:SS` in local time — the log's filename stamp and
// its per-line stamp.
std::string TimestampCompact();
std::string TimestampClock();
std::string TimestampHuman();

// The platform name for the log header ("win32", "darwin", "linux").
std::string PlatformName();

}  // namespace platform
}  // namespace installer
}  // namespace photon
