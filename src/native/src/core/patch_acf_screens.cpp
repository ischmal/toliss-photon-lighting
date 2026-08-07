#include "core/patch_acf_screens.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include "core/constants.h"
#include "core/fsutil.h"

namespace photon {
namespace patch_acf_screens {
namespace {

// Parse `_obja/<n>/<field>`. Returns false if the property is not one.
bool ParseObja(const std::string& prop, int& index, std::string& field) {
    static const std::string kPrefix = "_obja/";
    if (!fsutil::StartsWith(prop, kPrefix)) return false;
    std::size_t i = kPrefix.size();
    const std::size_t digitsStart = i;
    while (i < prop.size() && std::isdigit(static_cast<unsigned char>(prop[i]))) ++i;
    if (i == digitsStart) return false;              // `_obja/count` lands here
    if (i >= prop.size() || prop[i] != '/') return false;
    index = std::atoi(prop.substr(digitsStart, i - digitsStart).c_str());
    field = prop.substr(i + 1);
    return !field.empty();
}

struct SplitLine {
    std::string body;
    std::string eol;
};

SplitLine Split(const std::string& raw) {
    std::size_t n = raw.size();
    while (n > 0 && (raw[n - 1] == '\n' || raw[n - 1] == '\r')) --n;
    // A file whose last line has no terminator still needs one when we append
    // after it, so the fallback is CRLF — the flavor every real `.acf` uses.
    SplitLine out;
    out.body = raw.substr(0, n);
    out.eol = (n < raw.size()) ? raw.substr(n) : std::string("\r\n");
    return out;
}

}  // namespace

std::map<int, std::map<std::string, std::string>> AttachmentRows(
    const std::string& text) {
    std::map<int, std::map<std::string, std::string>> rows;
    for (const auto& kv : acf::ReadProps(text)) {
        int idx = 0;
        std::string field;
        if (ParseObja(kv.first, idx, field)) rows[idx][field] = kv.second;
    }
    return rows;
}

int FindObj(const std::string& text, const std::string& filename) {
    for (const auto& row : AttachmentRows(text)) {
        const auto it = row.second.find(kFileField);
        if (it != row.second.end() && fsutil::Trim(it->second) == filename) {
            return row.first;
        }
    }
    return -1;
}

int DeclaredCount(const std::string& text) {
    const auto props = acf::ReadProps(text);
    const auto it = props.find(kCountProp);
    if (it == props.end()) {
        throw acf::AcfError(std::string("no ") + kCountProp +
                            " in this .acf — not a ToLiss A3xx .acf, or its "
                            "format has changed");
    }
    const char* begin = it->second.c_str();
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin) {
        throw acf::AcfError(std::string(kCountProp) + " is not a number: '" +
                            it->second + "'");
    }
    return static_cast<int>(v);
}

bool IsAttached(const std::string& text) {
    const int idx = FindObj(text, kScreensObj);
    if (idx < 0) return false;
    return idx < DeclaredCount(text);
}

std::string PatchText(const std::string& text, int& changed) {
    changed = 0;
    if (IsAttached(text)) return text;

    const int tmplIdx = FindObj(text, kFrameTemplateObj);
    if (tmplIdx < 0) {
        throw acf::AcfError(
            std::string("no attachment row for ") + kFrameTemplateObj +
            " in this .acf, so there is no known-good frame to copy. " +
            kScreensObj + "'s light coordinates are authored in that OBJ's "
            "frame; attaching at a guessed datum would mis-place every screen "
            "light. Refusing.");
    }

    const auto rows = AttachmentRows(text);
    const int count = DeclaredCount(text);
    // ⚠ Append past BOTH the declared count and the highest index actually
    // present, so a table that already has rows beyond the count (another mod
    // mid-install, or a stale leftover) cannot make us overwrite one.
    int highest = -1;
    for (const auto& row : rows) highest = std::max(highest, row.first);
    const int newIdx = std::max(count, highest + 1);

    // Copy the template's fields VERBATIM — the values keep the host file's own
    // float style, which is what sidesteps the XP11/XP12 rendering trap entirely.
    const auto& templateRow = rows.at(tmplIdx);
    std::vector<std::string> block;
    block.reserve(templateRow.size());
    for (const auto& field : templateRow) {   // std::map iterates sorted, as Python's sorted()
        const std::string value =
            (field.first == kFileField) ? std::string(kScreensObj) : field.second;
        block.push_back("P _obja/" + std::to_string(newIdx) + "/" + field.first +
                        " " + value);
    }

    const std::vector<std::string> lines = fsutil::SplitLinesKeepEnds(text);
    // Insert directly after the LAST indexed `_obja` line, keeping the table
    // contiguous. `_obja/count` sits after the rows, so anchoring on the rows (not
    // on count) puts the block in the right place either way.
    std::ptrdiff_t lastRowLine = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const acf::PropLine p = acf::ParseLine(Split(lines[i]).body);
        int idx = 0;
        std::string field;
        if (p.matched && ParseObja(p.prop, idx, field)) {
            lastRowLine = static_cast<std::ptrdiff_t>(i);
        }
    }

