// `photon-installer` — the compiled end-user installer.
//
// Two modes, decided by the arguments alone:
//
//   photon-installer                  the interactive TUI (docs/installer.md)
//   photon-installer <subcommand> …   headless, scriptable, `--json`
//
// ⚠ THE PAYLOAD IS RESOLVED FROM THE EXECUTABLE'S OWN PATH, never the working
// directory. Users run this from Downloads, from a USB stick, and from inside
// X-Plane/Resources/plugins/ — the bundle travels with the binary. `--payload-dir`
// overrides that, which is what makes the dev loop a rebuild rather than a
// 58 MiB restage (see the flag's own note below).
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "core/fsutil.h"
#include "core/payload.h"
#include "installer/cli.h"
#include "installer/platform.h"
#include "installer/screens.h"

#ifdef PHOTON_GUI
#include "installer/gui.h"
#endif

#ifdef _WIN32
#include <windows.h>
// ⚠ Explicitly, because photoncore compiles every consumer with
// WIN32_LEAN_AND_MEAN — which is exactly what leaves CommandLineToArgvW out of
// windows.h.
#include <shellapi.h>
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

// Pull `--payload-dir PATH` (or `--payload-dir=PATH`) out of `args`, returning the
// rest untouched.
//
// ⚠ HANDLED HERE, BEFORE THE MODE SPLIT, so it applies to the TUI and to every
// subcommand from one implementation. Threading it through both parsers instead
// would give two chances to forget it, and the failure — a flag silently ignored
// while the installer reads the wrong bundle — looks exactly like the payload
// being broken.
std::vector<std::string> TakePayloadDir(const std::vector<std::string>& args,
                                        std::string& out, bool& bad) {
    static const std::string kEq = "--payload-dir=";
    std::vector<std::string> rest;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--payload-dir") {
            if (i + 1 >= args.size()) {
                bad = true;
                return rest;
            }
            out = args[++i];
        } else if (a.rfind(kEq, 0) == 0) {
            out = a.substr(kEq.size());
        } else {
            rest.push_back(a);
        }
    }
    return rest;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> rawArgs = Utf8Args(argc, argv);

#ifdef PHOTON_GUI
    // ⚠ BEFORE ANY OUTPUT, AND BEFORE THE MODE SPLIT. A GUI-subsystem binary
    // starts with no console, so every `std::cout` below this point — the whole
    // CLI, and the argument errors on both paths — would go nowhere at all. It
    // returns false on a double-click, which is the normal case and not an error.
    photon::installer::platform::AttachParentConsole();
#endif

#ifdef _WIN32
    // ⚠ The console has to be told the output is UTF-8, or the box-drawing glyphs
    // and the ⚠ in log lines come out as mojibake. This is the C++ equivalent of
    // the Python installer's `sys.stdout.reconfigure(encoding="utf-8")`.
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    std::string payloadDir;
    bool badPayloadDir = false;
    const std::vector<std::string> args =
        TakePayloadDir(rawArgs, payloadDir, badPayloadDir);
    if (badPayloadDir) {
        std::cerr << "--payload-dir needs a value\n\n"
                  << photon::installer::UsageText();
        return 2;
    }

    photon::payload::InitFromExecutable(ExecutablePath(args));
    if (!payloadDir.empty()) {
        // ⚠ A WRONG EXPLICIT PATH IS A USER ERROR, AND MUST SAY SO. Left to the
        // normal machinery a typo here surfaces as "this build is incomplete —
        // re-download", which sends someone chasing a broken bundle over a
        // mistyped argument.
        std::error_code ec;
        const std::filesystem::path given = photon::fsutil::PathFromUtf8(payloadDir);
        if (!std::filesystem::is_directory(given, ec)) {
            std::cerr << "--payload-dir is not a directory: " << payloadDir << "\n";
            return 2;
        }
        // Absolute, so the log line and the `--json` report name a path that means
        // the same thing to whoever reads them later. A relative argument resolves
        // against the working directory — correct for a flag, useless in a log.
        const std::filesystem::path abs = std::filesystem::weakly_canonical(given, ec);
        photon::payload::SetPayloadDir(
            photon::fsutil::PathToUtf8(ec ? given : abs));
    }

    if (photon::installer::IsCliInvocation(args)) {
        return photon::installer::RunCli(args);
    }

    // ─── the interactive installer ───────────────────────────────────────────
    // Three arguments reach it. Two mirror the Python installer's: `--dry-run`
    // (log everything, write nothing) and `--xplane-root PATH` (skip
    // auto-detection). `--tui` picks the terminal UI over the GUI. Anything else
    // is a typo, and saying so beats silently opening a full-screen UI over the
    // top of it.
    std::string xplaneRoot;
    bool dryRun = false;
    bool forceTui = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--dry-run") {
            dryRun = true;
        } else if (a == "--tui") {
            // ⚠ ACCEPTED AND IGNORED IN A NON-GUI BUILD, deliberately. The two
            // builds otherwise disagree about whether a valid argument exists,
            // and a script or a support instruction that says "run it with
            // --tui" would fail on exactly the build where it is redundant.
            forceTui = true;
        } else if (a == "--xplane-root" && i + 1 < args.size()) {
            xplaneRoot = args[++i];
        } else if (a.rfind("--xplane-root=", 0) == 0) {
            xplaneRoot = a.substr(std::string("--xplane-root=").size());
        } else {
            std::cerr << "unrecognized argument: " << a << "\n\n"
                      << photon::installer::UsageText();
            return 2;
        }
    }

#ifdef PHOTON_GUI
    // The GUI is what a double-click gets. `--tui` is the way back to the
    // terminal UI, which is what an SSH session or a headless box still needs.
    if (!forceTui) return photon::installer::RunGui(xplaneRoot, dryRun);
#else
    (void)forceTui;
#endif

    return photon::installer::RunTui(xplaneRoot, dryRun);
}
