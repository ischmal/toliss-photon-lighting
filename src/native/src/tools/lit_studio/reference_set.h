// The local ToLiss stock-texture reference set.
//
// The studio needs each airframe's *stock* `text.png` / `text_LIT.png` to tune a
// palette against. Browsing for them by hand is four dialogs per comparison, so
// they are harvested once into `reference/toliss/` and the tool finds them by
// itself on startup.
//
// ⚠ THIS SET IS LOCAL-ONLY AND MUST NEVER BE COMMITTED. `reference/` is otherwise
// a committed folder — `reference/gus/` is vendored WITH PERMISSION and says so
// in its README. ToLiss's textures are payware assets we have no right to
// redistribute, and this repository is public and GPLv3. So the payload is
// gitignored and only the README and this harvester are tracked: anyone with the
// aircraft can regenerate the set in one command, and nobody without them
// receives a byte.
//
// ⚠ HARVEST FROM `objects/`, NEVER FROM `Photon Backup Files/`. The backup holds
// whatever was stock when Photon last installed, and ToLiss ships updates: on
// 2026-08-24 the A319/A320 backup was a whole revision behind `objects/` — it
// still carried ToLiss's old, misaligned trim-scale artwork, which their update
// had since fixed. Harvesting the backup would have frozen a bug we exist to
// avoid.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/lit_recolor.h"

namespace photon {
namespace studio {

// One harvested texture pair.
struct ReferenceTexture {
    lit::Texture texture = lit::Texture::kPlacards;
    std::filesystem::path albedo;  // the day texture
    std::filesystem::path lit;     // the emissive one we recolor
};

// One airframe the tool can select. Several airframes share a `set` when their
// stock textures are byte-identical — the A319 and A320 do.
struct ReferenceAirframe {
    std::string key;    // "a320"
    std::string label;  // "A320"
    std::string set;    // "a319-a320"
    bool shared = false;  // is this set used by more than one airframe?
    std::vector<ReferenceTexture> textures;

    // Null when this airframe's harvest predates the texture, which is the one
    // case every caller has to answer for rather than assume.
    const ReferenceTexture* Find(lit::Texture t) const;
};

struct ReferenceSet {
    std::filesystem::path root;
    std::vector<ReferenceAirframe> airframes;
    // The install harvest scanned, kept so "Apply to sim" needs no path prompt.
    std::string xplaneRoot;
};

// Read `<dir>/index.json`. False (with `error`) when the folder has not been
// harvested yet — which is not a failure the tool should treat as fatal.
bool LoadReferenceSet(const std::filesystem::path& dir, ReferenceSet& out,
                      std::string& error);

// Walk up from `start` looking for `reference/toliss/index.json`, so the tool
// finds the set whether it is run from the build tree, the repo root or a
// shortcut. Empty when nothing is found.
std::filesystem::path FindReferenceDir(const std::filesystem::path& start);

// Gus's own finished `text_LIT.png`, vendored at `reference/gus/textures/`.
// Found the same way, so the studio can load the thing it is being measured
// against without a file dialog.
//
// ⚠ UNLIKE `reference/toliss/`, THIS ONE IS COMMITTED — Gus gave permission and
// his README says so. It is still someone else's artwork; the studio reads it
// and never writes it.
//
// ⚠ IT IS A RECOLOR OF THE A319/A320 STOCK ATLAS, so on an A321 it carries
// different ToLiss artwork in the trim-scale block. That is not a palette
// error, and the comparison must not count it as one — see the footprint test
// in the studio's compare pane.
std::filesystem::path FindGusTexture(const std::filesystem::path& start,
                                     lit::Texture texture);

// Scan an X-Plane install for ToLiss A3xx airframes, copy each one's stock
// `text.png` / `text_LIT.png` into `outDir`, de-duplicate byte-identical sets and
// write the index. `report` accumulates human-readable lines either way.
//
// It also re-checks the invariant the shared profiles rest on — that every
// preserve rect and promote region holds identical artwork in all harvested sets
// — and says so in the report. If that ever stops being true, one palette can no
// longer serve every airframe and the rect table has to become per-airframe.
bool HarvestReferenceSet(const std::string& xplaneRootUtf8,
                         const std::filesystem::path& outDir,
                         std::string& report, std::string& error);

// ⚠ IS THIS TEXTURE ALREADY RECOLORED? Asked of the PIXELS, never of the install
// marker.
//
// ⚠ AND NEVER OF AN ABSOLUTE CHANNEL VALUE. The first version tested the placard
// amber for `r >= 200 && blue <= 20`, which is true of the shipped incandescent
// palette and of nothing else. The moment a tuning session pulled red down to
// 180 — an ordinary thing to do in a tool that exists to change the palette —
// our own output stopped matching, read as stock, and the apply gate then
// refused to write ever again. The test is now a RATIO (blue against red), so it
// says "this amber has had its blue taken away" rather than "this is the one
// palette I know".
//
// On the knobs it is structural instead and needs no palette knowledge at all:
// the blackout rects hold 82,761 lit pixels in stock and none in ours.
bool LooksRecolored(const lit::Image& img, lit::Texture texture);

// --- pushing a look into the sim --------------------------------------------

struct ApplyResult {
    std::string label;    // "A320"
    std::string texture;  // "Placards" / "Knobs"
    std::string path;     // the texture written
    bool wrote = false;
    std::string note;     // what happened
    std::string warning;  // and anything worth saying about it
};

// One spec per texture. Missing entries are skipped, so a caller can push just
// the knobs without touching the placards.
struct SpecSet {
    bool has[lit::kTextureCount] = {false, false};
    lit::Spec spec[lit::kTextureCount];

    void Set(lit::Texture t, const lit::Spec& s) {
        has[static_cast<int>(t)] = true;
        spec[static_cast<int>(t)] = s;
    }
};

// Recolor every installed A3xx and write the results into its `objects/`, so the
// next aircraft reload in X-Plane shows them.
//
// ⚠ THE SOURCE IS ALWAYS THE HARVESTED STOCK, NEVER THE FILE BEING OVERWRITTEN.
// Recoloring our own output would compound the curve — and silently, since blue
// is already clipped to zero, so the second pass looks like a no-op while
// freezing the result in as if it were stock.
//
// ⚠ IT NEVER REFUSES. This is a bench, and its whole job is to write the same
// files over and over; a gate that declines to act is a gate that costs a tuning
// iteration every time it is wrong. Staleness is still DETECTED and reported —
// see `ApplyResult::warning` — but the write happens regardless. (The installer,
// which applies a fixed profile once to a stranger's aircraft, is where refusing
// is the right answer; that is §9 of docs/lit_recolor.md and a different
// caller.)
bool ApplyToAircraft(const std::string& xplaneRootUtf8, const ReferenceSet& set,
                     const SpecSet& specs, bool dryRun,
                     const std::string& onlyAirframe,
                     std::vector<ApplyResult>& out, std::string& error);

// Put every aircraft's stock textures back, from the harvested set.
bool RevertAircraft(const std::string& xplaneRootUtf8, const ReferenceSet& set,
                    const std::string& onlyAirframe,
                    std::vector<ApplyResult>& out, std::string& error);

}  // namespace studio
}  // namespace photon
