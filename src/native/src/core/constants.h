// Facts shared by the plugin, the installer, and the tests — the drift-killer.
//
// ⚠ THIS HEADER EXISTS BECAUSE THE SAME FACT KEPT BEING WRITTEN TWICE. The plugin
// and the installer each carried their own copy of the version (pinned only by a
// test, after they drifted for a whole feature), of the interior-OBJ needle
// (`kInteriorObjNeedle` in plugin.cpp vs `constants.INTERIOR_OBJ` in Python vs the
// gate names in the DSL — three files that must agree), and of the skin-glow index
// map (`GlowMapForIcao` vs `patch_glow.REDIRECT` vs `GLOW_AIRFRAMES`, where a
// mismatch makes redirected regions go DARK). One definition, three consumers.
//
// ⚠ NOTHING HERE MAY INCLUDE AN XPLM HEADER OR LINK THE SDK. `photoncore` is
// built on machines and CI runners with no X-Plane SDK, and that independence is
// what keeps the release matrix simple. Anything needing XPLM stays in plugin.cpp
// and calls INTO core, never the reverse.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/version.h"

namespace photon {

// ─── airframes ───────────────────────────────────────────────────────────────
// One row per supported ToLiss airframe.
//
// ⚠ `objName` is an airframe FINGERPRINT — detection falls back to it when the
// SkunkCrafts cfg is absent or unrecognized. NEVER put `lights_inn.obj` in this
// slot: the interior OBJ is byte-identical across A319/A320/A321, so it would make
// every A3xx match every other one.
//
// `folderGlob` is a LAST-RESORT identifier only. Detection primarily reads the
// SkunkCrafts cfg's contents (`cfgModule` / `cfgName` below), so a ToLiss install
// is found regardless of its folder name or where it sits under Aircraft/.
struct Airframe {
    const char* key;         // "a319"
    const char* folderGlob;  // "ToLissA319*"      — last-resort name match
    const char* treeFolder;  // "A319"
    const char* objName;     // "lights_out319_XP12.obj"  — the fingerprint
    const char* prettyName;  // fallback when skunkcrafts_updater.cfg is missing
    // How to recognize this airframe from its skunkcrafts_updater.cfg. Matched
    // case-insensitively as substrings, `cfgModule` first (the ToLiss update-repo
    // URL — the most stable machine id), then the display `cfgName`. The tokens
    // are mutually exclusive, so order between airframes is irrelevant.
    const char* cfgModule;   // "/A319/"
    const char* cfgName;     // "A319"
};

const std::vector<Airframe>& Airframes();
const Airframe* AirframeByKey(const std::string& key);

// ─── interior ("Gus Mod") ────────────────────────────────────────────────────
// A SEPARATE, OPT-IN install axis from the exterior one.
constexpr char kInteriorObj[] = "lights_inn.obj";

// ⚠ THE COCKPIT-SUBMENU SENTINEL, and the reason this string is here rather than
// in plugin.cpp. Three files must agree on it: the DSL's gate names, the plugin's
// detection scan, and the installer's. A `--target interior --write` build carries
// no installer version marker and is every bit as switchable as a released one, so
// the OBJ BINDING is the test — never the marker.
constexpr char kInteriorObjNeedle[] = "ToLissPhoton/interior/";

// The A330-900 is out of scope entirely (docs/interior_plan.md decision #14):
// different cockpit model, different rheostat indices, and Gus's mod does not
// cover it.
const std::vector<std::string>& InteriorAirframes();
bool AirframeHasInterior(const std::string& key);

// Gus's canonical texture set, installed into <aircraft>/objects/.
const std::vector<std::string>& InteriorTextures();
// ⚠ These two have NO stock counterpart, so uninstall must DELETE them rather
// than restore a backup that never existed — which would leave them in the
// aircraft folder forever.
bool InteriorTextureIsAdded(const std::string& name);

// A livery's own file SHADOWS ours: X-Plane substitutes a .dds for the .png an OBJ
// names, so a livery shipping chairs_LIT.dds wins over our chairs_LIT.png and the
// mod is invisible in that livery. Matched by stem against InteriorTextures().
const std::vector<std::string>& InteriorLiveryExts();

// The four .acf files per airframe that carry the cockpit spot block. All four
// hold identical spot values, so this is one constant patch applied four times.
// Photon's OBJ work is XP12-only but this patch is not: lights_inn.obj is shared
// by every variant, so an XP11 user would otherwise get patched OBJ lights
// alongside unpatched sim spots.
const std::vector<std::string>& AcfSuffixes();

// ─── display (screen) glow ───────────────────────────────────────────────────
// ⚠ Unlike every other OBJ we install, this one has NO stock counterpart and is
// NOT referenced by a stock ToLiss aircraft. Nothing draws it until the `.acf`
// `_obja/*` attachment table names it — which is what makes the install TWO steps
// that must both be undone, and why the OBJ goes in the manifest's `screens.added[]`
// and never in `backed_up[]`.
//
// Part of the BASE install on A3xx, not an opt-in: it works on a stock cockpit, so
// it does not belong behind the interior's opt-in, and whether the glow is visible
// is a runtime choice on the plugin's Displays tab.
constexpr char kScreensObj[] = "lights_screens.obj";
const std::vector<std::string>& ScreensAirframes();
bool AirframeHasScreens(const std::string& key);

// ─── skin glow ───────────────────────────────────────────────────────────────
// Airframes whose install repoints ToLiss's skin-glow LIT-texture regions to
// Photon's own dataref.
//
// ⚠ THREE PLACES MUST AGREE OR REDIRECTED REGIONS GO DARK: `GlowRedirect` below,
// `GlowMapForIcao` in plugin.cpp, and the installer's gate. A redirected index
// Photon does not drive reads 0 from our array, i.e. that painted region simply
// stops lighting up. Putting the table HERE is what makes the agreement automatic
// rather than a rule someone has to remember — plugin.cpp reads this vector.
struct GlowMapEntry {
    const char* airframeKey;
    std::vector<int> indices;
};
const std::vector<GlowMapEntry>& GlowRedirect();
const std::vector<int>* GlowIndicesFor(const std::string& airframeKey);

constexpr char kGlowOldDataRef[] = "AirbusFBW/ExternalLightBrightnesses";
constexpr char kGlowNewDataRef[] = "ToLissPhoton/exterior/glow";

// ─── wing-mod variants ───────────────────────────────────────────────────────
// ⚠ `WingsFor` is checked BEFORE any payload resolution. Without it, an airframe
// with no wing mods (a339) would still resolve a "durantula"/"realwings" variant —
// the DSL's mount lookup falls back to whatever mount exists — and ship a stock OBJ
// mislabeled as a mod build.
const std::vector<std::string>& Wings();
const std::vector<std::string>& WingsFor(const std::string& airframeKey);
bool WingIsOfferedFor(const std::string& airframeKey, const std::string& wing);
std::string WingLabel(const std::string& wing);
std::string WingActionLabel(const std::string& wing);

// airframe key -> RealWings mod subfolder under the aircraft's objects/. Empty
// means the airframe has no wing mods at all (a339).
std::string RealWingsDir(const std::string& airframeKey);

// ─── bookkeeping ─────────────────────────────────────────────────────────────
constexpr char kBackupDirName[] = "Photon Backup Files";
constexpr char kManifestName[] = "photon_manifest.json";

// ─── the plugin folder ───────────────────────────────────────────────────────
// A compiled fat-plugin folder holding one .xpl per platform:
//   Resources/plugins/ToLissPhoton/<arch>/ToLissPhoton.xpl
// X-Plane loads the subfolder matching the host OS. No XPPython3/Python needed.
constexpr char kPluginFolder[] = "ToLissPhoton";
constexpr char kXplName[] = "ToLissPhoton.xpl";

// Subfolders of the plugin folder that hold USER content as well as ours.
//
// ⚠ BOTH DIRECTIONS OF THIS RULE MATTER. Install writes only the names we ship, so
// a user's own images survive an upgrade; uninstall removes only the names we ship,
// so it can never rmtree a folder of hand-made PNGs that have no backup and no
// stock counterpart. And a source folder that happens to be a DEPLOYED plugin
// folder must not carry a dev machine's overlays into every install.
const std::vector<std::string>& PluginUserDirs();
bool PluginPathIsUserOwned(const std::string& relPosix);

// ⚠ A SHIPPING .xpl NEVER WRITES panelfx.txt (SavePanelFx is dev-only). User
// choices go in ToLissPhoton_profiles.json instead. Otherwise an installer upgrade
// clobbers a user's settings — and on a dev machine, where this file is a SYMLINK
// into the repo, a user-facing toggle writes itself into source control.
//
// ⚠ INSTALL AND UNINSTALL BOTH SKIP A SYMLINK AT THIS PATH, checked on the path
// itself and never after following it.
constexpr char kPanelFxFile[] = "panelfx.txt";
constexpr char kPluginDataDirName[] = "plugindata";

// ─── the OBJ version marker ──────────────────────────────────────────────────
// `# ToLissPhoton version: X wing: Y`, injected after the OBJ's POINT_COUNTS line.
// Read back both as the version source when no manifest is present AND to
// CORROBORATE a manifest: manifest intact but marker gone means an aircraft update
// reverted the OBJ, i.e. the install is stale and needs redoing.
constexpr char kMarkerPrefix[] = "# ToLissPhoton version: ";

// ─── links ───────────────────────────────────────────────────────────────────
constexpr char kXplaneOrgUrl[] =
    "https://forums.x-plane.org/files/file/"
    "100717-toliss-photon-exterior-lighting-mod-for-toliss-a319a320a321";
constexpr char kGithubUrl[] = "https://github.com/ischmal/toliss-photon-lighting";

// Gus Rodrigues' Light Mod on the .org — a SEPARATE file entry per variant, so the
// opt-in screen picks by airframe rather than reusing Photon's own page.
std::string InteriorOrgUrl(const std::string& airframeKey);

}  // namespace photon
