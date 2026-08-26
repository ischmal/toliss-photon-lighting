#include "core/patch_acf_screens.h"

#include <sstream>

#include "core/constants.h"
#include "core/fsutil.h"

namespace photon {
namespace patch_acf_screens {

// ⚠ THIN BY DESIGN. The row arithmetic — appending past the declared count,
// bumping `_obja/count`, renumbering down on removal, copying a template row's
// values verbatim — is `core/acf_attach`, because integral lighting appends rows
// too and two copies of that would drift. What stays here is the part that is
// about the GLOW: which OBJ, and which row's frame it has to borrow.
std::map<int, std::map<std::string, std::string>> AttachmentRows(
    const std::string& text) {
    return acf_attach::AttachmentRows(text);
}

int FindObj(const std::string& text, const std::string& filename) {
    return acf_attach::FindObj(text, filename);
}

int DeclaredCount(const std::string& text) {
    return acf_attach::DeclaredCount(text);
}

int FindFrameTemplate(const std::string& text) {
    for (const std::string& candidate : ScreensFrameTemplates()) {
        const int idx = FindObj(text, candidate);
        if (idx >= 0) return idx;
    }
    return -1;
}

bool IsAttached(const std::string& text) {
    return acf_attach::IsAttached(text, kScreensObj);
}

std::string PatchText(const std::string& text, int& changed) {
    changed = 0;
    if (IsAttached(text)) return text;

    const int tmplIdx = FindFrameTemplate(text);
    if (tmplIdx < 0) {
        std::string names;
        for (const std::string& c : ScreensFrameTemplates()) {
            if (!names.empty()) names += ", ";
            names += c;
        }
        throw acf::AcfError(
            "no attachment row for any of [" + names +
            "] in this .acf, so there is no known-good frame to copy. " +
            kScreensObj + "'s light coordinates are authored in that OBJ's "
            "frame, and attaching at a guessed datum would mis-place every "
            "screen light.");
    }
    return acf_attach::Attach(text, kScreensObj, tmplIdx, changed);
}

std::string UnpatchText(const std::string& text, int& changed) {
    return acf_attach::Detach(text, kScreensObj, changed);
}

std::vector<std::string> AcfFiles(const std::string& aircraftDirUtf8,
                                  acf::Variants which) {
    return acf_attach::AcfFiles(aircraftDirUtf8, which);
}
RunResult Run(const std::string& aircraftDirUtf8, bool reverse, bool dryRun,
              const progress::StepProgress& onProgress) {
    RunResult result;
    // ⚠ THE SCOPE FOLLOWS THE DIRECTION — see `acf::Variants`. Detaching must
    // reach the XP11 pair because Photon ≤ 0.8.4 attached our row there, and the
    // OBJ that row names is deleted by the same uninstall.
    const std::vector<std::string> files =
        AcfFiles(aircraftDirUtf8, reverse ? acf::Variants::Every
                                          : acf::Variants::Xp12);
    if (files.empty()) {
        result.log.push_back("  ! no .acf with an attachment table under " +
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
        if (!fsutil::ReadFileBytes(path, text)) continue;
        const char* state = reverse ? "detach" : "attach";
        int changed = 0;
        std::string out;
        try {
            out = reverse ? UnpatchText(text, changed) : PatchText(text, changed);
        } catch (const acf::AcfError& e) {
            // ⚠ PER FILE, not per run. A ToLiss folder holds four `.acf` variants
            // and they are not identical — the XP11 pair uses a different field
            // set — so one that cannot be patched must not abort the other three.
            // The refusal is reported and that file is left exactly as it was.
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
        // ⚠ FROM `changed`, NOT FROM `out != text` — a dry run must report what it
        // WOULD change rather than nothing at all.
        if (changed > 0) result.changed.push_back(pathUtf8);
    }
    if (onProgress) onProgress(1.0);
    return result;
}

}  // namespace patch_acf_screens
}  // namespace photon
