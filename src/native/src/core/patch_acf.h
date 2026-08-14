// The ToLiss `.acf` cockpit-spot patch (interior mod).
//
// ToLiss defines three sim 3D cockpit spot lights in the aircraft's `.acf`. Their
// COLOR lives there too, which means it can only be changed by editing the file —
// it cannot follow a runtime switch. So the interior install:
//
//   1. writes Gus's geometry values (variant-independent, so no runtime state),
//   2. DISABLES all three spots via `_spot_name_3d/{0,1,2} = none`, and
//   3. re-adds them as OBJ lights in lights_inn.obj, where `ANIM_hide` can switch
//      their color like every other interior light.
//
// Full rationale: docs/interior_plan.md §3.4 / §3.5. The XP11/XP12 format trap is
// documented on `core/acf.h`, which does the numeric parse and per-file re-render.
//
// ⚠ DETECTION IS BY PRESENCE OF OUR LINES, never a file hash: the `.acf`
// legitimately carries other mods' edits (Durantula rewrites 312 lines of the
// `_obja/*` table in the very same file). The sentinel is
// `_spot_name_3d/{0,1,2} == "none"` — a STRING value, so it sidesteps the float
// formatting problem entirely, it is unambiguous (stock is a dataref path), and it
// is the property that actually means "our patch is live". The numeric geometry
// properties are corroboration only.
//
// ⚠ REVERSAL WRITES THE RECORDED STOCK VALUES BACK rather than restoring a
// snapshot, so it composes with whatever else has touched the file since. This is
// also why the installer takes no backup of the `.acf`: a whole-file restore here
// would silently undo Durantula's edits.
//
// Coexistence: we touch only `acf/_spot*`, a namespace disjoint from Durantula's
// `_obja/*`. We are NOT safe from its UNINSTALL, which restores an
// `a320.acf.durantula.bak` predating our patch and silently wipes it — the presence
// check then reports "needs reinstall". Documented, not prevented.
#pragma once

#include <string>
#include <vector>

#include "core/acf.h"
#include "core/progress.h"

namespace photon {
namespace patch_acf {

// The `.acf` property whose presence marks a file as carrying a spot block.
constexpr char kSpotBlockNeedle[] = "acf/_spot_name_3d/0";

// True if OUR patch is live (the three disabled spot names).
bool IsPatched(const std::string& text);

// Corroboration for IsPatched: which numeric geometry properties do NOT hold our
// value. Compared numerically with a tolerance, never as strings. Empty means
// fully patched; non-empty on an otherwise-patched file means something partially
// reverted it.
std::vector<std::string> VerifyGeometry(const std::string& text);

// Apply / restore. Both are idempotent and both re-render in the file's own style.
std::string PatchText(const std::string& text, int& changed);
std::string UnpatchText(const std::string& text, int& changed);

std::vector<std::string> AcfFiles(const std::string& aircraftDirUtf8);

struct RunResult {
    std::vector<std::string> touched;   // .acf paths (UTF-8) actually processed
    std::vector<std::string> log;       // human-readable per-file lines
};

// Patch (or revert) every spot-carrying `.acf` in an aircraft folder.
RunResult Run(const std::string& aircraftDirUtf8, bool reverse, bool dryRun,
              const progress::StepProgress& onProgress = {});

}  // namespace patch_acf
}  // namespace photon
