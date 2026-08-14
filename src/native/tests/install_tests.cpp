// photoncore install tests — detection, payload resolution, and the install /
// uninstall round trip against a real temp X-Plane tree.
//
// ⚠ THIS FILE REPLACES A CORRECTNESS NET, IT DOES NOT ADD ONE. Until 2026-08-09
// `core/detect.cpp`, `core/actions.cpp` and `core/payload.cpp` had NO C++ tests at
// all: they were covered from Python — `tests/test_actions.py`,
// `test_detect.py`, `test_payload.py`, `test_interior_install.py`,
// `test_screens_install.py` — plus `tests/test_parity.py`, which ran both
// installers over two copies of one fake tree and diffed the results byte for
// byte. Deleting the Python installer deletes all of that at once, so every
// invariant those files pinned had to land here FIRST. The cases below are ports,
// and each keeps the reason it exists: they are shipped bug fixes, not coverage.
//
// What is deliberately NOT here: the `.acf` engine, the marker, the glow token
// swap, the attachment table's renumbering and the RealWings text rewrite. Those
// are unit-testable from a string and live in core_tests.cpp; this file is the
// layer above, where a fake payload meets a fake aircraft folder.
#include "test_harness.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/actions.h"
#include "core/constants.h"
#include "core/detect.h"
#include "core/fsutil.h"
#include "core/manifest.h"
#include "core/patch_acf_screens.h"
#include "core/patch_realwings.h"
#include "core/payload.h"
#include "core/progress.h"
#include "core/version.h"

namespace fs = std::filesystem;
using namespace photon;

namespace {

// A scratch directory that removes itself, so a failed run never poisons the next.
class TempDir {
public:
    TempDir() {
        const auto base = fs::temp_directory_path();
        for (int i = 0; i < 1000; ++i) {
            fs::path candidate =
                base / ("photon_install_test_" + std::to_string(::rand()) + "_" +
                        std::to_string(i));
            std::error_code ec;
            if (fs::create_directory(candidate, ec) && !ec) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error("could not create a temp directory");
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }
    fs::path operator/(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

void Write(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    CHECK(fsutil::AtomicWriteBytes(p, data));
}

std::string Read(const fs::path& p) {
    std::string out;
    CHECK(fsutil::ReadFileBytes(p, out));
    return out;
}

bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Any step line containing `needle` — the steps vector is the installer's own
// account of what it did, and several invariants are only observable there.
bool AnyStep(const std::vector<std::string>& steps, const std::string& needle) {
    for (const std::string& s : steps) {
        if (Contains(s, needle)) return true;
    }
    return false;
}

bool HasEntry(const std::vector<std::string>& v, const std::string& want) {
    return std::find(v.begin(), v.end(), want) != v.end();
}

std::string U8(const fs::path& p) { return fsutil::PathToUtf8(p); }

// ── the fake payload ─────────────────────────────────────────────────────────
// Distinct bytes per arch so a test can assert each `.xpl` lands verbatim rather
// than merely existing. Stand-ins for the compiled binaries: these tests care
// about placement and byte-fidelity, not that the file is a loadable plugin.
const char* kFakeXpl[3][2] = {
    {"win_x64", "MZ  fake win ToLissPhoton.xpl\n"},
    {"mac_x64", "\xcf\xfa\xed\xfe  fake mac ToLissPhoton.xpl\n"},
    {"lin_x64", "\x7f" "ELF  fake lin ToLissPhoton.xpl\n"},
};

// Enough of `realwings_patch.json` to rewrite the two baked lights the fake mod
// OBJ carries. The full-fidelity parse is core_tests.cpp's job; this only has to
// be loadable and produce a patch.
const char* kRwJson = R"({
  "_generated": "GENERATED — DO NOT HAND-EDIT",
  "schema": 1,
  "marker": "# >>> Photon RealWings",
  "sourceAirframe": "a320",
  "classes": {
    "airplane_nav_left_size": { "side": "port", "path": "wing_nav/fence/nav_beam" },
    "airplane_strobe_size":   { "side": null,   "path": "wing_strobe/fence/strobe_beam" }
  },
  "airframes": {
    "a320": {
      "airplane_nav_left_size": {
        "fence": {
          "port":      { "category": "nav", "class": "airplane_nav_bb", "index": 0,
                         "dir": "-0.574 0 -0.819", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "0.12 0.77 0.25", "intensity": 4000, "cone": "0.2"},
                           {"hide": "0 0", "rgb": "0.06 1 0",       "intensity": 5000, "cone": "0.2"}]},
          "starboard": { "category": "nav", "class": "airplane_nav_bb", "index": 0,
                         "dir": "0.574 0 -0.819", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "1 0.03 0", "intensity": 4000, "cone": "0.2"},
                           {"hide": "0 0", "rgb": "1 0.1 0",  "intensity": 5000, "cone": "0.2"}]}
        },
        "sharklet": {
          "port":      { "category": "nav", "class": "airplane_nav_bb", "index": 0,
                         "dir": "-0.574 0 -0.819", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "0.12 0.77 0.25", "intensity": 4000, "cone": "0.2"},
                           {"hide": "0 0", "rgb": "0.06 1 0",       "intensity": 5000, "cone": "0.2"}]},
          "starboard": { "category": "nav", "class": "airplane_nav_bb", "index": 0,
                         "dir": "0.574 0 -0.819", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "1 0.03 0", "intensity": 4000, "cone": "0.2"},
                           {"hide": "0 0", "rgb": "1 0.1 0",  "intensity": 5000, "cone": "0.2"}]}
        }
      },
      "airplane_strobe_size": {
        "fence": {
          "port":      { "category": "strobe", "class": "airplane_strobe_pm", "index": 0,
                         "dir": "-0.895 -0.094 -0.436", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "1 1 1", "intensity": 200000, "cone": "0.3"},
                           {"hide": "0 0", "rgb": "1 1 1", "intensity": 250000, "cone": "0.3"}]},
          "starboard": { "category": "strobe", "class": "airplane_strobe_pm", "index": 0,
                         "dir": "0.895 -0.094 -0.436", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "1 1 1", "intensity": 200000, "cone": "0.3"},
                           {"hide": "0 0", "rgb": "1 1 1", "intensity": 250000, "cone": "0.3"}]}
        },
        "sharklet": {
          "port":      { "category": "strobe", "class": "airplane_strobe_pm", "index": 0,
                         "dir": "-0.895 -0.094 -0.436", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "1 1 1", "intensity": 200000, "cone": "0.3"},
                           {"hide": "0 0", "rgb": "1 1 1", "intensity": 250000, "cone": "0.3"}]},
          "starboard": { "category": "strobe", "class": "airplane_strobe_pm", "index": 0,
                         "dir": "0.895 -0.094 -0.436", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "1 1 1", "intensity": 200000, "cone": "0.3"},
                           {"hide": "0 0", "rgb": "1 1 1", "intensity": 250000, "cone": "0.3"}]}
        }
      }
    }
  }
})";

// A complete `payload/` — every axis the installer can resolve, with stub
// contents. Built once per test into its own temp dir and pointed at with
// `payload::SetPayloadDir`, which is exactly what the Python suite did by
// reassigning `payload.PAYLOAD_DIR`.
//
// ⚠ EVERY interior texture must be present. `InteriorTextureFiles()` treats a
// missing one as an error rather than a skip — shipping 9 of 11 leaves the
// cockpit half-retextured, which reads in-sim as a broken mod.
class FakePayload {
public:
    explicit FakePayload(const fs::path& root) : root_(root) {
        for (const Airframe& af : Airframes()) {
            for (const std::string& wing : WingsFor(af.key)) {
                Write(root_ / "objs" / wing / af.objName,
                      std::string("I\n800\nOBJ\n\nPOINT_COUNTS\t0\t0\t0\t0\n"
                                  "# payload obj ") + af.key + " " + wing + "\n");
            }
        }
        Write(root_ / "objs" / "interior" / kInteriorObj,
              "I\n800\nOBJ\n\nPOINT_COUNTS\t0\t0\t0\t0\n"
              "# ToLissPhoton/interior/dome\n");
        // ⚠ PER AIRFRAME, and the stub says WHICH — the whole hazard this layout
        // exists to remove is installing one airframe's six positions into
        // another, where every light draws and every light is in the wrong place.
        // A stub that read the same for all of them could not catch that.
        for (const std::string& key : ScreensAirframes()) {
            Write(root_ / "objs" / "screens" / key / kScreensObj,
                  "I\n800\nOBJ\n\nPOINT_COUNTS\t0\t0\t0\t0\n# screens " + key + "\n");
        }
        for (const std::string& t : InteriorTextures()) {
            Write(root_ / "textures" / "interior" / t, "PNG stub " + t);
        }
        for (const auto& row : kFakeXpl) {
            Write(root_ / "plugin" / kPluginFolder / row[0] / kXplName, row[1]);
        }
        Write(root_ / kPluginDataDirName / kPanelFxFile, "# panel fx stub\n");
        Write(root_ / kPluginDataDirName / "overlays" / "fcu.png", "PNG overlay fcu");
        Write(root_ / kPluginDataDirName / "overlays" / "pfd.png", "PNG overlay pfd");
        Write(root_ / patch_realwings::kPatchJson, kRwJson);
        payload::SetPayloadDir(U8(root_));
    }

