#include "installer/platform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include "core/fsutil.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace photon {
namespace installer {
namespace platform {
namespace {

std::string FormatTime(const char* fmt) {
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    ::localtime_s(&tmv, &now);
#else
    ::localtime_r(&now, &tmv);
#endif
    char buf[128];
    const std::size_t n = std::strftime(buf, sizeof(buf), fmt, &tmv);
    return std::string(buf, n);
}

#ifndef _WIN32
// Launch a detached child without a shell, so a path containing a quote or a
// space cannot be reinterpreted as a command. The Python version used
// `os.system("open \"%s\"")`, which is exactly that hazard.
bool Spawn(const char* prog, const std::string& arg) {
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setsid();
        // The viewer must not write over our alt-screen UI.
        std::freopen("/dev/null", "w", stdout);
        std::freopen("/dev/null", "w", stderr);
        ::execlp(prog, prog, arg.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return true;
}
#endif

}  // namespace

void EnableVt() {
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);
    const HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (::GetConsoleMode(h, &mode) == 0) return;
    ::SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                            ENABLE_PROCESSED_OUTPUT);
#endif
}

void TerminalSize(int& cols, int& rows) {
    cols = 90;
    rows = 30;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(h, &info) != 0) {
        const int w = info.srWindow.Right - info.srWindow.Left + 1;
        const int t = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (w > 0) cols = w;
        if (t > 0) rows = t;
    }
#else
    struct winsize ws {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) cols = ws.ws_col;
        if (ws.ws_row > 0) rows = ws.ws_row;
    }
#endif
}

bool OpenUrl(const std::string& url) {
#ifdef _WIN32
    const std::wstring w = fsutil::PathFromUtf8(url).wstring();
    const HINSTANCE r =
        ::ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<std::intptr_t>(r) > 32;
#elif defined(__APPLE__)
    return Spawn("open", url);
#else
    return Spawn("xdg-open", url);
#endif
}

bool OpenInEditor(const std::string& pathUtf8) {
#ifdef _WIN32
    // ⚠ notepad.exe by name, never the shell association — see the header.
    const std::wstring w = fsutil::PathFromUtf8(pathUtf8).wstring();
    const std::wstring quoted = L"\"" + w + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"notepad.exe " + quoted;
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (ok == 0) return false;
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return true;
#elif defined(__APPLE__)
    return Spawn("open", pathUtf8);
#else
    return Spawn("xdg-open", pathUtf8);
#endif
}

std::string TempDir() {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::temp_directory_path(ec);
    if (!ec) return fsutil::PathToUtf8(p);
    return ".";
}

std::string TimestampCompact() { return FormatTime("%Y%m%d_%H%M%S"); }
std::string TimestampClock() { return FormatTime("%H:%M:%S"); }
std::string TimestampHuman() { return FormatTime("%a %b %d %H:%M:%S %Y"); }

std::string PlatformName() {
#ifdef _WIN32
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

}  // namespace platform
}  // namespace installer
}  // namespace photon
