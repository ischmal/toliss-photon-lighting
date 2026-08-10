#include "core/actions.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>

#include "core/constants.h"
#include "core/fsutil.h"
#include "core/marker.h"
#include "core/patch_acf.h"
#include "core/patch_acf_screens.h"
#include "core/patch_glow.h"
#include "core/patch_realwings.h"
#include "core/payload.h"
#include "core/version.h"

namespace photon {
namespace actions {
namespace {

namespace fs = std::filesystem;

std::string Join(const std::vector<std::string>& v, const char* sep) {
    std::ostringstream os;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) os << sep;
        os << v[i];
    }
    return os.str();
}

std::string NowIso8601Seconds() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    ::localtime_s(&tm, &t);
#else
    ::localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

void WriteOrThrow(const fs::path& dest, const std::string& data) {
    std::error_code ec;
    if (!fsutil::AtomicWriteBytes(dest, data, ec)) {
        throw ActionError("couldn't write " + fsutil::PathToUtf8(dest) + " (" +
                          ec.message() +
                          ") — check X-Plane is closed and you have permission to "
                          "write there (try running as administrator)");
    }
}

std::string CopyBytes(const fs::path& src) {
    std::string data;
    if (!fsutil::ReadFileBytes(src, data)) {
        throw ActionError("couldn't read " + fsutil::PathToUtf8(src));
    }
    return data;
}

bool Contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// What BackupOnce found.
//
// ⚠ THE CALLER MUST TELL kAlreadyHave FROM kNoOriginal, which a bool cannot: both
// mean "no new backup", but only the second makes the file the caller is about to
// write an ADDED one. Collapsing them is what left a Photon file in the aircraft
// folder forever when its stock counterpart happened to be missing.
enum class Backup {
    kRecorded,      // a new backup was taken (or an on-disk one adopted)
    kAlreadyHave,   // already in backed_up[] — still restorable, nothing to do
    kNoOriginal,    // nothing was there to back up
};

// ⚠ Copy `src`'s CURRENT contents into the backup dir the FIRST TIME ONLY. Never
// overwrite an existing backup: on a reinstall or upgrade that would clobber the
// true original with a Photon file.
//
// `added` is the manifest list to consult and record into when there is no stock
// original — pass it at a site that then WRITES a payload file over `src`, so
// uninstall deletes what it cannot restore. ⚠ Pass NULLPTR at a site that PATCHES
// `src` IN PLACE (the glow meshes): there a missing source means nothing was
// written, and recording it would make uninstall delete a file that is not ours.
//
// ⚠ Consulting `added` is what keeps a REINSTALL idempotent. Second time round the
// file exists — because we wrote it — so without that check the "back up the
// original" branch would fire and enshrine OUR file as the stock one, which is
// precisely the clobber the backup-once rule exists to prevent.
Backup BackupOnce(const fs::path& src, const fs::path& backupDir,
                  manifest::Manifest& m, std::vector<std::string>* added,
                  Log& log, bool dryRun) {
    const std::string rel = fsutil::PathToUtf8(src.filename());
    if (Contains(m.backedUp, rel)) {
        log.Write("backup skipped (already have one): " + rel);
        return Backup::kAlreadyHave;
    }
    if (added != nullptr && Contains(*added, rel)) {
        log.Write("backup skipped (ours from an earlier install, no stock "
                  "original): " + rel);
        return Backup::kNoOriginal;
    }
    const fs::path dest = backupDir / src.filename();
    std::error_code ec;
    if (fs::exists(dest, ec) && !ec) {
        log.Write("backup already present on disk, recording: " + rel, "WARN");
    } else if (fs::exists(src, ec) && !ec) {
        log.Write("backing up " + fsutil::PathToUtf8(src) + " → " +
                  fsutil::PathToUtf8(dest));
        if (!dryRun) {
            std::error_code mk;
            fs::create_directories(backupDir, mk);
            WriteOrThrow(dest, CopyBytes(src));
        }
    } else {
        log.Write("nothing to back up (no stock original): " +
                  fsutil::PathToUtf8(src), "WARN");
        if (added != nullptr && !Contains(*added, rel)) added->push_back(rel);
        return Backup::kNoOriginal;
    }
    m.backedUp.push_back(rel);
    return Backup::kRecorded;
}

void LogLines(Log& log, const char* tag, const std::vector<std::string>& lines) {
    for (const std::string& l : lines) log.Write(std::string("[") + tag + "] " + l);
}

std::vector<std::string> BaseNames(const std::vector<std::string>& paths) {
    std::vector<std::string> out;
    out.reserve(paths.size());
    for (const std::string& p : paths) {
        out.push_back(fsutil::PathToUtf8(fsutil::PathFromUtf8(p).filename()));
    }
    return out;
}

// Livery files that would SHADOW our interior textures.
//
// X-Plane substitutes a `.dds` for the `.png` an OBJ names, so a livery shipping
// `chairs_LIT.dds` wins over our `chairs_LIT.png` in objects/ and the mod is simply
// invisible in that livery. Dropping our PNG into the livery folder does nothing —
// the fix is to back up and REMOVE the livery's file so ours resolves.
//
// Only stems matching our texture set are considered, so a livery's own unrelated
// overrides are never touched.
std::vector<fs::path> InteriorLiveryOverrides(const fs::path& aircraftDir) {
    std::vector<std::string> stems;
    for (const std::string& n : InteriorTextures()) {
        stems.push_back(fsutil::ToLower(
            fsutil::PathToUtf8(fsutil::PathFromUtf8(n).stem())));
    }
    std::vector<fs::path> hits;
    const fs::path liveries = aircraftDir / "liveries";
    std::error_code ec;
    if (!fs::is_directory(liveries, ec) || ec) return hits;

    std::vector<fs::path> liveryDirs;
    for (fs::directory_iterator it(liveries, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code de;
        if (it->is_directory(de) && !de) liveryDirs.push_back(it->path());
    }
    std::sort(liveryDirs.begin(), liveryDirs.end());

    for (const fs::path& livery : liveryDirs) {
        const fs::path objs = livery / "objects";
        std::error_code oe;
        if (!fs::is_directory(objs, oe) || oe) continue;
        std::vector<fs::path> files;
        for (fs::directory_iterator f(objs, oe), fend; f != fend; f.increment(oe)) {
            if (oe) break;
            std::error_code fe;
            if (f->is_regular_file(fe) && !fe) files.push_back(f->path());
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            const std::string stem =
                fsutil::ToLower(fsutil::PathToUtf8(f.stem()));
            const std::string ext = fsutil::ToLower(fsutil::PathToUtf8(f.extension()));
            if (!Contains(stems, stem)) continue;
            if (!Contains(InteriorLiveryExts(), ext)) continue;
            hits.push_back(f);
        }
    }
    return hits;
}

// The backup name a livery override is stored under: its aircraft-relative POSIX
// path with `/` flattened to `__`.
std::string LiveryBackupName(const std::string& relPosix) {
    std::string out(relPosix);
    std::string flat;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '/') {
            flat += "__";
        } else {
            flat.push_back(out[i]);
        }
    }
    return flat;
}

}  // namespace

