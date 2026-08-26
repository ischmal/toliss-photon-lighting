#include "core/acf.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "core/fsutil.h"

namespace photon {
namespace acf {
namespace {

namespace fs = std::filesystem;

bool IsSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// Whether a value string is an XP12-style float: exactly nine decimal places.
// (Python: ^-?\d+\.\d{9}$)
bool IsXp12Float(const std::string& v) {
    std::size_t i = 0;
    if (i < v.size() && v[i] == '-') ++i;
    const std::size_t digitsStart = i;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
    if (i == digitsStart) return false;
    if (i >= v.size() || v[i] != '.') return false;
    ++i;
    const std::size_t fracStart = i;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
    return i == v.size() && (i - fracStart) == 9;
}

bool ParseDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    const char* begin = s.c_str();
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin) return false;
    while (end && *end && IsSpace(*end)) ++end;
    if (end && *end) return false;
    out = v;
    return true;
}

// Python's `repr(float)` — which is what wrote every XP11 `.acf` — reproduced.
//
// ⚠ `%g` IS NOT IT, and the difference is not cosmetic. `%.2g` of -70.0 is
// "-7e+01": shortest, and it round-trips, so a naive shortest-round-trip loop
// accepts it. Writing that into an `.acf` produces a line no ToLiss file has ever
// contained. Python only goes exponential when the decimal point falls outside
// (-4, 16], and the whole `.acf` value range sits inside that window — so in
// practice this must ALWAYS produce fixed notation, and the exponential branch
// exists only so a garbage input cannot produce garbage output.
//
// Two steps: find the shortest significant-digit count that round-trips (via %e,
// which never hides digits), then lay the decimal point out the way Python does.
std::string ShortestRoundTrip(double value) {
    if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";
    if (std::isnan(value) || std::isinf(value)) {
        // Not representable in an `.acf` at all; refuse rather than write a token
        // X-Plane's parser would take as zero.
        throw AcfError("cannot render a non-finite value into a .acf");
    }

    char buf[64];
    int sig = 17;
    for (int p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof(buf), "%.*e", p - 1, value);
        double back = 0.0;
        if (ParseDouble(buf, back) && back == value) {
            sig = p;
            break;
        }
    }
    std::snprintf(buf, sizeof(buf), "%.*e", sig - 1, value);

    // %e gives "[-]d[.ddd]e[+-]XX" — pull out the sign, the digit run, and the
    // exponent, so the point can be re-placed.
    std::string s(buf);
    std::string sign;
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        if (s[i] == '-') sign = "-";
        ++i;
    }
    std::string digits;
    for (; i < s.size() && s[i] != 'e' && s[i] != 'E'; ++i) {
        if (s[i] != '.') digits.push_back(s[i]);
    }
    int exp10 = 0;
    if (i < s.size()) exp10 = std::atoi(s.c_str() + i + 1);

    // Trailing zeros carry no information once the point is placed by `decpt`.
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

    const int decpt = exp10 + 1;   // digits before the decimal point
    const int nd = static_cast<int>(digits.size());

    if (decpt > -4 && decpt <= 16) {
        if (decpt <= 0) {
            return sign + "0." + std::string(static_cast<std::size_t>(-decpt), '0') +
                   digits;
        }
        if (decpt >= nd) {
            // ⚠ ALWAYS keep the decimal point: 1.0, never 1. Dropping it is
            // exactly the kind of "equivalent" rewrite that turns a one-property
            // patch into a whole-file diff.
            return sign + digits +
                   std::string(static_cast<std::size_t>(decpt - nd), '0') + ".0";
        }
        return sign + digits.substr(0, static_cast<std::size_t>(decpt)) + "." +
               digits.substr(static_cast<std::size_t>(decpt));
    }

    std::string out = sign + digits.substr(0, 1);
    if (nd > 1) out += "." + digits.substr(1);
    std::snprintf(buf, sizeof(buf), "e%+03d", exp10);
    return out + buf;
}

}  // namespace

PropLine ParseLine(const std::string& line) {
    // ^(?P<head>P\s+(?P<prop>\S+)\s+)(?P<val>.*?)(?P<eol>\s*)$
    PropLine out;
    std::size_t i = 0;
    if (i >= line.size() || line[i] != 'P') return out;
    ++i;
    const std::size_t afterP = i;
    while (i < line.size() && IsSpace(line[i])) ++i;
    if (i == afterP) return out;              // "P" must be followed by whitespace

    const std::size_t propStart = i;
    while (i < line.size() && !IsSpace(line[i])) ++i;
    if (i == propStart) return out;           // no property token
    const std::size_t propEnd = i;

    const std::size_t afterProp = i;
    while (i < line.size() && IsSpace(line[i])) ++i;
    if (i == afterProp) return out;           // property must be followed by space

    out.matched = true;
    out.head = line.substr(0, i);
    out.prop = line.substr(propStart, propEnd - propStart);
    out.value = fsutil::Trim(line.substr(i));
    return out;
}

std::map<std::string, std::string> ReadProps(const std::string& text) {
    std::map<std::string, std::string> out;
    for (const std::string& line : fsutil::SplitLines(text)) {
        const PropLine p = ParseLine(line);
        if (p.matched) out[p.prop] = p.value;
    }
    return out;
}

