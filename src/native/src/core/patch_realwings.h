// Apply the RealWings wingtip patch from PRECOMPUTED data.
//
// RealWings bakes its own wingtip lights into MainNEO/SecondaryNEO/Main OBJs using
// X-Plane's *sizeable* named lights, with fixed colors and no profile switching.
// Those lights sit inside deep, wing-specific animation chains, so they cannot be
// re-homed into `lights_out`. This rewrites each one IN PLACE into a ToLiss
// parametric light gated by the ToLissPhoton datarefs — same X/Y/Z, so it keeps
// RealWings' exact position and animation, but gains the original/LED color
// switch. The original line is preserved (commented) for reversibility.
//
// ⚠ NO DSL PARSER HERE, AND NEVER ONE. The replacement parameters — class, color,
// intensity, cone, direction, the category dataref, and the `@realwings { offset }`
// placement correction — are resolved out of `lights.layout.phdsl` AT BUNDLE TIME
// by `build/patch_realwings.py` and shipped as `payload/realwings_patch.json`. That
// split is what let install time stop depending on the DSL toolchain, which is the
// precondition for this installer existing at all (docs/installer_cpp_plan.md §2a).
//
// ⚠ THE JSON IS A GENERATED ARTIFACT, not a hand-maintained table — it carries a
// `_generated` header saying so. All lighting tuning still lives in the DSL. This
// is `payload/objs/`'s shape applied to numbers instead of files, not the
// hard-coded position table the tripwire forbids.
//
// This is a straight port of `installer/realwings.py`; the two must stay
// byte-compatible, which the parity harness checks by diffing patched trees.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/progress.h"

namespace photon {
namespace patch_realwings {

// The comment that marks our rewrite, and the "already patched?" sentinel.
constexpr char kMarker[] = "# >>> Photon RealWings";
constexpr char kPatchJson[] = "realwings_patch.json";

// Bumped when a record grows or loses a field, so an installer meeting an older
// or newer bundle says so instead of building LIGHT_PARAM lines from defaults.
constexpr int kSchema = 1;

class RealWingsError : public std::runtime_error {
public:
    explicit RealWingsError(const std::string& what) : std::runtime_error(what) {}
};

// One branch of the original/LED pair.
struct Branch {
    std::string hide;        // "1 1" or "0 0" — the ANIM_hide range
    std::string rgb;
    long long intensity = 0;
    std::string cone;
};

// The fully resolved replacement for one baked light at one (airframe, class,
// wingtip, side). No DSL vocabulary survives into this struct.
struct Record {
    std::string category;
    std::string lightClass;
    std::string index;
    std::string dir;                 // already side-mirrored
    bool hasOffset = false;
    double offset[3] = {0, 0, 0};    // already side-mirrored; absent == no correction
    std::vector<Branch> branches;
};

struct BakedClass {
    // ⚠ Empty means the side is only recoverable from the sign of the baked X:
    // RealWings bakes ONE strobe class for both wingtips. The nav classes name
    // their own side and pin it.
    std::string side;
    std::string path;                // "wing_nav/fence/nav_beam", for the comment
};

struct PatchData {
    std::string sourceAirframe;
    std::map<std::string, BakedClass> classes;
    // airframe -> class -> wingtip -> side -> record
    std::map<std::string,
             std::map<std::string,
                      std::map<std::string,
                               std::map<std::string, Record>>>> airframes;
};

// Load and shape-check `realwings_patch.json`. Throws RealWingsError.
PatchData Load(const std::string& jsonPathUtf8);
PatchData ParseJson(const std::string& text, const std::string& where);

// ⚠ Falls back to `sourceAirframe` for an unknown airframe, matching the DSL-side
// resolver: `--airframe ?` must behave as it always did. Appearance does not vary
// with the choice; only the placement correction does.
const Record& RecordFor(const PatchData& data, const std::string& airframe,
                        const std::string& cls, const std::string& wingtip,
                        const std::string& side);

// Which side a baked light is on.
std::string SideFor(const PatchData& data, const std::string& cls,
                    const std::string& x);

// ⚠ A null or zero offset passes the coordinates through UNCHANGED — the
// verbatim-position guarantee that keeps an untuned fixture at RealWings' exact
// bytes. Only a moved axis is reformatted.
void ShiftPos(const Record& rec, std::string& x, std::string& y, std::string& z);

// Rewrite every baked wingtip light in one mod OBJ's text.
std::string PatchText(const std::string& text, const PatchData& data,
                      const std::string& airframe, const std::string& objStem,
                      int& changed);

struct RunResult {
    int lights = 0;
    int files = 0;
    std::vector<std::string> log;
};

// ⚠ The REVERSE takes no patch data: it only restores `.obj.bak` siblings, so it
// must keep working against a bundle whose JSON is missing or a schema this build
// cannot read. Otherwise a user could end up unable to undo a patch an older
// installer applied successfully.
RunResult Run(const std::string& rootUtf8, const PatchData& data,
              const std::string& airframe, bool dryRun,
              const progress::StepProgress& onProgress = {});
RunResult RunReverse(const std::string& rootUtf8, bool dryRun);

}  // namespace patch_realwings
}  // namespace photon
