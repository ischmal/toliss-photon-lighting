#include "core/marker.h"

#include <cctype>
#include <vector>

#include "core/constants.h"
#include "core/fsutil.h"

namespace photon {
namespace marker {
namespace {

// Read a whitespace-delimited token starting at `pos`, advancing past it.
// Hand-rolled rather than <regex>: the pattern is trivial, and <regex> costs a
// surprising amount of binary size and startup time for what this is.
std::string NextToken(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    const std::size_t start = pos;
    while (pos < s.size() && !std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    return s.substr(start, pos - start);
}

}  // namespace

std::string Line(const std::string& version, const std::string& wing) {
    return std::string(kMarkerPrefix) + version + " wing: " + wing;
}

Marker Parse(const std::string& text) {
    Marker out;
    // Matches the Python side's regex `ToLissPhoton version:\s*(\S+)\s+wing:\s*(\S+)`.
    // Searching for the bare needle rather than kMarkerPrefix (which carries the
    // leading "# ") keeps a marker readable even if a tool reflowed the comment.
    static const std::string kNeedle = "ToLissPhoton version:";
    const std::size_t at = text.find(kNeedle);
    if (at == std::string::npos) return out;

    std::size_t pos = at + kNeedle.size();
    const std::string version = NextToken(text, pos);
    if (version.empty()) return out;
    const std::string wingKey = NextToken(text, pos);
    if (wingKey != "wing:") {
        // A version with no wing is still a marker — the wing simply reads as
        // unknown, which the status logic treats as "installed, wing unrecorded"
        // rather than as no marker at all.
        out.found = true;
        out.version = version;
        return out;
    }
    out.found = true;
    out.version = version;
    out.wing = NextToken(text, pos);
    return out;
}

std::string Inject(const std::string& text, const std::string& version,
                   const std::string& wing) {
    const std::string line = Line(version, wing);
    const std::string nl = fsutil::DetectNewline(text);
    std::vector<std::string> lines = fsutil::SplitLines(text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (fsutil::StartsWith(lines[i], "POINT_COUNTS")) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i) + 1, line);
            return fsutil::JoinLines(lines, nl);
        }
    }
    lines.insert(lines.begin(), line);
    return fsutil::JoinLines(lines, nl);
}

}  // namespace marker
}  // namespace photon
