#include "core/patch_intensity.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/constants.h"
#include "core/fsutil.h"
#include "third_party/nlohmann/json.hpp"

namespace photon {
namespace intensity {

namespace fs = std::filesystem;

namespace {

// Sized to the "%.2f" both the prefs file and the factor marker are written
// with: two factors that render the same two-decimal string compare equal.
constexpr double kEpsilon = 0.005;

// One spelling for every number this patcher writes back into a light line.
// %.6g keeps the generator's own look (whole numbers bare, no trailing zeros)
// and cannot reach scientific notation in the sizes' range.
std::string FormatNum(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

void SplitIndent(const std::string& line, std::string& indent,
                 std::string& body) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    indent = line.substr(0, i);
    body = line.substr(i);
}

struct Token {
    std::size_t begin = 0;
    std::size_t end = 0;   // one past
    std::string text;
};

std::vector<Token> Tokens(const std::string& body) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < body.size()) {
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
        if (i >= body.size()) break;
        Token t;
        t.begin = i;
        while (i < body.size() && body[i] != ' ' && body[i] != '\t') ++i;
        t.end = i;
        t.text = body.substr(t.begin, t.end - t.begin);
        out.push_back(t);
    }
    return out;
}

// Which token is the brightness (`size`, the OBJ line's slot 8) for this light
// line — or -1 for any line this patcher must leave alone. The two LIGHT_PARAM
// classes are Gus's whole roster; an unknown class is somebody else's light.
//
//   LIGHT_PARAM <class> x y z  r g b  index SIZE  dx dy dz  cone     -> token 9
//   LIGHT_SPILL_CUSTOM  x y z  r g b  alpha SIZE  dx dy dz  semi df  -> token 8
int SizeTokenIndex(const std::vector<Token>& toks) {
    if (toks.size() >= 14 && toks[0].text == "LIGHT_PARAM" &&
        (toks[1].text == "airplane_panel_sp" ||
         toks[1].text == "airplane_inst_sp")) {
        return 9;
    }
    if (toks.size() >= 14 && toks[0].text == "LIGHT_SPILL_CUSTOM") return 8;
    return -1;
}

// `body` with its size token scaled by `factor`; every other byte — spacing
// included — is carried through untouched. Returns `body` unchanged when the
// line does not parse (which the SizeTokenIndex gate makes unreachable, but a
// patcher must not turn a surprise into a corrupt line).
std::string ScaleSize(const std::string& body, double factor) {
    const std::vector<Token> toks = Tokens(body);
    const int idx = SizeTokenIndex(toks);
    if (idx < 0) return body;
    const Token& t = toks[static_cast<std::size_t>(idx)];
    char* endp = nullptr;
    const double v = std::strtod(t.text.c_str(), &endp);
    if (endp == t.text.c_str()) return body;
    return body.substr(0, t.begin) + FormatNum(v * factor) + body.substr(t.end);
}

}  // namespace

double Clamp(double factor) {
    // ⚠ The NaN-rejecting comparison: !(NaN >= kMin) is true, so garbage from a
    // hand-edited prefs file lands on the harmless end of the range.
    if (!(factor >= kMin)) return kMin;
    if (factor > kMax) return kMax;
    return factor;
}

double ClampEffective(double factor) {
    // Same NaN-rejecting shape as Clamp, against the product's own ceiling.
    if (!(factor >= kMin)) return kMin;
    if (factor > kEffectiveMax) return kEffectiveMax;
    return factor;
}

double EffectiveFactor(double userFactor, bool neo) {
    return ClampEffective(Clamp(userFactor) * (neo ? kNeoBoost : 1.0));
}

bool AtDefault(double factor) { return SameFactor(factor, kDefault); }

bool SameFactor(double a, double b) { return std::fabs(a - b) < kEpsilon; }

std::string Label(double factor) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3gx", factor);
    return buf;
}