    const fs::path& path() const { return root_; }

private:
    fs::path root_;
};

// ── the fake aircraft ────────────────────────────────────────────────────────
// A minimal `<X-Plane>/Aircraft/<folder>/objects/` holding an original,
// non-Photon exterior OBJ, as if freshly installed from ToLiss.
class FakeAircraft {
public:
    FakeAircraft(const fs::path& tmp, const std::string& airframe = "a320",
                 const std::string& folder = "ToLissA320_V1p3p2") {
        airframe_ = airframe;
        const Airframe* af = AirframeByKey(airframe);
        CHECK(af != nullptr);
        root_ = tmp / "X-Plane 12";
        folder_ = root_ / "Aircraft" / folder;
        objects_ = folder_ / "objects";
        objPath_ = objects_ / af->objName;
        Write(objPath_, kOriginalObj);
        std::error_code ec;
        fs::create_directories(root_ / "Resources" / "plugins", ec);
    }

    static constexpr const char* kOriginalObj =
        "I\n800\nOBJ\n\nGLOBAL_cockpit_lit\nPOINT_COUNTS\t0\t0\t0\t0\n"
        "# ORIGINAL STOCK CONTENT\n";

    actions::Options Opts(const std::string& wing = "stock") const {
        actions::Options o;
        o.airframeKey = airframe_;
        o.aircraftPath = U8(folder_);
        o.wing = wing;
        o.xplaneRoot = U8(root_);
        return o;
    }

    const fs::path& root() const { return root_; }
    const fs::path& folder() const { return folder_; }
    const fs::path& objects() const { return objects_; }
    const fs::path& objPath() const { return objPath_; }
    fs::path backupDir() const { return objects_ / kBackupDirName; }
    fs::path pluginRoot() const {
        return root_ / "Resources" / "plugins" / kPluginFolder;
    }

private:
    std::string airframe_;
    fs::path root_, folder_, objects_, objPath_;
};


// -- the progress recorder ---------------------------------------------------
// Everything `ProgressFn` reported during a run, in order.
//
// WARNING: this exists because the old bar failed SILENTLY. `ProgressFn` was called
// from exactly one loop - the interior texture copy - so an install without the
// cockpit mod reported NOTHING and the bar sat at 0% until the run ended and the GUI
// set it to 1.0 itself. Nothing in the code read as wrong; the callback was simply
// never reached, and a bar that goes 0 -> 100 looks like a fast install.
struct ProgressLog {
    std::vector<double> fractions;
    std::vector<std::string> steps;

    static void Callback(double f, const std::string& step, void* self) {
        auto* p = static_cast<ProgressLog*>(self);
        p->fractions.push_back(f);
        p->steps.push_back(step);
    }
    void AttachTo(actions::Options& o) {
        o.progress = &Callback;
        o.progressUserData = this;
    }
    bool SawStep(const std::string& name) const {
        return std::find(steps.begin(), steps.end(), name) != steps.end();
    }
    double Last() const { return fractions.empty() ? -1.0 : fractions.back(); }
};

// A SkunkCrafts cfg, the pipe-delimited form the real files use.
std::string SkunkCfg(const std::string& module, const std::string& name,
                     const std::string& version) {
    return "module|" + module + "\nname|" + name + "\nversion|" + version + "\n";
}

// A STOCK `.acf`: the cockpit spot block as ToLiss ships it, plus an `_obja` table
// with a `lights_inn.obj` frame for the screens attachment to copy a row from.
//
// ⚠ THE SPOT NAMES ARE THE SENTINEL AND IT READS BACKWARDS. A stock file names a
// DATAREF per spot; the install rewrites the three to the literal `none`, which is
// ToLiss's own idiom for "no spot here" (the A339 ships one). So `none` means
// PATCHED, and a fixture written with `none` is already-installed — it makes the
// install a no-op the test still passes, and it makes the reversal look wrong when
// it is right. Cost an hour on 2026-08-09.
std::string StockAcf() {
    return std::string(
        "I\r\n"
        "1 version\r\n"
        "ACF\r\n"
        "P acf/_spot1_3d_xyz/1 1.000000000\r\n"
        "P acf/_spot_angle/0 0.000000000\r\n"
        "P acf/_spot_size/0 20.000000000\r\n"
        "P acf/_spot_size/1 28.010000229\r\n"
        "P acf/_spot_size/2 38.009998322\r\n"
        "P acf/_spot_the/0 -70.000000000\r\n"
        "P acf/_spot_psi/0 -20.000000000\r\n"
        "P acf/_spot_name_3d/0 sim/cockpit2/electrical/panel_brightness_ratio[0]\r\n"
        "P acf/_spot_name_3d/1 sim/cockpit2/electrical/panel_brightness_ratio[3]\r\n"
        "P acf/_spot_name_3d/2 ckpt/lights/map\r\n"
        "P acf/_spot_name_3d/3 none\r\n"
        "P _obja/0/_v10_att_file_stl fuselage.obj\r\n"
        "P _obja/0/_v10_att_y_acf_prt_ref 0.000000000\r\n"
        "P _obja/1/_v10_att_file_stl lights_inn.obj\r\n"
        "P _obja/1/_v10_att_y_acf_prt_ref 0.000000000\r\n"
        "P _obja/count 2\r\n");
}

actions::NullLog gLog;

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// detect — identification is by CONTENT, never by folder name or location
// ─────────────────────────────────────────────────────────────────────────────
TEST("detect: a root needs both Aircraft/ and Resources/") {
    TempDir tmp;
    CHECK(!detect::IsValidRoot(U8(tmp / "nope")));
    std::error_code ec;
    fs::create_directories(tmp.path() / "half" / "Aircraft", ec);
    CHECK(!detect::IsValidRoot(U8(tmp / "half")));
    fs::create_directories(tmp.path() / "whole" / "Aircraft", ec);
    fs::create_directories(tmp.path() / "whole" / "Resources", ec);
    CHECK(detect::IsValidRoot(U8(tmp / "whole")));
}

TEST("detect: the SkunkCrafts cfg is pipe-delimited and drops a leading v") {
    const detect::SkunkCfg cfg = detect::ParseSkunkCfg(
        "module|https://…/aircraft-repositories/A321/\nname|ToLiss A321\n"
        "version|v1.4.1\nnoise|ignored\n");
    CHECK_EQ(cfg.module, std::string("https://…/aircraft-repositories/A321/"));
    CHECK_EQ(cfg.name, std::string("ToLiss A321"));
    CHECK_EQ(cfg.version, std::string("1.4.1"));
}

TEST("detect: a missing cfg is empty, never an error") {
    TempDir tmp;
    const detect::SkunkCfg cfg = detect::ReadSkunkCfg(U8(tmp.path()));
    CHECK(cfg.module.empty());
    CHECK(cfg.name.empty());
}

TEST("detect: the cfg MODULE identifies an aircraft in a renamed folder") {
    // ⚠ The whole point of content-based detection: users rename the folder and
    // drop installs in sub-hangars. A folder called "My A321" with no recognizable
    // name must still be found.
    TempDir tmp;
    const fs::path dir = tmp / "My Favourite Bus";
    Write(dir / "skunkcrafts_updater.cfg",
          SkunkCfg("https://x/aircraft-repositories/A321/", "Something Else", "1.4.1"));
    CHECK_EQ(detect::IdentifyAirframe(U8(dir)), std::string("a321"));
}

