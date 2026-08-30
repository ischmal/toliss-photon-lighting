// Integral lighting as a SWITCHABLE cockpit look — the OBJ half.
//
// `core/lit_recolor` derives the two eras of `text_LIT.png` / `knobs_LIT.png`.
// This module is what makes the choice between them live in the sim rather than
// in the installer: for each OBJ that binds one of those textures it writes a
// SECOND copy bound to the other era's file, and gates both on
// `ToLissPhoton/interior/integral`.
//
// ⚠ WHY GEOMETRY IS DUPLICATED TO SWITCH A TEXTURE. OBJ8 binds `TEXTURE_LIT`
// once, at load, and there is no dataref form of it — X-Plane has no runtime
// texture swap at all. The two ways to change what a cockpit's emissive layer
// looks like are therefore (a) rewrite the PNG and reload, or (b) ship both PNGs
// and pick the geometry that names one. (a) is what the cockpit BRIGHTNESS
// slider does (`core/patch_intensity`), and it costs a sim restart, because the
// one command that would re-read the file — `sim/operation/reload_aircraft` — is
// banned outright here after it took the simulator down twice (see CLAUDE.md
// §Sim-crash lessons). (b) is this, and it switches on the frame.
//
// The cost is stated so nobody has to rediscover it: `knobs.obj` is 26 MB of
// text, ~226 k vertices, and the duplicate is a second copy of all of it. On disk
// that is the honest price of an instant switch; in VRAM it is ~5 MB of geometry
// plus the second LIT atlas.
//
// ⚠ WHICH OBJs ARE INVOLVED IS DECIDED BY CONTENT AND BY THE ATTACHMENT TABLE,
// never by a hard-coded pair of names, and both halves of that matter:
//
//   * BY CONTENT, because the binding is the thing that makes an OBJ ours to
//     gate. Today `objects/lamps.obj` binds `text_LIT.png` and `objects/
//     knobs.obj` binds `knobs_LIT.png` on all three A3xx — but `lamps_Std.obj`
//     binds `text_LIT.png` too, and a table of two names would silently miss a
//     third binder if ToLiss ever split one.
//   * BY THE ATTACHMENT TABLE, because `lamps_Std.obj` is exactly that third
//     binder and X-Plane 12 never draws it: neither `a320.acf` nor
//     `a320_StdDef.acf` attaches it (both attach plain `lamps.obj`), so it is an
//     XP11 leftover. Patching it would be harmless; ATTACHING a LED twin of it
//     would add geometry the aeroplane did not draw before, which is a change
//     nobody asked for. So the rule is: gate what is drawn, leave the rest.
//
// ⚠ THE GATE'S POLARITY IS FAIL-OPEN TO INCANDESCENT. An OBJ binds dataref NAMES
// at load time and an unresolved one reads 0 forever, so a missing or older
// plugin gives the incandescent branch — the look Photon has always installed —
// and hides the LED twin. The reverse would leave a cockpit nobody chose.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/lit_recolor.h"

