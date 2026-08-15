#include "installer/platform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

#include "core/fsutil.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
// The Vista-era folder picker (IFileOpenDialog + FOS_PICKFOLDERS). Deliberately
// NOT SHBrowseForFolder, whose dialog has looked out of place since Windows 7 and
// cannot be resized or typed into.
#include <shlobj.h>
#include <shobjidl.h>
#else
#include <sys/ioctl.h>
#include <sys/wait.h>
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

// Run a child to completion and capture its stdout. ⚠ NO SHELL, for the reason
// `Spawn` states above and more sharply: `startUtf8` is a path the user chose, and
// building a command string around it is the injection hazard. argv is built
// BEFORE the fork so the child does nothing but dup/exec — after a fork in a
// threaded process (Slint owns several), allocating is not async-signal-safe.
//
// False means "no answer": failed to start, exited non-zero — which is how every
// picker here reports CANCELLED — or printed nothing. All three are the same thing
// to the caller, and none of them is an error worth surfacing.
bool RunCapture(const char* prog, const std::vector<std::string>& args,
                std::string* out) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(prog));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int fds[2];
    if (::pipe(fds) != 0) return false;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);
        ::execvp(prog, argv.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    std::string buf;
    char chunk[512];
    ssize_t n;
    while ((n = ::read(fds[0], chunk, sizeof(chunk))) > 0) {
        buf.append(chunk, static_cast<std::size_t>(n));
    }
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r')) buf.pop_back();
    if (buf.empty()) return false;
    *out = buf;
    return true;
}

#ifdef __APPLE__
// Escape for an AppleScript double-quoted string literal. The shell is out of the
// picture (RunCapture execs osascript directly), but the script text still has one
// level of quoting of its own, and a folder legitimately may contain a `"`.
std::string EscapeAppleScript(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}
#endif
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

std::string PickFolder(const std::string& titleUtf8, const std::string& startUtf8) {
#ifdef _WIN32
    // ⚠ COINIT_APARTMENTTHREADED, and the result is checked for RPC_E_CHANGED_MODE
    // rather than treated as failure: Slint's own backend may already have
    // initialized COM on this thread, and re-initializing with a different model
    // returns that code while leaving the existing apartment perfectly usable.
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool weInitialized = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return std::string();

    std::string chosen;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                     IID_IFileOpenDialog,
                                     reinterpret_cast<void**>(&dialog)))) {
        DWORD flags = 0;
        if (SUCCEEDED(dialog->GetOptions(&flags))) {
            dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        }
        if (!titleUtf8.empty()) {
            dialog->SetTitle(fsutil::PathFromUtf8(titleUtf8).wstring().c_str());
        }
        if (!startUtf8.empty()) {
            IShellItem* start = nullptr;
            if (SUCCEEDED(::SHCreateItemFromParsingName(
                    fsutil::PathFromUtf8(startUtf8).wstring().c_str(), nullptr,
                    IID_IShellItem, reinterpret_cast<void**>(&start)))) {
                dialog->SetFolder(start);
                start->Release();
            }
        }
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR wide = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide))) {
                    chosen = fsutil::PathToUtf8(std::filesystem::path(wide));
                    ::CoTaskMemFree(wide);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (weInitialized) ::CoUninitialize();
    return chosen;
#elif defined(__APPLE__)
    // AppleScript's own folder chooser, driven through osascript. ⚠ DELIBERATELY
    // NOT NSOpenPanel: that would need this file compiled as Objective-C++ (.mm)
    // and a run loop of its own to pump the panel, and the GUI already owns the
    // only run loop there is. osascript keeps the picker out of our process
    // entirely, which is also why it cannot deadlock against Slint's event loop.
    std::string script = "POSIX path of (choose folder with prompt \"" +
                         EscapeAppleScript(titleUtf8) + "\"";
    if (!startUtf8.empty()) {
        script += " default location POSIX file \"" + EscapeAppleScript(startUtf8) + "\"";
    }
    script += ")";
    std::string out;
    if (!RunCapture("osascript", {"-e", script}, &out)) return std::string();
    // ⚠ `POSIX path of` RETURNS A TRAILING SLASH on a folder. Everything else here
    // stores and compares paths without one, so a stray slash would make the same
    // folder read as two different roots between this and a typed path.
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
#else
    // zenity (GTK) then kdialog (KDE). ⚠ NEITHER IS GUARANTEED — a minimal or
    // headless box has no picker at all, and that is exactly the case platform.h
    // documents: return empty and let the user type the path into the field, which
    // is why that field is editable rather than a read-only display of the dialog's
    // answer. A missing picker must never be an error.
    std::string out;
    std::vector<std::string> zen = {"--file-selection", "--directory",
                                    "--title=" + titleUtf8};
    // The trailing slash is what makes zenity start INSIDE the folder rather than
    // beside it with the folder selected.
    if (!startUtf8.empty()) zen.push_back("--filename=" + startUtf8 + "/");
    if (RunCapture("zenity", zen, &out)) return out;

    const std::vector<std::string> kde = {
        "--getexistingdirectory", startUtf8.empty() ? std::string(".") : startUtf8};
    if (RunCapture("kdialog", kde, &out)) return out;
    return std::string();
#endif
}