bool PluginIsCurrent(const std::string& xplaneRootUtf8) {
    const fs::path destRoot = fsutil::PathFromUtf8(xplaneRootUtf8) / "Resources" /
                              "plugins" / kPluginFolder;
    std::error_code ec;
    if (!fs::is_directory(destRoot, ec) || ec) return false;
    try {
        for (const auto& kv : payload::PluginFiles()) {
            if (!fsutil::FilesIdentical(destRoot / fsutil::PathFromUtf8(kv.first),
                                        fsutil::PathFromUtf8(kv.second))) {
                return false;
            }
        }
    } catch (const payload::PayloadError&) {
        return false;
    }
    return true;
}

namespace {

// The plugin's runtime data (`panelfx.txt`, `overlays/`).
//
// ⚠ A SYMLINK IS LEFT ALONE. A dev keeps `panelfx.txt` symlinked at their repo copy
// so in-sim edits from a PHOTON_DEV build land in source control; overwriting the
// link with a plain file during a test install would detach that loop silently, and
// the next edit would go nowhere anyone looks. The check is on the DESTINATION and
// does not follow, so it holds whether or not the link resolves.
//
// Overlay images are written BY NAME, so a user's own images in the same folder are
// neither overwritten nor removed.
std::vector<std::string> InstallPluginData(const fs::path& pluginRoot, Log& log,
                                           bool dryRun) {
    std::vector<std::string> steps;
    const auto files = payload::PluginDataFiles();
    if (files.empty()) {
        log.Write("no plugin data files in this build (no panelfx.txt/overlays) "
                  "— skipped");
        return steps;
    }
    int wrote = 0;
    std::vector<std::string> linked;
    for (const auto& kv : files) {
        const fs::path dest = pluginRoot / fsutil::PathFromUtf8(kv.first);
        if (fsutil::IsSymlinkNoFollow(dest)) {
            log.Write("plugin data " + kv.first +
                      " is a symlink — left as-is (dev setup)");
            linked.push_back(kv.first);
            continue;
        }
        if (fsutil::FilesIdentical(dest, fsutil::PathFromUtf8(kv.second))) continue;
        ++wrote;
        if (!dryRun) WriteOrThrow(dest, CopyBytes(fsutil::PathFromUtf8(kv.second)));
    }
    log.Write("plugin data → " + fsutil::PathToUtf8(pluginRoot) + " (" +
              std::to_string(wrote) + " written, " +
              std::to_string(linked.size()) + " symlinked, " +
              std::to_string(files.size()) + " total)");
    steps.push_back(std::string("Installed Panel FX data (") + kPanelFxFile +
                    " + overlay images) → Resources/plugins/" + kPluginFolder);
    for (const std::string& rel : linked) {
        steps.push_back("Kept your symlinked " + rel + " — not overwritten");
    }
    return steps;
}

// The display-glow install: one OBJ plus one `.acf` attachment.
//
// ⚠ TWO STEPS, BOTH REVERSIBLE, NEITHER A BACKUP. `lights_screens.obj` has no stock
// counterpart, so it goes in `screens.added[]` and is DELETED on uninstall —
// BackupOnce would only log "nothing to back up" and leave it orphaned. The
// attachment is an `.acf` edit reversed by the patcher rather than by restoring a
// snapshot, so it composes with Durantula's rewrite of the very same `_obja` table.
//
// ⚠ THE OBJ ALONE DOES NOTHING. ToLiss neither ships nor references it, so an
// install that wrote the file but failed to attach it is not a half-working glow,
// it is no glow at all — which is why the attach result is its own step rather than
// folded into the OBJ's.
std::vector<std::string> InstallScreens(const fs::path& objects,
                                        manifest::Manifest& m, Log& log,
                                        bool dryRun) {
    std::vector<std::string> steps;
    const fs::path aircraftDir = objects.parent_path();

    if (!Contains(m.screens.added, kScreensObj)) {
        m.screens.added.emplace_back(kScreensObj);
    }
    const std::string text =
        marker::Inject(payload::ScreensObjText(), kPhotonVersion, "screens");
    const fs::path target = objects / kScreensObj;
    log.Write("writing " + fsutil::PathToUtf8(target) + " (" +
              std::to_string(text.size()) + " bytes)");
    if (!dryRun) WriteOrThrow(target, text);
    steps.push_back(std::string("Installed ") + kScreensObj + " (display glow) → " +
                    fsutil::PathToUtf8(objects.filename()));

    const auto res = patch_acf_screens::Run(fsutil::PathToUtf8(aircraftDir), false,
                                            dryRun);
    LogLines(log, "acf screens", res.log);
    m.screens.acfPatched = BaseNames(res.touched);
    if (!m.screens.acfPatched.empty()) {
        steps.push_back("Attached the display-glow OBJ in " +
                        std::to_string(m.screens.acfPatched.size()) +
                        " .acf file(s): " + Join(m.screens.acfPatched, ", "));
    } else {
        steps.push_back("! Could not attach the display-glow OBJ to any .acf — "
                        "display glow will not draw (the rest of the install is "
                        "fine)");
    }
    m.screens.installed = true;
    m.screens.version = kPhotonVersion;
    return steps;
}

// The interior ("Gus Mod") install — an INDEPENDENT, OPT-IN axis. Five parts: the
// OBJ, the texture set, the livery-shadow scan, the four-`.acf` cockpit-spot patch,
// and its own manifest block. A user who wants only the exterior mod never runs any
// of it, and uninstalling one axis does not disturb the other.
std::vector<std::string> InstallInterior(const Options& opts,
                                         const fs::path& objects,
                                         const fs::path& backupDir,
                                         manifest::Manifest& m, Log& log) {
    std::vector<std::string> steps;
    const fs::path aircraftDir = objects.parent_path();
    const bool dryRun = opts.dryRun;

    // 1. the OBJ. Inject anchors on POINT_COUNTS, which lights_inn.obj has.
    const fs::path target = objects / kInteriorObj;
    if (BackupOnce(target, backupDir, m, &m.interior.added, log, dryRun) ==
        Backup::kRecorded) {
        steps.push_back(std::string("Backed up original ") + kInteriorObj + " → " +
                        fsutil::PathToUtf8(objects.filename()) + "/" +
                        kBackupDirName);
    }
    const std::string text =
        marker::Inject(payload::InteriorObjText(), kPhotonVersion, "interior");
    log.Write("writing " + fsutil::PathToUtf8(target) + " (" +
              std::to_string(text.size()) + " bytes)");
    if (!dryRun) WriteOrThrow(target, text);
    steps.push_back(std::string("Installed ") + kInteriorObj +
                    " (interior lights, mod by Gus) → " +
                    fsutil::PathToUtf8(objects.filename()));

    // 2. the textures. ⚠ NINE replace a stock file and are restored from backup on
    //    uninstall; TWO have no stock counterpart, so they go in `added` and are
    //    DELETED instead — BackupOnce would only log "nothing to back up" and leave
    //    them orphaned in the aircraft folder forever.
    const auto textures = payload::InteriorTextureFiles();
    const int total = static_cast<int>(textures.size());
    int i = 0;
    for (const auto& kv : textures) {
        ++i;
        const fs::path dest = objects / fsutil::PathFromUtf8(kv.first);
        if (InteriorTextureIsAdded(kv.first)) {
            if (!Contains(m.interior.added, kv.first)) {
                m.interior.added.push_back(kv.first);
            }
        } else {
            // ⚠ The nine that REPLACE a stock file — but only if that file is
            // actually there. A ToLiss folder missing one (damaged, or a future
            // update that dropped it) would otherwise leave our copy behind on
            // uninstall, since a backup that was never taken cannot restore it.
            BackupOnce(dest, backupDir, m, &m.interior.added, log, dryRun);
        }
        if (opts.progress) opts.progress(i, total, kv.first, opts.progressUserData);
        if (!dryRun) WriteOrThrow(dest, CopyBytes(fsutil::PathFromUtf8(kv.second)));
    }
    steps.push_back("Installed " + std::to_string(total) +
                    " interior texture(s) → " +
                    fsutil::PathToUtf8(objects.filename()));

    // 3. the livery scan.
    const auto overrides = InteriorLiveryOverrides(aircraftDir);
    int cleared = 0;
    for (const fs::path& f : overrides) {
        std::error_code re;
        const fs::path rel = fs::relative(f, aircraftDir, re);
        if (re) continue;
        const std::string relPosix = rel.generic_string();
        if (Contains(m.interior.liveriesPatched, relPosix)) continue;
        const fs::path dest =
            backupDir / "liveries" / fsutil::PathFromUtf8(LiveryBackupName(relPosix));
        log.Write("livery override shadows our texture, removing: " +
                  fsutil::PathToUtf8(f));
        if (!dryRun) {
            WriteOrThrow(dest, CopyBytes(f));
            std::error_code de;
            fs::remove(f, de);
        }
        m.interior.liveriesPatched.push_back(relPosix);
        ++cleared;
    }
    if (cleared > 0) {
        steps.push_back("Cleared " + std::to_string(cleared) +
                        " livery texture override(s) that would have hidden the "
                        "interior mod");
    }
    // Liveries installed LATER aren't covered by this pass — surfaced as a hint
    // rather than pretending the coverage is permanent.
    steps.push_back("Note: re-run the installer after adding liveries to cover "
                    "them too");

    // 4. the .acf cockpit spots, all four files per airframe.
    const auto res = patch_acf::Run(fsutil::PathToUtf8(aircraftDir), false, dryRun);
    LogLines(log, "acf", res.log);
    m.interior.acfPatched = BaseNames(res.touched);
    if (!m.interior.acfPatched.empty()) {
        steps.push_back("Patched cockpit spot lights in " +
                        std::to_string(m.interior.acfPatched.size()) +
                        " .acf file(s): " + Join(m.interior.acfPatched, ", "));
    } else {
        steps.push_back("! No .acf cockpit spot block found — sim spots left as "
                        "they were");
    }

    m.interior.installed = true;
    m.interior.version = kPhotonVersion;
    m.interior.airframe = opts.airframeKey;
    return steps;
}

// Remove the shared plugin folder, keeping anything under PluginUserDirs that WE
// did not put there.
//
// ⚠ NOT `remove_all(pluginRoot)`. `overlays/` sits inside the plugin folder and is
// both our starter-image drop and the user's own — hand-made PNGs with no backup and
// no stock counterpart, which a blanket recursive delete would take with it. So a
// user dir is pruned BY NAME against the same list install wrote, rather than kept
// wholesale: that is what stops an uninstall from stranding our own images in a
// folder the user never asked for, while still never touching an image they made.
//
// ⚠ `IsSymlinkNoFollow` IS CHECKED BEFORE "is it ours": a dev's link whose target
// has moved reads as "not a file" and would otherwise fall through to the delete —
// taking the link with it for the one reason it was pointing at something worth
// keeping.
std::vector<std::string> RemovePluginDir(const fs::path& pluginRoot, Log& log,
                                         bool dryRun) {
    std::vector<std::string> steps;
    std::error_code ec;
    if (!fs::is_directory(pluginRoot, ec) || ec) {
        log.Write("shared plugin already absent: " + fsutil::PathToUtf8(pluginRoot));
        steps.push_back("Removed shared ToLiss Photon plugin (no other airframe "
                        "uses Photon)");
        return steps;
    }

    std::vector<std::string> ours;
    try {
        for (const auto& kv : payload::PluginDataFiles()) ours.push_back(kv.first);
    } catch (const payload::PayloadError&) {
        // A bundle that never shipped data removes none of it.
    }

    std::vector<fs::path> keptDirs;
    std::vector<std::string> keptNames;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(pluginRoot, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end());

    for (const fs::path& d : entries) {
        std::error_code de;
        if (!fs::is_directory(d, de) || de) continue;
        const std::string name = fsutil::PathToUtf8(d.filename());
        if (!Contains(PluginUserDirs(), name)) continue;

        bool keepHere = false;
        for (const fsutil::FileEntry& f : fsutil::ListFilesRecursive(d)) {
            const std::string rel = name + "/" + f.rel;
            const bool isOurs =
                Contains(ours, rel) && !fsutil::IsSymlinkNoFollow(f.abs);
            if (isOurs) {
                if (!dryRun) {
                    std::error_code re;
                    fs::remove(f.abs, re);
                }
            } else {
                keptNames.push_back(rel);
                keepHere = true;
            }
        }
        if (keepHere) keptDirs.push_back(d);
    }

    log.Write("removing shared plugin " + fsutil::PathToUtf8(pluginRoot) +
              " (keeping: " + (keptNames.empty() ? "nothing" : Join(keptNames, ", ")) +
              ")");
    if (!dryRun) {
        for (const fs::path& entry : entries) {
            if (std::find(keptDirs.begin(), keptDirs.end(), entry) != keptDirs.end()) {
                continue;
            }
            const std::string name = fsutil::PathToUtf8(entry.filename());
            if (fsutil::IsSymlinkNoFollow(entry) && Contains(ours, name)) {
                continue;               // a dev's link into their repo
            }
            std::error_code re;
            if (fs::is_directory(entry, re) && !re &&
                !fsutil::IsSymlinkNoFollow(entry)) {
                fs::remove_all(entry, re);
            } else {
                fs::remove(entry, re);
            }
        }
        fsutil::RemoveDirIfEmpty(pluginRoot);   // only succeeds if nothing was kept
    }

    steps.push_back("Removed shared ToLiss Photon plugin (no other airframe uses "
                    "Photon)");
    for (const fs::path& d : keptDirs) {
        steps.push_back("Kept your own file(s) in " +
                        fsutil::PathToUtf8(d.filename()) + "/ under Resources/"
                        "plugins/" + kPluginFolder +
                        " — delete them by hand if you don't want them");
    }
    return steps;
}

}  // namespace

