// `photon-installer` — the compiled end-user installer.
//
// Two modes, decided by the arguments alone:
//
//   photon-installer                  the interactive TUI (docs/installer.md)
//   photon-installer <subcommand> …   headless, scriptable, `--json`
//
// ⚠ THE PAYLOAD IS RESOLVED FROM THE EXECUTABLE'S OWN PATH, never the working
// directory. Users run this from Downloads, from a USB stick, and from inside
// X-Plane/Resources/plugins/ — the bundle travels with the binary.
#include <clocale>
#include <iostream>
#include <string>
#include <vector>

#include "core/payload.h"
#include "installer/cli.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// argv as UTF-8, whatever the platform hands us.
//
// ⚠ On Windows the ANSI argv would mangle a path containing any character outside
// the active code page — and an aircraft folder named by a user in their own
// language is exactly such a path. The wide command line is the only correct source.
std::vector<std::string> Utf8Args(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    int wargc = 0;
    LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
    if (wargv != nullptr) {
        for (int i = 0; i < wargc; ++i) {
            const int need = ::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr,
                                                   0, nullptr, nullptr);
            std::string s(need > 0 ? static_cast<std::size_t>(need - 1) : 0, '\0');
            if (need > 0) {
                ::WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), need,
                                      nullptr, nullptr);
            }
            out.push_back(std::move(s));
        }
        ::LocalFree(wargv);
        return out;
    }
#endif
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

std::string ExecutablePath(const std::vector<std::string>& args) {
#ifdef _WIN32
    wchar_t buf[32768];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, 32767);
    if (n > 0) {
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n),
                                               nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), s.data(), need,
                              nullptr, nullptr);
        return s;
    }
#endif
    return args.empty() ? std::string() : args[0];
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = Utf8Args(argc, argv);
    photon::payload::InitFromExecutable(ExecutablePath(args));

#ifdef _WIN32
    // ⚠ The console has to be told the output is UTF-8, or the box-drawing glyphs
    // and the ⚠ in log lines come out as mojibake. This is the C++ equivalent of
    // the Python installer's `sys.stdout.reconfigure(encoding="utf-8")`.
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    if (photon::installer::IsCliInvocation(args)) {
        return photon::installer::RunCli(args);
    }

    // The interactive TUI is Phase 4 of the port (docs/installer_cpp_plan.md §4).
    // Until it lands, a bare invocation says so rather than doing nothing —
    // a silent no-op would read as a broken download.
    std::cout << "The interactive installer is not built into this binary yet.\n"
                 "Use the headless commands, or the Python installer "
                 "(python install.py):\n\n"
              << photon::installer::UsageText();
    return 2;
}
