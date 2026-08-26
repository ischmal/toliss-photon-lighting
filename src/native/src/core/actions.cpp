#include "core/actions.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>

#include "core/backup.h"
#include "core/constants.h"
#include "core/fsutil.h"
#include "core/image_io.h"
#include "core/lit_recolor.h"
#include "core/marker.h"
#include "core/patch_acf.h"
#include "core/patch_acf_integral.h"
#include "core/patch_acf_screens.h"
#include "core/patch_glow.h"
#include "core/patch_integral.h"
#include "core/patch_intensity.h"
#include "core/patch_realwings.h"
#include "core/payload.h"
#include "core/progress.h"
#include "core/support.h"
#include "core/version.h"
#include "core/wingmod.h"

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
        log.Write("backup skipped (a backup already exists): " + rel);
        return Backup::kAlreadyHave;
    }
    if (added != nullptr && Contains(*added, rel)) {
        log.Write("backup skipped (written by an earlier Photon install, no "
                  "stock original): " + rel);
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

// How the backup folder is NAMED in the step list: its path relative to the
// aircraft folder, so a current install reads `Photon Backup Files` and one still
// waiting to be migrated reads `objects/Photon Backup Files`. ⚠ Derived, never a
// literal — a step line that names a folder the files are not in is worse than one
// that names none, because the user goes looking.
std::string BackupLabel(const fs::path& aircraftDir, const fs::path& backupDir) {
    std::error_code ec;
    const fs::path rel = fs::relative(backupDir, aircraftDir, ec);
    if (ec || rel.empty()) return fsutil::PathToUtf8(backupDir.filename());
    return fsutil::PathToUtf8Generic(rel);
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

// ─── the progress plan ───────────────────────────────────────────────────────
// The step names. ⚠ ONE SPELLING EACH, because `Plan::Begin` matches BY NAME and a
// typo is not an error — it is a step that silently never lights up, on a bar that
// still looks plausible. `ProgressStepNames()` below is what the test walks.
constexpr char kStepPlugin[] = "Plugin";
constexpr char kStepPluginData[] = "Plugin data";
constexpr char kStepBackupDir[] = "Backup folder";
constexpr char kStepWingCheck[] = "Wing mod check";
constexpr char kStepExteriorObj[] = "Exterior lights";
constexpr char kStepGlow[] = "Skin glow";
constexpr char kStepRealWings[] = "RealWings wingtips";
constexpr char kStepScreens[] = "Display glow";
constexpr char kStepInteriorObj[] = "Cockpit lights";
constexpr char kStepInteriorTex[] = "Cockpit textures";
constexpr char kStepLiveries[] = "Livery overrides";
constexpr char kStepInteriorAcf[] = "Cockpit spot lights";
constexpr char kStepIntegralTex[] = "Integral light textures";
constexpr char kStepIntegralObj[] = "Integral light objects";
constexpr char kStepManifest[] = "Manifest";
constexpr char kStepRestore[] = "Restoring originals";
constexpr char kStepRemove[] = "Removing added files";

// A few phases are real work with nothing to size — a directory walk, a JSON write.
// They are in the plan anyway so the step exists and the name is reportable; the
// magnitude is measured (1.5-9.4 ms for the livery scan) and deliberately small.
constexpr double kMsLiveryScan = 8.0;
constexpr double kMsManifest = 2.0;

// Moving a legacy backup folder out of `objects/`. ⚠ PRICED SMALL ON PURPOSE,
// even though the folder can hold 1.6 GB of A339 mesh backups: both paths are
// inside the aircraft folder, so it is a same-volume rename — a metadata operation
// whose cost does not depend on the bytes. Only the copy fallback (something holds
// a file open) is slow, and that is the anomaly rather than the shape to plan for.
constexpr double kMsBackupMove = 3.0;

// Reading the two wing OBJs the attachment table names. ⚠ FLAT ON PURPOSE, unlike
// every other size-derived estimate here: the plan is built before anything is
// read, and WHICH OBJs those are is only known after parsing the `.acf` — so a
// size-derived figure would need the very read it is trying to price. It is a
// rounding error either way: 2 x ~2 MB at the scan rate is ~14 ms against the
// ~65 ms the four `.acf` themselves cost in the same step.
constexpr double kMsWingObjs = 15.0;

// One integral-lighting recolor pass — decode the stock PNG, run the palette
// over it, re-encode. ⚠ PRICED PER MEGAPIXEL, NOT PER BYTE, which is the odd one
// out in this file and deliberate: everything else here is I/O and scales with
// the file, while this is dominated by the encoder and scales with the ATLAS.
// The two are not the same shape at all — `knobs_LIT.png` is 0.16 MB of file and
// 4.2 megapixels, so a byte-derived estimate would price it at a fiftieth of its
// real cost and the bar would hang there.
//
// Measured 2026-08-24, this machine: `photon-lit-studio recolor` over the A320's
// 4096² placard atlas, 1.31 s end to end, i.e. 16.78 Mpx in 1310 ms.
constexpr double kMsPerMegapixelRecolor = 78.0;

// What one texture's two eras cost, from the atlas size the spec is authored
// against — known without opening the file, which is what planning needs.
double RecolorMs(lit::Texture texture) {
    const double side = static_cast<double>(lit::TextureReferenceSize(texture));
    return 2.0 * kMsPerMegapixelRecolor * (side * side) / 1.0e6;
}

// A wing token in a SENTENCE. ⚠ Not `WingLabel`, which renders `stock` as
// "Default" — right on a form where it is one option among three, wrong in
// "this aircraft is running Default wings". Not `--wing`'s token either, which is
// the mistake `WingLabel` was added to fix on the Complete screen.
std::string WingProse(const std::string& wing) {
    if (wing == "durantula") return "Durantula";
    if (wing == "realwings") return "RealWings";
    if (wing == "stock") return "stock";
    return wing;
}

// The bar's driver: the plan, plus the callback that publishes it.
//
// ⚠ EVERY PHASE MUST Begin AND Finish, INCLUDING ONES IT SKIPS. `Finish` banks the
// step's whole estimate, so a phase that returns early without it leaves its cost
// unbanked forever and the bar can never pass (total - that step) / total. The
// `Skip` helper is the one-liner for the branch that does nothing.
struct Reporter {
    progress::Plan plan;
    const Options* opts = nullptr;

    void Publish(const std::string& step) const {
        if (opts != nullptr && opts->progress != nullptr) {
            opts->progress(plan.Fraction(), step, opts->progressUserData);
        }
    }
    void Begin(const char* step) {
        plan.Begin(step);
        Publish(step);
    }
    void Finish(const char* step) {
        plan.Finish();
        Publish(step);
    }
    void Skip(const char* step) {
        plan.Begin(step);
        plan.Finish();
        Publish(step);
    }
    // Hand to a patcher so an eleven-second scan animates instead of sitting still.
    progress::StepProgress Sub(const char* step) {
        return [this, step](double f) {
            plan.Within(f);
            Publish(step);
        };
    }
};

// Total `.acf` bytes in the aircraft folder. ⚠ NOT `patch_acf*::AcfFiles()`, which
// READS every candidate to find the block it needs — the whole point of planning
// from sizes is that it costs nothing, and calling the finder here would pay the
// 9 MB read twice per run to predict the cost of paying it once.
std::uint64_t AcfBytes(const fs::path& aircraftDir) {
    return progress::BytesOfFilesWithExt(fsutil::PathToUtf8(aircraftDir), ".acf");
}

// ⚠ A DRY RUN DOES NOT COPY, SO IT MUST NOT BE PRICED AS IF IT DID. Every write
// here is `if (!dryRun) WriteOrThrow(dest, CopyBytes(src))` — one guard covering the
// READ as well as the write — so a dry run skips both halves of a copy and pays only
// for the scans and parses, which run either way. Left unmodelled, the 59 MB cockpit
// texture copy is more than half an interior plan that never happens, and the bar
// crawls to ~40% and jumps. Measured: `interior:textures` does not reach 1 ms on a
// dry run, against ~390 ms predicted for a real one.
double CopyMs(std::uint64_t bytes, bool dryRun) {
    return dryRun ? 0.0 : progress::MsForCopy(bytes);
}

progress::Plan BuildInstallPlan(const Options& opts, const fs::path& aircraftDir,
                                const fs::path& objects) {
    progress::Plan plan;
    const std::string objectsUtf8 = fsutil::PathToUtf8(objects);
    const bool dry = opts.dryRun;

    // The plugin: both sides are read to compare, then one may be written.
    std::uint64_t pluginBytes = 0;
    try {
        for (const auto& kv : payload::PluginFiles()) {
            pluginBytes += progress::FileSizeOrZero(kv.second);
        }
    } catch (const payload::PayloadError&) {
    }
    // ⚠ The compare reads both sides even in a dry run; only the write is skipped.
    plan.Add(kStepPlugin,
             progress::MsForCopy(pluginBytes) + CopyMs(pluginBytes, dry));

    std::uint64_t dataBytes = 0;
    try {
        for (const auto& kv : payload::PluginDataFiles()) {
            dataBytes += progress::FileSizeOrZero(kv.second);
        }
    } catch (const payload::PayloadError&) {
    }
    plan.Add(kStepPluginData, CopyMs(2 * dataBytes, dry));

    // ⚠ THE SAME CONDITION AS THE RUN'S, evaluated at the same moment: the plan is
    // built before any work, and nothing between here and the move can create or
    // remove a legacy folder. A dry run still enters the step — it reports what it
    // WOULD move — so this is not gated on `dry` either.
    if (backup::LegacyDirInUse(aircraftDir)) {
        plan.Add(kStepBackupDir, kMsBackupMove);
    }

    // ⚠ PRICED WITH THE SCAN RATE, NOT THE `.acf` PARSE RATE, and the difference is
    // an order of magnitude. `MsForAcfAttach` (37.6 ms/MiB) measures a full
    // `acf::ReadProps` map build plus a `SetProps` rewrite; this step reads each
    // file once and picks the `_obja` lines out of it, which is what
    // `MsForObjScan` (3.5 ms/MiB) measures. Pricing a read as a parse would put a
    // ~65 ms step in the plan as a ~350 ms one and give the bar a long dead spot
    // exactly where nothing takes any time.
    //
    // ⚠ THE SAME CONDITION AS THE RUN'S: `Install` enters this whenever the
    // airframe HAS wing mods, so the plan prices it on the same test. See
    // core/wingmod.h — the A330-900 reads nothing and answers instantly.
    if (WingsFor(opts.airframeKey).size() > 1) {
        plan.Add(kStepWingCheck,
                 progress::MsForObjScan(AcfBytes(aircraftDir)) + kMsWingObjs);
    }

    // The exterior OBJ: back up what is there, write ours over it.
    const Airframe* af = AirframeByKey(opts.airframeKey);
    std::uint64_t extBytes = 0;
    if (af != nullptr) {
        extBytes += progress::FileSizeOrZero(
            fsutil::PathToUtf8(objects / af->objName));
        try {
            extBytes += progress::FileSizeOrZero(
                payload::ObjPath(opts.wing, opts.airframeKey));
        } catch (const payload::PayloadError&) {
        }
    }
    plan.Add(kStepExteriorObj, CopyMs(2 * extBytes, dry));

    // ⚠ THE BIGGEST STEP IN THE INSTALLER, AND ONLY ONE AIRFRAME HAS IT. The glow
    // redirect reads every `.obj` in objects/ to find its targets: 1,597 MB on the
    // A330-900, the only airframe in `GlowRedirect()`. It is ~97% of that install
    // and 0% of every other, which is why the plan is per-run and not a table.
    if (GlowIndicesFor(opts.airframeKey) != nullptr) {
        plan.Add(kStepGlow, progress::MsForObjScan(progress::BytesOfFilesWithExt(
                                objectsUtf8, ".obj")));
    }

    // ⚠ Same rule: `Install` enters this on the wing alone, so the plan prices it on
    // the wing alone. An airframe with no RealWings folder is a zero-byte estimate,
    // not an absent step.
    if (opts.wing == "realwings") {
        const std::string rwDir = RealWingsDir(opts.airframeKey);
        const std::uint64_t b =
            rwDir.empty() ? 0
                          : progress::BytesOfFilesWithExt(
                                fsutil::PathToUtf8(objects /
                                                   fsutil::PathFromUtf8(rwDir)),
                                ".obj", /*recursive=*/true);
        plan.Add(kStepRealWings, progress::MsForObjPatch(b));
    }

    if (AirframeHasScreens(opts.airframeKey) &&
        payload::ScreensAvailable(opts.airframeKey)) {
        std::uint64_t b = 0;
        try {
            b = progress::FileSizeOrZero(
                payload::ScreensObjPath(opts.airframeKey));
        } catch (const payload::PayloadError&) {
        }
        // The `.acf` parse happens either way; only its write is skipped.
        plan.Add(kStepScreens,
                 CopyMs(2 * b, dry) + progress::MsForAcfAttach(AcfBytes(aircraftDir)));
    }

    if (opts.interior && AirframeHasInterior(opts.airframeKey)) {
        std::uint64_t objBytes = 0;
        std::uint64_t texBytes = 0;
        try {
            objBytes = progress::FileSizeOrZero(payload::InteriorObjPath());
            for (const auto& kv : payload::InteriorTextureFiles()) {
                // Our copy in, plus the stock one backed up on the way — a first
                // install pays both and a reinstall only the first, so this is the
                // ceiling. Over-estimating makes the bar lag; under-estimating makes
                // it stall at a step's ceiling, which reads far worse.
                texBytes += progress::FileSizeOrZero(kv.second);
                texBytes += progress::FileSizeOrZero(
                    fsutil::PathToUtf8(objects / fsutil::PathFromUtf8(kv.first)));
            }
        } catch (const payload::PayloadError&) {
        }
        plan.Add(kStepInteriorObj, CopyMs(2 * objBytes, dry));
        plan.Add(kStepInteriorTex, CopyMs(2 * texBytes, dry));
        plan.Add(kStepLiveries, kMsLiveryScan);
        plan.Add(kStepInteriorAcf, progress::MsForAcfSpots(AcfBytes(aircraftDir)));

        // ⚠ THESE TWO RUN UNCONDITIONALLY INSIDE `InstallInterior`, so they are
        // priced unconditionally here — same branch, both sides. A step the run
        // enters that the plan never priced is invisible in the numbers (it is
        // missing from the denominator too, so the run still ends on exactly
        // 1.000); `progress::SetUnknownStepHook` is what catches it, and it is
        // armed by install_tests.cpp.
        //
        // ⚠ The OBJ figure is the honest one to be surprised by: the twin of
        // `knobs.obj` is a 26 MB write, which dwarfs every other file this
        // install copies.
        double recolorMs = 0.0;
        std::uint64_t twinBytes = 0;
        for (const integral::Fixture& f : integral::FixturesIn(objects)) {
            recolorMs += RecolorMs(f.texture);
            twinBytes += progress::FileSizeOrZero(
                fsutil::PathToUtf8(objects / fsutil::PathFromUtf8(f.obj)));
        }
        plan.Add(kStepIntegralTex, recolorMs);
        // Read the stock OBJ, write it back gated, write the twin: three passes.
        plan.Add(kStepIntegralObj, CopyMs(3 * twinBytes, dry) +
                                       progress::MsForAcfAttach(AcfBytes(aircraftDir)));
    }

    plan.Add(kStepManifest, kMsManifest);
    return plan;
}

progress::Plan BuildUninstallPlan(const manifest::Manifest& m,
                                  const fs::path& aircraftDir,
                                  const fs::path& objects,
                                  const fs::path& backupDir, bool dryRun) {
    progress::Plan plan;

    // Restoring reads each backup and writes it back over the installed file.
    std::uint64_t restoreBytes = 0;
    for (const std::string& rel : m.backedUp) {
        restoreBytes += progress::FileSizeOrZero(
            fsutil::PathToUtf8(backupDir / fsutil::PathFromUtf8(rel)));
    }
    plan.Add(kStepRestore, CopyMs(2 * restoreBytes, dryRun));
    // Deletes are metadata operations, not copies — cheap, but a real step.
    plan.Add(kStepRemove, kMsManifest);

    if (m.realwingsPatched && !m.realwingsDir.empty()) {
        plan.Add(kStepRealWings, progress::MsForObjPatch(
                                     progress::BytesOfFilesWithExt(
                                         m.realwingsDir, ".obj",
                                         /*recursive=*/true)));
    }
    // ⚠ THE CONDITION MUST BE THE RUN'S, NOT A NARROWER ONE. This read
    // `m.screens.installed && !m.screens.acfPatched.empty()` while `Uninstall`
    // enters the step on `m.screens.installed` alone — so an install that wrote the
    // OBJ but attached no `.acf` row entered a step the plan had never priced, and
    // the bar had a dead spot exactly where the detach happens. Found by the
    // unpriced-step hook on its first run; invisible to every arithmetic check,
    // because an unpriced step is missing from the denominator too.
    if (m.screens.installed) {
        plan.Add(kStepScreens, progress::MsForAcfAttach(AcfBytes(aircraftDir)));
    }
    if (m.interior.installed) {
        std::uint64_t liveryBytes = 0;
        for (const std::string& rel : m.interior.liveriesPatched) {
            liveryBytes += progress::FileSizeOrZero(fsutil::PathToUtf8(
                backupDir / "liveries" / fsutil::PathFromUtf8(LiveryBackupName(rel))));
        }
        plan.Add(kStepLiveries, kMsLiveryScan + CopyMs(2 * liveryBytes, dryRun));
        plan.Add(kStepInteriorAcf, progress::MsForAcfSpots(AcfBytes(aircraftDir)));
        // ⚠ The detach runs whenever the interior did, so it is priced on the
        // same condition — the mismatch this file has already been bitten by
        // once (the screens detach, 2026-08-13) is a `rep.Begin` with no
        // `plan.Add` under the same `if`.
        plan.Add(kStepIntegralObj, progress::MsForAcfAttach(AcfBytes(aircraftDir)));
    }
    (void)objects;
    plan.Add(kStepManifest, kMsManifest);
    return plan;
}

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
                                        const std::string& airframeKey,
                                        manifest::Manifest& m, Log& log,
                                        bool dryRun, Reporter& rep) {
    std::vector<std::string> steps;
    const fs::path aircraftDir = objects.parent_path();

    if (!Contains(m.screens.added, kScreensObj)) {
        m.screens.added.emplace_back(kScreensObj);
    }
    // ⚠ The airframe key is what picks the OBJ. Every airframe writes the same
    // FILENAME into its own objects/ folder, so a wrong key here is invisible on
    // disk and shows up only as six lights in the wrong part of the cockpit.
    const std::string text =
        marker::Inject(payload::ScreensObjText(airframeKey), kPhotonVersion,
                       "screens");
    const fs::path target = objects / kScreensObj;
    log.Write("writing " + fsutil::PathToUtf8(target) + " (" +
              std::to_string(text.size()) + " bytes)");
    if (!dryRun) WriteOrThrow(target, text);
    steps.push_back(std::string("Installed ") + kScreensObj + " (display glow) → " +
                    fsutil::PathToUtf8(objects.filename()));

    const auto res = patch_acf_screens::Run(fsutil::PathToUtf8(aircraftDir), false,
                                            dryRun, rep.Sub(kStepScreens));
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

// ─── integral lighting ───────────────────────────────────────────────────────
// The backlit placards and knob markings, as a look the USER can switch in the
// sim rather than one the installer bakes. Part of the interior axis, because
// the two textures it owns are cockpit textures the interior install has always
// replaced — so it installs and uninstalls with the cockpit mod and needs no
// second opt-in.

// ⚠ A BACKUP CAN BE POISONED AND `BackupOnce` ALONE CAN NEVER HEAL IT. A
// manifest lost between two installs makes the next one back OUR OUTPUT up as
// the stock original (the failure backup.h calls unrecoverable) — and from then
// on every reinstall finds our output in `objects/`, falls back to that backup,
// reads it as recolored too, and refuses. Seen on a tester's A319 on
// 2026-08-25: no stock placard atlas left anywhere on the aircraft. The one
// moment the poisoning is PROVABLE is when `objects/` itself reads as stock —
// the backup exists only to answer what `objects/` cannot, so a backup that is
// recolored (or unreadable) beside a stock `objects/` is strictly worse than a
// copy of that stock, and `objects/` is also the FRESHER stock (it may carry a
// ToLiss revision the backup predates). Refreshing needs BOTH judgments —
// `objects/` provably stock AND the backup provably not — each with
// `LooksRecolored`'s stated margins; a missing backup is `BackupOnce`'s to
// create, never this function's.
static void RefreshPoisonedBackup(const fs::path& inObjects,
                                  const fs::path& inBackup, lit::Texture texture,
                                  Log& log, bool dryRun) {
    std::error_code ec;
    if (!fs::exists(inBackup, ec) || ec) return;
    lit::Image backup;
    std::string err;
    if (image::LoadPng(inBackup, backup, err) &&
        !lit::LooksRecolored(backup, texture)) {
        return;   // the backup is stock — the normal case, leave it alone
    }
    log.Write("the backed-up " + fsutil::PathToUtf8(inBackup.filename()) +
              " is not a stock file — refreshing it from the stock copy in "
              "objects/", "WARN");
    if (dryRun) return;
    fs::copy_file(inObjects, inBackup, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log.Write("could not refresh the backup: " + ec.message(), "WARN");
    }
}
//
// Four things happen per drawn OBJ:
//   1. its stock LIT texture is recolored ONCE PER ERA — incandescent over the
//      stock filename, LED into a `_photon_led` twin;
//   2. the OBJ itself is wrapped in a gate that draws it only in the
//      incandescent branch;
//   3. a twin OBJ is written, bound to the LED texture and gated the other way;
//   4. the twin is attached in the XP12 `.acf` pair.
//
// ⚠ WHERE "STOCK" COMES FROM IS THE WHOLE REINSTALL PROBLEM, AND IT IS ASKED OF
// THE PIXELS. On a first install the file in `objects/` is stock. On a reinstall
// it is OUR OUTPUT, and re-deriving from it would compound the curve — silently,
// because blue is already clipped to zero, so the second pass looks like a no-op
// while freezing the result in as though it were stock. So: use `objects/` when
// `lit::LooksRecolored` says it is stock, and the backup otherwise.
//
// ⚠ AND THAT ORDER IS NOT ARBITRARY — `objects/` IS PREFERRED. A backup records
// what was stock at INSTALL TIME and can be a ToLiss revision behind: the
// A319/A320 backup here still held the old misaligned trim artwork that a ToLiss
// update has since fixed. When `objects/` holds a stock file it is the FRESHER
// stock, so deriving from it picks up ToLiss's corrections for free. The backup
// is the fallback for the one case `objects/` cannot answer — when we overwrote
// it ourselves.
std::vector<std::string> InstallIntegral(const Options& opts,
                                         const fs::path& objects,
                                         const fs::path& backupDir,
                                         manifest::Manifest& m, Log& log,
                                         Reporter& rep) {
    std::vector<std::string> steps;
    const fs::path aircraftDir = objects.parent_path();
    const bool dryRun = opts.dryRun;

    // ⚠ BOTH TESTS, AND THE SECOND IS THE ONE THAT MATTERS. "Binds one of our
    // textures" finds `lamps_Std.obj` as well; "is in the attachment table"
    // rules it out, because X-Plane 12 never draws it and twinning it would ADD
    // geometry to an aeroplane that was not drawing it.
    const auto drawn = patch_acf_integral::DrawnFixtures(
        fsutil::PathToUtf8(aircraftDir), integral::FixturesIn(objects));

    rep.Begin(kStepIntegralTex);
    if (drawn.empty()) {
        log.Write("no attached OBJ binds an integral-lighting texture — "
                  "switchable integral lighting skipped", "WARN");
        steps.push_back("! No cockpit OBJ binds the placard or knob light "
                        "textures — integral lighting left stock");
        rep.Finish(kStepIntegralTex);
        rep.Skip(kStepIntegralObj);
        return steps;
    }

    // 1 + 2. The textures, both eras, per fixture.
    struct Ready {
        integral::Fixture fixture;
        bool ok = false;
    };
    std::vector<Ready> ready;
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        const integral::Fixture& f = drawn[i];
        rep.Sub(kStepIntegralTex)(static_cast<double>(i) / drawn.size());
        Ready r;
        r.fixture = f;

        // The stock LIT is about to be overwritten, so it is backed up like any
        // other replaced file — and `nullptr` is wrong here: we WRITE over it,
        // so a missing original must be recorded as ours to delete.
        BackupOnce(objects / fsutil::PathFromUtf8(f.stockLit), backupDir, m,
                   &m.interior.added, log, dryRun);

        lit::Image stock;
        std::string err;
        const fs::path inObjects = objects / fsutil::PathFromUtf8(f.stockLit);
        const fs::path inBackup = backupDir / fsutil::PathFromUtf8(f.stockLit);
        std::string source;
        if (image::LoadPng(inObjects, stock, err) &&
            !lit::LooksRecolored(stock, f.texture)) {
            source = fsutil::PathToUtf8(inObjects);
            RefreshPoisonedBackup(inObjects, inBackup, f.texture, log, dryRun);
        } else if (image::LoadPng(inBackup, stock, err)) {
            source = fsutil::PathToUtf8(inBackup);
            if (lit::LooksRecolored(stock, f.texture)) {
                // ⚠ REFUSE. Both copies are already ours, so there is no stock
                // anywhere and every era we could derive would be a second pass
                // over a first one. Leaving the aircraft as it is beats baking a
                // compounded curve in permanently.
                log.Write("both " + f.stockLit + " and its backup are already "
                          "recolored — no stock source left to derive from",
                          "WARN");
                steps.push_back("! " + f.stockLit +
                                " has no stock copy left (neither in objects/ "
                                "nor in " + BackupLabel(aircraftDir, backupDir) +
                                ") — left as it is");
                ready.push_back(r);
                continue;
            }
        } else {
            log.Write("cannot read a stock " + f.stockLit + ": " + err, "WARN");
            steps.push_back("! Could not read " + f.stockLit +
                            " — integral lighting skipped for it");
            ready.push_back(r);
            continue;
        }
        log.Write("integral: deriving " + f.stockLit + " from " + source);

        // ⚠ The DAY texture is only read by a PROMOTE region, which the placards
        // have (PEDAL DISC) and the knobs do not. A missing one must cost the
        // promotion and nothing else, so it is loaded optionally.
        lit::Image albedo;
        std::string aerr;
        const bool haveAlbedo = image::LoadPng(
            objects / fsutil::PathFromUtf8(lit::TextureDayName(f.texture)),
            albedo, aerr);

        bool wroteBoth = true;
        for (const integral::Era era :
             {integral::Era::kIncandescent, integral::Era::kLed}) {
            lit::Image work = stock;   // each era derives from STOCK, never from
                                       // the other era's output
            const std::size_t changed =
                lit::Apply(work, haveAlbedo ? &albedo : nullptr,
                           lit::SpecForProfile(integral::ProfileFor(era),
                                               f.texture));
            const std::string name =
                (era == integral::Era::kLed) ? f.ledLit : f.stockLit;
            const fs::path dest = objects / fsutil::PathFromUtf8(name);
            log.Write("integral: " + std::string(integral::EraKey(era)) + " " +
                      name + " — " + std::to_string(changed) + " px changed");
            if (!dryRun) {
                std::string werr;
                if (!image::SavePng(dest, work, werr)) {
                    log.Write("cannot write " + fsutil::PathToUtf8(dest) + ": " +
                              werr, "WARN");
                    wroteBoth = false;
                    break;
                }
            }
            // The LED file has no stock counterpart, so uninstall DELETES it.
            if (era == integral::Era::kLed && !Contains(m.interior.added, name)) {
                m.interior.added.push_back(name);
            }
        }
        r.ok = wroteBoth;
        ready.push_back(r);
    }
    std::size_t okCount = 0;
    for (const Ready& r : ready) {
        if (r.ok) ++okCount;
    }
    steps.push_back("Derived Incandescent and LED integral lighting for " +
                    std::to_string(okCount) + " of " +
                    std::to_string(drawn.size()) + " cockpit texture(s)");
    rep.Finish(kStepIntegralTex);

    // ⚠ THE SWITCH IS ALL-OR-NOTHING ACROSS FIXTURES. A cockpit with a
    // switchable knob atlas and unswitchable placards is not half a feature, it
    // is a broken-looking one: the plugin offers the Integral row for ANY twin
    // OBJ on disk (DetectIntegral is a name test, deliberately), so flipping it
    // would move the knob markings while the placards inches away held still.
    // Shipped exactly that way to a tester's A319 on 2026-08-25 — a poisoned
    // backup refused the placard derivation and the knobs went ahead alone.
    // Withholding step 3 keeps the cockpit consistent: every fixture that COULD
    // derive still wears its fresh incandescent recolor, there is just no LED
    // row offered until a stock copy of the failed file is restored (ToLiss
    // updater or SkunkCrafts re-sync) and Photon is reinstalled.
    if (okCount != ready.size()) {
        log.Write("switchable integral lighting not installed — " +
                  std::to_string(ready.size() - okCount) + " of " +
                  std::to_string(ready.size()) + " cockpit texture(s) have no "
                  "stock source, and a partial switch would move some lights "
                  "and not others", "WARN");
        steps.push_back("! Switchable integral lighting NOT installed — restore "
                        "a stock copy of the file(s) named above (ToLiss "
                        "updater / re-download) and reinstall to enable it");
        rep.Skip(kStepIntegralObj);
        return steps;
    }

    // 3. The OBJs — the stock one gated in place, its twin written beside it.
    rep.Begin(kStepIntegralObj);
    std::vector<integral::Fixture> attach;
    for (std::size_t i = 0; i < ready.size(); ++i) {
        if (!ready[i].ok) continue;
        const integral::Fixture& f = ready[i].fixture;
        rep.Sub(kStepIntegralObj)(static_cast<double>(i) / (ready.size() + 1));

        const fs::path objPath = objects / fsutil::PathFromUtf8(f.obj);
        BackupOnce(objPath, backupDir, m, nullptr, log, dryRun);

        // ⚠ SOURCE THE STOCK TEXT THE SAME WAY THE TEXTURE IS SOURCED, for the
        // same reason: on a reinstall the file in `objects/` already carries the
        // gate, and `PatchText` refuses a wrapped file rather than nesting one.
        std::string stockText;
        const fs::path backupObj = backupDir / fsutil::PathFromUtf8(f.obj);
        if (fsutil::ReadFileBytes(objPath, stockText) &&
            !integral::IsPatched(stockText)) {
            // stock, as found. Same healing rule as the texture above: a
            // backed-up OBJ that already carries the gate beside a provably
            // un-gated objects/ copy is a poisoned backup, and un-gated is as
            // provable as `LooksRecolored`'s stock answer.
            std::error_code bec;
            std::string backupText;
            if (fs::exists(backupObj, bec) && !bec &&
                (!fsutil::ReadFileBytes(backupObj, backupText) ||
                 integral::IsPatched(backupText))) {
                log.Write("the backed-up " + f.obj + " is not a stock file — "
                          "refreshing it from the stock copy in objects/",
                          "WARN");
                if (!dryRun) {
                    fs::copy_file(objPath, backupObj,
                                  fs::copy_options::overwrite_existing, bec);
                    if (bec) {
                        log.Write("could not refresh the backup: " +
                                  bec.message(), "WARN");
                    }
                }
            }
        } else if (!fsutil::ReadFileBytes(backupObj, stockText) ||
                   integral::IsPatched(stockText)) {
            log.Write("no un-gated copy of " + f.obj + " to derive from", "WARN");
            steps.push_back("! No stock " + f.obj +
                            " to gate — integral lighting skipped for it");
            continue;
        }

        std::string incText, ledText, err;
        if (!integral::PatchText(stockText, integral::Era::kIncandescent, "",
                                 kPhotonVersion, incText, err) ||
            !integral::PatchText(stockText, integral::Era::kLed, f.ledLit,
                                 kPhotonVersion, ledText, err)) {
            log.Write("cannot gate " + f.obj + ": " + err, "WARN");
            steps.push_back("! Could not gate " + f.obj + " (" + err + ")");
            continue;
        }
        if (!dryRun) {
            WriteOrThrow(objPath, incText);
            WriteOrThrow(objects / fsutil::PathFromUtf8(f.ledObj), ledText);
        }
        if (!Contains(m.interior.added, f.ledObj)) {
            m.interior.added.push_back(f.ledObj);
        }
        attach.push_back(f);
    }

    // 4. Attach the twins. Nothing draws them until this runs, which is exactly
    //    what makes the whole feature reversible.
    if (!attach.empty()) {
        const auto res = patch_acf_integral::Run(
            fsutil::PathToUtf8(aircraftDir), attach, false, dryRun,
            rep.Sub(kStepIntegralObj));
        LogLines(log, "acf integral", res.log);
        if (!res.changed.empty() || !res.touched.empty()) {
            steps.push_back(
                "Installed switchable integral lighting (Incandescent / LED) — " +
                std::to_string(attach.size()) + " cockpit object(s), attached in " +
                std::to_string(res.touched.size()) + " .acf file(s)");
        } else {
            steps.push_back("! Could not attach the LED cockpit object(s) — "
                            "integral lighting will stay on Incandescent");
        }
    }
    rep.Finish(kStepIntegralObj);
    return steps;
}

// The interior ("Gus Mod") install — an INDEPENDENT, OPT-IN axis. Six parts: the
// OBJ, the texture set, the livery-shadow scan, the four-`.acf` cockpit-spot patch,
// the switchable integral lighting, and its own manifest block. A user who wants
// only the exterior mod never runs any of it, and uninstalling one axis does not
// disturb the other.
std::vector<std::string> InstallInterior(const Options& opts,
                                         const fs::path& objects,
                                         const fs::path& backupDir,
                                         manifest::Manifest& m, Log& log,
                                         Reporter& rep) {
    std::vector<std::string> steps;
    const fs::path aircraftDir = objects.parent_path();
    const bool dryRun = opts.dryRun;

    rep.Begin(kStepInteriorObj);
    // 1. the OBJ. Inject anchors on POINT_COUNTS, which lights_inn.obj has.
    const fs::path target = objects / kInteriorObj;
    if (BackupOnce(target, backupDir, m, &m.interior.added, log, dryRun) ==
        Backup::kRecorded) {
        steps.push_back(std::string("Backed up original ") + kInteriorObj + " → " +
                        BackupLabel(aircraftDir, backupDir));
    }
    std::string text =
        marker::Inject(payload::InteriorObjText(), kPhotonVersion, "interior");
    // The cockpit-light intensity multiplier the user set on the plugin's
    // Settings tab, read back out of the prefs file and re-applied to the
    // fresh OBJ — without this every reinstall silently resets the cockpit to
    // 1x, which reads as the install having broken the setting. Applied to the
    // in-memory text so the aircraft still sees exactly one atomic write. A
    // user who never touched the slider has no prefs entry, SavedFactor
    // answers 1x, and the staged bytes are untouched.
    //
    // ⚠ AND THE BSS NEO COMPENSATION MULTIPLIES IT. A lighting mod that raises
    // X-Plane's spill-light cutoff culls exactly these lamps (core/neomod.h), so
    // the cockpit has to be built brighter to show through it. `opts.neo` is the
    // installer screen's checkbox, auto-ticked from neomod::Detect.
    //
    // ⚠ THE FLAG IS PERSISTED FIRST, and it matters that it is persisted at all:
    // the plugin recomputes this same product on every aircraft load and rewrites
    // the OBJ when it disagrees. If the installer baked a boost the plugin could
    // not know about, the very next load would quietly patch it back out.
    //
    // ⚠ GUARDED BY dryRun LIKE EVERY OTHER WRITE IN THIS FILE. It is the one
    // write here that lands OUTSIDE the aircraft folder — in the user's own
    // preferences — which is exactly why it is easy to forget: nothing about a
    // dry run on an aeroplane suggests a file two directories away.
    if (!dryRun && !intensity::SaveNeo(opts.xplaneRoot, opts.neo)) {
        // Never fatal — a preference is not worth failing an install over. It
        // does mean the plugin will level the OBJ back on the next load, so the
        // line has to be loud enough to explain that if it happens.
        log.Write("could not record the BSS NEO setting in the plugin's "
                  "preferences; the cockpit brightness compensation may be reset "
                  "on the next aircraft load",
                  "WARN");
    }
    const double userFactor = intensity::SavedFactor(opts.xplaneRoot);
    const double intensityFactor = intensity::EffectiveFactor(userFactor, opts.neo);
    if (!intensity::AtDefault(intensityFactor)) {
        int scaledLines = 0;
        text = intensity::PatchText(text, intensityFactor, scaledLines);
        log.Write("cockpit light intensity " + intensity::Label(intensityFactor) +
                  " (setting " + intensity::Label(userFactor) +
                  (opts.neo ? ", BSS NEO compensation " +
                                  intensity::Label(intensity::kNeoBoost)
                            : std::string()) +
                  ") applied to " + std::to_string(scaledLines) + " light line(s)");
        // ⚠ TWO DIFFERENT SENTENCES, because they are two different claims. One
        // says "your setting was preserved"; the other says "we changed your
        // cockpit because of something else on this machine", and a user who is
        // not told the second reads a brighter cockpit as the installer having
        // ignored their slider.
        steps.push_back(
            opts.neo
                ? "Brightened the cockpit for BSS NEO (" +
                      intensity::Label(intensityFactor) + ", from your " +
                      intensity::Label(userFactor) + " setting)"
                : "Applied your cockpit light intensity setting (" +
                      intensity::Label(intensityFactor) + ")");
    }
    log.Write("writing " + fsutil::PathToUtf8(target) + " (" +
              std::to_string(text.size()) + " bytes)");
    if (!dryRun) WriteOrThrow(target, text);
    steps.push_back(std::string("Installed ") + kInteriorObj +
                    " (interior lights, mod by Gus) → " +
                    fsutil::PathToUtf8(objects.filename()));

    rep.Finish(kStepInteriorObj);
    rep.Begin(kStepInteriorTex);
    // 2. the textures. ⚠ NINE replace a stock file and are restored from backup on
    //    uninstall; TWO have no stock counterpart, so they go in `added` and are
    //    DELETED instead — BackupOnce would only log "nothing to back up" and leave
    //    them orphaned in the aircraft folder forever.
    const auto textures = payload::InteriorTextureFiles();
    const int total = static_cast<int>(textures.size());
    std::uint64_t totalTexBytes = 0;
    for (const auto& kv : textures) {
        totalTexBytes += progress::FileSizeOrZero(kv.second);
    }
    std::uint64_t copied = 0;
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
        if (!dryRun) WriteOrThrow(dest, CopyBytes(fsutil::PathFromUtf8(kv.second)));
        // ⚠ AFTER THE WRITE, and weighted by BYTES rather than by file: this set
        // runs from 47 KB (walls_bottom_LIT) to 16 MB (knobs_LIT), so one step per
        // file would race through eight of eleven and then appear to hang.
        copied += progress::FileSizeOrZero(kv.second);
        rep.Sub(kStepInteriorTex)(totalTexBytes > 0
                                      ? static_cast<double>(copied) /
                                            static_cast<double>(totalTexBytes)
                                      : 1.0);
    }
    steps.push_back("Installed " + std::to_string(total) +
                    " interior texture(s) → " +
                    fsutil::PathToUtf8(objects.filename()));

    rep.Finish(kStepInteriorTex);
    rep.Begin(kStepLiveries);
    // 3. the livery scan.
    const auto overrides = InteriorLiveryOverrides(aircraftDir);
    int cleared = 0;
    for (const fs::path& f : overrides) {
        std::error_code re;
        const fs::path rel = fs::relative(f, aircraftDir, re);
        if (re) continue;
        // ⚠ UTF-8, NEVER generic_string(): this string becomes the manifest entry
        // AND the backup's file name, and the livery folder was named by a user in
        // whatever language they write. See fsutil::PathToUtf8Generic.
        const std::string relPosix = fsutil::PathToUtf8Generic(rel);
        if (Contains(m.interior.liveriesPatched, relPosix)) continue;
        const fs::path dest =
            backupDir / "liveries" / fsutil::PathFromUtf8(LiveryBackupName(relPosix));
        log.Write("livery override shadows the installed texture, removing: " +
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

    rep.Finish(kStepLiveries);
    rep.Begin(kStepInteriorAcf);
    // 4. the .acf cockpit spots, all four files per airframe.
    const auto res = patch_acf::Run(fsutil::PathToUtf8(aircraftDir), false, dryRun,
                                    rep.Sub(kStepInteriorAcf));
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

    rep.Finish(kStepInteriorAcf);

    // 5. ⚠ AFTER the texture copy, not inside it. That loop writes the payload's
    //    nine files; these two are DERIVED from the aircraft's own stock, and
    //    running them first would mean deriving from a file the copy is about to
    //    replace.
    const auto integralSteps =
        InstallIntegral(opts, objects, backupDir, m, log, rep);
    steps.insert(steps.end(), integralSteps.begin(), integralSteps.end());

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

    // ⚠ THE ONE LINE THAT SAYS WHICH AEROPLANE THIS RUN IS ABOUT, and it is
    // written for a READER THAT IS NOT A HUMAN: the plugin's Help tab collects the
    // newest installer log PER AIRFRAME into a support report, and everything else
    // in this file names an aircraft only incidentally, inside a "writing <path>"
    // line. Attributing a log by guessing at those paths is exactly the kind of
    // thing that is right until a user renames their aircraft folder.
    //
    // ⚠ THE KEY COMES FIRST AND UNADORNED (`install target: a321 ...`).
    // support::AirframesNamedIn reads to the first space; anything before the key
    // would have to be parsed instead of skipped. See core/support.h.
    log.Write(std::string(support::kInstallTargetTag) + af->key + " (" +
              af->prettyName + ") - install - " + opts.aircraftPath);

    const fs::path aircraftDir = fsutil::PathFromUtf8(opts.aircraftPath);
    const fs::path objects = aircraftDir / "objects";
    const std::string objectsUtf8 = fsutil::PathToUtf8(objects);
    const bool dryRun = opts.dryRun;
    std::vector<std::string> steps;

    // ⚠ THE BACKUP FOLDER IS RESOLVED BELOW, AFTER THE MIGRATION STEP, and never
    // built by hand — see core/backup.h. Reading the manifest here is location-
    // agnostic for the same reason: `manifest::Read` asks the same resolver.
    manifest::Manifest m = manifest::Read(objectsUtf8);
    m.present = true;

    // ⚠ THE WHOLE PLAN BEFORE ANY WORK. Sizes only, so it costs microseconds even
    // where it is pricing an eleven-second scan — and a denominator that grew as the
    // run proceeded would walk the bar backwards. See core/progress.h.
    Reporter rep;
    rep.opts = &opts;
    rep.plan = BuildInstallPlan(opts, aircraftDir, objects);

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
    rep.Begin(kStepPlugin);
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

    rep.Finish(kStepPlugin);

    // The plugin's runtime data goes in right after the binary. Plain data files, so
    // unlike the `.xpl` they are never locked by a running sim and this step cannot
    // be the one that fails.
    rep.Begin(kStepPluginData);
    for (std::string& s : InstallPluginData(pluginRoot, log, dryRun)) {
        steps.push_back(std::move(s));
    }
    rep.Finish(kStepPluginData);

    // ── the backup folder ────────────────────────────────────────────────────
    // ⚠ BEFORE ANYTHING TOUCHES A BACKUP, AND AFTER THE PLUGIN. Before, because
    // every path below resolves the folder once and holds it; after, because the
    // `.xpl` write is the step that fails on a running sim and it must stay the
    // first thing that can abort a run.
    //
    // Retroactive by design: a user upgrading from an earlier build has their whole
    // backup folder in `objects/`, and it is moved out here rather than left behind — a
    // second folder at the new location would split the bookkeeping from the files
    // it accounts for.
    if (backup::LegacyDirInUse(aircraftDir)) {
        rep.Begin(kStepBackupDir);
        const backup::MigrateResult mig = backup::Migrate(aircraftDir, dryRun);
        LogLines(log, "backup", mig.log);
        if (dryRun) {
            steps.push_back("Would move Photon's backup folder out of objects/ → " +
                            fsutil::PathToUtf8(
                                backup::PreferredDir(aircraftDir).filename()) +
                            " (" + std::to_string(mig.files) + " file(s))");
        } else if (mig.complete) {
            steps.push_back("Moved Photon's backup folder out of objects/ → " +
                            fsutil::PathToUtf8(
                                backup::PreferredDir(aircraftDir).filename()) +
                            " (" + std::to_string(mig.files) + " file(s))");
        } else {
            steps.push_back("! Could not move Photon's backup folder out of "
                            "objects/ — it is still there and still works; the "
                            "next install will try again");
        }
        rep.Finish(kStepBackupDir);
    }
    const fs::path backupDir = backup::Dir(aircraftDir);
    const std::string backupLabel = BackupLabel(aircraftDir, backupDir);

    // ── which wing mod is this aircraft ACTUALLY running? ────────────────────
    //
    // ⚠ IMMEDIATELY BEFORE THE OBJ THE ANSWER GOVERNS, so the log reads as one
    // thought: "this aircraft is running Durantula" then "installed the stock wing
    // variant". Put anywhere earlier it is a paragraph the user has scrolled past
    // by the time the line it contradicts appears.
    //
    // ⚠ READ-ONLY, NON-THROWING, AND IT NEVER CHANGES WHAT IS INSTALLED. The user's
    // choice is the user's; a detector that silently substituted a different wing
    // variant would be unexplainable from the screen they clicked through, and it
    // would be wrong exactly when detection is wrong. It reports, and that is all.
    if (WingsFor(opts.airframeKey).size() > 1) {
        rep.Begin(kStepWingCheck);
        const wingmod::Result wm =
            wingmod::Detect(opts.aircraftPath, opts.airframeKey);
        log.Write(wm.summary);
        for (const wingmod::Finding& f : wm.findings) {
            const bool bad = f.severity == wingmod::Severity::Problem;
            log.Write(f.text, bad ? "WARN" : "INFO");
            steps.push_back((bad ? "! " : "") + f.text);
        }
        // ⚠ THE MISMATCH IS THE POINT OF THE WHOLE CHECK. Photon's `wing_left` /
        // `wing_right` mounts are copies of the aircraft's own flex chain, so a
        // build for the wrong variant binds the wingtip lights to a driver the
        // wing no longer uses: they hold still while the wing flexes. It costs
        // nothing on the ground and needs a loaded aircraft in the air to see.
        if (wm.determined && wm.wing != opts.wing) {
            const std::string line =
                "This aircraft is running " + WingProse(wm.wing) +
                " wings, but you chose the " + WingProse(opts.wing) +
                " build — Photon's wingtip lights will not follow the wing. "
                "Re-run and pick " + WingProse(wm.wing) + ".";
            log.Write(line, "WARN");
            steps.push_back("! " + line);
        } else if (wm.determined && wm.Healthy()) {
            log.Write("wing variant matches this aircraft (" + opts.wing + ")");
        }
        rep.Finish(kStepWingCheck);
    }

    rep.Begin(kStepExteriorObj);
    const fs::path target = objects / af->objName;
    switch (BackupOnce(target, backupDir, m, &m.added, log, dryRun)) {
        case Backup::kRecorded:
            steps.push_back(std::string("Backed up original ") + af->objName +
                            " → " + backupLabel);
            break;
        case Backup::kAlreadyHave:
            steps.push_back(std::string("Original ") + af->objName +
                            " already backed up — reused");
            break;
        case Backup::kNoOriginal:
            steps.push_back(std::string("! No original ") + af->objName +
                            " to back up — the installed copy will be removed "
                            "on uninstall");
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
    rep.Finish(kStepExteriorObj);

    // Redirect ToLiss's skin-glow LIT-texture regions to Photon's own dataref so the
    // painted skin flashes in lockstep with the LED billboards. Each affected mesh
    // OBJ is backed up once before patching; uninstall restores it via the existing
    // backed_up list, so there is no extra bookkeeping and no `.bak` sibling.
    if (GlowIndicesFor(opts.airframeKey) != nullptr) {
        rep.Begin(kStepGlow);
        // ⚠ SCANNED ONCE AND PASSED ON. This reads every `.obj` in the aircraft —
        // 1,597 MB on the A330-900 — and `patch_glow::Run` used to do the identical
        // scan again immediately afterwards, which was 11.1 s of an 11.5 s install.
        const std::vector<std::string> glowTargets = patch_glow::TargetFiles(
            objectsUtf8, opts.airframeKey, false, rep.Sub(kStepGlow));
        for (const std::string& f : glowTargets) {
            const fs::path p = fsutil::PathFromUtf8(f);
            // ⚠ nullptr: the glow pass PATCHES these meshes in place rather than
            // writing one of ours over them, so a missing source means nothing
            // was written — recording it would have uninstall delete a ToLiss
            // file Photon never created.
            if (BackupOnce(p, backupDir, m, nullptr, log, dryRun) ==
                Backup::kRecorded) {
                steps.push_back("Backed up original " +
                                fsutil::PathToUtf8(p.filename()) + " → " +
                                backupLabel);
            }
        }
        const auto res = patch_glow::Run(objectsUtf8, opts.airframeKey, false,
                                         dryRun, &glowTargets);
        LogLines(log, "glow", res.log);
        if (res.regions > 0) {
            steps.push_back("Redirected " + std::to_string(res.regions) +
                            " skin-glow region(s) to Photon dataref");
        }
        rep.Finish(kStepGlow);
    }

    // Capture any prior RealWings patch state BEFORE clearing it, so a switch away
    // from RealWings can undo the in-place mod patch.
    const bool wasRealwings = m.realwingsPatched;
    const std::string oldRwDir = m.realwingsDir;
    m.realwingsPatched = false;
    m.realwingsDir.clear();

    if (opts.wing == "realwings") {
        rep.Begin(kStepRealWings);
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
                    opts.airframeKey, dryRun, rep.Sub(kStepRealWings));
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
        rep.Finish(kStepRealWings);
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

    // Display glow — part of the BASE install, not an opt-in. It works on a stock
    // cockpit, so it does not belong behind the interior's opt-in; whether the
    // glow is VISIBLE is a runtime choice on the Displays tab.
    if (AirframeHasScreens(opts.airframeKey)) {
        if (payload::ScreensAvailable(opts.airframeKey)) {
            rep.Begin(kStepScreens);
            for (std::string& s :
                 InstallScreens(objects, opts.airframeKey, m, log, dryRun, rep)) {
                steps.push_back(std::move(s));
            }
            rep.Finish(kStepScreens);
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
                 InstallInterior(opts, objects, backupDir, m, log, rep)) {
                steps.push_back(std::move(s));
            }
        }
    }

    rep.Begin(kStepManifest);
    m.version = kPhotonVersion;
    m.wing = opts.wing;
    m.installedAt = NowIso8601Seconds();
    if (!dryRun) {
        // The note goes in FIRST, so a manifest write that fails does not leave a
        // folder of the user's stock files with nothing in it saying what they are.
        // ⚠ Best-effort: a README that cannot be written is not a reason to fail an
        // install that otherwise worked, so this one write is not `WriteOrThrow`.
        std::error_code ec;
        fsutil::AtomicWriteBytes(backupDir / backup::kReadmeName,
                                 backup::ReadmeText(), ec);
        std::string err;
        if (!manifest::Write(objectsUtf8, m, err)) throw ActionError(err);
    }
    steps.push_back("Wrote install manifest");
    rep.Finish(kStepManifest);
    return steps;
}

std::vector<std::string> Uninstall(const Options& opts, Log& log) {
    // The same machine-readable stamp Install writes — an uninstall is a run a
    // bug report may well be about, and a log with no target line at all reads to
    // the collector as a run that never chose an aircraft.
    if (const Airframe* af = AirframeByKey(opts.airframeKey)) {
        log.Write(std::string(support::kInstallTargetTag) + af->key + " (" +
                  af->prettyName + ") - uninstall - " + opts.aircraftPath);
    }
    const fs::path aircraftDir = fsutil::PathFromUtf8(opts.aircraftPath);
    const fs::path objects = aircraftDir / "objects";
    // ⚠ RESOLVED, AND DELIBERATELY NOT MIGRATED FIRST. An uninstall is about to
    // empty this folder and remove it, so moving it out of `objects/` on the way
    // would be a rename to buy nothing — and it would put the one code path a user
    // runs when something is already wrong through an extra step that can fail.
    // Either location restores identically; both are cleaned up at the end.
    const fs::path backupDir = backup::Dir(aircraftDir);
    const std::string objectsUtf8 = fsutil::PathToUtf8(objects);
    const bool dryRun = opts.dryRun;

    const manifest::Manifest m = manifest::Read(objectsUtf8);
    if (!m.present) {
        throw ActionError("no Photon installation found to uninstall (no manifest)");
    }
    std::vector<std::string> steps;

    // Same rule as Install: the whole plan first, from the manifest and from sizes.
    Reporter rep;
    rep.opts = &opts;
    rep.plan = BuildUninstallPlan(m, aircraftDir, objects, backupDir, dryRun);

    // ⚠ `backed_up[]` RESTORES. The added[] lists below DELETE.
    rep.Begin(kStepRestore);
    for (std::size_t ri = 0; ri < m.backedUp.size(); ++ri) {
        rep.Sub(kStepRestore)(static_cast<double>(ri) /
                              (m.backedUp.empty() ? 1.0 : m.backedUp.size()));
        const std::string& rel = m.backedUp[ri];
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

    rep.Finish(kStepRestore);

    // ⚠ THE MIRROR OF THE LOOP ABOVE, and normally empty: a file we wrote that had
    // no stock original is ours alone, so it is DELETED rather than restored.
    // UNGATED, unlike the interior and screens lists — there is no per-axis flag to
    // hang it on, and an entry here means an exterior install already happened.
    rep.Begin(kStepRemove);
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

    rep.Finish(kStepRemove);

    if (m.realwingsPatched && !m.realwingsDir.empty()) {
        rep.Begin(kStepRealWings);
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
        rep.Finish(kStepRealWings);
    }

    // ── display glow ─────────────────────────────────────────────────────────
    // Nothing here is in `backed_up`, because nothing here replaced a stock file.
    //
    // ⚠ DETACH FIRST, THEN DELETE. An `.acf` still naming an OBJ that is gone is a
    // missing-object warning on every load, whereas an attached-but-unused row left
    // behind by the reverse order is invisible until the user wonders why their
    // `.acf` differs from stock.
    if (m.screens.installed) {
        rep.Begin(kStepScreens);
        if (!m.screens.acfPatched.empty()) {
            const auto res = patch_acf_screens::Run(fsutil::PathToUtf8(aircraftDir),
                                                    true, dryRun,
                                                    rep.Sub(kStepScreens));
            LogLines(log, "acf screens", res.log);
            // ⚠ `changed`, NOT `touched`. A detach visits every variant — including
            // the XP11 pair, which only an older Photon ever attached — so counting
            // files PROCESSED would report "detached from 4" against the install's
            // own "attached in 2", which reads as one of them being wrong.
            steps.push_back("Detached the display-glow OBJ from " +
                            std::to_string(res.changed.size()) + " .acf file(s)");
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
        rep.Finish(kStepScreens);
    }

    // ── interior ─────────────────────────────────────────────────────────────
    // The OBJ and the nine stock-replacing textures are already restored by the
    // backed_up loop above. What remains is what a backup cannot express.
    // Livery backups whose livery is gone stay behind (see below); the count
    // decides whether the folder's README stays with them.
    int keptLiveryBackups = 0;
    if (m.interior.installed) {
        rep.Begin(kStepLiveries);
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
            // ⚠ ONLY INTO A LIVERY THAT STILL EXISTS. `rel` is
            // `liveries/<name>/objects/<file>`, and AtomicWriteBytes creates every
            // missing parent — so a livery the user deleted or renamed since the
            // install would come BACK as a folder holding one texture: a ghost
            // livery X-Plane lists, ToLiss logs about, and one more entry in the
            // livery index X-Plane keeps in the aircraft prefs. The backup stays
            // where it is (a renamed livery's owner may still want it) and the step
            // line says where.
            const fs::path liveryDir = dest.parent_path().parent_path();
            std::error_code le;
            if (!fs::is_directory(liveryDir, le) || le) {
                log.Write("livery folder no longer exists, not recreating it: " +
                          fsutil::PathToUtf8(liveryDir), "WARN");
                steps.push_back("! Livery folder for " + rel +
                                " no longer exists — not recreated; its backed-up "
                                "texture was left in " +
                                BackupLabel(aircraftDir, backupDir));
                ++keptLiveryBackups;
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
        rep.Finish(kStepLiveries);
        rep.Begin(kStepInteriorAcf);
        if (!m.interior.acfPatched.empty()) {
            const auto res = patch_acf::Run(fsutil::PathToUtf8(aircraftDir), true,
                                            dryRun, rep.Sub(kStepInteriorAcf));
            LogLines(log, "acf", res.log);
            // ⚠ `changed`, NOT `touched` — same reason as the detach above.
            steps.push_back("Restored stock cockpit spot lights in " +
                            std::to_string(res.changed.size()) + " .acf file(s)");
        }

        if (!dryRun) fsutil::RemoveDirIfEmpty(backupDir / "liveries");
        rep.Finish(kStepInteriorAcf);

        // 4. ⚠ The LED twins' attachment rows, found BY SUFFIX rather than from
        //    the manifest. The twin OBJs themselves were just deleted by the
        //    `added[]` loop above, so a row left behind would name a file that is
        //    gone — and X-Plane logs a missing attachment on every load. Working
        //    by suffix also means an uninstall still cleans up when the manifest
        //    has lost the list, which is the state a half-finished install or a
        //    hand-edited folder leaves behind.
        //
        //    The gated `lamps.obj`/`knobs.obj` need nothing here: they are in
        //    `backed_up[]` and the restore loop has already put the stock files
        //    back, gate and all.
        rep.Begin(kStepIntegralObj);
        {
            const auto res = patch_acf_integral::Run(
                fsutil::PathToUtf8(aircraftDir), {}, true, dryRun,
                rep.Sub(kStepIntegralObj));
            LogLines(log, "acf integral", res.log);
            if (!res.changed.empty()) {
                steps.push_back("Detached the LED cockpit object(s) from " +
                                std::to_string(res.changed.size()) +
                                " .acf file(s)");
            }
        }
        rep.Finish(kStepIntegralObj);
    }

    rep.Begin(kStepManifest);
    log.Write("removing " + fsutil::PathToUtf8(backupDir));
    if (!dryRun) {
        for (const std::string& rel : m.backedUp) {
            std::error_code ec;
            fs::remove(backupDir / fsutil::PathFromUtf8(rel), ec);
        }
        std::error_code ec;
        fs::remove(backupDir / kManifestName, ec);
        // ⚠ The README stays with anything that stays: a livery backup kept
        // because its livery is gone must not sit in a folder nothing explains.
        if (keptLiveryBackups == 0) fs::remove(backupDir / backup::kReadmeName, ec);
        fsutil::RemoveDirIfEmpty(backupDir);   // only succeeds if now empty
        // ⚠ BOTH LOCATIONS, and the second is not redundant. An install that could
        // not finish its migration leaves an empty folder behind in `objects/`, and
        // an uninstall that tidies only the folder it restored FROM would leave the
        // very folder this move exists to get rid of sitting there empty forever.
        // Only-if-empty, so a legacy folder still holding files is never touched.
        fsutil::RemoveDirIfEmpty(backup::LegacyDir(aircraftDir));
    }
    steps.push_back(keptLiveryBackups == 0
                        ? std::string("Removed ") + kBackupDirName + " and manifest"
                        : "Removed manifest; " + std::to_string(keptLiveryBackups) +
                              " livery texture backup(s) kept in " +
                              BackupLabel(aircraftDir, backupDir));
    rep.Finish(kStepManifest);

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