double FactorIn(const std::string& text) {
    for (const std::string& line : fsutil::SplitLines(text)) {
        const std::string t = fsutil::Trim(line);
        if (!fsutil::StartsWith(t, kFactorPrefix)) continue;
        const std::string value = t.substr(sizeof(kFactorPrefix) - 1);
        char* endp = nullptr;
        const double f = std::strtod(value.c_str(), &endp);
        if (endp == value.c_str()) return kDefault;
        return f;
    }
    return kDefault;
}

bool CanPatch(const std::string& text) {
    return text.find(kInteriorObjNeedle) != std::string::npos &&
           text.find(kDebugNeedle) == std::string::npos;
}

std::string PatchText(const std::string& text, double factor, int& scaled) {
    scaled = 0;
    // ⚠ ClampEffective, not Clamp — this is a write path, and the BSS NEO
    // compensation legitimately puts the baked factor above the slider's kMax.
    const double f = ClampEffective(factor);
    const bool active = !AtDefault(f);
    const std::string nl = fsutil::DetectNewline(text);
    const std::vector<std::string> lines = fsutil::SplitLines(text);

    std::vector<std::string> out;
    out.reserve(lines.size() + 1);

    // The original light line parsed out of a `# ToLissPhoton orig ` comment,
    // waiting for the scaled line that follows it (which is discarded).
    bool havePending = false;
    std::string pendingOrig;
    std::string pendingIndent;

    for (const std::string& line : lines) {
        std::string indent, body;
        SplitIndent(line, indent, body);

        // A previous patch's own lines are consumed, never copied: the factor
        // marker is re-derived at the end, and an orig comment becomes the base
        // for the light line that follows it.
        if (fsutil::StartsWith(body, kFactorPrefix)) continue;
        if (fsutil::StartsWith(body, kOrigPrefix)) {
            pendingOrig = body.substr(sizeof(kOrigPrefix) - 1);
            pendingIndent = indent;
            havePending = true;
            continue;
        }

        const bool isLight = SizeTokenIndex(Tokens(body)) >= 0;
        if (!isLight) {
            // ⚠ An orig comment not followed by a light line (a hand-edited
            // file) is put back verbatim rather than silently swallowed.
            if (havePending) {
                out.push_back(pendingIndent + kOrigPrefix + pendingOrig);
                havePending = false;
            }
            out.push_back(line);
            continue;
        }

        // A light line. Its authored form is the preserved original when one
        // is pending, else the line itself is still authored.
        const std::string base = havePending ? pendingOrig : body;
        const bool restored = havePending;
        havePending = false;
        if (active) {
            out.push_back(indent + kOrigPrefix + base);
            out.push_back(indent + ScaleSize(base, f));
            ++scaled;
        } else {
            out.push_back(indent + base);
            if (restored) ++scaled;
        }
    }
    if (havePending) out.push_back(pendingIndent + kOrigPrefix + pendingOrig);

    // The factor marker, after POINT_COUNTS like the installer's version
    // marker — and only while a factor is applied, so a 1x file is exactly the
    // authored one.
    if (active) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%.2f", kFactorPrefix, f);
        bool inserted = false;
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (fsutil::StartsWith(out[i], "POINT_COUNTS")) {
                out.insert(out.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                           std::string(buf));
                inserted = true;
                break;
            }
        }
        if (!inserted) out.insert(out.begin(), std::string(buf));
    }
    return fsutil::JoinLines(out, nl);
}

namespace {

// The "$cockpit" block out of the prefs file, or a null json when anything at
// all is missing or unreadable. ⚠ ONE READER FOR BOTH KEYS: the factor and the
// BSS NEO flag are read on the same code path in both halves of the project, so
// a change to where prefs live cannot move one and leave the other behind.
nlohmann::json CockpitBlock(const std::string& xplaneRootUtf8) {
    if (xplaneRootUtf8.empty()) return nlohmann::json();
    const fs::path p = fsutil::PathFromUtf8(xplaneRootUtf8) / "Output" /
                       "preferences" / kProfilesJsonName;
    std::string text;
    if (!fsutil::ReadFileBytes(p, text)) return nlohmann::json();
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception&) {
        return nlohmann::json();   // corrupt reads as "never set", never as an error
    }
    if (!root.is_object()) return nlohmann::json();
    const auto block = root.find(kPrefsCockpitKey);
    if (block == root.end() || !block->is_object()) return nlohmann::json();
    return *block;
}

}  // namespace

