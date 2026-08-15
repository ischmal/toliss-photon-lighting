#include "core/patch_acf.h"

#include <sstream>
#include <utility>

#include "core/fsutil.h"

namespace photon {
namespace patch_acf {
namespace {

struct NumericProp {
    const char* prop;
    double value;
};

// What we WRITE. Values from Gus's readme (Plane Maker steps) and
// docs/interior_plan.md §3.4:
//   _spot1_3d_xyz/1  raise spot 1 to 4.00 ft on the vertical
//   _spot_angle/0    50 degrees (from 0)
//   _spot_size/*     8 / 9 / 10
//   _spot_the/0      pitch -60
//   _spot_psi/0      heading 180
const NumericProp kNumericPatch[] = {
    {"acf/_spot1_3d_xyz/1", 4.0},
    {"acf/_spot_angle/0", 50.0},
    {"acf/_spot_size/0", 8.0},
    {"acf/_spot_size/1", 9.0},
    {"acf/_spot_size/2", 10.0},
    {"acf/_spot_the/0", -60.0},
    {"acf/_spot_psi/0", 180.0},
};

// ⚠ THE SENTINEL: disabling the three spots. A *string* value, immune to the
// float-format trap. The A339 ships `_spot_name_3d/2 none` itself, so this is
// ToLiss's own idiom for "no spot here", not an invention.
const std::pair<const char*, const char*> kDisable[] = {
    {"acf/_spot_name_3d/0", "none"},
    {"acf/_spot_name_3d/1", "none"},
    {"acf/_spot_name_3d/2", "none"},
};

// Stock values, for reversal. Identical across A319/A320/A321 and across all four
// `.acf` per airframe — verified against the local installs. The only per-airframe
// difference in the whole spot block is `_spot*_3d_xyz/2` (longitudinal position:
// 14.75 / 9.49 / -4.48), which we never touch.
const NumericProp kStockNumeric[] = {
    {"acf/_spot1_3d_xyz/1", 1.0},
    {"acf/_spot_angle/0", 0.0},
    {"acf/_spot_size/0", 20.0},
    {"acf/_spot_size/1", 28.010000229},
    {"acf/_spot_size/2", 38.009998322},
    {"acf/_spot_the/0", -70.0},
    {"acf/_spot_psi/0", -20.0},
};

const std::pair<const char*, const char*> kStockString[] = {
    {"acf/_spot_name_3d/0", "sim/cockpit2/electrical/panel_brightness_ratio[0]"},
    {"acf/_spot_name_3d/1", "sim/cockpit2/electrical/panel_brightness_ratio[3]"},
    {"acf/_spot_name_3d/2", "ckpt/lights/map"},
};

}  // namespace

bool IsPatched(const std::string& text) {
    const auto props = acf::ReadProps(text);
    for (const auto& kv : kDisable) {
        const auto it = props.find(kv.first);
        if (it == props.end()) return false;
        if (fsutil::ToLower(fsutil::Trim(it->second)) != kv.second) return false;
    }
    return true;
}

std::vector<std::string> VerifyGeometry(const std::string& text) {
    const auto props = acf::ReadProps(text);
    std::vector<std::string> bad;
    for (const NumericProp& np : kNumericPatch) {
        const auto it = props.find(np.prop);
        if (it == props.end() || !acf::NumericEquals(it->second, np.value)) {
            bad.emplace_back(np.prop);
        }
    }
    return bad;
}

std::string PatchText(const std::string& text, int& changed) {
    const acf::Style style = acf::DetectStyle(text);
    std::map<std::string, std::string> values;
    for (const NumericProp& np : kNumericPatch) {
        values[np.prop] = acf::Render(np.value, style);
    }
    for (const auto& kv : kDisable) values[kv.first] = kv.second;
    return acf::SetProps(text, values, changed);
}

std::string UnpatchText(const std::string& text, int& changed) {
    const acf::Style style = acf::DetectStyle(text);
    std::map<std::string, std::string> values;
    for (const NumericProp& np : kStockNumeric) {
        values[np.prop] = acf::Render(np.value, style);
    }
    for (const auto& kv : kStockString) values[kv.first] = kv.second;
    return acf::SetProps(text, values, changed);
}

std::vector<std::string> AcfFiles(const std::string& aircraftDirUtf8,
                                  acf::Variants which) {
    return acf::AcfFilesContaining(aircraftDirUtf8, kSpotBlockNeedle, which);
}

RunResult Run(const std::string& aircraftDirUtf8, bool reverse, bool dryRun,
              const progress::StepProgress& onProgress) {
    RunResult result;
    // ⚠ THE SCOPE FOLLOWS THE DIRECTION. Patching writes to the pair X-Plane 12
    // loads; reverting has to reach whatever an older Photon wrote to, which was
    // all four. `acf::Variants` carries the whole argument.
    const std::vector<std::string> files =
        AcfFiles(aircraftDirUtf8, reverse ? acf::Variants::Every
                                          : acf::Variants::Xp12);
    if (files.empty()) {
        result.log.push_back("  ! no .acf with a cockpit spot block under " +
                             aircraftDirUtf8);
        return result;
    }
    // ⚠ REPORTED AT THE TOP OF THE ITERATION, NOT THE BOTTOM. Half the paths
    // through this loop are `continue` — an unreadable file, a refused patch —
    // and a report at the end would skip exactly the files that went wrong.
    // The four `.acf` are within 9% of each other in size, so file count is a
    // fair weight here; the big scans elsewhere weight by bytes instead.
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (onProgress) onProgress(static_cast<double>(i) / files.size());
        const std::string& pathUtf8 = files[i];
        const auto path = fsutil::PathFromUtf8(pathUtf8);
        std::string text;
        if (!fsutil::ReadFileBytes(path, text)) {
            result.log.push_back("  ! could not read " +
                                 fsutil::PathToUtf8(path.filename()));
            continue;
        }
        const acf::Style style = acf::DetectStyle(text);
        int changed = 0;
        std::string out;
        try {
            out = reverse ? UnpatchText(text, changed) : PatchText(text, changed);
        } catch (const acf::AcfError& e) {
            // PER FILE, not per run. A ToLiss folder holds four `.acf` variants and
            // they are not identical, so one that cannot be patched must not abort
            // the other three.
            result.log.push_back("  ! " + fsutil::PathToUtf8(path.filename()) +
                                 " refused: " + e.what());
            continue;
        }
        std::ostringstream line;
        line << "  - " << fsutil::PathToUtf8(path.filename())
             << " [" << (style == acf::Style::Xp12 ? "xp12" : "xp11") << "] "
             << (reverse ? "revert" : "patch") << ": " << changed
             << " line(s) changed" << (dryRun ? " (dry run)" : "");
        result.log.push_back(line.str());
        if (!dryRun && out != text) {
            std::error_code ec;
            if (!fsutil::AtomicWriteBytes(path, out, ec)) {
                result.log.push_back("  ! could not write " +
                                     fsutil::PathToUtf8(path.filename()));
                continue;
            }
        }
        result.touched.push_back(pathUtf8);
        // ⚠ FROM `changed`, NOT FROM `out != text`, so a dry run reports what it
        // WOULD change rather than nothing at all.
        if (changed > 0) result.changed.push_back(pathUtf8);
    }
    if (onProgress) onProgress(1.0);
    return result;
}

}  // namespace patch_acf
}  // namespace photon