std::vector<std::string> Install(const Options& opts, Log& log) {
    if (!payload::Available()) {
        throw ActionError("bundled payload not found next to the installer (" +
                          payload::PayloadDir() + ") — this build is incomplete");
    }
    const Airframe* af = AirframeByKey(opts.airframeKey);
    if (af == nullptr) throw ActionError("unknown airframe: " + opts.airframeKey);

    const fs::path aircraftDir = fsutil::PathFromUtf8(opts.aircraftPath);
    const fs::path objects = aircraftDir / "objects";
    const fs::path backupDir = objects / kBackupDirName;
    const std::string objectsUtf8 = fsutil::PathToUtf8(objects);
    const bool dryRun = opts.dryRun;
    std::vector<std::string> steps;

    manifest::Manifest m = manifest::Read(objectsUtf8);
    m.present = true;

    // ⚠ THE PLUGIN GOES FIRST. Its `.xpl` is the one artifact X-Plane locks while
    // loaded — Windows keeps a loaded module memory-mapped and refuses to overwrite
    // it — so on a running sim this is the step that fails. Doing it before we touch
    // the OBJ means a locked-plugin failure aborts BEFORE the version marker is
    // injected; otherwise a marked-but-orphaned OBJ (marker written, plugin never
    // copied, manifest never reached) reads back as a completed install on the next
    // run.
    const fs::path pluginRoot = fsutil::PathFromUtf8(opts.xplaneRoot) / "Resources" /
                                "plugins" / kPluginFolder;
    const auto arches = payload::PluginArches();
    bool wrotePlugin = false;
    for (const auto& kv : payload::PluginFiles()) {
        const fs::path dest = pluginRoot / fsutil::PathFromUtf8(kv.first);
        // ⚠ Only (over)write a `.xpl` that is not already byte-identical. Replacing
        // an unchanged, loaded, memory-mapped one fails with "file in use" for zero
        // benefit — and skipping it is precisely what lets an OBJ-only reinstall run
        // while X-Plane is open, which the "please close X-Plane" screen relies on.
        if (fsutil::FilesIdentical(dest, fsutil::PathFromUtf8(kv.second))) continue;
        wrotePlugin = true;
        if (!dryRun) WriteOrThrow(dest, CopyBytes(fsutil::PathFromUtf8(kv.second)));
    }
    if (wrotePlugin) {
        log.Write("wrote native plugin → " + fsutil::PathToUtf8(pluginRoot) + " (" +
                  Join(arches, ", ") + ")");
        steps.push_back("Installed ToLiss Photon plugin [" + Join(arches, ", ") +
                        "] → Resources/plugins/" + kPluginFolder);
    } else {
        log.Write("native plugin already current at " +
                  fsutil::PathToUtf8(pluginRoot) +
                  " — not rewriting (avoids locking a loaded .xpl)");
        steps.push_back("ToLiss Photon plugin already current [" +
                        Join(arches, ", ") + "] — kept");
    }

    // The plugin's runtime data goes in right after the binary. Plain data files, so
    // unlike the `.xpl` they are never locked by a running sim and this step cannot
    // be the one that fails.
    for (std::string& s : InstallPluginData(pluginRoot, log, dryRun)) {
        steps.push_back(std::move(s));
    }

    const fs::path target = objects / af->objName;
    switch (BackupOnce(target, backupDir, m, &m.added, log, dryRun)) {
        case Backup::kRecorded:
            steps.push_back(std::string("Backed up original ") + af->objName +
                            " → " + fsutil::PathToUtf8(objects.filename()) + "/" +
                            kBackupDirName);
            break;
        case Backup::kAlreadyHave:
            steps.push_back(std::string("Original ") + af->objName +
                            " already backed up — reused");
            break;
        case Backup::kNoOriginal:
            steps.push_back(std::string("! No original ") + af->objName +
                            " to back up — ours will be removed on uninstall");
            break;
    }

    const std::string objText =
        marker::Inject(payload::ObjText(opts.wing, opts.airframeKey),
                       kPhotonVersion, opts.wing);
    log.Write("writing " + fsutil::PathToUtf8(target) + " (" +
              std::to_string(objText.size()) + " bytes)");
    if (!dryRun) WriteOrThrow(target, objText);
    steps.push_back(std::string("Installed ") + af->objName + " (" + opts.wing +
                    " wing variant) → " + fsutil::PathToUtf8(objects.filename()));

    // Redirect ToLiss's skin-glow LIT-texture regions to Photon's own dataref so the
    // painted skin flashes in lockstep with the LED billboards. Each affected mesh
    // OBJ is backed up once before patching; uninstall restores it via the existing
    // backed_up list, so there is no extra bookkeeping and no `.bak` sibling.
    if (GlowIndicesFor(opts.airframeKey) != nullptr) {
        for (const std::string& f :
             patch_glow::TargetFiles(objectsUtf8, opts.airframeKey)) {
            const fs::path p = fsutil::PathFromUtf8(f);
            // ⚠ nullptr: the glow pass PATCHES these meshes in place rather than
            // writing one of ours over them, so a missing source means nothing
            // was written — recording it would have uninstall delete a ToLiss
            // file Photon never created.
            if (BackupOnce(p, backupDir, m, nullptr, log, dryRun) ==
                Backup::kRecorded) {
                steps.push_back("Backed up original " +
                                fsutil::PathToUtf8(p.filename()) + " → " +
                                fsutil::PathToUtf8(objects.filename()) + "/" +
                                kBackupDirName);
            }
        }
        const auto res =
            patch_glow::Run(objectsUtf8, opts.airframeKey, false, dryRun);
        LogLines(log, "glow", res.log);
        if (res.regions > 0) {
            steps.push_back("Redirected " + std::to_string(res.regions) +
                            " skin-glow region(s) to Photon dataref");
        }
    }

    // Capture any prior RealWings patch state BEFORE clearing it, so a switch away
    // from RealWings can undo the in-place mod patch.
    const bool wasRealwings = m.realwingsPatched;
    const std::string oldRwDir = m.realwingsDir;
    m.realwingsPatched = false;
    m.realwingsDir.clear();

    if (opts.wing == "realwings") {
        const fs::path rwDir =
            objects / fsutil::PathFromUtf8(RealWingsDir(opts.airframeKey));
        std::error_code ec;
        if (!payload::RealWingsAvailable()) {
            // A bundle built before the DSL decoupling has no realwings_patch.json.
            // Reported rather than raised: the base install is complete and correct,
            // and the mod's baked lights simply keep their fixed colors.
            log.Write(std::string("no ") + patch_realwings::kPatchJson +
                      " in this build — skipping the RealWings wingtip patch",
                      "WARN");
            steps.push_back("! This build carries no RealWings patch data — the "
                            "mod's own wingtip lights are left as they are");
        } else if (fs::is_directory(rwDir, ec) && !ec) {
            try {
                const auto res = patch_realwings::Run(
                    fsutil::PathToUtf8(rwDir), payload::RealWingsPatchData(),
                    opts.airframeKey, dryRun);
                LogLines(log, "realwings", res.log);
                m.realwingsDir = fsutil::PathToUtf8(rwDir);
                m.realwingsPatched = true;
                steps.push_back("Patched " + std::to_string(res.lights) +
                                " RealWings wingtip light(s) in " +
                                fsutil::PathToUtf8(rwDir.filename()));
            } catch (const patch_realwings::RealWingsError& e) {
                log.Write(std::string("realwings patch: ") + e.what(), "WARN");
            }
        } else {
            steps.push_back("RealWings mod not found at " +
                            fsutil::PathToUtf8(rwDir.filename()) +
                            " — continuing with base install only");
            log.Write("realwings dir missing: " + fsutil::PathToUtf8(rwDir), "WARN");
        }
    } else if (wasRealwings && !oldRwDir.empty()) {
        // ⚠ Switching from a RealWings install to stock/durantula: the new
        // `lights_out` OBJ carries its OWN wingtip nav/strobe (those fixtures are
        // omitted only under `--wing realwings`). If the mod's baked lights stay
        // patched they DOUBLE UP — and the patch also leaks, untracked, past a later
        // uninstall.
        const fs::path rwDir = fsutil::PathFromUtf8(oldRwDir);
        std::error_code ec;
        if (fs::is_directory(rwDir, ec) && !ec) {
            try {
                const auto res =
                    patch_realwings::RunReverse(oldRwDir, dryRun);
                LogLines(log, "realwings", res.log);
                steps.push_back("Reversed previous RealWings patch (" +
                                std::to_string(res.files) + " file(s) restored)");
            } catch (const patch_realwings::RealWingsError& e) {
                log.Write(std::string("realwings reverse: ") + e.what(), "WARN");
            }
        } else {
            log.Write("previous realwings dir gone, nothing to reverse: " + oldRwDir,
                      "WARN");
        }
    }

    // Display glow — part of the BASE install on the A3xx, not an opt-in. It works
    // on a stock cockpit, so it does not belong behind the interior's opt-in;
    // whether the glow is VISIBLE is a runtime choice on the Displays tab.
    if (AirframeHasScreens(opts.airframeKey)) {
        if (payload::ScreensAvailable()) {
            for (std::string& s : InstallScreens(objects, m, log, dryRun)) {
                steps.push_back(std::move(s));
            }
        } else {
            log.Write("no screen-glow OBJ in this build — skipping display glow",
                      "WARN");
        }
    }

    // Interior — opt-in and independent of everything above.
    if (opts.interior) {
        if (!AirframeHasInterior(opts.airframeKey)) {
            log.Write("interior mod not available for " + opts.airframeKey, "WARN");
            steps.push_back("Interior lighting not available for this airframe — "
                            "skipped");
        } else {
            for (std::string& s :
                 InstallInterior(opts, objects, backupDir, m, log)) {
                steps.push_back(std::move(s));
            }
        }
    }

    m.version = kPhotonVersion;
    m.wing = opts.wing;
    m.installedAt = NowIso8601Seconds();
    if (!dryRun) {
        std::string err;
        if (!manifest::Write(objectsUtf8, m, err)) throw ActionError(err);
    }
    steps.push_back("Wrote install manifest");
    return steps;
}

