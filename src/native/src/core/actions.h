// Backup / install / uninstall — the only module in the installer with side
// effects on the user's filesystem.
//
// Every write is guarded by `dryRun` and lands via `fsutil::AtomicWriteBytes`, so a
// crash or a permission error cannot leave a half-written OBJ in place.
//
// ⚠ THE ORDERING AND ASYMMETRY RULES BELOW ARE SHIPPED BUG FIXES. Each one is
// restated at its call site in actions.cpp; the summary is here because the shape
// of install() is not otherwise obvious:
//
//  * THE PLUGIN BINARY GOES FIRST. Its `.xpl` is the one artifact X-Plane locks
//    while loaded, so on a running sim it is the step that fails — and it must fail
//    BEFORE the version marker reaches the OBJ. Otherwise a marked-but-orphaned OBJ
//    reads back as a completed install on the next run: the "installer thinks it
//    succeeded even though it failed" bug.
//  * A BYTE-IDENTICAL `.xpl` IS NOT REWRITTEN. Re-replacing an unchanged, loaded,
//    memory-mapped file fails with "file in use" for zero benefit — and skipping it
//    is precisely what lets an OBJ-only reinstall run while X-Plane is open, which
//    the "please close X-Plane" screen relies on via `PluginIsCurrent`.
//  * `_BackupOnce` NEVER OVERWRITES AN EXISTING BACKUP. That would clobber the true
//    original with a Photon file on reinstall or upgrade.
//  * `backed_up[]` RESTORES; `added[]` DELETES. Restoring a backup that never
//    existed leaves the added files in place forever.
//  * SCREENS DETACH THE `.acf` FIRST, THEN DELETE THE OBJ. An `.acf` naming an OBJ
//    that is gone is a missing-object warning on every load; the reverse order
//    leaves an attached-but-unused row that nobody notices.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "core/detect.h"
#include "core/manifest.h"

namespace photon {
namespace actions {

// A real, user-facing failure (permission denied, missing payload, no install
// found) — distinct from a bug, and meant to be shown as-is.
class ActionError : public std::runtime_error {
public:
    explicit ActionError(const std::string& what) : std::runtime_error(what) {}
};

// Where the verbose log goes. The caller supplies it so the TUI, the CLI and the
// tests can each route it differently.
class Log {
public:
    virtual ~Log() = default;
    virtual void Write(const std::string& msg, const std::string& level = "INFO") = 0;
};

class NullLog : public Log {
public:
    void Write(const std::string&, const std::string&) override {}
};

// How far through the WHOLE run we are, 0..1, with the name of the step now
// running.
//
// ⚠ IT USED TO MEAN "TEXTURES COPIED OUT OF ELEVEN" and was called from exactly one
// loop — step 2 of 5 of the last phase, and only when the cockpit mod was selected.
// The bar it fed therefore sat at 0% for an entire ordinary install and then jumped
// to 100%, and with the cockpit mod it ran to 100% partway through and stalled
// there. The fraction now comes from `progress::Plan`, which is built from file
// sizes before any work starts; see core/progress.h for the cost model and the
// measurements behind it.
//
// ⚠ THE FRACTION IS MONOTONIC AND NEVER REACHES 1.0 FROM HERE. Only the caller
// knows the run is finished — the plan can only say every step it knew about is
// done — so the last step to 1.0 belongs to whoever owns the UI.
using ProgressFn = void (*)(double fraction, const std::string& step,
                            void* userData);

struct Options {
    std::string airframeKey;
    std::string aircraftPath;    // the aircraft folder, UTF-8
    std::string wing = "stock";
    std::string xplaneRoot;
    bool interior = false;
    bool dryRun = false;
    bool removePlugin = false;   // uninstall only
    ProgressFn progress = nullptr;
    void* progressUserData = nullptr;
};

// True if the plugin already installed in X-Plane is byte-identical to the one
// this installer would write. When so, a (re)install only rewrites the aircraft
// OBJs — which X-Plane loads into memory and releases, so overwriting them
// mid-session is safe — and nothing needs to touch the loaded, locked `.xpl`.
bool PluginIsCurrent(const std::string& xplaneRootUtf8);

// Human-readable step descriptions, for the Perform screen and the CLI's JSON.
std::vector<std::string> Install(const Options& opts, Log& log);
std::vector<std::string> Uninstall(const Options& opts, Log& log);

}  // namespace actions
}  // namespace photon
