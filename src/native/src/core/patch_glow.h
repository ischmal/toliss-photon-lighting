// Redirect ToLiss's skin-glow LIT-texture regions to Photon's own dataref.
//
// ToLiss's exterior mesh OBJs carry lines like
//
//     ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[N] <nits>
//
// that make painted "glow" regions — wingtip strobe halos, the tail strobe, the
// beacon domes — light up. ToLiss's own plugin animates that array with its
// strike-and-fade envelope, and the array is READ-ONLY to other plugins, so Photon
// cannot make the skin flash in lockstep with its crisp LED-square billboards by
// writing it. The fix is a one-token-per-region string swap: repoint each region
// Photon drives at `ToLissPhoton/exterior/glow[N]`, an array the plugin registers
// and drives from the same engine value the billboard gets.
//
// No OBJ content is added — only the dataref token changes; the index and the
// trailing nits are left exactly as ToLiss wrote them, so X-Plane's parser sees an
// identical `ATTR_light_level` line.
//
// ⚠ ONLY THE INDICES PHOTON ACTUALLY DRIVES ARE REDIRECTED, and the table lives in
// `core/constants.h` for that reason. A redirected index Photon does NOT drive goes
// DARK — our array reads 0 where we never write — so `GlowRedirect()` must match
// `GlowMapForIcao` in plugin.cpp. Both now read the same vector, which is what
// turns "keep three files in sync" into a fact about one.
//
// The installer backs up each modified OBJ into `Photon Backup Files/` before
// patching and restores it on uninstall, so no `.bak` siblings are written here.
// The reverse is provided for the dev/live-tuning path — the swap is exactly
// invertible, so it restores ToLiss's original line byte for byte.
#pragma once

#include <string>
#include <vector>

#include "core/progress.h"

namespace photon {
namespace patch_glow {

const std::vector<int>* IndicesFor(const std::string& airframeKey);

// OBJs under `root` that still need (un)patching for this airframe — i.e. that
// contain the search-side token for any redirected index.
//
// Scanning rather than a hard-coded file list on purpose: it survives ToLiss
// moving a region between OBJs, and it is what the installer uses to know which
// files to back up before patching.
// ⚠ THIS IS THE EXPENSIVE ONE. It reads every `.obj` in the folder in full, which
// is 233 MB on an A319 and 1,597 MB on an A330-900 — the largest single cost in an
// install, dwarfing everything Photon actually writes. `onProgress` is reported
// per file and weighted by BYTES; see the note at the loop.
std::vector<std::string> TargetFiles(const std::string& objectsDirUtf8,
                                     const std::string& airframeKey,
                                     bool reverse = false,
                                     const progress::StepProgress& onProgress = {});

// Swap the glow datarefs in one OBJ's text. Returns the number of regions changed.
// Idempotent: re-running finds nothing left to swap.
std::string PatchText(const std::string& text, const std::string& airframeKey,
                      bool reverse, int& changed);

struct RunResult {
    int regions = 0;
    std::vector<std::string> touched;
    std::vector<std::string> log;
};

// ⚠ PASS `scanned` IF YOU ALREADY CALLED `TargetFiles`. Without it this scans the
// whole folder AGAIN — the installer does need the list first, to back the files up,
// so the default path read every `.obj` in the aircraft twice per install. `nullptr`
// keeps the self-scanning behavior for callers that have no list.
RunResult Run(const std::string& objectsDirUtf8, const std::string& airframeKey,
              bool reverse, bool dryRun,
              const std::vector<std::string>* scanned = nullptr,
              const progress::StepProgress& onProgress = {});

}  // namespace patch_glow
}  // namespace photon
