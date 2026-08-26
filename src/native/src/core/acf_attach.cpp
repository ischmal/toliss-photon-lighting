#include "core/acf_attach.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "core/fsutil.h"

namespace photon {
namespace acf_attach {
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

bool IsAttached(const std::string& text, const std::string& filename) {
    const int idx = FindObj(text, filename);
    if (idx < 0) return false;
    return idx < DeclaredCount(text);
}

std::string Attach(const std::string& text, const std::string& filename,
                   int templateIndex, int& changed) {
    changed = 0;
    if (IsAttached(text, filename)) return text;

    const auto rows = AttachmentRows(text);
    const auto tmpl = rows.find(templateIndex);
    if (tmpl == rows.end()) {
        throw acf::AcfError("no attachment row at index " +
                            std::to_string(templateIndex) + " to copy a frame from");
    }
    const int count = DeclaredCount(text);
    // ⚠ Append past BOTH the declared count and the highest index actually
    // present, so a table that already has rows beyond the count (another mod
    // mid-install, or a stale leftover) cannot make us overwrite one.
    int highest = -1;
    for (const auto& row : rows) highest = std::max(highest, row.first);
    const int newIdx = std::max(count, highest + 1);

    // Copy the template's fields VERBATIM — the values keep the host file's own
    // float style, which is what sidesteps the XP11/XP12 rendering trap entirely.
    std::vector<std::string> block;
    block.reserve(tmpl->second.size());
    for (const auto& field : tmpl->second) {  // std::map iterates sorted, as Python's sorted()
        const std::string value =
            (field.first == kFileField) ? filename : field.second;
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

std::string InsertAfter(const std::string& text, const std::string& filename,
                        int templateIndex, int& changed) {
    changed = 0;
    if (IsAttached(text, filename)) return text;

    const auto rows = AttachmentRows(text);
    const auto tmpl = rows.find(templateIndex);
    if (tmpl == rows.end()) {
        throw acf::AcfError("no attachment row at index " +
                            std::to_string(templateIndex) + " to sit beside");
    }
    const int count = DeclaredCount(text);
    const int newIdx = templateIndex + 1;

    // Verbatim, as in Attach — the values keep the host file's own float style.
    std::vector<std::string> block;
    block.reserve(tmpl->second.size());
    for (const auto& field : tmpl->second) {
        const std::string value =
            (field.first == kFileField) ? filename : field.second;
        block.push_back("P _obja/" + std::to_string(newIdx) + "/" + field.first +
                        " " + value);
    }

    const std::vector<std::string> lines = fsutil::SplitLinesKeepEnds(text);
    // ⚠ The LAST line of the template's row, found in the ORIGINAL numbering —
    // the block goes after the whole row, not after its first field.
    std::ptrdiff_t insertAfterLine = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const acf::PropLine p = acf::ParseLine(Split(lines[i]).body);
        int idx = 0;
        std::string field;
        if (p.matched && ParseObja(p.prop, idx, field) && idx == templateIndex) {
            insertAfterLine = static_cast<std::ptrdiff_t>(i);
        }
    }
    if (insertAfterLine < 0) {
        throw acf::AcfError("no _obja lines found for index " +
                            std::to_string(templateIndex));
    }

    std::string out;
    out.reserve(text.size() + block.size() * 64);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const SplitLine sl = Split(lines[i]);
        const acf::PropLine p = acf::ParseLine(sl.body);
        if (p.matched && p.prop == kCountProp) {
            const std::string rewritten = p.head + std::to_string(count + 1);
            if (rewritten != sl.body) ++changed;
            out += rewritten;
            out += sl.eol;
            continue;
        }
        int rowIdx = 0;
        std::string field;
        if (p.matched && ParseObja(p.prop, rowIdx, field) &&
            rowIdx > templateIndex) {
            // ⚠ RENUMBER UP to make room. Leaving the hole unshifted would put
            // two rows at the same index; skipping the shift entirely would put
            // ours on top of whatever was there.
            out += "P _obja/" + std::to_string(rowIdx + 1) + "/" + field + " " +
                   p.value;
            out += sl.eol;
            ++changed;
        } else {
            out += lines[i];
        }
        if (static_cast<std::ptrdiff_t>(i) == insertAfterLine) {
            for (const std::string& b : block) {
                out += b;
                out += sl.eol;
                ++changed;
            }
        }
    }
    return out;
}

std::string Detach(const std::string& text, const std::string& filename,
                   int& changed) {
    changed = 0;
    const int idx = FindObj(text, filename);
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

std::vector<std::string> AcfFiles(const std::string& aircraftDirUtf8,
                                  acf::Variants which) {
    return acf::AcfFilesContaining(aircraftDirUtf8, kCountProp, which);
}

}  // namespace acf_attach
}  // namespace photon
