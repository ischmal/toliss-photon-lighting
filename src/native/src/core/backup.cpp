#include "core/backup.h"

#include <system_error>

#include "core/constants.h"
#include "core/fsutil.h"

namespace photon {
namespace backup {
namespace {

// Every regular file under `dir`, or nothing at all if it is not a folder.
std::vector<fsutil::FileEntry> FilesUnder(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return {};
    return fsutil::ListFilesRecursive(dir);
}

// Move one file, destination first. Returns false only if the DESTINATION could
// not be written — a source that survives is reported by the caller's re-scan.
bool MoveFile(const fs::path& from, const fs::path& to,
              std::vector<std::string>& log) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);

    // ⚠ A RENAME REPLACES AN EXISTING DESTINATION on both platforms we ship
    // (MoveFileEx with MOVEFILE_REPLACE_EXISTING, POSIX rename), which is the
    // legacy-wins rule for free — and it is a metadata operation, so a 1.6 GB
    // folder of A339 mesh backups costs the same as an empty one.
    std::error_code re;
    fs::rename(from, to, re);
    if (!re) return true;

    // The fallback: copy, verify by writing atomically, then drop the source.
    // Reachable when something holds the file open (an antivirus scan mid-install
    // is the realistic one) — never because of a volume boundary, since both paths
    // are inside the aircraft folder.
    std::string data;
    if (!fsutil::ReadFileBytes(from, data)) {
        log.push_back("  ! could not read " + fsutil::PathToUtf8(from));
        return false;
    }
    std::error_code we;
    if (!fsutil::AtomicWriteBytes(to, data, we)) {
        log.push_back("  ! could not write " + fsutil::PathToUtf8(to) + " (" +
                      we.message() + ")");
        return false;
    }
    std::error_code de;
    fs::remove(from, de);
    if (de) {
        log.push_back("  ~ copied but could not remove " +
                      fsutil::PathToUtf8(from) + " — it will be retried");
    }
    return true;
}

}  // namespace

const char kReadmeName[] = "README.txt";

fs::path PreferredDir(const fs::path& aircraftDir) {
    return aircraftDir / kBackupDirName;
}

fs::path LegacyDir(const fs::path& aircraftDir) {
    return aircraftDir / "objects" / kBackupDirName;
}

bool LegacyDirInUse(const fs::path& aircraftDir) {
    return !FilesUnder(LegacyDir(aircraftDir)).empty();
}

fs::path Dir(const fs::path& aircraftDir) {
    if (LegacyDirInUse(aircraftDir)) return LegacyDir(aircraftDir);
    return PreferredDir(aircraftDir);
}

fs::path DirForObjects(const fs::path& objectsDir) {
    // `a / "objects"` has no trailing separator, but a path that came in as text
    // may: `…/objects/` has an EMPTY filename, so `parent_path()` on it answers
    // `…/objects` and the backup folder would land one level too deep.
    fs::path objects = objectsDir;
    if (objects.filename().empty()) objects = objects.parent_path();
    return Dir(objects.parent_path());
}

MigrateResult Migrate(const fs::path& aircraftDir, bool dryRun) {
    MigrateResult r;
    const fs::path legacy = LegacyDir(aircraftDir);
    const fs::path dest = PreferredDir(aircraftDir);

    const std::vector<fsutil::FileEntry> files = FilesUnder(legacy);
    if (files.empty()) {
        // An empty leftover folder — from a partial move, or an uninstall that
        // could not remove it — is still in `objects/`, so it still goes.
        if (!dryRun) fsutil::RemoveDirIfEmpty(legacy);
        r.complete = true;
        return r;
    }

    r.needed = true;
    r.files = static_cast<int>(files.size());
    r.log.push_back("moving " + fsutil::PathToUtf8(legacy) + " → " +
                    fsutil::PathToUtf8(dest) + " (" + std::to_string(r.files) +
                    " file(s))");
    if (dryRun) {
        r.log.push_back("  . dry run — nothing moved");
        return r;
    }

    // The fast path: rename the whole folder. Only available when the destination
    // is not already there — `fs::rename` will not merge two directories.
    std::error_code ec;
    if (!fs::exists(dest, ec)) {
        std::error_code re;
        fs::rename(legacy, dest, re);
        if (!re) {
            r.complete = true;
            return r;
        }
        r.log.push_back("  ~ folder rename failed (" + re.message() +
                        ") — moving file by file");
    }

    for (const fsutil::FileEntry& f : files) {
        MoveFile(f.abs, dest / fsutil::PathFromUtf8(f.rel), r.log);
    }

    // ⚠ RE-SCANNED, NOT INFERRED FROM THE LOOP'S RETURN VALUES. What matters is
    // whether anything is LEFT — a file that was copied but whose source could not
    // be removed is a successful move whose cleanup failed, and one that was never
    // written at all must keep the folder in use so the next run tries again.
    if (FilesUnder(legacy).empty()) {
        std::error_code re;
        fs::remove_all(legacy, re);   // only empty directories remain by here
        r.complete = !re;
    }
    if (!r.complete) {
        r.log.push_back("  ! some files could not be moved — the folder under "
                        "objects/ stays in use, and the move is retried on the "
                        "next run");
    }
    return r;
}

std::string ReadmeText() {
    // ⚠ CRLF and plain ASCII. This is opened by double-clicking it on Windows, and
    // a stray é or a lone LF is a worse first impression than the plainest possible
    // text. It names no version and no date so a reinstall rewrites it identically.
    static const char* const kLines[] = {
        "ToLiss Photon Lighting - backup folder",
        "",
        "This folder holds the ORIGINAL files that came with your aircraft, saved",
        "before ToLiss Photon Lighting replaced them, plus the record of what was",
        "changed (photon_manifest.json).",
        "",
        "DO NOT DELETE THIS FOLDER while Photon is installed. Uninstalling Photon",
        "puts these files back and then removes the folder for you. Without it,",
        "the uninstaller has nothing to restore and you would have to reinstall",
        "the aircraft to get the original files back.",
        "",
        "It is safe to leave here. X-Plane never loads anything from this folder.",
        "",
        "Earlier versions of the installer kept this folder inside objects/. If you",
        "still have one there, it was moved here automatically and nothing is lost.",
        "",
    };
    std::string out;
    for (const char* line : kLines) {
        out += line;
        out += "\r\n";
    }
    return out;
}

}  // namespace backup
}  // namespace photon