TEST("detect: the cfg NAME identifies when the module URL is unrecognized") {
    TempDir tmp;
    const fs::path dir = tmp / "renamed";
    Write(dir / "skunkcrafts_updater.cfg",
          SkunkCfg("https://x/some-other-repo/", "ToLiss A330-900", "1.0.4"));
    CHECK_EQ(detect::IdentifyAirframe(U8(dir)), std::string("a339"));
}

TEST("detect: with no usable cfg the OBJ FILENAME is the fingerprint") {
    // ⚠ And this is why kInteriorObj must never sit in the Airframe::objName slot:
    // lights_inn.obj is byte-identical across A319/A320/A321, so it would make
    // every A3xx match every other one.
    TempDir tmp;
    const fs::path dir = tmp / "no-cfg-at-all";
    Write(dir / "objects" / "lights_out319_XP12.obj", "I\n800\nOBJ\n");
    CHECK_EQ(detect::IdentifyAirframe(U8(dir)), std::string("a319"));
}

TEST("detect: a folder that is none of ours identifies as nothing") {
    TempDir tmp;
    const fs::path dir = tmp / "Cessna 172";
    Write(dir / "objects" / "wing.obj", "I\n800\nOBJ\n");
    CHECK(detect::IdentifyAirframe(U8(dir)).empty());
}

TEST("detect: a fresh aircraft reads as not installed") {
    TempDir tmp;
    FakeAircraft fa(tmp.path());
    const detect::Status st = detect::PhotonStatus(U8(fa.objects()));
    CHECK(!st.installed);
    CHECK(st.version.empty());
}

TEST("detect: with no manifest the OBJ MARKER still reports the install") {
    TempDir tmp;
    FakeAircraft fa(tmp.path());
    Write(fa.objPath(),
          std::string("I\n800\nOBJ\n\nPOINT_COUNTS\t0\t0\t0\t0\n") +
              kMarkerPrefix + "0.7 wing: durantula\n");
    const detect::Status st = detect::PhotonStatus(U8(fa.objects()));
    CHECK(st.installed);
    CHECK_EQ(st.version, std::string("0.7"));
    CHECK_EQ(st.wing, std::string("durantula"));
}

TEST("detect: the manifest outranks the marker for version and wing") {
    TempDir tmp;
    FakeAircraft fa(tmp.path());
    Write(fa.objPath(),
          std::string("I\n800\nOBJ\n\nPOINT_COUNTS\t0\t0\t0\t0\n") +
              kMarkerPrefix + "0.6 wing: stock\n");
    manifest::Manifest m;
    m.present = true;
    m.version = "0.8";
    m.wing = "realwings";
    std::string err;
    CHECK(manifest::Write(U8(fa.objects()), m, err));

    const detect::Status st = detect::PhotonStatus(U8(fa.objects()));
    CHECK(st.installed);
    CHECK_EQ(st.version, std::string("0.8"));
    CHECK_EQ(st.wing, std::string("realwings"));
    CHECK(!st.stale);
}

TEST("detect: manifest present but marker GONE is STALE, not installed-and-fine") {
    // ⚠ THE MARKER CORROBORATES THE MANIFEST. An aircraft update replaces the OBJ
    // and takes our edit with it while leaving the bookkeeping behind; without this
    // the installer reports a healthy install over a reverted file.
    TempDir tmp;
    FakeAircraft fa(tmp.path());
    manifest::Manifest m;
    m.present = true;
    m.version = kPhotonVersion;
    m.wing = "stock";
    std::string err;
    CHECK(manifest::Write(U8(fa.objects()), m, err));
    // fa.objPath() is still the pristine ToLiss file — no marker in it.
    const detect::Status st = detect::PhotonStatus(U8(fa.objects()));
    CHECK(st.installed);
    CHECK(st.stale);
}

TEST("detect: the interior is a SECOND, INDEPENDENT status axis") {
    // ⚠ Deliberately not folded into PhotonStatus, which scans only the exterior
    // OBJs: an aircraft with the interior installed and the exterior reverted would
    // otherwise read as "not installed at all" and hide a live modification.
    TempDir tmp;
    FakeAircraft fa(tmp.path());
    Write(fa.objects() / kInteriorObj,
          std::string("I\n800\nOBJ\n\nPOINT_COUNTS\t0\t0\t0\t0\n") + kMarkerPrefix +
              "0.8 wing: stock\nANIM_hide 0.5 2.5 " + kInteriorObjNeedle + "dome\n");
    CHECK(detect::InteriorStatus(U8(fa.objects())).installed);
    CHECK(!detect::PhotonStatus(U8(fa.objects())).installed);
}

TEST("detect: aircraft are found in SUB-HANGARS, not just directly under Aircraft/") {
    TempDir tmp;
    const fs::path root = tmp / "X-Plane 12";
    std::error_code ec;
    fs::create_directories(root / "Resources", ec);
    const fs::path nested = root / "Aircraft" / "My Hangar" / "Buses" / "renamed-a320";
    Write(nested / "skunkcrafts_updater.cfg",
          SkunkCfg("https://x/aircraft-repositories/A320/", "ToLiss A320", "1.3.2"));
    Write(nested / "objects" / "lights_out320_XP12.obj", "I\n800\nOBJ\n");

    const std::vector<detect::Aircraft> found = detect::DetectAircraft(U8(root));
    CHECK_EQ(found.size(), std::size_t(1));
    CHECK_EQ(found[0].airframe, std::string("a320"));
    CHECK_EQ(found[0].acVer, std::string("1.3.2"));
    // The label keeps the path relative to Aircraft/, so two same-named folders in
    // different hangars are still tellable apart in the list.
    CHECK(Contains(found[0].folder, "My Hangar"));
}

TEST("detect: the last-airframe check compares PATHS, not folder names") {
    // Nested layouts can hold same-named folders in different hangars; comparing on
    // the name would keep the shared plugin installed for an aircraft that is gone.
    TempDir tmp;
    const fs::path root = tmp / "X-Plane 12";
    std::error_code ec;
    fs::create_directories(root / "Resources", ec);
    const fs::path a = root / "Aircraft" / "HangarA" / "ToLissA320";
    const fs::path b = root / "Aircraft" / "HangarB" / "ToLissA320";
    for (const fs::path& p : {a, b}) {
        Write(p / "skunkcrafts_updater.cfg",
              SkunkCfg("https://x/aircraft-repositories/A320/", "ToLiss A320", "1.3.2"));
        Write(p / "objects" / "lights_out320_XP12.obj",
              std::string("I\n800\nOBJ\n\nPOINT_COUNTS\t0\t0\t0\t0\n") +
                  kMarkerPrefix + "0.8 wing: stock\n");
    }
    CHECK(detect::AnyOtherAirframeHasPhoton(U8(root), U8(a)));
    // Remove B's marker and A is the only one left carrying Photon.
    Write(b / "objects" / "lights_out320_XP12.obj", "I\n800\nOBJ\n");
    CHECK(!detect::AnyOtherAirframeHasPhoton(U8(root), U8(a)));
}

// ─────────────────────────────────────────────────────────────────────────────
// payload
// ─────────────────────────────────────────────────────────────────────────────
TEST("payload: `payload/` wins over `data/` beside the binary") {
    // ⚠ TWO NAMES, ONE BINARY: a checkout stages `payload/`, a shipped bundle ships
    // `data/`. The order matters — a repo checkout that has staged a payload must
    // never be shadowed by a stale `data/` left over from a bundle build.
    TempDir tmp;
    const fs::path dir = tmp / "beside";
    Write(dir / "payload" / "objs" / "stock" / "lights_out320_XP12.obj", "from payload");
    Write(dir / "data" / "objs" / "stock" / "lights_out320_XP12.obj", "from data");
    payload::InitFromExecutable(U8(dir / "photon-installer"));
    CHECK_EQ(payload::ObjText("stock", "a320"), std::string("from payload"));

    // With only `data/` present, that is what a download reads.
    std::error_code ec;
    fs::remove_all(dir / "payload", ec);
    payload::InitFromExecutable(U8(dir / "photon-installer"));
    CHECK(payload::Available());
    CHECK_EQ(payload::ObjText("stock", "a320"), std::string("from data"));
}

