// Locate the installable artifacts — the OBJs, the textures, the plugin, the
// plugin data, and the precomputed RealWings patch parameters.
//
// **ONE MODE: bundled.** The `payload/` directory sits beside the installer
// binary, resolved relative to the EXECUTABLE'S OWN PATH and never the working
// directory, so the installer runs from anywhere (Downloads, a USB stick, or from
// inside X-Plane/Resources/plugins/).
//
// ⚠ THERE IS NO DEV-TREE FALLBACK, AND ADDING ONE WOULD UNDO THE PORT. The Python
// installer used to build each OBJ on the fly from the DSL when run out of a repo
// checkout. That made the DSL toolchain an INSTALL-TIME dependency — the one thing
// a compiled installer cannot honor — and it meant two code paths that could drift
// with only a shipped bundle to notice on. The dev path is an explicit staging step:
//
//     python build/make_release.py --payload-only
//
// Layout:
//     payload/objs/<stock|durantula|realwings>/<obj filename>
//     payload/objs/interior/lights_inn.obj
//     payload/objs/screens/lights_screens.obj
//     payload/textures/interior/*.png
//     payload/plugin/ToLissPhoton/<arch>/ToLissPhoton.xpl
//     payload/plugindata/{panelfx.txt,overlays/*.png}
//     payload/realwings_patch.json
#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/patch_realwings.h"

namespace photon {
namespace payload {

// The artifacts could not be located — a broken or incomplete bundle, not a
// user-fixable condition.
class PayloadError : public std::runtime_error {
public:
    explicit PayloadError(const std::string& what) : std::runtime_error(what) {}
};

// Where the bundle lives. Defaults to `<dir of argv[0]>/payload`; `SetPayloadDir`
// exists so tests can point it at a fixture.
void InitFromExecutable(const std::string& exePathUtf8);
void SetPayloadDir(const std::string& dirUtf8);
std::string PayloadDir();

bool Available();

// ─── OBJs ────────────────────────────────────────────────────────────────────
// ⚠ THE WING GATE FIRES BEFORE THE BUNDLE CHECK. Otherwise `a339 + durantula`
// reports "this build is incomplete" and sends the user to re-download over a
// combination that does not exist and never will.
std::string ObjPath(const std::string& wing, const std::string& airframeKey);
std::string ObjText(const std::string& wing, const std::string& airframeKey);

std::string InteriorObjPath();
std::string InteriorObjText();

// ⚠ Must never be a `--debug` build: that variant is a tuning artifact bound to
// `ToLissPhoton/debug/light/*`, is not installable, and would leave six lights
// ignoring the DU knobs. `make_release` emits it straight from the Emitter for
// exactly that reason rather than copying `dist/`.
std::string ScreensObjPath();
std::string ScreensObjText();
bool ScreensAvailable();

// ─── interior textures ───────────────────────────────────────────────────────
// (filename, absolute source), ordered by name so install progress is stable run
// to run.
//
// ⚠ A MISSING FILE IS AN ERROR, NOT A SKIP. Shipping 9 of 11 leaves the cockpit
// half-retextured, which reads in-sim as the mod being broken rather than as an
// incomplete download.
std::vector<std::pair<std::string, std::string>> InteriorTextureFiles();
bool InteriorAvailable();

// ─── the RealWings patch data ────────────────────────────────────────────────
std::string RealWingsPatchPath();
patch_realwings::PatchData RealWingsPatchData();
// A bundle built before the DSL decoupling has no such file; the install then
// continues without the wingtip patch rather than refusing to run.
bool RealWingsAvailable();

// ─── the plugin ──────────────────────────────────────────────────────────────
std::string PluginSrcRoot();

// Every file in the fat-plugin folder as (folder-relative POSIX path, absolute
// source).
//
// ⚠ A WHOLE-TREE WALK MINUS `PluginUserDirs()`. The source folder may legitimately
// be a DEPLOYED plugin folder — that is how a merged multi-arch bundle gets
// assembled — and on a dev machine such a folder holds that dev's own overlay
// images. Walking blind would carry them into every install.
std::vector<std::pair<std::string, std::string>> PluginFiles();

// The runtime DATA files that go into the plugin folder alongside the `.xpl`.
//
// ⚠ Enumerated from a FIXED SET OF ROOTS, not a blind walk: the roots are what
// `make_release.stage_plugin_data` writes, and keeping the two the same shape is
// what stops a stray file in `plugindata/` becoming an installed artifact.
std::vector<std::pair<std::string, std::string>> PluginDataFiles();

// Platform arch subfolders that actually contain a `.xpl`.
std::vector<std::string> PluginArches();

}  // namespace payload
}  // namespace photon
