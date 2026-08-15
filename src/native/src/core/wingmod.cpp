#include "core/wingmod.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

#include "core/acf.h"
#include "core/constants.h"
#include "core/fsutil.h"
#include "core/patch_acf_screens.h"

namespace photon {
namespace wingmod {
namespace {

namespace fs = std::filesystem;

std::string BaseName(const std::string& p) {
    const std::size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// `dir/file.obj` -> `dir`, or empty when the path has no directory part.
std::string FirstSegment(const std::string& p) {
    const std::size_t slash = p.find_first_of("/\\");
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

std::string Join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out += sep;
        out += parts[i];
    }
    return out;
}

std::size_t CountOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

// ─── one `.acf` variant, read once ───────────────────────────────────────────
// ⚠ THE SAME ENUMERATION RULE AS `acf::AcfFilesContaining`, DELIBERATELY NOT THE
// SAME CALL. That helper reads every candidate to test a needle and returns paths,
// so a caller that then parses them pays the 9 MB read twice — and this check runs
// inside an install that already pays it twice more (the screens attach, the
// cockpit spot patch). Reading once and keeping the text is the only difference;
// the `.acf` suffix test still excludes both `a320.acf.bak` and Durantula's
// `a320.acf.durantula.bak`, and files are still identified by CONTENT below rather
// than by an assumed `a320{,_StdDef,_XP11,_XP11_StdDef}.acf` naming.
//
// ⚠ THE XP11 PAIR IS SKIPPED, AND HERE THAT IS A CORRECTNESS FIX RATHER THAN A
// SAVING (2026-08-15). The consensus below asks the variants to agree — but
// RealWings' install is a Plane Maker session on ONE file, and the two variants
// X-Plane 12 never loads are not evidence about the aeroplane. Including them made
// a correct RealWings install report that its `.acf` files "do not agree" and name
// an XP11 file as the dissenter, which is a Problem the user cannot act on and
// should not have been shown. ⚠ `_StdDef` STAYS IN: X-Plane 12 loads it, so a real
// disagreement there is a real warning.
struct AcfFile {
    std::string name;    // filename only — every message names a file
    std::string text;
};

std::vector<AcfFile> ReadAcfFiles(const fs::path& aircraftDir) {
    std::vector<fs::path> candidates;
    std::error_code ec;
    if (!fs::is_directory(aircraftDir, ec)) return {};
    for (fs::directory_iterator it(aircraftDir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;
        const std::string name = fsutil::PathToUtf8(it->path().filename());
        if (!fsutil::EndsWith(fsutil::ToLower(name), ".acf")) continue;
        candidates.push_back(it->path());
    }
    std::sort(candidates.begin(), candidates.end());

    std::vector<AcfFile> out;
    for (const fs::path& p : candidates) {
        AcfFile f;
        f.name = fsutil::PathToUtf8(p.filename());
        if (!fsutil::ReadFileBytes(p, f.text)) continue;
        if (!acf::IsForXp12(f.text)) continue;
        out.push_back(std::move(f));
    }
    return out;
}

// The attachments X-Plane actually READS, and the ones it does not.
//
// ⚠ `_obja/count` IS THE LIVE/INERT LINE AND IT IS NOT A FORMALITY. X-Plane reads
// rows 0..count-1; a row past it sits in the file, greps as installed, and draws
// nothing. `patch_acf_screens::IsAttached` already has to know this about our own
// row, and a wing mod's rows are added by hand in Plane Maker, so they are at
// least as able to end up there.
struct Attachments {
    std::vector<std::string> live;
    std::vector<std::string> inert;
    bool usable = false;
};

Attachments ReadAttachments(const std::string& text) {
    Attachments out;
    int count = 0;
    try {
        count = patch_acf_screens::DeclaredCount(text);
    } catch (const acf::AcfError&) {
        return out;    // not an aircraft `.acf` with an attachment table
    }
    out.usable = true;
    for (const auto& row : patch_acf_screens::AttachmentRows(text)) {
        const auto it = row.second.find(patch_acf_screens::kFileField);
        if (it == row.second.end()) continue;
        const std::string file = fsutil::Trim(it->second);
        if (file.empty()) continue;
        (row.first < count ? out.live : out.inert).push_back(file);
    }
    return out;
}

// ─── the aircraft's own wing OBJs, classified by their flex driver ───────────
struct ObjFlex {
    bool read = false;
    int durantula = 0;   // `…wing_tip_deflection_deg` blocks
    int stock = 0;       // `anim/winglex` blocks

    // ⚠ DURANTULA WINS ON ANY COUNT, and stock is not "no durantula". A converted
    // wing OBJ still carries `anim/winglex` on the chains the mod does not touch
    // (7 of them in a stock A320 `wingL.obj`, 3 of which the mod replaces), so
    // "has anim/winglex" is true of BOTH states and can only ever answer the
    // negative case.
    const char* Wing() const {
        if (!read) return nullptr;
        if (durantula > 0) return kDurantula;
        if (stock > 0) return kStock;
        return nullptr;   // a wing OBJ with no flex chain at all
    }
};

class ObjCache {
public:
    explicit ObjCache(const fs::path& objects) : objects_(objects) {}

    // Keyed on the attachment string, so the same OBJ named by all four `.acf`
    // variants is read once rather than four times.
    const ObjFlex& Get(const std::string& attachment) {
        const auto it = cache_.find(attachment);
        if (it != cache_.end()) return it->second;
        ObjFlex f;
        std::string text;
        if (fsutil::ReadFileBytes(objects_ / fsutil::PathFromUtf8(attachment),
                                  text)) {
            f.read = true;
            f.durantula = CountDurantulaBlocks(text);
            f.stock = static_cast<int>(CountOccurrences(text, kStockFlexRef));
        }
        return cache_.emplace(attachment, f).first->second;
    }

private:
    fs::path objects_;
    std::map<std::string, ObjFlex> cache_;
};

// ─── one `.acf`'s answer ─────────────────────────────────────────────────────
struct FileVerdict {
    std::string name;
    std::string wing;                     // empty = this file could not say
    std::vector<std::string> rwLive;
    std::vector<std::string> rwInert;
    std::vector<std::string> stockWings;  // live, flex-carrying
    std::vector<std::string> stockGlass;  // live
    std::vector<std::string> mixedWings;  // stock and durantula side by side
    bool usable = false;
};

FileVerdict Inspect(const AcfFile& file, const std::string& realWingsDir,
                    ObjCache& objs) {
    FileVerdict v;
    v.name = file.name;
    const Attachments att = ReadAttachments(file.text);
    if (!att.usable) return v;
    v.usable = true;

    for (const std::string& a : att.live) {
        if (!realWingsDir.empty() && IsUnderDir(a, realWingsDir)) {
            v.rwLive.push_back(a);
        } else if (IsStockWingObj(a)) {
            v.stockWings.push_back(a);
        } else if (IsStockWingGlassObj(a)) {
            v.stockGlass.push_back(a);
        }
    }
    for (const std::string& a : att.inert) {
        if (!realWingsDir.empty() && IsUnderDir(a, realWingsDir)) {
            v.rwInert.push_back(a);
        }
    }

    // ⚠ REALWINGS WINS WHEN BOTH ARE PRESENT, and it has to. If the table does not
    // name `wingL.obj`, then whatever Durantula wrote into `wingL.obj` is not
    // drawn — the file is still on disk, still modded, and completely inert.
    if (!v.rwLive.empty()) {
        v.wing = kRealWings;
        return v;
    }

    bool anyDurantula = false, anyStock = false;
    for (const std::string& a : v.stockWings) {
        const char* w = objs.Get(a).Wing();
        // Unreadable, or a wing OBJ with no flex chain at all: it contributes
        // nothing either way rather than voting for stock by default.
        if (w == nullptr) continue;
        if (std::string(w) == kDurantula) {
            anyDurantula = true;
        } else {
            anyStock = true;
            v.mixedWings.push_back(a);
        }
    }
    if (anyDurantula) {
        v.wing = kDurantula;
        // Only a MIX is worth naming; an all-Durantula aircraft has no leftovers.
        if (!anyStock) v.mixedWings.clear();
    } else if (anyStock) {
        v.wing = kStock;
        v.mixedWings.clear();
    }
    return v;
}

void Add(Result& r, Severity s, const std::string& text) {
    r.findings.push_back({s, text});
}

std::string Plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

// ─── the small public predicates ─────────────────────────────────────────────

int CountDurantulaBlocks(const std::string& objText) {
    return static_cast<int>(CountOccurrences(objText, kDurantulaFlexRef));
}

bool IsStockWingObj(const std::string& attachmentPath) {
    const std::string base = fsutil::ToLower(BaseName(attachmentPath));
    if (!fsutil::EndsWith(base, ".obj")) return false;
    if (base.find("wing") == std::string::npos) return false;
    return base.find("glass") == std::string::npos;
}

bool IsStockWingGlassObj(const std::string& attachmentPath) {
    const std::string base = fsutil::ToLower(BaseName(attachmentPath));
    if (!fsutil::EndsWith(base, ".obj")) return false;
    return base.find("wing") != std::string::npos &&
           base.find("glass") != std::string::npos;
}

bool IsUnderDir(const std::string& attachmentPath, const std::string& dirName) {
    if (dirName.empty()) return false;
    return fsutil::ToLower(FirstSegment(attachmentPath)) ==
           fsutil::ToLower(dirName);
}

bool Result::Healthy() const {
    for (const Finding& f : findings) {
        if (f.severity == Severity::Problem) return false;
    }
    return true;
}

std::vector<std::string> Result::Texts(Severity s) const {
    std::vector<std::string> out;
    for (const Finding& f : findings) {
        if (f.severity == s) out.push_back(f.text);
    }
    return out;
}

// ─── the check ───────────────────────────────────────────────────────────────

Result Detect(const std::string& aircraftDirUtf8, const std::string& airframeKey) {
    Result r;

    // ⚠ THE AIRFRAME GATE COMES FIRST AND READS NOTHING. The A330-900 has no wing
    // mods in existence, so there is no such thing as a wrong answer to find — and
    // `RealWingsDir` returns empty for it, which would make every predicate below
    // vacuously false and the verdict "undetermined" on an aircraft where stock is
    // the only possibility there has ever been.
    if (WingsFor(airframeKey).size() <= 1) {
        r.wing = kStock;
        r.determined = true;
        r.summary = "wing mod: none exists for this airframe — nothing to check";
        return r;
    }

    const fs::path aircraftDir = fsutil::PathFromUtf8(aircraftDirUtf8);
    const fs::path objects = aircraftDir / "objects";
    const std::string rwDir = RealWingsDir(airframeKey);

    const std::vector<AcfFile> files = ReadAcfFiles(aircraftDir);
    ObjCache objs(objects);

    std::vector<FileVerdict> verdicts;
    for (const AcfFile& f : files) {
        FileVerdict v = Inspect(f, rwDir, objs);
        if (v.usable) verdicts.push_back(std::move(v));
    }

    if (verdicts.empty()) {
        r.summary =
            "wing mod: could not be determined — no .acf with an attachment table "
            "in " + fsutil::PathToUtf8(aircraftDir.filename());
        Add(r, Severity::Note,
            "Photon cannot tell which wing mod this aircraft is running, so the "
            "wing variant you picked is taken at face value");
        return r;
    }

    // ── the consensus ────────────────────────────────────────────────────────
    std::map<std::string, int> tally;
    for (const FileVerdict& v : verdicts) {
        if (!v.wing.empty()) ++tally[v.wing];
    }
    if (!tally.empty()) {
        int best = 0;
        for (const auto& kv : tally) best = std::max(best, kv.second);
        // ⚠ MAJORITY, WITH TIES BROKEN BY FILE ORDER rather than by the tally's
        // (alphabetical) map order. Sorted order puts the plain `a320.acf` — the
        // file RealWings' own instructions name, and the one almost everybody
        // flies — ahead of its three variants, so a 2-2 split answers with the
        // main aircraft rather than with whichever mod name sorts first.
        for (const FileVerdict& v : verdicts) {
            if (!v.wing.empty() && tally[v.wing] == best) {
                r.wing = v.wing;
                break;
            }
        }
        r.determined = true;
    }
    r.agreed = tally.size() <= 1;

    // ── per-file problems ────────────────────────────────────────────────────
    for (const FileVerdict& v : verdicts) {
        // ⚠ ATTACHED BUT PAST `_obja/count`: in the file, never read, draws
        // nothing. The single hardest wing-mod failure to see by eye, because
        // every instruction the user followed IS in the `.acf`.
        if (!v.rwInert.empty() && v.rwLive.empty()) {
            Add(r, Severity::Problem,
                v.name + " lists " + Plural(v.rwInert.size(), "RealWings object",
                                            "RealWings objects") +
                    " past _obja/count, so X-Plane never reads them — the mod is "
                    "attached but does not draw (" + Join(v.rwInert, ", ") + ")");
        }
        // Both sets of wings attached at once. RealWings' instructions clear the
        // stock rows; skipping that step leaves two wings in the same place.
        if (!v.rwLive.empty()) {
            std::vector<std::string> leftovers = v.stockWings;
            leftovers.insert(leftovers.end(), v.stockGlass.begin(),
                             v.stockGlass.end());
            if (!leftovers.empty()) {
                Add(r, Severity::Problem,
                    v.name + " attaches RealWings AND still attaches the stock " +
                        Join(leftovers, ", ") +
                        " — both sets of wings will draw; RealWings' instructions "
                        "clear those rows");
            }
        }
        // Named in the table, absent from disk.
        for (const std::string& a : v.rwLive) {
            std::error_code ec;
            if (!fs::is_regular_file(objects / fsutil::PathFromUtf8(a), ec) || ec) {
                Add(r, Severity::Problem,
                    v.name + " attaches " + a +
                        " but that file is not in objects/ — that part of the wing "
                        "will not draw");
            }
        }
        if (!v.mixedWings.empty()) {
            Add(r, Severity::Problem,
                "Durantula's wingflex is in some of this aircraft's wing OBJs but "
                "not in " + Join(v.mixedWings, ", ") +
                    " — that wing keeps ToLiss's own flex and the two will not "
                    "match. Its manual has six replacement blocks, three per wing");
        }
        // ⚠ A NOTE, AND THE HONEST ONE. A table naming neither the aircraft's own
        // wings nor `RealWings<nnn>/` means SOMETHING replaced the wing geometry —
        // RealWings under a folder the user renamed, or a mod Photon has never
        // heard of. Guessing which of the other forty attachments is a wing would
        // be speculation; saying the wings are not ToLiss's is a fact, and it
        // stops "undetermined" reading as "nothing to see here".
        if (v.rwLive.empty() && v.stockWings.empty()) {
            Add(r, Severity::Note,
                v.name + " attaches neither this aircraft's own wing OBJs nor " +
                    (rwDir.empty() ? std::string("a known wing mod")
                                   : rwDir + "/") +
                    " — something has replaced the wings that Photon does not "
                    "recognize");
        }
    }

    // ── the four variants must agree ─────────────────────────────────────────
    //
    // ⚠ THE MOST LIKELY REALWINGS MIS-INSTALL THERE IS, and Photon cannot paper
    // over it: one `lights_out` OBJ serves all four `.acf` variants, so a build for
    // the wing mod is right for the variants that have it and wrong for the ones
    // that do not. The fix is in Plane Maker, not here.
    if (!r.agreed) {
        std::vector<std::string> detail;
        for (const FileVerdict& v : verdicts) {
            detail.push_back(v.name + " = " +
                             (v.wing.empty() ? "undetermined" : v.wing));
        }
        Add(r, Severity::Problem,
            "this aircraft's .acf variants do not agree about the wing mod (" +
                Join(detail, ", ") +
                ") — whichever variant you load decides which wings you get, and "
                "one Photon build cannot be right for both");
    }

    // ── Durantula's own completeness ─────────────────────────────────────────
    if (r.wing == kDurantula) {
        std::set<std::string> seen;
        std::vector<std::string> names;
        std::vector<int> counts;
        for (const FileVerdict& v : verdicts) {
            for (const std::string& a : v.stockWings) {
                if (!seen.insert(a).second) continue;
                const ObjFlex& f = objs.Get(a);
                if (f.durantula <= 0) continue;
                names.push_back(BaseName(a) + " (" + std::to_string(f.durantula) +
                                ")");
                counts.push_back(f.durantula);
            }
        }
        const bool uneven =
            !counts.empty() &&
            *std::min_element(counts.begin(), counts.end()) !=
                *std::max_element(counts.begin(), counts.end());
        if (uneven) {
            // ⚠ ASYMMETRY IS THE VERSION-PROOF TEST. The mod is symmetric by
            // construction, so port and starboard must carry the same number of
            // converted blocks whatever release the user has; the absolute count
            // is only ever a note, since a later version may add a case.
            Add(r, Severity::Problem,
                "Durantula's wingflex is applied unevenly (" + Join(names, ", ") +
                    ") — the wings will flex by different amounts. Its manual has "
                    "six replacement blocks, three per wing");
        } else if (!counts.empty() && counts.front() != kDurantulaBlocksPerWing) {
            Add(r, Severity::Note,
                "Durantula's wingflex converts " + std::to_string(counts.front()) +
                    " block(s) per wing here; V1.3's manual lists " +
                    std::to_string(kDurantulaBlocksPerWing) +
                    ". If that is not the version you have, this is nothing");
        }
    }

    // ── assets on disk that nothing draws ────────────────────────────────────
    //
    // ⚠ A NOTE, NEVER A PROBLEM, AND THIS IS THE WHOLE POINT OF THE FILE. Both
    // mods leave their folders behind after being reverted, so "RealWings320/ is
    // in objects/" is true of aircraft running stock wings, Durantula wings, and
    // RealWings wings alike. Saying it out loud is useful; treating it as an
    // install would be the exact mistake this check exists to avoid.
    if (r.wing != kRealWings && !rwDir.empty()) {
        std::error_code ec;
        if (fs::is_directory(objects / fsutil::PathFromUtf8(rwDir), ec) && !ec) {
            Add(r, Severity::Note,
                "objects/" + rwDir +
                    "/ is present but no .acf attaches it — the mod's files are "
                    "installed, its wings are not");
        }
    }
    if (r.wing == kRealWings) {
        std::set<std::string> seen;
        std::vector<std::string> dormant;
        for (const FileVerdict& v : verdicts) {
            for (const std::string& a : v.stockWings) {
                if (!seen.insert(a).second) continue;
                if (objs.Get(a).durantula > 0) dormant.push_back(BaseName(a));
            }
        }
        // The stock rows are gone under RealWings, so these are read only when
        // some variant still attaches them — hence the check above, not instead
        // of it. Worth a line either way: it explains a Durantula folder that is
        // doing nothing.
        if (!dormant.empty()) {
            Add(r, Severity::Note,
                "Durantula's wingflex is still in " + Join(dormant, ", ") +
                    ", but RealWings replaces those meshes, so it does not draw");
        }
    }

    // ── the verdict line ─────────────────────────────────────────────────────
    std::ostringstream s;
    if (r.wing == kRealWings) {
        std::size_t n = 0;
        for (const FileVerdict& v : verdicts) n = std::max(n, v.rwLive.size());
        s << "wing mod: RealWings — " << Plural(n, "object", "objects") << " from "
          << rwDir << "/ attached in the .acf";
    } else if (r.wing == kDurantula) {
        std::set<std::string> seen;
        std::vector<std::string> names;
        for (const FileVerdict& v : verdicts) {
            for (const std::string& a : v.stockWings) {
                if (!seen.insert(a).second) continue;
                const ObjFlex& f = objs.Get(a);
                names.push_back(BaseName(a) + ": " + std::to_string(f.durantula));
            }
        }
        s << "wing mod: Durantula wingflex — converted blocks per wing OBJ ("
          << Join(names, ", ") << ")";
    } else if (r.wing == kStock) {
        std::set<std::string> seen;
        std::vector<std::string> names;
        for (const FileVerdict& v : verdicts) {
            for (const std::string& a : v.stockWings) {
                if (seen.insert(a).second) names.push_back(BaseName(a));
            }
        }
        s << "wing mod: none — " << Join(names, ", ")
          << " still carry ToLiss's own " << kStockFlexRef << " flex chain";
    } else {
        s << "wing mod: could not be determined — the .acf attachment table names "
             "no wing OBJ this installer recognizes";
        Add(r, Severity::Note,
            "the wing variant you picked is taken at face value");
    }
    r.summary = s.str();
    return r;
}

}  // namespace wingmod
}  // namespace photon