TEST("payload: the WING GATE fires before the bundle check") {
    // ⚠ Otherwise `a339 + durantula` reports "this build is incomplete" and sends
    // the user to re-download over a combination that does not exist and never will.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    CHECK_THROWS(payload::ObjText("durantula", "a339"), payload::PayloadError);
    // …and the supported combination still resolves out of the same bundle.
    CHECK(Contains(payload::ObjText("stock", "a339"), "payload obj a339 stock"));
}

TEST("payload: an EMPTY bundle dir still fails per-artifact, not up front") {
    // ⚠ `Available()` answers "is there a payload folder", NOT "is it complete".
    // Completeness is decided per artifact, at the point of use, so a bundle
    // missing only the interior can still do an exterior install rather than
    // refusing wholesale.
    TempDir tmp;
    const fs::path dir = tmp / "empty";
    std::error_code ec;
    fs::create_directories(dir / "payload", ec);
    payload::InitFromExecutable(U8(dir / "photon-installer"));
    CHECK(payload::Available());
    CHECK(!payload::InteriorAvailable());
    // ⚠ Per airframe since the a339 joined: its OBJ is a different file, so one
    // bool cannot answer for the fleet. Both are asked here because "missing"
    // must be per artifact — a bundle short of one airframe's screen-glow OBJ
    // still installs everything else.
    CHECK(!payload::ScreensAvailable("a320"));
    CHECK(!payload::ScreensAvailable("a339"));
    // ⚠ An airframe this feature does not cover reports NOT SUPPORTED rather than
    // "your download is incomplete" — the gate fires before the file check, the
    // same ordering ObjPath uses for wings and for the same reason: never send a
    // user to re-download over a combination that does not exist.
    CHECK_THROWS(payload::ScreensObjPath("a340"), payload::PayloadError);
    CHECK_THROWS(payload::ObjText("stock", "a320"), payload::PayloadError);
}

TEST("payload: the plugin walk skips the user-owned overlays folder") {
    // ⚠ --plugin-dir may legitimately point at a DEPLOYED plugin folder, which on a
    // dev machine holds that dev's own overlay images. A blind walk carries them
    // into every install.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    Write(pay.path() / "plugin" / kPluginFolder / "overlays" / "mine.png", "dev image");
    for (const auto& f : payload::PluginFiles()) {
        CHECK(!Contains(f.first, "overlays/"));
    }
}

TEST("payload: every interior texture must be present, a missing one is an error") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    CHECK(payload::InteriorAvailable());
    CHECK_EQ(payload::InteriorTextureFiles().size(), InteriorTextures().size());
    std::error_code ec;
    fs::remove(pay.path() / "textures" / "interior" / "knobs.png", ec);
    CHECK(!payload::InteriorAvailable());
}

// ─────────────────────────────────────────────────────────────────────────────
// install / uninstall — the exterior round trip
// ─────────────────────────────────────────────────────────────────────────────
TEST("install: writes the marker and backs up the original") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    const std::string original = Read(fa.objPath());

    const std::vector<std::string> steps = actions::Install(fa.Opts("stock"), gLog);
    CHECK(AnyStep(steps, "Backed up"));
    CHECK(AnyStep(steps, "Installed"));

    CHECK(Contains(Read(fa.objPath()),
                   std::string(kMarkerPrefix) + kPhotonVersion + " wing: stock"));
    CHECK_EQ(Read(fa.backupDir() / fa.objPath().filename()), original);
}

TEST("install: the manifest records version, wing and the backed-up OBJ") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    CHECK(!manifest::Read(U8(fa.objects())).present);

    actions::Install(fa.Opts("durantula"), gLog);
    const manifest::Manifest m = manifest::Read(U8(fa.objects()));
    CHECK(m.present);
    CHECK_EQ(m.version, std::string(kPhotonVersion));
    CHECK_EQ(m.wing, std::string("durantula"));
    CHECK(HasEntry(m.backedUp, U8(fa.objPath().filename())));
}

TEST("install: a reinstall NEVER clobbers the true original backup") {
    // ⚠ _BackupOnce. Overwriting would replace the pristine ToLiss file with a
    // Photon one, and the user's "restore original" would restore our own work.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    const std::string original = Read(fa.objPath());

    actions::Install(fa.Opts("stock"), gLog);
    CHECK(Read(fa.objPath()) != original);
    actions::Install(fa.Opts("durantula"), gLog);   // reinstall onto our own file

    CHECK_EQ(Read(fa.backupDir() / fa.objPath().filename()), original);
}

TEST("install: a target with NO stock original is ADDED, so uninstall deletes it") {
    // ⚠ THE THIRD BACKUP OUTCOME. `BackupOnce` has always had it — "nothing to
    // back up (no stock original)" — but it used to return the same false as
    // "already have one", so the file we then wrote landed in NEITHER manifest
    // list: not restorable, not deletable, ours in the user's aircraft forever.
    // Reachable whenever a target is absent — a damaged folder, or a ToLiss
    // update that drops a file we replace.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    std::error_code ec;
    fs::remove(fa.objPath(), ec);            // the stock exterior OBJ is not there

    actions::Install(fa.Opts("stock"), gLog);
    const manifest::Manifest m = manifest::Read(U8(fa.objects()));
    const std::string rel = U8(fa.objPath().filename());
    CHECK(HasEntry(m.added, rel));
    CHECK(!HasEntry(m.backedUp, rel));       // never both — restore would resurrect it
    CHECK(Exists(fa.objPath()));

    actions::Uninstall(fa.Opts("stock"), gLog);
    CHECK(!Exists(fa.objPath()));
}

TEST("install: a REINSTALL never enshrines OUR file as the stock original") {
    // ⚠ THE TRAP THE CASE ABOVE OPENS. Second time round the file exists — because
    // we wrote it — so a backup keyed only on "does the target exist" would copy
    // Photon's own OBJ into the backup dir and call it the original, defeating the
    // backup-once rule from the other side. It stays in added[], never in
    // backed_up[], and uninstall still removes it.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    std::error_code ec;
    fs::remove(fa.objPath(), ec);

    actions::Install(fa.Opts("stock"), gLog);
    actions::Install(fa.Opts("durantula"), gLog);

    const manifest::Manifest m = manifest::Read(U8(fa.objects()));
    const std::string rel = U8(fa.objPath().filename());
    CHECK(HasEntry(m.added, rel));
    CHECK(!HasEntry(m.backedUp, rel));
    CHECK(!Exists(fa.backupDir() / fa.objPath().filename()));

    actions::Uninstall(fa.Opts("durantula"), gLog);
    CHECK(!Exists(fa.objPath()));
}

TEST("uninstall: restores the original byte for byte and takes the backup dir") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    const std::string original = Read(fa.objPath());

    actions::Install(fa.Opts("stock"), gLog);
    actions::Uninstall(fa.Opts("stock"), gLog);

    CHECK_EQ(Read(fa.objPath()), original);
    CHECK(!Exists(fa.backupDir()));
}

TEST("uninstall: with nothing installed it refuses rather than half-running") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    CHECK_THROWS(actions::Uninstall(fa.Opts("stock"), gLog), actions::ActionError);
}

TEST("install: a dry run writes absolutely nothing") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    const std::string original = Read(fa.objPath());

    actions::Options o = fa.Opts("stock");
    o.dryRun = true;
    actions::Install(o, gLog);

    CHECK_EQ(Read(fa.objPath()), original);
    CHECK(!Exists(fa.backupDir()));
    CHECK(!manifest::Read(U8(fa.objects())).present);
    CHECK(!Exists(fa.pluginRoot()));
}

TEST("install: the plugin lands for EVERY arch, byte for byte") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    actions::Install(fa.Opts("stock"), gLog);

    for (const auto& row : kFakeXpl) {
        const fs::path xpl = fa.pluginRoot() / row[0] / kXplName;
        CHECK(Exists(xpl));
        CHECK_EQ(Read(xpl), std::string(row[1]));
    }
}

