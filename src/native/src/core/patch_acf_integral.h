// Attaching the LED twin OBJs — the `.acf` half of switchable integral lighting.
//
// `core/patch_integral` writes `lamps_photon_led.obj` beside `lamps.obj`; until
// it appears in the aircraft's `_obja/*` table nothing draws it, which is what
// makes the whole feature reversible. The row engine is `core/acf_attach`; this
// is the feature's own policy.
//
// ⚠ THE FRAME IS THE TWIN'S OWN SOURCE ROW, AND NOTHING ELSE CAN BE RIGHT. An
// attachment row carries a position against a per-airframe datum. The twin is a
// byte-for-byte copy of `lamps.obj`'s geometry with one texture name changed, so
// the only frame that puts it exactly on top of the original is the original's.
// Unlike the display glow — which borrows `lights_inn.obj`'s frame and has to
// refuse when that OBJ is missing — there is never a guess here: if the source
// row is absent, the source OBJ is not drawn, and neither should its twin be.
//
// ⚠ WHICH IS ALSO THE DRAWN-OR-NOT TEST, and it is load-bearing. `lamps_Std.obj`
// binds `text_LIT.png` exactly as `lamps.obj` does, but neither XP12 `.acf`
// attaches it. Twinning it would ADD geometry to an aeroplane that was not
// drawing it — a change nobody asked for, on a file X-Plane 12 never loads. So
// `DrawnFixtures` intersects "binds one of our textures" with "is in the table",
// and only that intersection is ever patched or attached.
#pragma once

#include <string>
#include <vector>

#include "core/patch_integral.h"
#include "core/progress.h"

namespace photon {
namespace patch_acf_integral {

// The subset of `all` whose STOCK OBJ is attached in at least one XP12 `.acf`.
// ⚠ The installer must call this before it writes anything: a twin of an
// unattached OBJ is a 26 MB file in the aircraft folder that nothing will ever
// draw.
std::vector<integral::Fixture> DrawnFixtures(
    const std::string& aircraftDirUtf8,
    const std::vector<integral::Fixture>& all);

struct RunResult {
    std::vector<std::string> touched;
    // ⚠ The subset that actually differed — see `patch_acf_screens::RunResult`.
    std::vector<std::string> changed;
    std::vector<std::string> log;
};

// Attach every fixture's twin, or (reversing) drop every row naming a file with
// our suffix.
//
// ⚠ REVERSING IGNORES `fixtures` AND WORKS BY SUFFIX. An uninstall runs after
// the aircraft may have been half-restored, so the twin OBJs it is detaching may
// already be gone from disk and cannot be re-derived by content. The rows,
// however, still name them — and `integral::kLedSuffix` is the one thing every
// file this feature writes has in common.
RunResult Run(const std::string& aircraftDirUtf8,
              const std::vector<integral::Fixture>& fixtures, bool reverse,
              bool dryRun, const progress::StepProgress& onProgress = {});

}  // namespace patch_acf_integral
}  // namespace photon
