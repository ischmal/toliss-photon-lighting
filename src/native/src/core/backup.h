// WHERE PHOTON'S BOOKKEEPING LIVES — the stock files it replaced, the manifest
// that indexes them, and the note that tells a user not to delete either.
//
// ⚠ IT MOVED OUT OF `objects/` ON 2026-08-14 AND BOTH LOCATIONS ARE STILL LIVE:
//
//     now                 <aircraft>/Photon Backup Files/
//     every earlier       <aircraft>/objects/Photon Backup Files/
//     installer
//
// ⚠ THE ERA IS TOLD FROM THE FOLDER, NEVER FROM A VERSION NUMBER. Nothing reads
// the manifest's `version` to decide this, and nothing should: a tester bundle can
// carry any version with either layout, and the folder that is actually on disk is
// the only thing that has never been wrong.
//
// `objects/` is the aircraft's ASSET folder — the one X-Plane resolves OBJ and
// texture references against, the one SkunkCrafts syncs on an aircraft update, and
// the one every tool that walks an aircraft (ours included) treats as "the files
// this aeroplane is made of". A second, byte-identical copy of ToLiss's own OBJs
// sitting one level down inside it is a thing waiting to be found by something that
// recurses: our own glow pass iterates one level deep and is safe *by accident*, and
// on the A330-900 that copy runs to hundreds of megabytes. The backups belong beside
// the aircraft's assets, not among them.
//
// ⚠ THE LEGACY FOLDER WINS WHILE IT EXISTS, and that is the whole rule. `Dir()` is
// what every reader and writer asks, so the backups and the manifest that indexes
// them can never end up in different folders — including when a migration fails
// halfway. `Migrate()` is the only thing that changes the answer.
//
// ⚠ NOT FINDING A LEGACY MANIFEST IS THE EXPENSIVE FAILURE, not moving one. An
// install that reads "no manifest" on an aircraft that HAS one takes a fresh backup
// of the file it installed last time — enshrining Photon's own OBJ as the stock
// original, which is exactly what the backup-once rule exists to prevent, and it is
// unrecoverable. Every read path therefore goes through `Dir()`; none of them may
// name a location directly.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace photon {
namespace backup {

namespace fs = std::filesystem;

// `<aircraft>/Photon Backup Files/` — where an install puts it now.
fs::path PreferredDir(const fs::path& aircraftDir);

// `<aircraft>/objects/Photon Backup Files/` — where every earlier installer put it.
fs::path LegacyDir(const fs::path& aircraftDir);

// True while a legacy folder still holds FILES. ⚠ Files, not merely a folder:
// an empty leftover must not keep sending new backups back into `objects/`.
bool LegacyDirInUse(const fs::path& aircraftDir);

// THE ONE RESOLVER. Legacy while it is in use, the preferred location otherwise.
fs::path Dir(const fs::path& aircraftDir);

// The same answer for a caller that holds the `objects/` folder instead — the
// manifest API is keyed that way and every one of its callers already has it.
// A trailing separator is tolerated, so `…/objects/` does not resolve one level
// short of the aircraft.
fs::path DirForObjects(const fs::path& objectsDir);

// What `Migrate` did, in the shape the installer's step list and log want.
struct MigrateResult {
    bool needed = false;    // there was a legacy folder with files in it
    bool complete = false;  // …and it is now gone. False after a dry run.
    int files = 0;
    std::vector<std::string> log;
};

// Move a legacy folder to the new location. A no-op when there is nothing to move,
// and safe to call on every install.
//
// ⚠ A LEGACY FILE OVERWRITES A SAME-NAMED ONE AT THE DESTINATION. It cannot be
// newer — nothing has written to `objects/` since the upgrade — so it is the older
// copy, i.e. the one closer to stock, and the backup-once rule says the oldest copy
// is the one to keep.
//
// ⚠ THE DESTINATION IS WRITTEN BEFORE THE SOURCE IS REMOVED, always. A failure
// partway leaves the file in both places and the legacy folder still in use, so the
// next run resolves to legacy and merges again — it converges instead of losing a
// user's only copy of a stock file.
MigrateResult Migrate(const fs::path& aircraftDir, bool dryRun);

// ─── the note in the folder ──────────────────────────────────────────────────
// At the aircraft root the folder is somewhere a user actually browses, so it says
// what it is. ⚠ VERSION-FREE AND DATE-FREE: it is rewritten on every install, and
// content that changed per run would churn a file nobody reads twice.
extern const char kReadmeName[];
std::string ReadmeText();

}  // namespace backup
}  // namespace photon
