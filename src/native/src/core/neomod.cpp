#include "core/neomod.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

#include "core/fsutil.h"

namespace photon {
namespace neomod {
namespace {

namespace fs = std::filesystem;

// A Lua line comment. ⚠ NEEDED, not fastidiousness: the mod's own script carries
// commented-out experiments, and a commented `set(...)` writes nothing. Counting
// one would report a mod on an install whose user had already disabled the line
// by hand — the exact "warns me about something I already fixed" failure that the
// `Scripts (disabled)` rule exists to prevent.
bool LineIsCommented(const std::string& text, std::size_t pos) {
    std::size_t bol = text.rfind('\n', pos);
    bol = (bol == std::string::npos) ? 0 : bol + 1;
    for (std::size_t i = bol; i + 1 < pos; ++i) {
        if (text[i] == '-' && text[i + 1] == '-') return true;
    }
    return false;
}

// Occurrences of `needle` that are not inside a Lua comment.
std::vector<std::size_t> LiveHits(const std::string& text, const std::string& needle) {
    std::vector<std::size_t> hits;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
        if (!LineIsCommented(text, at)) hits.push_back(at);
    }
    return hits;
}

// Is the occurrence at `at` an argument to a WRITE?
//
// ⚠ A READ MUST NOT COUNT. FlyWithLua reaches a dataref two ways: `set("<ref>",
// v)` writes it, and `dataref("name", "<ref>", "writable")` binds it — the bind
// alone still writes nothing, but the mode string is what distinguishes a script
// that intends to write from one that only watches. So: `set(` before the ref,
// or a `"writable"` after it on the same call.
// The last `name(` call opening before `upTo`, as a lowercase bare name.
//
// ⚠ WORD-BOUNDED. A plain "ends with set(" test matches `offset(`, `reset(`,
// `asset(` — and a false positive here does not merely mis-parse, it tells a user
// to brighten a cockpit nothing is dimming.
std::string LastCallHead(const std::string& prefix) {
    std::size_t best = std::string::npos;
    std::string bestName;
    for (const char* name : {"set", "dataref"}) {
        const std::string open = std::string(name) + "(";
        for (std::size_t at = prefix.find(open); at != std::string::npos;
             at = prefix.find(open, at + 1)) {
            if (at > 0) {
                const unsigned char before = (unsigned char)prefix[at - 1];
                if (std::isalnum(before) || before == '_') continue;
            }
            if (best == std::string::npos || at > best) {
                best = at;
                bestName = name;
            }
        }
    }
    return bestName;
}

bool IsWriteContext(const std::string& text, std::size_t at, std::size_t needleLen) {
    // ⚠ FROM THE START OF THE LINE, not by walking back over punctuation. The
    // first version of this scanned backwards over `("` and whitespace, which
    // works for `set("<ref>", v)` and fails on `dataref("name", "<ref>", …)` —
    // where the reference is the SECOND argument and a comma plus another string
    // literal sit between it and the call head. That is the exact form the mods
    // in question use for half their datarefs.
    std::size_t bol = text.rfind('\n', at);
    bol = (bol == std::string::npos) ? 0 : bol + 1;
    const std::string head = fsutil::ToLower(text.substr(bol, at - bol));
    const std::string call = LastCallHead(head);

    if (call == "set") return true;
    if (call == "dataref") {
        // The mode string is the third argument. ⚠ Stop at the statement's end so
        // a later, unrelated "writable" on another line cannot be borrowed.
        const std::size_t tailFrom = at + needleLen;
        std::size_t eol = text.find('\n', tailFrom);
        if (eol == std::string::npos) eol = text.size();
        const std::string tail = fsutil::ToLower(text.substr(tailFrom, eol - tailFrom));
        return tail.find("writable") != std::string::npos;
    }
    return false;
}

}  // namespace

// ─── the pieces ──────────────────────────────────────────────────────────────

bool WritesSpillCutoff(const std::string& luaText) {
    const std::string needle(kSpillCutoffRef);
    for (std::size_t at : LiveHits(luaText, needle)) {
        if (IsWriteContext(luaText, at, needle.size())) return true;
    }
    return false;
}

bool ParseSpillCutoff(const std::string& luaText, double& out) {
    const std::string needle(kSpillCutoffRef);
    for (std::size_t at : LiveHits(luaText, needle)) {
        if (!IsWriteContext(luaText, at, needle.size())) continue;
        // …"<ref>" , <number> )   — everything between the quote and the `)`.
        std::size_t i = at + needle.size();
        while (i < luaText.size() && (luaText[i] == '"' || luaText[i] == '\'')) ++i;
        while (i < luaText.size() &&
               (std::isspace(static_cast<unsigned char>(luaText[i])) ||
                luaText[i] == ',')) {
            ++i;
        }
        const char* begin = luaText.c_str() + i;
        char* end = nullptr;
        const double v = std::strtod(begin, &end);
        // ⚠ A BARE `strtod` RETURNS 0.0 ON FAILURE, and 0.0 is a legal cutoff
        // meaning "cull nothing". Without the pointer test a `dataref()` binding
        // (whose next token is a mode string, not a number) would be reported as
        // a mod that had switched culling OFF — the opposite of the truth.
        if (end == begin) continue;
        out = v;
        return true;
    }
    return false;
}

bool LooksRecognized(const std::string& luaText) {
    return !LiveHits(luaText, kSpillCutoffRef).empty() &&
           !LiveHits(luaText, kPhotoBbEvRef).empty() &&
           !LiveHits(luaText, kTurbidityRef).empty();
}

// ─── the result ──────────────────────────────────────────────────────────────

std::vector<std::string> Result::Texts(Severity s) const {
    std::vector<std::string> out;
    for (const Finding& f : findings) {
        if (f.severity == s) out.push_back(f.text);
    }
    return out;
}

std::string Result::Token() const {
    switch (confidence) {
        case Confidence::kRecognized: return "bss_neo";
        case Confidence::kUnknownMod: return "unknown";
        case Confidence::kNone:       break;
    }
    return "none";
}

// ─── the check ───────────────────────────────────────────────────────────────

Result Detect(const std::string& xplaneRootUtf8) {
    Result r;
    r.summary = "no lighting mod is raising the spill-light cutoff";

    if (xplaneRootUtf8.empty()) {
        r.findings.push_back({Severity::Note, "no X-Plane root given; not checked"});
        return r;
    }

    // ⚠ ONE DIRECTORY, NOT RECURSIVE, AND NEVER THE DISABLED SIBLINGS. See the
    // header: `Scripts (disabled)/` and `Scripts (Quarantine)/` sit right beside
    // this one on a real install and neither runs.
    const fs::path scripts =
        fsutil::PathFromUtf8(xplaneRootUtf8) / fsutil::PathFromUtf8(kScriptsDir);

    std::error_code ec;
    if (!fs::is_directory(scripts, ec) || ec) {
        // No FlyWithLua at all is the ordinary case, not a problem worth a line.
        return r;
    }

    fs::directory_iterator it(scripts, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        r.findings.push_back(
            {Severity::Note, "could not read the FlyWithLua Scripts folder; not checked"});
        return r;
    }

    // ⚠ `it.increment(ec)`, the house idiom (see detect.cpp, acf.cpp) — a
    // range-for's operator++ THROWS on an iteration error, which would break the
    // non-throwing contract in the header from inside an install.
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::directory_entry& e = *it;
        std::error_code fec;
        if (!e.is_regular_file(fec) || fec) continue;
        if (fsutil::ToLower(fsutil::PathToUtf8(e.path().extension())) != ".lua") continue;

        std::string text;
        if (!fsutil::ReadFileBytes(e.path(), text)) {
            // ⚠ Unreadable is NOT "clean" and NOT an error either — say so and
            // move on, so a locked file cannot silently produce a false all-clear.
            r.findings.push_back({Severity::Note,
                                  "could not read " +
                                      fsutil::PathToUtf8(e.path().filename()) +
                                      "; it was not checked"});
            continue;
        }
        if (!WritesSpillCutoff(text)) continue;

        const bool recognized = LooksRecognized(text);
        const std::string name = fsutil::PathToUtf8(e.path().filename());

        // ⚠ A RECOGNIZED SCRIPT OUTRANKS AN UNRECOGNIZED ONE, whatever the
        // directory order hands us. Two scripts both writing the cutoff is
        // vanishingly rare, but reporting "unknown" because it sorted first
        // would lose the one thing the user can act on: the mod's name.
        if (recognized || !r.Detected()) {
            r.scriptName = name;
            r.spillCutoff = -1.0;
            double v = 0.0;
            if (ParseSpillCutoff(text, v)) r.spillCutoff = v;
            r.confidence =
                recognized ? Confidence::kRecognized : Confidence::kUnknownMod;
        }
    }