Style DetectStyle(const std::string& text) {
    int wide = 0, narrow = 0;
    for (const std::string& line : fsutil::SplitLines(text)) {
        const PropLine p = ParseLine(line);
        if (!p.matched || !fsutil::StartsWith(p.prop, "acf/")) continue;
        double unused = 0.0;
        if (!ParseDouble(p.value, unused)) continue;
        if (IsXp12Float(p.value)) {
            ++wide;
        } else if (p.value.find('.') != std::string::npos) {
            ++narrow;
        }
    }
    return wide > narrow ? Style::Xp12 : Style::Xp11;
}

std::string Render(double value, Style style) {
    if (style == Style::Xp12) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.9f", value);
        return std::string(buf);
    }
    // XP11 writes the shortest representation that round-trips, and ALWAYS keeps a
    // decimal point (1.0, not 1) — dropping it is exactly the kind of "equivalent"
    // rewrite that turns a patch into a whole-file diff.
    std::string s = ShortestRoundTrip(value);
    const bool hasPoint = s.find('.') != std::string::npos ||
                          s.find('e') != std::string::npos ||
                          s.find('E') != std::string::npos;
    return hasPoint ? s : s + ".0";
}

std::string SetProps(const std::string& text,
                     const std::map<std::string, std::string>& values,
                     int& changed) {
    std::map<std::string, std::string> remaining(values);
    std::string out;
    out.reserve(text.size() + 64);
    changed = 0;

    // Keeping each line's own terminator is what makes a CRLF file stay CRLF and a
    // mixed-ending file survive untouched outside the lines we rewrite.
    for (const std::string& raw : fsutil::SplitLinesKeepEnds(text)) {
        std::size_t bodyLen = raw.size();
        while (bodyLen > 0 && (raw[bodyLen - 1] == '\n' || raw[bodyLen - 1] == '\r')) {
            --bodyLen;
        }
        const std::string body = raw.substr(0, bodyLen);
        const std::string eol = raw.substr(bodyLen);

        const PropLine p = ParseLine(body);
        const auto it = p.matched ? remaining.find(p.prop) : remaining.end();
        if (it == remaining.end()) {
            out += raw;
            continue;
        }
        const std::string rewritten = p.head + it->second;
        if (rewritten != body) ++changed;
        remaining.erase(it);
        out += rewritten;
        out += eol;
    }

    if (!remaining.empty()) {
        std::ostringstream msg;
        msg << "properties not found in this .acf: ";
        bool first = true;
        for (const auto& kv : remaining) {
            if (!first) msg << ", ";
            msg << kv.first;
            first = false;
        }
        msg << " — not a ToLiss A3xx .acf, or its format has changed";
        throw AcfError(msg.str());
    }
    return out;
}

bool NumericEquals(const std::string& raw, double want) {
    double got = 0.0;
    if (!ParseDouble(raw, got)) return false;
    return std::fabs(got - want) <= kTolerance;
}

int FormatVersion(const std::string& text) {
    // ⚠ THE FIRST FEW LINES ONLY. The stamp is line 2 of every `.acf` X-Plane
    // writes; a whole-file search for "Version" would eventually find a livery
    // name, a comment or a property value and report it as the format.
    constexpr int kHeaderLines = 6;
    std::size_t at = 0;
    for (int line = 0; line < kHeaderLines && at < text.size(); ++line) {
        const std::size_t eol = text.find('\n', at);
        const std::string body = fsutil::Trim(
            text.substr(at, eol == std::string::npos ? std::string::npos : eol - at));
        at = eol == std::string::npos ? text.size() : eol + 1;

        const std::size_t sp = body.find(' ');
        if (sp == std::string::npos || sp == 0) continue;
        if (fsutil::Trim(body.substr(sp + 1)) != "Version") continue;
        const std::string digits = body.substr(0, sp);
        if (digits.find_first_not_of("0123456789") != std::string::npos) continue;
        return std::atoi(digits.c_str());
    }
    return 0;
}

bool IsForXp12(const std::string& text) {
    const int version = FormatVersion(text);
    return version == 0 || version >= kXp12FormatVersion;
}

std::vector<std::string> AcfFilesContaining(const std::string& aircraftDirUtf8,
                                            const std::string& needle,
                                            Variants which) {
    std::vector<std::string> found;
    const fs::path dir = fsutil::PathFromUtf8(aircraftDirUtf8);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return found;

    std::vector<fs::path> candidates;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;
        const std::string name = fsutil::PathToUtf8(it->path().filename());
        // ⚠ The `.acf` suffix test is also what excludes backups: Durantula keeps
        // an `a320.acf.durantula.bak` of this very file, and patching a backup
        // would corrupt that mod's uninstall.
        if (!fsutil::EndsWith(fsutil::ToLower(name), ".acf")) continue;
        candidates.push_back(it->path());
    }
    std::sort(candidates.begin(), candidates.end());

    for (const fs::path& p : candidates) {
        std::string text;
        if (!fsutil::ReadFileBytes(p, text)) continue;
        if (text.find(needle) == std::string::npos) continue;
        // ⚠ AFTER THE NEEDLE, NOT BEFORE IT. The stamp only decides between files
        // that are already the kind we act on, so a folder holding no matching
        // `.acf` at all still reports "none found" rather than "none for XP12".
        if (which == Variants::Xp12 && !IsForXp12(text)) continue;
        found.push_back(fsutil::PathToUtf8(p));
    }
    return found;
}

}  // namespace acf
}  // namespace photon
