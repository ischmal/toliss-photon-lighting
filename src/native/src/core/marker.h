// The OBJ version marker: `# ToLissPhoton version: X wing: Y`.
//
// Injected right after the OBJ's POINT_COUNTS header line, and read back for TWO
// jobs that are easy to conflate:
//
//  1. it is the version source when no manifest is present (a manifest-less
//     hand-copy, or a pre-manifest install), and
//  2. ⚠ it CORROBORATES a manifest. Manifest intact but marker gone means an
//     aircraft update (SkunkCrafts) reverted the OBJ to stock out from under us
//     while leaving the bookkeeping in `Photon Backup Files/` untouched. The
//     install needs redoing even though the manifest survived — so a surviving
//     manifest alone is NOT proof the edit is still live.
#pragma once

#include <string>

namespace photon {
namespace marker {

struct Marker {
    bool found = false;
    std::string version;
    std::string wing;
};

// Parse the first marker in `text`. Callers scanning a large OBJ pass only its
// head — the marker sits within the first few lines by construction.
Marker Parse(const std::string& text);

// Insert the marker after the POINT_COUNTS line, or at the top if there is none.
//
// ⚠ PRESERVES THE FILE'S OWN NEWLINE FLAVOR. These OBJs are read and rewritten by
// other tools; flipping every line ending is a gratuitous whole-file diff.
std::string Inject(const std::string& text, const std::string& version,
                   const std::string& wing);

// The exact line `Inject` writes, exposed so tests and the installer's step text
// can name it without rebuilding the format string.
std::string Line(const std::string& version, const std::string& wing);

}  // namespace marker
}  // namespace photon