    if (!r.Detected()) return r;

    const std::string who = (r.confidence == Confidence::kRecognized)
                                ? "BSS NEO"
                                : "a FlyWithLua script";
    std::string cutoff;
    if (r.spillCutoff >= 0.0) {
        // Two decimals is the whole precision anyone authors this at.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", r.spillCutoff);
        cutoff = std::string(" to ") + buf;
    }

    r.summary = who + " (" + r.scriptName + ") raises the spill-light cutoff" +
                cutoff;

    // ⚠ A PROBLEM, NOT A NOTE — it dims the cockpit the user paid attention to,
    // and it is the reason this whole module exists. But see the compensation in
    // core/patch_intensity.h: Photon's answer is to brighten its OWN lights, never
    // to write the setting back. The finding says what to do, not what we did.
    r.findings.push_back(
        {Severity::Problem,
         who + " is raising X-Plane's spill-light cutoff, which dims Photon's "
               "cockpit lamps. Enable the BSS NEO option so the cockpit is built "
               "bright enough to show through it."});

    if (r.confidence == Confidence::kUnknownMod) {
        r.findings.push_back(
            {Severity::Note,
             "the script was not recognized as BSS NEO; the compensation is the "
             "same either way"});
    }
    return r;
}

}  // namespace neomod
}  // namespace photon
