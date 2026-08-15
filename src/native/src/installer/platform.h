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

// ⚠ FOR THE WINDOWS SUBSYSTEM BUILD ONLY (`-DPHOTON_GUI=ON`), and it must run
// BEFORE any CLI output. A GUI-subsystem binary is started with no console
// attached, so `photon-installer detect --json` run from an existing terminal
// writes to nothing at all and exits 0 — output silently lost, which reads as the
// subcommand being broken. This re-attaches the parent's console.
//
// ⚠ IT MUST NOT TOUCH A REDIRECTED STREAM. When stdout is a pipe or a file the
// handle is inherited and already correct; reopening it onto CONOUT$ would send
// `--json` to the terminal instead of into the caller's pipe. Each of the three
// standard streams is therefore re-pointed only when it has no handle of its own.
//
// Returns false when there was no parent console to attach to (a double-click),
// which is not an error — it is the normal GUI case. No-op off Windows.
bool AttachParentConsole();

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

// Show a DIRECTORY in the platform's file browser — the aircraft list's
// "Open aircraft folder..." context-menu row.
//
// ⚠ NOT `OpenInEditor` WITH A DIRECTORY, and not `OpenUrl` with a path either,
// even though all three end up in the same two system calls. `OpenInEditor` hands
// its argument to notepad.exe by name, which would open a Save-As-style dialog on
// a folder; `OpenUrl` documents its argument as a URL, and a Windows path with a
// space in it is not one. The shell association this relies on is the only one
// that is always registered — `Directory\shell\open` — which is exactly why the
// association hazard written up on `OpenInEditor` does not apply here.
bool OpenFolder(const std::string& pathUtf8);

// Put UTF-8 text on the system clipboard — the installer log's "Copy log".
//
// ⚠ NOT SLINT'S. Slint exposes the clipboard only on its `Platform` interface, for
// code that IMPLEMENTS a backend; there is no app-facing setter, and the one place
// the toolkit uses it (`TextInput::copy`) is unreachable now that the log is drawn
// as colored rows rather than as one editable string. Hence a system call here.
//
// Best effort like everything else in this file: false when the platform call
// failed, and never a throw.
bool SetClipboardText(const std::string& textUtf8);

// ─── the folder chooser (GUI only) ───────────────────────────────────────────
// A native "pick a folder" dialog, for the Browse button on the X-Plane directory
// screen. Returns the chosen path as UTF-8, or "" if the user cancelled.
//
// ⚠ WINDOWS ONLY TODAY, and it returns "" everywhere else rather than pretending.
// That degrades to typing the path into the field, which is why the field is
// editable and not a read-only display of whatever the dialog returned — the
// screen has to stay usable on a platform with no picker behind it.
std::string PickFolder(const std::string& titleUtf8, const std::string& startUtf8);

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