TEST("install: an unsupported wing fails loudly, never as a mislabeled stock OBJ") {
    // a339 has no wing mods. The UI never offers the combination, but calling in
    // directly must still refuse — otherwise a stock OBJ ships under the durantula
    // label, which is the exact failure make_release's SUPPORTED_WINGS gate exists
    // to prevent on the bundle side.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path(), "a339", "ToLissA339_V1p0p4");
    CHECK_THROWS(actions::Install(fa.Opts("durantula"), gLog), payload::PayloadError);
    // The refusal comes from payload resolution, which runs AFTER the original has
    // been backed up — so a backup folder may exist. What must not exist is a
    // marked OBJ or a manifest claiming the install happened.
    CHECK(!Contains(Read(fa.objPath()), kMarkerPrefix));
    CHECK(!manifest::Read(U8(fa.objects())).present);
}

// ─────────────────────────────────────────────────────────────────────────────
// the plugin: PluginIsCurrent and the identical-file write skip
// ─────────────────────────────────────────────────────────────────────────────
TEST("plugin: PluginIsCurrent is false when absent, true after an install") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    CHECK(!actions::PluginIsCurrent(U8(fa.root())));
    actions::Install(fa.Opts("stock"), gLog);
    CHECK(actions::PluginIsCurrent(U8(fa.root())));
}

TEST("plugin: one differing arch is enough to make it NOT current") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    actions::Install(fa.Opts("stock"), gLog);
    Write(fa.pluginRoot() / "win_x64" / kXplName, "# an older build\n");
    CHECK(!actions::PluginIsCurrent(U8(fa.root())));
}

TEST("plugin: a reinstall does NOT rewrite an identical .xpl") {
    // ⚠ X-Plane keeps a loaded `.xpl` memory-mapped and locked, so re-replacing an
    // unchanged one fails with "file in use" on a running sim for zero benefit —
    // and skipping it is exactly what lets an OBJ-only reinstall run with the sim
    // open, which the "please close X-Plane" screen relies on.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    actions::Install(fa.Opts("stock"), gLog);

    const fs::path xpl = fa.pluginRoot() / "win_x64" / kXplName;
    const auto before = fs::last_write_time(xpl);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const std::vector<std::string> steps = actions::Install(fa.Opts("stock"), gLog);
    CHECK(AnyStep(steps, "already current"));
    CHECK(fs::last_write_time(xpl) == before);
}

TEST("plugin: a DIFFERING .xpl is rewritten on reinstall") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    actions::Install(fa.Opts("stock"), gLog);
    const fs::path xpl = fa.pluginRoot() / "win_x64" / kXplName;
    Write(xpl, "# an older build\n");

    const std::vector<std::string> steps = actions::Install(fa.Opts("stock"), gLog);
    CHECK(AnyStep(steps, "Installed ToLiss Photon plugin"));
    CHECK_EQ(Read(xpl), std::string(kFakeXpl[0][1]));
}

// ─────────────────────────────────────────────────────────────────────────────
// the plugin folder's data files: panelfx.txt + overlays/
// ─────────────────────────────────────────────────────────────────────────────
TEST("plugindata: install writes panelfx.txt and the shipped overlays") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    actions::Install(fa.Opts("stock"), gLog);

    CHECK_EQ(Read(fa.pluginRoot() / kPanelFxFile), std::string("# panel fx stub\n"));
    CHECK_EQ(Read(fa.pluginRoot() / "overlays" / "fcu.png"),
             std::string("PNG overlay fcu"));
}

TEST("plugindata: a user's OWN overlay image survives an install and an uninstall") {
    // ⚠ BOTH DIRECTIONS OF THE RULE. Install writes only the names we ship, so a
    // hand-made image survives an upgrade; uninstall removes only those names, so
    // it can never rmtree a folder of PNGs that have no backup and no stock
    // counterpart.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    actions::Install(fa.Opts("stock"), gLog);

    const fs::path mine = fa.pluginRoot() / "overlays" / "my_own.png";
    Write(mine, "hand made");
    actions::Install(fa.Opts("stock"), gLog);
    CHECK_EQ(Read(mine), std::string("hand made"));

    actions::Options o = fa.Opts("stock");
    o.removePlugin = true;
    const std::vector<std::string> steps = actions::Uninstall(o, gLog);
    CHECK_EQ(Read(mine), std::string("hand made"));
    CHECK(AnyStep(steps, "Kept your own file"));
}

TEST("plugindata: with no user files the plugin folder goes entirely") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    actions::Install(fa.Opts("stock"), gLog);

    actions::Options o = fa.Opts("stock");
    o.removePlugin = true;
    actions::Uninstall(o, gLog);
    CHECK(!Exists(fa.pluginRoot()));
}

// ─────────────────────────────────────────────────────────────────────────────
// RealWings — the one patch that rewrites the USER'S OWN mod files in place
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// A fake RealWings mod folder with two baked wingtip lights, the shape the
// patcher rewrites.
fs::path MakeRealWings(const FakeAircraft& fa) {
    const fs::path modObj = fa.objects() / "RealWings320" / "MainNEO.obj";
    Write(modObj,
          "I\n800\nOBJ\n\n"
          "LIGHT_PARAM airplane_nav_left_size -5.1 0.5 -1.2 1 0 0 4 1000cd 0 0 -1 0.5\n"
          "LIGHT_PARAM airplane_strobe_size -5.2 0.5 -1.3 1 1 1 4 200000cd 0 0 -1 0.5\n");
    return modObj;
}

}  // namespace

TEST("realwings: install patches the mod OBJ and uninstall reverses it") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    const fs::path modObj = MakeRealWings(fa);
    const fs::path bak = fs::path(modObj).replace_extension(".obj.bak");

    actions::Install(fa.Opts("realwings"), gLog);
    CHECK(Contains(Read(modObj), patch_realwings::kMarker));
    CHECK(Exists(bak));
    CHECK(manifest::Read(U8(fa.objects())).realwingsPatched);

    actions::Uninstall(fa.Opts("realwings"), gLog);
    CHECK(!Contains(Read(modObj), patch_realwings::kMarker));
    CHECK(!Exists(bak));
}

TEST("realwings: switching to STOCK reverses the in-place mod patch") {
    // ⚠ Regression. Without it the baked wingtip lights double up with the stock
    // OBJ's own, and the patch leaks untracked past a later uninstall.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    const fs::path modObj = MakeRealWings(fa);
    const std::string originalMod = Read(modObj);

    actions::Install(fa.Opts("realwings"), gLog);
    CHECK(Contains(Read(modObj), patch_realwings::kMarker));

    const std::vector<std::string> steps = actions::Install(fa.Opts("stock"), gLog);
    CHECK(AnyStep(steps, "Reversed previous RealWings"));
    CHECK_EQ(Read(modObj), originalMod);
    CHECK(!Exists(fs::path(modObj).replace_extension(".obj.bak")));
    CHECK(!manifest::Read(U8(fa.objects())).realwingsPatched);
}

TEST("realwings: a DRY-RUN uninstall leaves the patch and the .bak alone") {
    // ⚠ Regression: the reverse once ignored the dry flag, so a dry-run uninstall
    // really did restore and delete the user's mod backups.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    const fs::path modObj = MakeRealWings(fa);

    actions::Install(fa.Opts("realwings"), gLog);
    const std::string patched = Read(modObj);
    const fs::path bak = fs::path(modObj).replace_extension(".obj.bak");
    CHECK(Exists(bak));

    actions::Options o = fa.Opts("realwings");
    o.dryRun = true;
    actions::Uninstall(o, gLog);
    CHECK_EQ(Read(modObj), patched);
    CHECK(Exists(bak));
}

TEST("realwings: a missing mod folder falls back to the base install") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    const std::vector<std::string> steps = actions::Install(fa.Opts("realwings"), gLog);
    CHECK(AnyStep(steps, "RealWings mod not found"));
    CHECK(Contains(Read(fa.objPath()), kMarkerPrefix));
}

// ─────────────────────────────────────────────────────────────────────────────
// the skin-glow redirect (a339)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

const char* kMeshObj =
    "I\n800\nOBJ\n\nPOINT_COUNTS 1 0 0 3\n"
    "\tATTR_light_level\t0\t1\tAirbusFBW/ExternalLightBrightnesses[0]\t5000\n"
    "\tATTR_light_level\t0\t1\tAirbusFBW/ExternalLightBrightnesses[12]\t5000\n"
    "\tATTR_light_level\t0\t1\tAirbusFBW/ExternalLightBrightnesses[13]\t5000\n";

}  // namespace

