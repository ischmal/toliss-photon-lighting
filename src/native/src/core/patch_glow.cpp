#include "core/patch_glow.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include "core/constants.h"
#include "core/fsutil.h"

namespace photon {
namespace patch_glow {
namespace {

namespace fs = std::filesystem;

// (search, replace) dataref-token pairs for the given indices.
//
// ⚠ The closing `]` is PART OF EACH TOKEN, so `[1]` can never match inside `[12]`.
std::vector<std::pair<std::string, std::string>> TokenPairs(
    const std::vector<int>& indices, bool reverse) {
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(indices.size());
    for (int n : indices) {
        const std::string idx = "[" + std::to_string(n) + "]";
        const std::string oldTok = std::string(kGlowOldDataRef) + idx;
        const std::string newTok = std::string(kGlowNewDataRef) + idx;
        if (reverse) {
            pairs.emplace_back(newTok, oldTok);
        } else {
            pairs.emplace_back(oldTok, newTok);
        }
    }
    return pairs;
}

int ReplaceAll(std::string& text, const std::string& search,
               const std::string& repl) {
    if (search.empty()) return 0;
    int n = 0;
    std::size_t at = 0;
    while ((at = text.find(search, at)) != std::string::npos) {
        text.replace(at, search.size(), repl);
        at += repl.size();
        ++n;
    }
    return n;
}

}  // namespace

const std::vector<int>* IndicesFor(const std::string& airframeKey) {
    return GlowIndicesFor(airframeKey);
}

std::vector<std::string> TargetFiles(const std::string& objectsDirUtf8,
                                     const std::string& airframeKey,
                                     bool reverse) {
    std::vector<std::string> hits;
    const std::vector<int>* indices = IndicesFor(airframeKey);
    if (indices == nullptr || indices->empty()) return hits;
    const auto pairs = TokenPairs(*indices, reverse);

    const fs::path dir = fsutil::PathFromUtf8(objectsDirUtf8);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return hits;

    std::vector<fs::path> objs;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;
        const std::string name =
            fsutil::ToLower(fsutil::PathToUtf8(it->path().filename()));
        if (fsutil::EndsWith(name, ".obj")) objs.push_back(it->path());
    }
    std::sort(objs.begin(), objs.end());

    for (const fs::path& p : objs) {
        std::string text;
        if (!fsutil::ReadFileBytes(p, text)) continue;
        for (const auto& pr : pairs) {
            if (text.find(pr.first) != std::string::npos) {
                hits.push_back(fsutil::PathToUtf8(p));
                break;
            }
        }
    }
    return hits;
}

std::string PatchText(const std::string& text, const std::string& airframeKey,
                      bool reverse, int& changed) {
    changed = 0;
    std::string out(text);
    const std::vector<int>* indices = IndicesFor(airframeKey);
    if (indices == nullptr) return out;
    for (const auto& pr : TokenPairs(*indices, reverse)) {
        changed += ReplaceAll(out, pr.first, pr.second);
    }
    return out;
}

RunResult Run(const std::string& objectsDirUtf8, const std::string& airframeKey,
              bool reverse, bool dryRun) {
    RunResult result;
    const std::vector<int>* indices = IndicesFor(airframeKey);
    if (indices == nullptr || indices->empty()) {
        result.log.push_back("no skin-glow redirect defined for airframe '" +
                             airframeKey + "' — nothing to do");
        return result;
    }
    const std::vector<std::string> files =
        TargetFiles(objectsDirUtf8, airframeKey, reverse);
    const char* verb = dryRun ? "would change"
                              : (reverse ? "restored" : "redirected");
    for (const std::string& pathUtf8 : files) {
        const auto path = fsutil::PathFromUtf8(pathUtf8);
        std::string text;
        if (!fsutil::ReadFileBytes(path, text)) continue;
        int changed = 0;
        const std::string out = PatchText(text, airframeKey, reverse, changed);
        if (changed > 0 && !dryRun) {
            std::error_code ec;
            if (!fsutil::AtomicWriteBytes(path, out, ec)) {
                result.log.push_back("  ! could not write " +
                                     fsutil::PathToUtf8(path.filename()));
                continue;
            }
        }
        std::ostringstream line;
        line << "  " << (changed ? '~' : '.') << " "
             << fsutil::PathToUtf8(path.filename()) << ": " << verb << " "
             << changed << " glow region(s)";
        result.log.push_back(line.str());
        result.regions += changed;
        if (changed) result.touched.push_back(pathUtf8);
    }
    std::ostringstream tail;
    tail << verb << " " << result.regions << " glow region(s) across "
         << files.size() << " file(s)";
    result.log.push_back(tail.str());
    return result;
}

}  // namespace patch_glow
}  // namespace photon
