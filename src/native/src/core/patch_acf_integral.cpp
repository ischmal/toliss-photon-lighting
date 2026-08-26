#include "core/patch_acf_integral.h"

#include <sstream>

#include "core/acf_attach.h"
#include "core/fsutil.h"

namespace photon {
namespace patch_acf_integral {
namespace {

// Every row whose attached filename carries our suffix, most-recent index first
// so successive removals never renumber a row we have not looked at yet.
std::vector<std::string> OurRows(const std::string& text) {
    std::vector<std::string> names;
    for (const auto& row : acf_attach::AttachmentRows(text)) {
        const auto it = row.second.find(acf_attach::kFileField);
        if (it == row.second.end()) continue;
        const std::string name = fsutil::Trim(it->second);
        if (name.find(integral::kLedSuffix) != std::string::npos) {
            names.push_back(name);
        }
    }
    return names;
}

}  // namespace

std::vector<integral::Fixture> DrawnFixtures(
    const std::string& aircraftDirUtf8,
    const std::vector<integral::Fixture>& all) {
    std::vector<integral::Fixture> drawn;
    const std::vector<std::string> files =
        acf_attach::AcfFiles(aircraftDirUtf8, acf::Variants::Xp12);
    for (const integral::Fixture& f : all) {
        for (const std::string& pathUtf8 : files) {
            std::string text;
            if (!fsutil::ReadFileBytes(fsutil::PathFromUtf8(pathUtf8), text)) {
                continue;
            }
            if (acf_attach::FindObj(text, f.obj) >= 0) {
                drawn.push_back(f);
                break;   // one XP12 variant drawing it is enough
            }
        }
    }
    return drawn;
}

RunResult Run(const std::string& aircraftDirUtf8,
              const std::vector<integral::Fixture>& fixtures, bool reverse,
              bool dryRun, const progress::StepProgress& onProgress) {
    RunResult result;
    // ⚠ THE SCOPE FOLLOWS THE DIRECTION — see `acf::Variants`. Attaching touches
    // only what X-Plane 12 loads; detaching reaches every variant, because an
    // XP11 file that somehow acquired a row would otherwise keep naming an OBJ
    // this same uninstall deletes.
    const std::vector<std::string> files = acf_attach::AcfFiles(
        aircraftDirUtf8, reverse ? acf::Variants::Every : acf::Variants::Xp12);
    if (files.empty()) {
        result.log.push_back("  ! no .acf with an attachment table under " +
                             aircraftDirUtf8);
        return result;
    }

    for (std::size_t i = 0; i < files.size(); ++i) {
        if (onProgress) onProgress(static_cast<double>(i) / files.size());
        const std::string& pathUtf8 = files[i];
        const auto path = fsutil::PathFromUtf8(pathUtf8);
        std::string text;
        if (!fsutil::ReadFileBytes(path, text)) continue;
        const char* state = reverse ? "detach" : "attach";

        std::string out = text;
        int changed = 0;
        try {
            if (reverse) {
                // ⚠ ONE AT A TIME, RE-READING THE TABLE EACH ROUND. `Detach`
                // renumbers everything above the row it drops, so a list
                // gathered once and walked would be wrong from the second
                // removal on.
                for (;;) {
                    const std::vector<std::string> ours = OurRows(out);
                    if (ours.empty()) break;
                    int one = 0;
                    out = acf_attach::Detach(out, ours.front(), one);
                    if (one == 0) break;      // nothing moved; do not spin
                    changed += one;
                }
            } else {
                for (const integral::Fixture& f : fixtures) {
                    const int tmplIdx = acf_attach::FindObj(out, f.obj);
                    if (tmplIdx < 0) {
                        // Not drawn in THIS variant. Not an error — the caller
                        // already established it is drawn in at least one.
                        result.log.push_back(
                            "  - " + fsutil::PathToUtf8(path.filename()) +
                            ": " + f.obj + " not attached here, skipping " +
                            f.ledObj);
                        continue;
                    }
                    // ⚠ A MISPLACED TWIN IS DROPPED FIRST, and without this a
                    // reinstall could not repair one. `InsertAfter` is
                    // idempotent on an ALREADY-ATTACHED file — it returns the
                    // text unchanged — so an aircraft carrying a twin appended
                    // at the end by the 2026-08-24 build would have had the
                    // installer report success and move nothing.
                    const int existing = acf_attach::FindObj(out, f.ledObj);
                    if (existing >= 0 && existing != tmplIdx + 1) {
                        int drop = 0;
                        out = acf_attach::Detach(out, f.ledObj, drop);
                        changed += drop;
                        result.log.push_back(
                            "  - " + fsutil::PathToUtf8(path.filename()) + ": " +
                            f.ledObj + " was at row " + std::to_string(existing) +
                            ", moving it beside " + f.obj);
                    }
                    // ⚠ INSERTED BESIDE ITS SOURCE, NOT APPENDED. X-Plane walks
                    // `_obja` in index order and neither of these OBJs declares
                    // `ATTR_no_blend`, so a twin at the end of the table draws
                    // after ToLiss's transparent glass instead of with the rest
                    // of the flight deck. See acf_attach::InsertAfter.
                    //
                    // ⚠ Re-resolved: the drop above renumbers, and a stale
                    // template index would insert beside the wrong object.
                    int one = 0;
                    out = acf_attach::InsertAfter(
                        out, f.ledObj, acf_attach::FindObj(out, f.obj), one);
                    changed += one;
                }
            }
        } catch (const acf::AcfError& e) {
            // ⚠ PER FILE, not per run — a ToLiss folder holds four `.acf`
            // variants and they are not identical.
            result.log.push_back("  ! " + fsutil::PathToUtf8(path.filename()) +
                                 ": " + state + " failed — " + e.what());
            continue;
        }

        std::ostringstream line;
        line << "  - " << fsutil::PathToUtf8(path.filename()) << " " << state
             << ": " << changed << " line(s) changed"
             << (changed ? "" : " (already done)") << (dryRun ? " [dry run]" : "");
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
        // ⚠ FROM `changed`, NOT FROM `out != text` — a dry run must report what
        // it WOULD change rather than nothing at all.
        if (changed > 0) result.changed.push_back(pathUtf8);
    }
    if (onProgress) onProgress(1.0);
    return result;
}

}  // namespace patch_acf_integral
}  // namespace photon