std::vector<std::string> Uninstall(const Options& opts, Log& log) {
    const fs::path aircraftDir = fsutil::PathFromUtf8(opts.aircraftPath);
    const fs::path objects = aircraftDir / "objects";
    const fs::path backupDir = objects / kBackupDirName;
    const std::string objectsUtf8 = fsutil::PathToUtf8(objects);
    const bool dryRun = opts.dryRun;

    const manifest::Manifest m = manifest::Read(objectsUtf8);
    if (!m.present) {
        throw ActionError("no Photon installation found to uninstall (no manifest)");
    }
    std::vector<std::string> steps;

    // ⚠ `backed_up[]` RESTORES. The added[] lists below DELETE.
    for (const std::string& rel : m.backedUp) {
        const fs::path src = backupDir / fsutil::PathFromUtf8(rel);
        const fs::path dest = objects / fsutil::PathFromUtf8(rel);
        std::error_code ec;
        if (!fs::is_regular_file(src, ec) || ec) {
            log.Write("backup missing, cannot restore: " + fsutil::PathToUtf8(src),
                      "WARN");
            steps.push_back("! Backup missing for " + rel + " — left as-is");
            continue;
        }
        log.Write("restoring " + fsutil::PathToUtf8(dest) + " <- " +
                  fsutil::PathToUtf8(src));
        if (!dryRun) WriteOrThrow(dest, CopyBytes(src));
        steps.push_back("Restored original " + rel);
    }

    // ⚠ THE MIRROR OF THE LOOP ABOVE, and normally empty: a file we wrote that had
    // no stock original is ours alone, so it is DELETED rather than restored.
    // UNGATED, unlike the interior and screens lists — there is no per-axis flag to
    // hang it on, and an entry here means an exterior install already happened.
    for (const std::string& rel : m.added) {
        const fs::path f = objects / fsutil::PathFromUtf8(rel);
        std::error_code ec;
        if (fs::is_regular_file(f, ec) && !ec) {
            log.Write("removing added file (no stock original): " +
                      fsutil::PathToUtf8(f));
            if (!dryRun) fs::remove(f, ec);
            steps.push_back("Removed added file " + rel);
        }
    }

    if (m.realwingsPatched && !m.realwingsDir.empty()) {
        std::error_code ec;
        if (fs::is_directory(fsutil::PathFromUtf8(m.realwingsDir), ec) && !ec) {
            try {
                // ⚠ The reverse takes NO patch data: it only restores `.obj.bak`
                // siblings, so it keeps working against a bundle whose JSON is
                // missing or a schema this build cannot read. A user must always be
                // able to undo a patch they successfully applied.
                const auto res = patch_realwings::RunReverse(m.realwingsDir, dryRun);
                LogLines(log, "realwings", res.log);
                steps.push_back("Reversed RealWings patch (" +
                                std::to_string(res.files) + " file(s) restored)");
            } catch (const patch_realwings::RealWingsError& e) {
                log.Write(std::string("realwings reverse: ") + e.what(), "WARN");
            }
        } else {
            log.Write("realwings dir gone, nothing to reverse: " + m.realwingsDir,
                      "WARN");
            steps.push_back("RealWings mod folder no longer present — nothing to "
                            "reverse");
        }
    }

    // ── display glow ─────────────────────────────────────────────────────────
    // Nothing here is in `backed_up`, because nothing here replaced a stock file.
    //
    // ⚠ DETACH FIRST, THEN DELETE. An `.acf` still naming an OBJ that is gone is a
    // missing-object warning on every load, whereas an attached-but-unused row left
    // behind by the reverse order is invisible until the user wonders why their
    // `.acf` differs from stock.
    if (m.screens.installed) {
        if (!m.screens.acfPatched.empty()) {
            const auto res = patch_acf_screens::Run(fsutil::PathToUtf8(aircraftDir),
                                                    true, dryRun);
            LogLines(log, "acf screens", res.log);
            steps.push_back("Detached the display-glow OBJ from " +
                            std::to_string(res.touched.size()) + " .acf file(s)");
        }
        for (const std::string& name : m.screens.added) {
            const fs::path f = objects / fsutil::PathFromUtf8(name);
            std::error_code ec;
            if (fs::is_regular_file(f, ec) && !ec) {
                log.Write("removing added display-glow file " +
                          fsutil::PathToUtf8(f));
                if (!dryRun) fs::remove(f, ec);
                steps.push_back("Removed added file " + name);
            }
        }
    }

    // ── interior ─────────────────────────────────────────────────────────────
    // The OBJ and the nine stock-replacing textures are already restored by the
    // backed_up loop above. What remains is what a backup cannot express.
    if (m.interior.installed) {
        // 1. Files we ADDED, which never had a stock original to restore. These must
        //    be deleted or they linger in the aircraft folder forever.
        for (const std::string& name : m.interior.added) {
            const fs::path f = objects / fsutil::PathFromUtf8(name);
            std::error_code ec;
            if (fs::is_regular_file(f, ec) && !ec) {
                log.Write("removing added interior file " + fsutil::PathToUtf8(f));
                if (!dryRun) fs::remove(f, ec);
                steps.push_back("Removed added file " + name);
            }
        }

        // 2. Livery overrides we backed up and removed so our PNG could resolve.
        for (const std::string& rel : m.interior.liveriesPatched) {
            const fs::path src =
                backupDir / "liveries" / fsutil::PathFromUtf8(LiveryBackupName(rel));
            const fs::path dest = aircraftDir / fsutil::PathFromUtf8(rel);
            std::error_code ec;
            if (!fs::is_regular_file(src, ec) || ec) {
                log.Write("livery backup missing, cannot restore: " +
                          fsutil::PathToUtf8(src), "WARN");
                steps.push_back("! Livery backup missing for " + rel + " — left as-is");
                continue;
            }
            log.Write("restoring livery override " + fsutil::PathToUtf8(dest));
            if (!dryRun) {
                WriteOrThrow(dest, CopyBytes(src));
                std::error_code re;
                fs::remove(src, re);
            }
            steps.push_back("Restored livery override " + rel);
        }

        // 3. ⚠ The `.acf` spot patch, reversed by WRITING STOCK VALUES BACK rather
        //    than restoring a snapshot — so other mods' edits to the same file
        //    (Durantula rewrites 312 lines of it) survive.
        if (!m.interior.acfPatched.empty()) {
            const auto res =
                patch_acf::Run(fsutil::PathToUtf8(aircraftDir), true, dryRun);
            LogLines(log, "acf", res.log);
            steps.push_back("Restored stock cockpit spot lights in " +
                            std::to_string(res.touched.size()) + " .acf file(s)");
        }

        if (!dryRun) fsutil::RemoveDirIfEmpty(backupDir / "liveries");
    }

    log.Write("removing " + fsutil::PathToUtf8(backupDir));
    if (!dryRun) {
        for (const std::string& rel : m.backedUp) {
            std::error_code ec;
            fs::remove(backupDir / fsutil::PathFromUtf8(rel), ec);
        }
        std::error_code ec;
        fs::remove(backupDir / kManifestName, ec);
        fsutil::RemoveDirIfEmpty(backupDir);   // only succeeds if now empty
    }
    steps.push_back(std::string("Removed ") + kBackupDirName + " and manifest");

    if (opts.removePlugin) {
        const fs::path pluginRoot = fsutil::PathFromUtf8(opts.xplaneRoot) /
                                    "Resources" / "plugins" / kPluginFolder;
        for (std::string& s : RemovePluginDir(pluginRoot, log, dryRun)) {
            steps.push_back(std::move(s));
        }
    }
    return steps;
}

}  // namespace actions
}  // namespace photon