TEST("glow: install redirects only the driven regions and uninstall restores") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path(), "a339", "ToLissA339_V1p0p4");
    const fs::path mesh = fa.objects() / "WingL.obj";
    Write(mesh, kMeshObj);
    const std::string original = Read(mesh);

    const std::vector<std::string> steps = actions::Install(fa.Opts("stock"), gLog);
    CHECK(AnyStep(steps, "skin-glow"));

    const std::string patched = Read(mesh);
    CHECK(Contains(patched, std::string(kGlowNewDataRef) + "[12]"));
    CHECK(Contains(patched, std::string(kGlowNewDataRef) + "[13]"));
    // ⚠ An index Photon does NOT drive must keep reading ToLiss's array — ours
    // would report 0 there and that painted region would simply go dark.
    CHECK(Contains(patched, std::string(kGlowOldDataRef) + "[0]"));
    CHECK(!Contains(patched, std::string(kGlowNewDataRef) + "[0]"));

    CHECK(HasEntry(manifest::Read(U8(fa.objects())).backedUp, "WingL.obj"));
    CHECK_EQ(Read(fa.backupDir() / "WingL.obj"), original);

    actions::Uninstall(fa.Opts("stock"), gLog);
    CHECK_EQ(Read(mesh), original);
}

TEST("glow: a reinstall keeps the TRUE mesh backup") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path(), "a339", "ToLissA339_V1p0p4");
    const fs::path mesh = fa.objects() / "WingL.obj";
    Write(mesh, kMeshObj);
    const std::string original = Read(mesh);

    actions::Install(fa.Opts("stock"), gLog);
    actions::Install(fa.Opts("stock"), gLog);   // over the already-redirected mesh
    CHECK_EQ(Read(fa.backupDir() / "WingL.obj"), original);
}

// ─────────────────────────────────────────────────────────────────────────────
// the interior ("Gus Mod") — the opt-in axis
// ─────────────────────────────────────────────────────────────────────────────
TEST("interior: install writes the OBJ, the textures and the .acf spot patch") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    Write(fa.folder() / "ToLissA320.acf", StockAcf());

    actions::Options o = fa.Opts("stock");
    o.interior = true;
    actions::Install(o, gLog);

    CHECK(Contains(Read(fa.objects() / kInteriorObj), kInteriorObjNeedle));
    for (const std::string& t : InteriorTextures()) {
        CHECK(Exists(fa.objects() / t));
    }
    const manifest::Manifest m = manifest::Read(U8(fa.objects()));
    CHECK(m.interior.installed);
    CHECK(HasEntry(m.interior.acfPatched, "ToLissA320.acf"));

    // ⚠ THE SPOT SENTINEL, BOTH WAYS. The install DISABLES ToLiss's three sim
    // spots by naming them `none` (our OBJ provides its own), and the reversal
    // writes the stock DATAREF NAMES back rather than restoring a snapshot — so it
    // composes with whatever else has edited the file since. Asserting only one
    // direction is what makes the sentinel easy to read backwards.
    const std::string patched = Read(fa.folder() / "ToLissA320.acf");
    CHECK(Contains(patched, "_spot_name_3d/0 none"));
    CHECK(Contains(patched, "_spot_name_3d/1 none"));
    CHECK(Contains(patched, "_spot_name_3d/2 none"));
    CHECK(!Contains(patched, "ckpt/lights/map"));

    actions::Uninstall(o, gLog);
    const std::string reverted = Read(fa.folder() / "ToLissA320.acf");
    CHECK(Contains(reverted,
                   "_spot_name_3d/0 sim/cockpit2/electrical/panel_brightness_ratio[0]"));
    CHECK(Contains(reverted, "_spot_name_3d/2 ckpt/lights/map"));
    // ⚠ Slot 3 was `none` in the stock file and must STAY `none` — the patch owns
    // three spots, not four.
    CHECK(Contains(reverted, "_spot_name_3d/3 none"));
}

TEST("interior: the .acf is CRLF on disk and stays CRLF through both directions") {
    // ⚠ `.acf` files are CRLF, and a text-mode read that universal-newlines them
    // turns a three-line edit into a whole-file rewrite of a file other mods also
    // back up. This exact bug shipped in the Python installer twice (in the OBJ
    // reader and in two patchers) and was only ever caught by a BYTE comparison.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    const fs::path acf = fa.folder() / "ToLissA320.acf";
    Write(acf, StockAcf());

    actions::Options o = fa.Opts("stock");
    o.interior = true;
    actions::Install(o, gLog);
    const std::string patched = Read(acf);
    CHECK(!patched.empty());
    CHECK(patched.find("\r\n") != std::string::npos);
    CHECK(patched.find('\n') == patched.find("\r\n") + 1);   // no bare LF first

    actions::Uninstall(o, gLog);
    CHECK_EQ(Read(acf), StockAcf());   // byte-for-byte, terminators included
}

TEST("interior: the two ADDED textures are DELETED on uninstall, never restored") {
    // ⚠ pedals_details_{1,2}.png have no stock counterpart. Restoring a backup that
    // never existed leaves them in the aircraft folder forever.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    Write(fa.folder() / "ToLissA320.acf", StockAcf());

    actions::Options o = fa.Opts("stock");
    o.interior = true;
    actions::Install(o, gLog);

    const manifest::Manifest m = manifest::Read(U8(fa.objects()));
    for (const std::string& t : InteriorTextures()) {
        if (InteriorTextureIsAdded(t)) {
            CHECK(HasEntry(m.interior.added, t));
            CHECK(!HasEntry(m.backedUp, t));
        }
    }

    actions::Uninstall(o, gLog);
    for (const std::string& t : InteriorTextures()) {
        if (InteriorTextureIsAdded(t)) CHECK(!Exists(fa.objects() / t));
    }
}

TEST("interior: a stock texture we replaced comes back byte for byte") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    Write(fa.folder() / "ToLissA320.acf", StockAcf());
    const fs::path stockTex = fa.objects() / "chairs_LIT.png";
    Write(stockTex, "THE STOCK TOLISS TEXTURE");

    actions::Options o = fa.Opts("stock");
    o.interior = true;
    actions::Install(o, gLog);
    CHECK(Read(stockTex) != std::string("THE STOCK TOLISS TEXTURE"));

    actions::Uninstall(o, gLog);
    CHECK_EQ(Read(stockTex), std::string("THE STOCK TOLISS TEXTURE"));
}

TEST("interior: a texture with no stock file to replace is DELETED, not orphaned") {
    // The same third outcome on the interior axis, and the likelier way to meet it:
    // nine of the eleven textures replace a ToLiss file, so an aircraft missing one
    // (or a future ToLiss that stops shipping it) used to keep our copy forever.
    // ⚠ The two genuinely-added ones must stay a SEPARATE story — they reach
    // interior.added without consulting the disk at all.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    Write(fa.folder() / "ToLissA320.acf", StockAcf());
    const fs::path present = fa.objects() / "chairs_LIT.png";
    Write(present, "THE STOCK TOLISS TEXTURE");     // this one HAS a stock original
    const fs::path absent = fa.objects() / "knobs_LIT.png";   // this one does not

    actions::Options o = fa.Opts("stock");
    o.interior = true;
    actions::Install(o, gLog);

    const manifest::Manifest m = manifest::Read(U8(fa.objects()));
    CHECK(HasEntry(m.backedUp, "chairs_LIT.png"));
    CHECK(!HasEntry(m.interior.added, "chairs_LIT.png"));
    CHECK(HasEntry(m.interior.added, "knobs_LIT.png"));
    CHECK(!HasEntry(m.backedUp, "knobs_LIT.png"));

    actions::Uninstall(o, gLog);
    CHECK_EQ(Read(present), std::string("THE STOCK TOLISS TEXTURE"));   // restored
    CHECK(!Exists(absent));                                            // deleted
}