bool AttachParentConsole() {
#ifdef _WIN32
    if (::AttachConsole(ATTACH_PARENT_PROCESS) == 0) return false;
    // Only the streams that have no handle of their own — see the header. A
    // redirected stdout arrives here already valid and is left exactly as it is.
    struct Stream {
        DWORD id;
        FILE* file;
        const char* device;
        const char* mode;
    };
    const Stream kStreams[] = {
        {STD_OUTPUT_HANDLE, stdout, "CONOUT$", "w"},
        {STD_ERROR_HANDLE, stderr, "CONOUT$", "w"},
        {STD_INPUT_HANDLE, stdin, "CONIN$", "r"},
    };
    for (const Stream& s : kStreams) {
        const HANDLE h = ::GetStdHandle(s.id);
        if (h != nullptr && h != INVALID_HANDLE_VALUE) continue;
        FILE* reopened = nullptr;
        ::freopen_s(&reopened, s.device, s.mode, s.file);
    }
    return true;
#else
    return false;
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

bool OpenFolder(const std::string& pathUtf8) {
#ifdef _WIN32
    // ⚠ THE DEFAULT VERB, NOT L"explore". Both open the folder; `explore` also
    // forces the navigation pane open, which overrides whatever the user has
    // chosen for every other Explorer window they open.
    const std::wstring w = fsutil::PathFromUtf8(pathUtf8).wstring();
    const HINSTANCE r =
        ::ShellExecuteW(nullptr, nullptr, w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<std::intptr_t>(r) > 32;
#elif defined(__APPLE__)
    return Spawn("open", pathUtf8);
#else
    return Spawn("xdg-open", pathUtf8);
#endif
}

bool SetClipboardText(const std::string& textUtf8) {
#ifdef _WIN32
    // ⚠ CF_UNICODETEXT, NOT CF_TEXT. An installer log holds aircraft folder names
    // and X-Plane paths, which are routinely non-ASCII; CF_TEXT would hand them to
    // the clipboard in the system codepage and mangle exactly the lines someone is
    // copying in order to report a problem.
    const std::wstring wide = fsutil::PathFromUtf8(textUtf8).wstring();
    if (::OpenClipboard(nullptr) == 0) return false;
    bool ok = false;
    if (::EmptyClipboard() != 0) {
        const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        // ⚠ GMEM_MOVEABLE, AND OWNERSHIP PASSES TO THE CLIPBOARD. A successful
        // SetClipboardData means the block must NOT be freed here; freeing it is a
        // use-after-free the next time anything pastes.
        HGLOBAL block = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (block != nullptr) {
            void* dest = ::GlobalLock(block);
            if (dest != nullptr) {
                std::memcpy(dest, wide.c_str(), bytes);
                ::GlobalUnlock(block);
                ok = ::SetClipboardData(CF_UNICODETEXT, block) != nullptr;
            }
            if (!ok) ::GlobalFree(block);
        }
    }
    ::CloseClipboard();
    return ok;
#else
    // ⚠ popen, NOT the `Spawn` helper above — this one has to WRITE to the child,
    // and Spawn deliberately closes its streams. Still no shell metacharacter
    // hazard: the command is a literal and the payload goes down the pipe.
    const char* cmd =
#    ifdef __APPLE__
        "pbcopy";
#    else
        "xclip -selection clipboard";
#    endif
    FILE* pipe = ::popen(cmd, "w");
    if (pipe == nullptr) return false;
    const std::size_t written =
        std::fwrite(textUtf8.data(), 1, textUtf8.size(), pipe);
    const int status = ::pclose(pipe);
    return written == textUtf8.size() && status == 0;
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