    std::string out;
    out.reserve(text.size() + block.size() * 64);
    bool inserted = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const SplitLine sl = Split(lines[i]);
        const acf::PropLine p = acf::ParseLine(sl.body);
        if (p.matched && p.prop == kCountProp) {
            const std::string rewritten = p.head + std::to_string(newIdx + 1);
            if (rewritten != sl.body) ++changed;
            out += rewritten;
            out += sl.eol;
            continue;
        }
        out += lines[i];
        if (static_cast<std::ptrdiff_t>(i) == lastRowLine) {
            for (const std::string& b : block) {
                out += b;
                out += sl.eol;
                ++changed;
            }
            inserted = true;
        }
    }
    if (!inserted) throw acf::AcfError("no _obja rows found to append after");
    return out;
}

std::string UnpatchText(const std::string& text, int& changed) {
    changed = 0;
    const int idx = FindObj(text, kScreensObj);
    if (idx < 0) return text;

    int count = 0;
    try {
        count = DeclaredCount(text);
    } catch (const acf::AcfError&) {
        int highest = -1;
        for (const auto& row : AttachmentRows(text)) {
            highest = std::max(highest, row.first);
        }
        count = highest + 1;
    }

    std::string out;
    out.reserve(text.size());
    for (const std::string& raw : fsutil::SplitLinesKeepEnds(text)) {
        const SplitLine sl = Split(raw);
        const acf::PropLine p = acf::ParseLine(sl.body);
        if (!p.matched) {
            out += raw;
            continue;
        }
        if (p.prop == kCountProp) {
            const std::string rewritten = p.head + std::to_string(std::max(0, count - 1));
            if (rewritten != sl.body) ++changed;
            out += rewritten;
            out += sl.eol;
            continue;
        }
        int rowIdx = 0;
        std::string field;
        if (!ParseObja(p.prop, rowIdx, field)) {
            out += raw;
            continue;
        }
        if (rowIdx == idx) {
            ++changed;
            continue;                       // drop our row entirely
        }
        if (rowIdx > idx) {
            // ⚠ RENUMBER DOWN rather than leave a hole. X-Plane reads indices
            // 0..count-1, so a missing index in that span would drop whatever the
            // table says it is — silently unloading some other mod's OBJ. In the
            // normal case ours is the last row and nothing is renumbered.
            out += "P _obja/" + std::to_string(rowIdx - 1) + "/" + field + " " +
                   p.value;
            out += sl.eol;
            ++changed;
            continue;
        }
        out += raw;
    }
    return out;
}

std::vector<std::string> AcfFiles(const std::string& aircraftDirUtf8) {
    return acf::AcfFilesContaining(aircraftDirUtf8, kCountProp);
}

RunResult Run(const std::string& aircraftDirUtf8, bool reverse, bool dryRun) {
    RunResult result;
    const std::vector<std::string> files = AcfFiles(aircraftDirUtf8);
    if (files.empty()) {
        result.log.push_back("  ! no .acf with an attachment table under " +
                             aircraftDirUtf8);
        return result;
    }
    for (const std::string& pathUtf8 : files) {
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
                                 " " + state + " refused: " + e.what());
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
    }
    return result;
}

}  // namespace patch_acf_screens
}  // namespace photon