TEST("interior: a livery .dds that SHADOWS our texture is moved aside and put back") {
    // ⚠ X-Plane substitutes a `.dds` for the `.png` an OBJ names, so a livery
    // shipping chairs_LIT.dds wins over ours and the whole mod is invisible in that
    // livery. The fix is to back the livery file up and REMOVE it.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    Write(fa.folder() / "ToLissA320.acf", StockAcf());
    const fs::path liveryTex =
        fa.folder() / "liveries" / "Some Airline" / "objects" / "chairs_LIT.dds";
    Write(liveryTex, "LIVERY OVERRIDE");

    actions::Options o = fa.Opts("stock");
    o.interior = true;
    actions::Install(o, gLog);
    CHECK(!Exists(liveryTex));
    CHECK(!manifest::Read(U8(fa.objects())).interior.liveriesPatched.empty());

    actions::Uninstall(o, gLog);
    CHECK_EQ(Read(liveryTex), std::string("LIVERY OVERRIDE"));
}

TEST("interior: an airframe without it is refused, not silently skipped") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path(), "a339", "ToLissA339_V1p0p4");
    actions::Options o = fa.Opts("stock");
    o.interior = true;
    const std::vector<std::string> steps = actions::Install(o, gLog);
    CHECK(AnyStep(steps, "Interior lighting not available"));
    CHECK(!Exists(fa.objects() / kInteriorObj));
}

// ─────────────────────────────────────────────────────────────────────────────
// the display-glow OBJ — added[], never backed_up[]
// ─────────────────────────────────────────────────────────────────────────────
TEST("screens: the OBJ is installed and ATTACHED to the .acf on a stock A3xx") {
    // Part of the BASE install, not an opt-in: it works on a stock cockpit.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    Write(fa.folder() / "ToLissA320.acf", StockAcf());

    const std::vector<std::string> steps = actions::Install(fa.Opts("stock"), gLog);
    CHECK(AnyStep(steps, "display glow"));
    CHECK(Exists(fa.objects() / kScreensObj));

    const manifest::Manifest m = manifest::Read(U8(fa.objects()));
    CHECK(m.screens.installed);
    // ⚠ added[], NEVER backed_up[] — there is no stock counterpart to restore.
    CHECK(HasEntry(m.screens.added, kScreensObj));
    CHECK(!HasEntry(m.backedUp, kScreensObj));
    CHECK(HasEntry(m.screens.acfPatched, "ToLissA320.acf"));
    CHECK(Contains(Read(fa.folder() / "ToLissA320.acf"), kScreensObj));
}

TEST("screens: uninstall DETACHES the .acf and DELETES the OBJ") {
    // ⚠ Detach first. An `.acf` naming an OBJ that is gone is a missing-object
    // warning on every load; the reverse order leaves an attached-but-unused row
    // nobody notices.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    const fs::path acf = fa.folder() / "ToLissA320.acf";
    Write(acf, StockAcf());

    actions::Install(fa.Opts("stock"), gLog);
    actions::Uninstall(fa.Opts("stock"), gLog);

    CHECK(!Exists(fa.objects() / kScreensObj));
    CHECK(!Contains(Read(acf), kScreensObj));
}

TEST("screens: a DRY RUN of the .acf attachment writes nothing") {
    // Ported from tests/test_screens.py's AcfDryRunTests, which drove the Python
    // patcher. Attaching and detaching are covered as text transforms in
    // core_tests.cpp; this is the one case about the FILE.
    TempDir tmp;
    const fs::path acf = tmp.path() / "aircraft" / "a320.acf";
    Write(acf, StockAcf());
    const std::string original = Read(acf);

    const patch_acf_screens::RunResult res =
        patch_acf_screens::Run(U8(acf.parent_path()), /*reverse=*/false,
                               /*dryRun=*/true);
    CHECK_EQ(res.touched.size(), std::size_t(1));   // it reports what it WOULD do
    CHECK_EQ(Read(acf), original);                  // and does none of it
}

// ⚠ THE FAILURE THIS CATCHES DRAWS PERFECTLY. Every airframe writes the same
// FILENAME into its own objects/ folder, so installing the A320's six positions
// onto an A330-900 leaves a file that is present, correctly attached, correctly
// listed in the manifest, and lighting six places nobody is sitting. Nothing short
// of reading the OBJ's contents can tell the two apart, which is why the payload
// stubs name their airframe and why this asserts on that name.
TEST("screens: the a339 gets ITS OWN OBJ, not the A3xx one") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path(), "a339", "ToLissA339_V1p0p4");
    // Its cockpit-lighting row is the frame ours is copied from — there is no
    // lights_inn.obj on this airframe, which is what the template LIST is for.
    Write(fa.folder() / "A330-900.acf",
          std::string("I\r\nACF\r\n"
                      "P _obja/0/_v10_att_file_stl Fuselage_Fwd.obj\r\n"
                      "P _obja/0/_v10_att_y_acf_prt_ref -3.000000000\r\n"
                      "P _obja/1/_obj_hide_dataref AirbusFBW/ObjectKill\r\n"
                      "P _obja/1/_v10_att_file_stl CockpitLighting_XP12.obj\r\n"
                      "P _obja/1/_v10_att_y_acf_prt_ref -3.000000000\r\n"
                      "P _obja/count 2\r\n"));

    const std::vector<std::string> steps = actions::Install(fa.Opts("stock"), gLog);
    CHECK(AnyStep(steps, "display glow"));
    CHECK(Exists(fa.objects() / kScreensObj));
    CHECK(Contains(Read(fa.objects() / kScreensObj), "# screens a339"));
    CHECK(!Contains(Read(fa.objects() / kScreensObj), "# screens a320"));

    const manifest::Manifest m = manifest::Read(U8(fa.objects()));
    CHECK(m.screens.installed);
    CHECK(HasEntry(m.screens.added, kScreensObj));      // added[], never backed_up[]
    CHECK(!HasEntry(m.backedUp, kScreensObj));
    CHECK(HasEntry(m.screens.acfPatched, "A330-900.acf"));

    // ...and it comes back out again, both halves.
    actions::Options o = fa.Opts("stock");
    actions::Uninstall(o, gLog);
    CHECK(!Exists(fa.objects() / kScreensObj));
    CHECK(!patch_acf_screens::IsAttached(Read(fa.folder() / "A330-900.acf")));
}

// ─────────────────────────────────────────────────────────────────────────────
// the manifest is the ONLY thing uninstall works from
// ─────────────────────────────────────────────────────────────────────────────
TEST("manifest: a key this build does not model survives an install round trip") {
    // ⚠ A user who installed with a newer build and uninstalls with an older one
    // must not have that build's bookkeeping silently dropped.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    actions::Install(fa.Opts("stock"), gLog);

    // ⚠ The manifest lives INSIDE the backup folder, beside the files it accounts
    // for — so a user who deletes "Photon Backup Files" takes the bookkeeping with
    // the backups rather than leaving a manifest pointing at nothing.
    const fs::path mf = fs::path(fsutil::PathFromUtf8(manifest::PathFor(U8(fa.objects()))));
    std::string json = Read(mf);
    const std::size_t brace = json.find('{');
    json.insert(brace + 1, "\"future_axis\": {\"installed\": true}, ");
    Write(mf, json);

    actions::Install(fa.Opts("stock"), gLog);
    CHECK(Contains(Read(mf), "future_axis"));
}


// --- progress ---------------------------------------------------------------
// The cost model and the measurements behind it live in core/progress.h.

TEST("progress: an ordinary install reports from its first phase, not its last") {
    // The regression this whole module exists for. Before, the only call site was
    // inside the interior texture loop, so this exact run - the common one, and the
    // only one available on the A330-900 - reported zero times.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    ProgressLog prog;
    actions::Options o = fa.Opts("stock");
    prog.AttachTo(o);

    actions::Install(o, gLog);

    CHECK(!prog.fractions.empty());
    CHECK(prog.SawStep("Plugin"));
    CHECK(prog.SawStep("Exterior lights"));
    // It must MOVE, not merely fire: a run that reports 0.0 every time is the old
    // behavior wearing a callback.
    CHECK(prog.Last() > 0.0);
}

