#include "installer/platform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

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
#else
    (void)titleUtf8;
    (void)startUtf8;
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
