// `photon_manifest.json` — the record of what an install did, and therefore the
// only thing an uninstall has to work from.
//
// ⚠ THE SCHEMA DOES NOT CHANGE IN THE C++ PORT. Manifests written by every prior
// Python installer version must keep working: a user who installed with 0.7 and
// uninstalls with 0.9 gets their files back only if this reads what that wrote.
//
// ⚠ READS ARE TOLERANT — missing or corrupt is treated as ABSENT, never as an
// error, exactly as the Python `read_manifest` is. A manifest we cannot parse must
// read as "no install found" (which the caller reports honestly) rather than
// aborting an uninstall the user still needs.
//
// The asymmetry that matters most is `backed_up[]` vs `added[]`:
//
//   backed_up[]  a stock file we replaced      -> uninstall RESTORES it
//   added[]      a file with no stock original -> uninstall DELETES it
//
// Restoring a backup that never existed leaves `pedals_details_{1,2}.png` and
// `lights_screens.obj` in the aircraft folder forever, which is why they are in
// their own list rather than in `backed_up[]`.
#pragma once

#include <string>
#include <vector>

namespace photon {
namespace manifest {

// The interior ("Gus Mod") block — an independent, opt-in axis.
struct Interior {
    bool installed = false;
    std::string version;
    std::string airframe;
    // ⚠ DELETED on uninstall (no stock counterpart), not restored.
    std::vector<std::string> added;
    // Livery `.dds`/`.png` files that SHADOWED our textures, backed up under
    // `backup/liveries/` with `/` flattened to `__`, and removed so our PNG
    // resolves. Uninstall puts them back.
    std::vector<std::string> liveriesPatched;
    // `.acf` file NAMES whose cockpit spot block we patched. Reversal writes
    // stock values back rather than restoring a snapshot.
    std::vector<std::string> acfPatched;
};

// The display-glow block. Two steps, both reversible, NEITHER a backup.
struct Screens {
    bool installed = false;
    std::string version;
    std::vector<std::string> added;        // lights_screens.obj — DELETE on uninstall
    std::vector<std::string> acfPatched;   // detach BEFORE deleting the OBJ
};

struct Manifest {
    // False when there was no file, or it was corrupt. Callers must check this
    // rather than inferring from an empty version.
    bool present = false;

    std::string version;
    std::string wing;
    std::string installedAt;      // ISO 8601, seconds
    std::vector<std::string> backedUp;
    // ⚠ The counterpart of `backedUp` for files we WROTE that had no stock
    // original — DELETED on uninstall. Normally empty: every file an exterior
    // install writes into the aircraft replaces a ToLiss one. It fills only when
    // a target was ABSENT at install time (a damaged folder, or a ToLiss update
    // that dropped a file), where recording nothing would orphan our copy in the
    // aircraft forever. `interior.added` and `screens.added` are the same idea
    // per-axis; this is the top-level one, so it is not gated on a flag.
    std::vector<std::string> added;

    bool realwingsPatched = false;
    std::string realwingsDir;

    Interior interior;
    Screens screens;

    // Anything this build did not recognize, preserved verbatim so a round trip
    // through an older installer cannot silently drop a newer one's bookkeeping.
    std::string unknownJson;
};

// Path of the manifest for an aircraft, addressed by its `objects/` folder — the
// key every caller already has.
//
// ⚠ IT IS NOT INSIDE `objects/`. The manifest lives in the backup folder, beside
// the files it accounts for, and that folder moved to the aircraft root on
// 2026-08-14 while the old location stays readable. `backup::Dir` decides; nothing
// here or anywhere else may build the path by hand.
std::string PathFor(const std::string& objectsDirUtf8);

// ⚠ Never throws. Missing/corrupt comes back with `present == false`.
Manifest Read(const std::string& objectsDirUtf8);
Manifest Parse(const std::string& json);

std::string Serialize(const Manifest& m);
bool Write(const std::string& objectsDirUtf8, const Manifest& m,
           std::string& errorOut);

}  // namespace manifest
}  // namespace photon
