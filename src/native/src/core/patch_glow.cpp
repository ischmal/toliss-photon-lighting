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
                                     bool reverse,
                                     const progress::StepProgress& onProgress) {
    std::vector<std::string> hits;
    const std::vector<int>* indices = IndicesFor(airframeKey);
    if (indices == nullptr || indices->empty()) return hits;
    const auto pairs = TokenPairs(*indices, reverse);

    const fs::path dir = fsutil::PathFromUtf8(objectsDirUtf8);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return hits;

    std::vector<fs::path> objs;
    // ⚠ SIZES ARE COLLECTED IN THE SAME WALK. Progress here has to be weighted by
    // BYTES, not by file count: on the A330-900 these 61 files run from 4 KB to
    // 133 MB, so a bar stepping 1/61 per file crawls through the first fifty and
    // then hangs for seconds on the last few — which is exactly the "stuck" reading
    // this whole exercise exists to remove.
    std::vector<std::uintmax_t> sizes;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;
        const std::string name =
            fsutil::ToLower(fsutil::PathToUtf8(it->path().filename()));
        if (!fsutil::EndsWith(name, ".obj")) continue;
        std::error_code se;
        const std::uintmax_t sz = it->file_size(se);
        objs.push_back(it->path());
        sizes.push_back(se ? 0 : sz);
    }
    // Sort paths and sizes together so the reported fraction matches the order the
    // files are actually read in.
    std::vector<std::size_t> order(objs.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&objs](std::size_t a, std::size_t b) { return objs[a] < objs[b]; });

    std::uintmax_t totalBytes = 0;
    for (std::uintmax_t s : sizes) totalBytes += s;

    std::uintmax_t readBytes = 0;
    for (std::size_t idx : order) {
        const fs::path& p = objs[idx];
        std::string text;
        const bool got = fsutil::ReadFileBytes(p, text);
        readBytes += sizes[idx];
        if (onProgress && totalBytes > 0) {
            onProgress(static_cast<double>(readBytes) /
                       static_cast<double>(totalBytes));
        }
        if (!got) continue;
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
              bool reverse, bool dryRun, const std::vector<std::string>* scanned,
              const progress::StepProgress& onProgress) {
    RunResult result;
    const std::vector<int>* indices = IndicesFor(airframeKey);
    if (indices == nullptr || indices->empty()) {
        result.log.push_back("no skin-glow redirect defined for airframe '" +
                             airframeKey + "' — nothing to do");
        return result;
    }
    // ⚠ THE INSTALLER ALREADY SCANNED, AND THE SCAN IS THE EXPENSIVE HALF. It needs
    // the list first to back each file up, then called us — and we scanned the whole
    // folder again, reading every `.obj` in the aircraft a second time. On the
    // A330-900 that is 1,597 MB twice, and it was 11.1 s of an 11.5 s install: the
    // duplicate pass was the single largest cost in the entire installer.
    // Passing the list in cuts it in half. `nullptr` keeps the old self-scanning
    // behavior for the CLI and the tests, which have no list to hand.
    std::vector<std::string> ownScan;
    if (scanned == nullptr) {
        ownScan = TargetFiles(objectsDirUtf8, airframeKey, reverse, onProgress);
        scanned = &ownScan;
    }
    const std::vector<std::string>& files = *scanned;
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