double SavedFactor(const std::string& xplaneRootUtf8) {
    const nlohmann::json block = CockpitBlock(xplaneRootUtf8);
    if (!block.is_object()) return kDefault;
    const auto f = block.find(kIntensityJsonKey);
    if (f == block.end() || !f->is_number()) return kDefault;
    return Clamp(f->get<double>());
}

bool SavedNeo(const std::string& xplaneRootUtf8) {
    const nlohmann::json block = CockpitBlock(xplaneRootUtf8);
    if (!block.is_object()) return false;
    const auto f = block.find(kNeoJsonKey);
    // ⚠ Booleans only. A hand-edited string or number reads as false rather than
    // as truthy — the compensation changes what ships in the OBJ, so an
    // ambiguous prefs file must land on "do nothing".
    if (f == block.end() || !f->is_boolean()) return false;
    return f->get<bool>();
}

bool SaveNeo(const std::string& xplaneRootUtf8, bool neo) {
    if (xplaneRootUtf8.empty()) return false;
    const fs::path p = fsutil::PathFromUtf8(xplaneRootUtf8) / "Output" /
                       "preferences" / kProfilesJsonName;

    std::string text;
    const bool had = fsutil::ReadFileBytes(p, text);
    // Absence already means false; see the header. Nothing to do, and nothing
    // to create.
    if (!had && !neo) return true;

    nlohmann::json root = nlohmann::json::object();
    if (had) {
        try {
            root = nlohmann::json::parse(text);
        } catch (const nlohmann::json::exception&) {
            // ⚠ REFUSE, do not overwrite. A corrupt prefs file may still be the
            // only copy of the user's per-livery profiles, and rewriting it from
            // scratch to record one boolean would destroy them.
            return false;
        }
        if (!root.is_object()) return false;
    }

    nlohmann::json& block = root[kPrefsCockpitKey];
    if (!block.is_object()) block = nlohmann::json::object();
    block[kNeoJsonKey] = neo;

    return fsutil::AtomicWriteBytes(p, root.dump(2));
}

double FactorOfFile(const fs::path& obj) {
    std::string text;
    if (!fsutil::ReadFileBytes(obj, text)) return kDefault;
    return FactorIn(text);
}

Applied ApplyToFile(const fs::path& obj, double factor, std::string& detail) {
    const double f = ClampEffective(factor);
    std::string text;
    if (!fsutil::ReadFileBytes(obj, text)) {
        detail = "could not read " + fsutil::PathToUtf8(obj);
        return Applied::kFailed;
    }
    if (!CanPatch(text)) {
        detail = fsutil::PathToUtf8(obj.filename()) +
                 " is not a patchable Photon interior OBJ (stock, or a --debug "
                 "build) - left alone";
        return Applied::kSkipped;
    }
    if (SameFactor(FactorIn(text), f)) {
        detail = "already at " + Label(f);
        return Applied::kAlready;
    }
    int scaled = 0;
    const std::string patched = PatchText(text, f, scaled);
    std::error_code ec;
    if (!fsutil::AtomicWriteBytes(obj, patched, ec)) {
        detail = "could not write " + fsutil::PathToUtf8(obj) + " (" +
                 ec.message() + ")";
        return Applied::kFailed;
    }
    detail = Label(f) + " applied to " + std::to_string(scaled) +
             " light line(s) - takes effect on the next aircraft load";
    return Applied::kPatched;
}

}  // namespace intensity
}  // namespace photon