TEST("progress: the fraction never goes backwards") {
    // A bar that retreats reads as a bug in whatever step was running. `Plan` makes
    // this structural - `Within` only moves the CURRENT step and takes a max - but
    // the property is the point, so it is checked over a real run.
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    ProgressLog prog;
    actions::Options o = fa.Opts("stock");
    o.interior = true;
    prog.AttachTo(o);

    actions::Install(o, gLog);

    CHECK(prog.fractions.size() > 1);
    for (std::size_t i = 1; i < prog.fractions.size(); ++i) {
        CHECK(prog.fractions[i] >= prog.fractions[i - 1]);
    }
    for (double f : prog.fractions) {
        CHECK(f >= 0.0);
        CHECK(f <= 1.0);
    }
}

TEST("progress: every phase the plan priced is one the run enters") {
    // HALF THE TYPO GUARD, by arithmetic: a step that is priced but never entered is
    // never banked, while still sitting in the denominator, so the run ends short of
    // 1.0. Renaming one `Begin` and not its `Add` fails here.
    //
    // The OTHER half - a step the run enters that the plan never priced - is NOT
    // visible from the fraction and has its own test below. It is missing from the
    // denominator too, so the arithmetic still lands on exactly 1.000. That is not a
    // guess: this test was written believing it covered both, and it passed against
    // a mutation that deleted the glow step's `plan.Add`.
    //
    // A phase that forgets its `Finish` is NOT a bug and is not tested for -
    // `Plan::Begin` banks the step it is leaving, so only the last step in a run
    // needs its own `Finish`, and it has one.
    for (bool interior : {false, true}) {
        TempDir tmp;
        FakePayload pay(tmp / "payload");
        FakeAircraft fa(tmp.path());
        ProgressLog prog;
        actions::Options o = fa.Opts("stock");
        o.interior = interior;
        prog.AttachTo(o);

        actions::Install(o, gLog);
        CHECK(prog.Last() > 0.999);
    }
}

TEST("progress: no phase enters a step the plan never priced") {
    // THE OTHER HALF, and it needs a hook because the numbers cannot show it - see
    // progress.h. Every branch of a real run is walked with the hook armed; any call
    // at all is a phase whose `Begin` has no matching `Add`, which in production is
    // a silent dead spot in the bar.
    std::vector<std::string> unknown;
    progress::SetUnknownStepHook(
        [&unknown](const std::string& n) { unknown.push_back(n); });

    {
        TempDir tmp;
        FakePayload pay(tmp / "payload");
        // Every airframe, both wing branches that exist, cockpit mod on and off, and
        // the uninstall - the steps differ per branch and an unpriced one only shows
        // up on the branch that enters it.
        for (const Airframe& af : Airframes()) {
            for (const std::string& wing : WingsFor(af.key)) {
                for (bool interior : {false, true}) {
                    FakeAircraft fa(tmp.path(), af.key,
                                    std::string("ToLiss_") + af.key + "_" + wing +
                                        (interior ? "_int" : ""));
                    actions::Options o = fa.Opts(wing);
                    o.interior = interior;
                    o.progress = &ProgressLog::Callback;
                    ProgressLog sink;
                    o.progressUserData = &sink;
                    actions::Install(o, gLog);
                    actions::Uninstall(o, gLog);
                }
            }
        }
    }

    progress::SetUnknownStepHook(nullptr);
    for (const std::string& n : unknown) {
        std::cout << "    unpriced step entered: " << n << std::endl;
    }
    CHECK(unknown.empty());
}

TEST("progress: an uninstall is planned too, and also completes") {
    TempDir tmp;
    FakePayload pay(tmp / "payload");
    FakeAircraft fa(tmp.path());
    actions::Options in = fa.Opts("stock");
    in.interior = true;
    actions::Install(in, gLog);

    ProgressLog prog;
    actions::Options out = fa.Opts("stock");
    prog.AttachTo(out);
    actions::Uninstall(out, gLog);

    CHECK(!prog.fractions.empty());
    CHECK(prog.SawStep("Restoring originals"));
    CHECK(prog.Last() > 0.999);
}

TEST("progress: only the airframe with a glow map is charged for the scan") {
    // THE REASON THE PLAN IS PER RUN. Reading every `.obj` in objects/ is ~97% of an
    // A330-900 install and 0% of an A320's, because `GlowRedirect()` has one entry.
    // A shared weight table would be off by a factor of twenty on one of them
    // whichever way it was tuned.
    TempDir tmp;
    FakePayload pay(tmp / "payload");

    FakeAircraft a320(tmp.path(), "a320", "ToLissA320_V1p3p2");
    ProgressLog p320;
    actions::Options o320 = a320.Opts("stock");
    p320.AttachTo(o320);
    actions::Install(o320, gLog);
    CHECK(!p320.SawStep("Skin glow"));

    FakeAircraft a339(tmp.path(), "a339", "ToLissA339_V1p0p4");
    ProgressLog p339;
    actions::Options o339 = a339.Opts("stock");
    p339.AttachTo(o339);
    actions::Install(o339, gLog);
    CHECK(p339.SawStep("Skin glow"));
    CHECK(p339.Last() > 0.999);
}

TEST("progress: a plan with nothing in it reports 0, never 1") {
    // An empty plan means "nothing is known", not "everything is done". Reporting
    // 1.0 there would put a full bar on screen before the work started - the same
    // lie as the empty bar it replaced, told the other way round.
    progress::Plan empty;
    CHECK(empty.Empty());
    CHECK_EQ(empty.Fraction(), 0.0);

    progress::Plan zero;
    zero.Add("nothing", 0.0);
    CHECK_EQ(zero.Fraction(), 0.0);
}

TEST("progress: a step's fraction is clamped and can only advance") {
    progress::Plan plan;
    plan.Add("a", 100.0);
    plan.Add("b", 100.0);

    plan.Begin("a");
    plan.Within(0.5);
    CHECK_EQ(plan.Fraction(), 0.25);
    // Backwards within a step is ignored - a patcher reporting its files out of
    // order must not walk the bar back.
    plan.Within(0.1);
    CHECK_EQ(plan.Fraction(), 0.25);
    // Out of range is clamped, not wrapped or trusted.
    plan.Within(9.0);
    CHECK_EQ(plan.Fraction(), 0.5);
    plan.Finish();
    CHECK_EQ(plan.Fraction(), 0.5);

    plan.Begin("b");
    plan.Within(1.0);
    plan.Finish();
    CHECK_EQ(plan.Fraction(), 1.0);
}

TEST("progress: an unknown step name cannot corrupt the fraction") {
    // Belt and braces for the guard above: a mistyped name loses its own progress
    // and nothing else. It must never bank another step's estimate or throw.
    progress::Plan plan;
    plan.Add("real", 100.0);
    plan.Begin("typo");
    plan.Within(1.0);
    plan.Finish();
    CHECK_EQ(plan.Fraction(), 0.0);
    plan.Begin("real");
    plan.Finish();
    CHECK_EQ(plan.Fraction(), 1.0);
}

TEST("progress: entering a step banks the one before it") {
    // Callers that walk Begin -> Begin without a Finish still get a monotonic bar.
    progress::Plan plan;
    plan.Add("a", 50.0);
    plan.Add("b", 50.0);
    plan.Begin("a");
    plan.Begin("b");
    CHECK_EQ(plan.Fraction(), 0.5);
}

TEST("progress: the plan is built from sizes and never reads the files") {
    // THE PROPERTY THAT MAKES PLANNING FREE. `BytesOfFilesWithExt` prices a 1.6 GB
    // folder without opening it; if it ever grew a read, planning would cost exactly
    // as much as the phase it is predicting.
    TempDir tmp;
    const fs::path d = tmp / "objs";
    Write(d / "one.obj", std::string(1000, 'x'));
    Write(d / "two.OBJ", std::string(2000, 'y'));
    Write(d / "skip.txt", std::string(4000, 'z'));
    Write(d / "nested" / "deep.obj", std::string(8000, 'w'));

    CHECK_EQ(progress::BytesOfFilesWithExt(U8(d), ".obj"), 3000u);
    // Recursion is opt-in and must match the phase being priced: patch_glow walks
    // one level, patch_realwings recurses.
    CHECK_EQ(progress::BytesOfFilesWithExt(U8(d), ".obj", true), 11000u);
    CHECK_EQ(progress::FileSizeOrZero(U8(d / "one.obj")), 1000u);
    CHECK_EQ(progress::FileSizeOrZero(U8(d / "nope.obj")), 0u);
}