namespace photon {
namespace integral {

namespace fs = std::filesystem;

// The dataref every branch gates on. 0 = old halogen, 1 = new halogen, 2 = LED —
// the same value space the other five interior categories use. ⚠ Registered by
// the plugin at XPluginStart like every other category dataref; the name is
// shared with `plugin.cpp` through this constant rather than spelled twice.
extern const char kDataRef[];

// ⚠ THE NEEDLE, and it is the dataref itself. A patched OBJ is one that mentions
// the gate — content, not a filename or a version, the same test `DetectInstall`
// uses on the exterior OBJ.
extern const char kObjNeedle[];

// ⚠ THREE ERAS SINCE 2026-08-27, AND THE FIRST ONE IS STILL THE FILE'S OWN NAME.
// The era at value 0 is written over the stock file and keeps its name; each
// later era gets a twin beside it, suffixed. That asymmetry is deliberate and
// load-bearing: see `kEraSuffix` and the fail-open note in the header.
enum class Era {
    kOldHalogen = 0,
    kNewHalogen = 1,
    kLed = 2,
};

constexpr int kEraCount = 3;

const char* EraKey(Era era);    // "old-halogen" / "new-halogen" / "led"
const char* EraLabel(Era era);  // "Old Halogen" / "New Halogen" / "LED"

// The suffix inserted before the extension of every file this module generates,
// so one glob per era finds all of them at uninstall and nothing has to remember
// a list. ⚠ EMPTY FOR `kOldHalogen`, which is not an oversight: that era is
// written over the stock file in place, because it is the one a plugin-less or
// older-plugin aircraft must draw (an OBJ binds dataref NAMES at load and an
// unresolved one reads 0 forever). A suffix there would mean the stock name held
// stock artwork and nothing drew our work at all.
const char* EraSuffix(Era era);  // "" / "_photon_newhal" / "_photon_led"

// Every suffix this module has ever written, era order, `kOldHalogen`'s empty
// entry included. ⚠ THE ONE PLACE THE LIST LIVES — uninstall globs it, the
// fixture scan skips it and the plugin's detection reads it, and a suffix that
// three call sites spell for themselves is a suffix that gets added to two of
// them.
const char* const* EraSuffixes();  // [kEraCount]

// The `lit::Profile` an era installs. Kept here rather than left to each caller
// because "which spec does the LED branch get" is exactly the kind of mapping
// that reads as obvious and gets written backwards once.
lit::Profile ProfileFor(Era era);

// `lamps.obj` + kLed -> `lamps_photon_led.obj`, `text_LIT.png` + kNewHalogen ->
// `text_LIT_photon_newhal.png`. One rule, so a reader never has to check whether
// the OBJs and the textures spell it differently. ⚠ Returns `stockName`
// unchanged for `kOldHalogen` — see `EraSuffix`.
std::string EraNameFor(const std::string& stockName, Era era);

// Does this name carry any era suffix we write? ⚠ ASKED, NEVER SPELLED — a scan
// that tested only for `_photon_led` would find `lamps_photon_newhal.obj` a
// stock-looking OBJ and generate a twin OF A TWIN.
bool IsEraName(const std::string& name);

// One drawn OBJ that binds a texture we recolor, and everything derived from it.
// ⚠ INDEXED BY ERA, not a pair of named fields. It was `obj`/`ledObj` while there
// were two, and every consumer then said `era == kLed ? a : b` — which is exactly
// the shape that silently keeps working, and keeps ignoring the third era, when a
// third arrives.
struct Fixture {
    // [kOldHalogen] is the stock name, patched IN PLACE; the rest are twins.
    std::string obj[kEraCount];   // "lamps.obj", "lamps_photon_newhal.obj", ...
    std::string lit[kEraCount];   // "text_LIT.png", "text_LIT_photon_newhal.png", ...
    lit::Texture texture;

    const std::string& StockObj() const { return obj[0]; }
    const std::string& StockLit() const { return lit[0]; }
};

// The TEXTURE_LIT an OBJ binds, or empty. ⚠ HEADER ONLY — these files run to
// 26 MB and the answer is on line 7, so reading whole ones to enumerate a
// 52-row attachment table would be a gigabyte of I/O for eight lines.
std::string TextureLitOfFile(const fs::path& objPath);
std::string TextureLitOf(const std::string& objHead);

// Every OBJ in `objectsDir` binding one of the two textures, whether or not it
// is drawn. The caller intersects this with the attachment table — see the
// header note on `lamps_Std.obj`.
std::vector<Fixture> FixturesIn(const fs::path& objectsDir);

// Does this OBJ already carry our gate?
bool IsPatched(const std::string& objText);

// Wrap an OBJ's DRAW SECTION in a gate for `era`, and (when `litName` is given)
// repoint its `TEXTURE_LIT` at that file.
//
// ⚠ THE MIDDLE ERA TAKES **TWO** `ANIM_hide` LINES AND THE OUTER TWO TAKE ONE.
// A hide acts only while its range matches, so a branch has to hide itself at
// every value that is not its own: 0 hides over [0.5, 2.5], 2 hides over
// [-0.5, 1.5], and 1 — having neighbours on both sides — needs [-0.5, 0.5] AND
// [1.5, 2.5]. Multiple hides in one block AND together, which is what makes this
// work without nesting and without a per-era boolean dataref. This is the same
// ternary form the generated interior OBJ uses; see CLAUDE.md §OBJ gating.
//
// ⚠ THE VERTEX AND INDEX TABLES STAY OUTSIDE THE BLOCK. `VT`/`IDX`/`IDX10` are
// data, not drawing: OBJ8 requires them between `POINT_COUNTS` and the first
// command, and an `ANIM_begin` above them is not a no-op, it is a malformed
// file. The block therefore opens at the first real command.
//
// ⚠ REFUSES AN ALREADY-PATCHED FILE rather than wrapping it twice. A double wrap
// is invisible in a diff of a 300 k-line file and shows up in the cockpit as the
// branch never drawing at all. The installer avoids ever asking by sourcing from
// the backup, so a refusal here means a bug, and it says so.
bool PatchText(const std::string& stockObj, Era era, const std::string& litName,
               const std::string& version, std::string& out, std::string& error);

}  // namespace integral
}  // namespace photon
