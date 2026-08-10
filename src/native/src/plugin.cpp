// plugin.cpp — ToLiss Photon Lighting, native .xpl (X-Plane SDK)
//
// The shipping runtime. No XPPython3, no Python. CLAUDE.md holds the design, the
// dataref contract and the tripwires; this file holds the reasons that only make
// sense next to the code.
//
// Order: constants → JSON → waveform engine → state → accessors → profile
// resolution → persistence → ImGui windows → menu → engine loop → Auto loop →
// dev tooling (#if PHOTON_DEV) → lifecycle.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// The Dev window's Build tab shells out to the repo's Python tooling. MSVC spells
// the pipe functions with an underscore; everyone else does not.
#if defined(_WIN32)
  #define POPEN  _popen
  #define PCLOSE _pclose
#else
  #define POPEN  popen
  #define PCLOSE pclose
#endif

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlanes.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

// The facts the plugin and the INSTALLER both have to agree on: the version, the
// interior-OBJ needle, the skin-glow index map. Each of those used to be defined
// twice — once here and once on the installer side — and each pair drifted or could:
// the version silently did (0.6 vs 0.7, for a whole feature), and a glow-index
// mismatch makes redirected regions go dark. See src/core/constants.h.
//
// ⚠ photoncore links NO XPLM and knows nothing about X-Plane. The dependency runs
// one way only: plugin.cpp calls into core, never the reverse.
#include "core/constants.h"
#include "core/version.h"

// Every window in this plugin is Dear ImGui, shipping ones included, so the
// backend comes in unconditionally.
#include "imgui_xplm.h"

// CPU and GPU load for the performance tool. Sampled on its own thread and only
// while a performance pane has been opened — see sysmetrics.h.
#include "sysmetrics.h"

// Raw GL. The Panel FX compositor SHIPS, so this is unconditional — only the
// panel PROBE (the magenta diagnostic that paints over the displays) and the rest
// of the dev tooling stay behind -DPHOTON_DEV=ON.
#if IBM
  #include <windows.h>
  #include <GL/gl.h>
#elif APL
  #include <OpenGL/gl.h>
#else
  #include <GL/gl.h>
#endif

// The FX compositor's overlay images. The SDK has no image loader — XPLM4 dropped
// XPLMLoadTexture and left only "here is a texture number, upload it yourself" —
// so one is vendored.
// STBI_NO_STDIO: files are read through std::ifstream from a fs::path, so a
// non-ASCII path behaves like every other path in this plugin.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include "stb_image.h"

#ifndef XPLM300
#error "ToLiss Photon requires the X-Plane SDK 3.0+ (define XPLM300). See CMakeLists.txt."
#endif

namespace fs = std::filesystem;

// ================================ constants ==================================
static const int NCAT = 9;

// key, dataref, menu/row label, and the "classic" (dataref=0) look for THIS light
// type — beacon/strobe are Xenon flash tubes, everything else Halogen.
struct Category { const char* key; const char* dataref; const char* label; const char* classic; };
// order matches Categories in the Python plugin (also the Custom window order)
static const Category kCategories[NCAT] = {
    {"taxi",       "ToLissPhoton/exterior/taxi",       "Taxi Light",      "Halogen"},
    {"to",         "ToLissPhoton/exterior/to",         "T.O. Light",      "Halogen"},
    {"rwyturnoff", "ToLissPhoton/exterior/rwyturnoff", "Runway Turn Off", "Halogen"},
    {"wing",       "ToLissPhoton/exterior/wing",       "Wing Lights",     "Halogen"},
    {"landing",    "ToLissPhoton/exterior/landing",    "Landing Lights",  "Halogen"},
    {"beacon",     "ToLissPhoton/exterior/beacon",     "Beacon",          "Xenon"},
    {"nav",        "ToLissPhoton/exterior/nav",        "Nav Lights",      "Halogen"},
    {"strobe",     "ToLissPhoton/exterior/strobe",     "Strobes",         "Xenon"},
    {"logo",       "ToLissPhoton/exterior/logo",       "Logo Lights",     "Halogen"},
};

static const char* kIsLedDataref   = "ToLissPhoton/exterior/is_led";        // deprecated alias
static const char* kBeaconAlwaysOn = "ToLissPhoton/debug/beacon_always_on"; // hidden screenshot flag
static const char* kStrobeAlwaysOn = "ToLissPhoton/debug/strobe_always_on"; // hidden screenshot flag

// ------------------------------- interior ------------------------------------
// The cockpit ("Gus Mod") lights — a SEPARATE axis: own categories, own profile
// enum, own persisted keys. TERNARY (0 old halogen / 1 new halogen / 2 LED),
// which is why lights_inn.obj gates each branch with a PAIR of ANIM_hide lines.
//
// ⚠ `values` is per-category and NOTHING here may assume 3. The dome was
// two-valued between 2026-07-28 and 2026-08-03 on the theory that it is a
// fluorescent fitting with no incandescent era to follow; it is not — the A320's
// dome lamps are halogen like the rest, so it went back to the three-way ramp and
// the fluorescent look is gone. What is left is the machinery, kept because the
// value spaces genuinely may differ again and every writer already routes through
// IntValueForProfile / ClampIntValue. A two-valued category is only ever legal
// for 0 and 1: its gate hides branch 0 at exactly 1 and branch 1 at exactly 0, so
// at 2 NEITHER hides and both draw.
//
// One category per brightness source — panel[0] dome, panel[1],[2] mainpnl,
// panel[3] pedestal, inst[22],[23] console, and `map` as the documented
// exception (grouped by fixture role). panel[1],[2] is FLOOD LT MAIN PNL, not
// the map lights; that pair shipped the wrong way round once, and the `mainpnl`
// assignment still rests on a single in-sim observation. See CLAUDE.md.
static const int NINT = 5;
struct IntCategory {
    const char* key;
    const char* dataref;
    const char* label;
    int         values;      // 2 or 3 — how many looks this category has
    const char* valueLabel[3];   // [values] used; the tail is nullptr
};
static const IntCategory kIntCategories[NINT] = {
    {"dome",     "ToLissPhoton/interior/dome",     "Dome Lights",       3,
     {"Old Halogen", "New Halogen", "LED"}},
    {"map",      "ToLissPhoton/interior/map",      "Map Lights",        3,
     {"Old Halogen", "New Halogen", "LED"}},
    {"mainpnl",  "ToLissPhoton/interior/mainpnl",  "Main Panel Flood",  3,
     {"Old Halogen", "New Halogen", "LED"}},
    {"pedestal", "ToLissPhoton/interior/pedestal", "Pedestal & Tables", 3,
     {"Old Halogen", "New Halogen", "LED"}},
    {"console",  "ToLissPhoton/interior/console",  "Console Lights",    3,
     {"Old Halogen", "New Halogen", "LED"}},
};
// ─── the reduced-light-count cockpit ─────────────────────────────────────────
// A SIXTH interior dataref, and deliberately not a category: it selects between
// two ways of drawing the same look, not between two looks.
//
// Gus stacks two or three coincident spill lights on each main panel flood to
// fake a soft falloff (X-Plane's spill cone has a hard edge). It is a good
// effect and it stays the default — but it is ten lights where the panel has
// four lamps. `optimized` picks the reduced set instead: one light per position,
// the widest cone of its stack, at 1.5x size to stand in for the ones that went
// away. Which lights those are is the DSL's business (`optimize:` in
// lights.layout.phdsl); all this side does is publish the switch.
//
// ⚠ POLARITY IS FAIL-OPEN: 0 is Gus's full stack. An OBJ8 ANIM_hide reading a
// dataref that does not exist sees 0 forever (an OBJ binds dataref NAMES at load
// time), so an OBJ newer than the plugin — or a plugin that failed to start —
// gives the authored cockpit, never a reduced set nobody chose.
//
// ⚠ NOT a per-livery choice. It is a statement about the machine, not about the
// aeroplane, so it lives with the Displays settings in the file's one global
// block rather than in a livery entry (see kCockpitKey).
static const char* kIntOptimizedRef = "ToLissPhoton/interior/optimized";

struct CockpitSettings {
    bool optimized = false;      // false = Gus's full stack
};
static CockpitSettings gCockpit;

// Needed only by the prefs migration below. Kept next to the table so a reorder
// is caught by eye rather than by a wrong-looking cockpit.
static const int kIntDomeIndex    = 0;
static const int kIntMapIndex     = 1;
static const int kIntMainPnlIndex = 2;

// Force a category's value into its own legal range. Every path that sets an
// interior value ends here, so nothing out of range can reach a dataref.
static int ClampIntValue(int idx, int v) {
    int last = kIntCategories[idx].values - 1;
    if (v < 0) return 0;
    if (v > last) return last;
    return v;
}

// Prefs schema version for `interior_categories`. Bumped when a category's VALUE
// SPACE changes, which a clamp cannot recover from: a v2 `dome: 1` meant LED, a
// v1 and a v3 `dome: 1` mean new halogen — same number, different look, and no
// clamp can tell them apart. Absent == 1.
//
// The dome's history is the whole reason this key exists:
//   v1  ternary  0 old halogen / 1 new halogen / 2 LED
//   v2  TWO-valued 0 fluorescent / 1 LED          (2026-07-28 .. 2026-08-03)
//   v3  ternary again, i.e. v1's value space exactly — so only a v2 file needs
//       moving, and a v1 one is read as it stands.
static const int kIntPrefsSchema = 3;

// The one custom light in the interior OBJ: the re-added map light spot, and the
// only light on the map rheostat. Its brightness comes from ToLiss's
// `ckpt/lights/map` rather than a rheostat INDEX, which airplane_panel_sp cannot
// express, so it is a LIGHT_SPILL_CUSTOM on a 9-float dataref we own. See
// ReadSpillMap and docs/interior_plan.md §3.4.
//
// With the map knob down gMapSpillAlpha is 0 and the light is invisible, so the
// Map Lights row looks inert. That is the rheostat, not the gating.
static const char* kSpillMapDataRef = "ToLissPhoton/interior/spill/map";
static const char* kMapRheostatRef  = "ckpt/lights/map";
static const int   kSpillParamCount = 9;   // r g b a s dx dy dz semi
static const int   kSpillAlphaSlot  = 3;   // the map spot's one driven slot
// The screen glow drives SIZE instead (2026-08-09) — see kScreenSizeSlot below.
// ⚠ Both 3 and 4 are index-stable across the two custom-light orderings (the
// rotation is confined to the trailing four: cone, then dx/dy/dz), which is what
// makes driving either one safe. A test pins it.
static const int   kSpillSizeSlot   = 4;

// ─── display (screen) glow — EXPERIMENTAL, docs/screens_plan.md ──────────────
// Six spill lights, one per main display unit, washing each screen's own light
// back into the cockpit. Same mechanism as the map spot above and for the same
// reason: the level comes from a per-screen ToLiss dataref, which no rheostat
// INDEX can express, so each light is a LIGHT_SPILL_CUSTOM on a 9-float dataref
// we own and whose SIZE SLOT ALONE we drive.
//
// ⚠ THE DU KNOB DRIVES SIZE, NOT ALPHA (2026-08-09). A screen is an area
// source: turning it up spreads its wash further across the glareshield rather
// than making a fixed pool more intense, and a pool that only ever changed
// opacity read as a decal fading in and out. So `alpha:` in the DSL is now a
// CONSTANT that this plugin never overwrites — the authored intensity, drawn
// identically whether or not we are loaded — and `size:` is the reach at a
// screen turned fully up, which the knob scales down to 0.
//
// ⚠ THREE PLACES MUST AGREE, or a light silently binds the wrong screen:
//   * these six ToLissPhoton/screens/spill/<n> names,
//   * kScreenDU below — which DUBrightness index feeds slot n,
//   * the per-fixture `spill_dref:` overrides in src/lights/lights.layout.phdsl.
// The DSL is the authority on WHERE each light is; this table is the authority
// on WHICH SCREEN drives it.
//
// This is entirely opt-in: lights_screens.obj is not attached to a stock ToLiss
// aircraft, so with the experiment uninstalled these accessors exist and nothing
// reads them. They cost six registrations and one array read per frame.
static const char* kSpillScreenPrefix = "ToLissPhoton/screens/spill/";
static const char* kDUBrightnessRef   = "AirbusFBW/DUBrightness";
static const int   kScreenCount       = 6;

// The whole-OBJ master gate. lights_screens.obj wraps all six lights in ONE
// ANIM_hide block reading this, so with "backlight illumination" switched off
// the renderer skips the block instead of drawing six spill lights at size 0.
// Writing 0 into the driven slot always made them invisible; it never made them
// free, and free is the point of the switch.
//
// ⚠ NAMED FOR THE OFF STATE, and that is not a style choice: a dataref an OBJ
// cannot find reads 0 forever, so 0 must mean DRAW. Called `.../enabled` it
// would make a missing or older plugin an invisible OBJ, which in-sim reads as
// "the .acf attachment never happened" and sends the next person after the wrong
// bug — the same reasoning that bakes the OBJ's alpha low rather than at 0.
//
// ⚠ Must equal TARGET_MASTER_GATE["screens"] in build/build_objs.py; a test
// compares them.
static const char* kScreensDisabledRef = "ToLissPhoton/screens/disabled";

// Slot n -> AirbusFBW/DUBrightness index. SETTLED IN-SIM 2026-07-29 by dimming
// one display at a time and seeing which glow followed. It is NOT left to right
// across the panel, which was the seed and was wrong for four of the six; it is
// the Airbus DU numbering, pilot-side pair first:
//
//     DUBrightness[0] CA PFD   [1] CA ND   [2] FO PFD
//     DUBrightness[3] FO ND    [4] E/WD    [5] SD
//
// Our slot order is fixture order in the DSL — capt PFD, capt ND, upper ECAM,
// lower ECAM, F/O ND, F/O PFD — hence the crossover in the middle four.
// ⚠ Must equal the `index:` on each screen fixture in lights.layout.phdsl; a
// test compares the two. A wrong entry is invisible until the dimming test.
static const int kScreenDU[kScreenCount] = { 0, 1, 4, 5, 3, 2 };

// How much of a screen's brightness becomes spill — now a fraction of the OBJ's
// authored `size`, which is the reach at a screen turned fully up. This only
// trims the top end.
//
// 1.0 since 2026-07-29, was 0.85. The dev tool drives the same multiplier this
// is, so the look signed off in-sim at full is what gain 1 reproduces — 0.85
// would have shipped the approved tuning 15% short. Trim `size`, `alpha` or the
// palette if it wants toning down, not this.
static const float kScreenGain = 1.0f;

// ─── the DC bus behind the displays ──────────────────────────────────────────
// The driven readouts go dark when their bus dies, and "dark" has to
// mean dark for BOTH halves of this feature: the tint painted on the glass AND
// the spill thrown off it. A screen still washing the cockpit with blue after an
// electrical failure is the most visible wrong thing this plugin could do,
// because it contradicts the one event the user is watching for.
//
// ⚠ ToLiss's own AirbusFBW/DUBrightness does NOT fall to zero with the bus — it
// is the KNOB, and a knob keeps its setting through a power loss. So nothing
// already in the spill or driver path notices, and no amount of reshaping
// BrightnessResponse could have produced this. It has to be a separate gate.
//
// One bus for every gated face, as specified — kPanelBuses names which faces
// those are. On the real aircraft the displays are split across DC ESS / DC 2 /
// DC BAT, so if a partial failure ever needs to show only some screens dying, the
// fix is per-rect indices in that table (the mechanism already takes them) — not
// a second global.
static const char*  kDcBusRef        = "AirbusFBW/DCBusVoltages";
static const int    kDcBusIndex      = 0;
static const float  kDcBusLiveVolts  = 24.0f;

static XPLMDataRef    gDcBusRef    = nullptr;
static XPLMDataTypeID gDcBusTypes  = 0;
static int            gDcBusCount  = 0;
static bool           gDcBusLogged = false;   // one log line per aircraft, not per retry

static void ResolveDcBus();

// ⚠ UNKNOWN MEANS LIVE. An aircraft without this dataref, or an index past the
// end of the array, must leave the screens lit — the same rule the FX drivers
// follow ("a missing driver means factor 1, never 0"). "Everything went black
// and the log says nothing" is a far worse failure than an ungated glow.
//
// ⚠ Reads with the getter matching the ADVERTISED type. See ResolveDcBus.
static bool DcBusLive() {
    if (!gDcBusRef || kDcBusIndex < 0 || kDcBusIndex >= gDcBusCount) return true;
    if (gDcBusTypes & xplmType_FloatArray) {
        float v = 0.0f;
        if (XPLMGetDatavf(gDcBusRef, &v, kDcBusIndex, 1) < 1) return true;
        return v > kDcBusLiveVolts;
    }
    if (gDcBusTypes & xplmType_IntArray) {
        int v = 0;
        if (XPLMGetDatavi(gDcBusRef, &v, kDcBusIndex, 1) < 1) return true;
        return (float)v > kDcBusLiveVolts;
    }
    return true;
}

// ─── DCDU brightness, tracked from the buttons ───────────────────────────────
// The DCDUs have BrightnessUp/Down COMMANDS and no level dataref anyone has been
// able to find, so the level is not readable — it has to be INFERRED by watching
// the commands go past. We register a handler on each of ToLiss's four commands,
// count the presses, and publish the result as a dataref of our own so the
// ordinary FX driver mechanism can read it like any other brightness source.
//
// ⚠ Consequences of inferring rather than reading, all of them real:
//   * We see only presses that go through the COMMAND. A click on the 3-D button
//     does raise the command, but anything ToLiss changes internally (a reset on
//     power-up, a saved-state restore) does not, and we will be out of step until
//     the next press.
//   * The starting level is an ASSUMPTION (kDcduLevelDefault), reset on every
//     aircraft load because that is the only moment we can be sure of anything.
//   * The range is an ASSUMPTION too. Both are stated here as constants rather
//     than buried in the arithmetic precisely because they are the two things
//     in-sim testing is expected to correct.
//
// This is why the value is published as a dataref: it can be watched in
// DataRefTool while pressing the buttons, which is how the two guesses get
// settled. A plain internal float would make that a debugger session.
static const char* kDcduBrightnessRef = "ToLissPhoton/screens/dcdu_brightness";
static const int   kDcduCount         = 2;      // captain, first officer
static const int   kDcduLevelMin      = 0;
static const int   kDcduLevelMax      = 10;     // ⚠ assumed; confirm in-sim
static const int   kDcduLevelDefault  = 8;      // ⚠ assumed; confirm in-sim
static int gDcduLevel[kDcduCount] = { kDcduLevelDefault, kDcduLevelDefault };

static void UnbindDcduCommands();

// ─── the Displays axis (user settings) ───────────────────────────────────────
// A THIRD axis alongside Exterior and Cockpit, and unlike those two it is not a
// profile enum: there is one look per effect and the only questions are "on?" and
// "how strong?". Three switches, mapped onto the two features that draw on and
// around the displays:
//
//   spill    "backlight illumination" — the six LIGHT_SPILL_CUSTOM lights above,
//            i.e. the wash the screens throw INTO the cockpit. `spillGain`
//            multiplies the alpha this plugin writes, so 0 is invisible and the
//            OBJ's own `size` and palette still set the reach and the color.
//   screenFx "backlight visual effect" — the Panel FX layer stacks over the six
//            DU rects PLUS both MCDUs, both DCDUs and the ISIS, i.e. what the
//            screens look like ON the glass. Membership is `PanelRectIsScreen`,
//            not `du >= 0`: the five extras have no DUBrightness index but are
//            backlit screens a pilot reads the same way, and a switch called
//            "backlight visual effect" that left them out reads as half-working.
//   otherFx  "modified display colors" — the Panel FX stacks over every OTHER
//            readout: FCU, radios, the clock, the DME windows, the overhead.
//
// ⚠ The screenFx/otherFx split is decided PER RECT (`PanelRectIsScreen`), never
// per stack. A group target can legitimately span both — "everything the captain
// looks at" is a reasonable group — and a per-stack test would put the whole
// group on whichever switch its first member happened to match.
//
// ⚠ These are USER settings and live in ToLissPhoton_profiles.json. They are NOT
// written back into panelfx.txt, which is a shipped artifact the installer
// replaces on upgrade (and, on a dev machine, a symlink into the repo).
//
// `enabled` is the MASTER switch over all three. It is a separate flag rather
// than "turn the other three off" so that switching the page back on restores
// what the user had set rather than everything at once — and so the answer to
// "does Photon touch my displays at all?" is one line in the prefs file.
struct DisplaySettings {
    bool  enabled      = true;
    bool  spill        = true;
    float spillGain    = 1.0f;
    bool  screenFx     = true;
    float screenFxGain = 1.0f;
    bool  otherFx      = true;
};
static DisplaySettings gDisplays;

// ⚠ EVERY consumer of a Displays switch goes through here, so the master cannot
// be forgotten at one of them. Forgetting it is silent in exactly the way this
// axis always fails: two of the three effects vanish and the third stays on,
// which reads as that one effect being broken rather than as a missed gate.
static bool DisplayFeatureOn(bool feature) { return gDisplays.enabled && feature; }

// 0..1, deliberately not a boost. These scale a finished, signed-off look: the
// spill's reach is set by the OBJ's `size` and a layer's punch by its blend mode,
// and letting a slider push past 1 would drive a blend opacity above 1 — where
// half the modes have no defined meaning and the other half just clip.
static float ClampGain(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Is the cockpit mod on this aircraft? It is an independent opt-in install, and
// without it every Cockpit menu item is inert (the stock lights_inn.obj names
// none of our datarefs) — which reads as a broken mod. So the submenu is built
// only when the modded OBJ is really there.
//
// The sentinel is the OBJ BINDING OUR DATAREFS, not the installer's version
// marker: `build --target interior --write` installs a generated OBJ with no
// marker, and that build is as switchable as a released one. Keep in step with
// installer/constants.py INTERIOR_OBJ and the gate names the DSL emits.
// ⚠ BOTH NAMES NOW COME FROM `core/constants.h`, which the installer reads too.
// They used to be defined here AND in installer/constants.py AND (as gate names) in
// the DSL — three files that had to agree, with a mismatch showing up in-sim as the
// Cockpit submenu simply not appearing, i.e. exactly like the mod not being
// installed. One definition removes the class.
static const char*  kInteriorObjName     = photon::kInteriorObj;
static const char*  kInteriorObjNeedle   = photon::kInteriorObjNeedle;
static const size_t kInteriorObjMaxBytes = 4u * 1024u * 1024u;   // sanity cap

// profiles
enum { PROFILE_AUTO = 0, PROFILE_CLASSIC = 1, PROFILE_HYBRID = 2,
       PROFILE_FULL_LED = 3, PROFILE_CUSTOM = 4 };
struct ProfileItem { const char* label; int value; };
static const ProfileItem kProfileItems[] = {   // exterior submenu order
    {"Auto",       PROFILE_AUTO},
    {"Classic",    PROFILE_CLASSIC},
    {"Hybrid LED", PROFILE_HYBRID},
    {"Full LED",   PROFILE_FULL_LED},
};
static const int NPROFILE = (int)(sizeof(kProfileItems) / sizeof(kProfileItems[0]));

// Interior profiles. Its own enum because the exterior's are meaningless for a
// cockpit — "Hybrid LED" (LED position lights, halogen beams) has no interior
// analogue. INT_PROFILE_CUSTOM mirrors the exterior's: set by toggling any single
// category in the Custom window, and the only state that persists per-category.
enum { INT_PROFILE_AUTO = 0, INT_PROFILE_OLD = 1, INT_PROFILE_NEW = 2,
       INT_PROFILE_LED = 3, INT_PROFILE_CUSTOM = 4 };
static const ProfileItem kIntProfileItems[] = {   // cockpit submenu order
    {"Auto",         INT_PROFILE_AUTO},
    {"Old Halogen",  INT_PROFILE_OLD},
    {"New Halogen",  INT_PROFILE_NEW},
    {"LED",          INT_PROFILE_LED},
};
static const int NINTPROFILE = (int)(sizeof(kIntProfileItems) / sizeof(kIntProfileItems[0]));

// Row button labels come from kIntCategories[row].valueLabel. The wording matches
// the submenu profile items deliberately: the same name selects the same look, so
// these two lists are edited together.

// Shown in the About window.
//
// ⚠ NOT DEFINED HERE ANY MORE — `core/version.h` is THE definition, and the
// installer reads the same one. These two DID drift (0.6 against 0.7) through a
// whole feature, because nothing in-sim looks wrong when they disagree: the About
// tab simply reports an older version than the files beside it, which is precisely
// what a half-finished install looks like.
static const char* kPhotonVersion = photon::kPhotonVersion;
static const char* kOrgUrl = photon::kXplaneOrgUrl;
static const char* kGitHubUrl = "https://github.com/ischmal/toliss-photon-lighting";
// The cockpit lighting's author, credited on the About window.
static const char* kGusUrl =
    "https://forums.x-plane.org/profile/6767-gusrodrigues/";

// Categories that are LED in Hybrid LED (position/anti-collision); beam lights stay
// halogen. Mirrors HybridLedCategories = ("beacon", "strobe", "nav").
static bool IsHybridLed(int idx) {
    const char* k = kCategories[idx].key;
    return !std::strcmp(k, "beacon") || !std::strcmp(k, "strobe") || !std::strcmp(k, "nav");
}

// Auto gating. kMfrlGate: ToLiss AirbusFBW/LandingLightLocation (1 = MFRL fitted =>
// all-LED airframe, 0 = not). Both gates are read via ReadDataRefDouble, which uses
// the getter matching the dataref's REAL type — reading an int-typed dataref with
// XPLMGetDataf silently returns 0 (gotcha #4 in reverse; see ReadDataRefDouble).
static const char*  kSharkletGate      = "anim/SHARK";
static const double kSharkletThreshold = 0.5;
static const char*  kMfrlGate          = "AirbusFBW/LandingLightLocation";
static const double kMfrlThreshold     = 0.5;
static const int    kAutoFallback      = PROFILE_CLASSIC;

// engine tuning (mirrors the Lua/Python constants)
static const double kGateThreshold         = 0.5;
static const bool   kInvertStrobeGate      = false;
static const double kActivityThreshold     = 0.05;
static const double kActivityWindowSeconds = 1.2;
static const double kMaxDeltaTime          = 0.25;

static const char* kBeaconRatioDataRef = "sim/flightmodel2/lights/beacon_brightness_ratio";
static const char* kStrobeRatioDataRef = "sim/flightmodel2/lights/strobe_brightness_ratio";
static const char* kOverrideDataRef    = "sim/flightmodel2/lights/override_beacons_and_strobes";
static const char* kSimTimeDataRef     = "sim/time/total_running_time_sec";
static const char* kStrobeGateDataRef  = "sim/cockpit/electrical/strobe_lights_on";
static const char* kLiveryIndexDataRef = "sim/aircraft/view/acf_livery_index";
static const char* kIcaoDataRef        = "sim/aircraft/view/acf_ICAO";
// Photon's OWN skin-glow array. ToLiss's wing/fuselage/glass meshes carry
// `ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[N]` LIT-texture
// regions that ToLiss's plugin animates with its own strike-and-fade envelope.
// Photon overrides only the sim's beacon/strobe *ratio* arrays, so without a sync
// the painted skin keeps ToLiss's soft fade while our billboards go LED-square.
// ToLiss's ExternalLightBrightnesses array is READ-ONLY to other plugins, so we
// can't write it. Instead the installer repoints the mesh OBJs' ATTR_light_level
// lines at THIS array (a plain dataref-token swap — see build/patch_glow.py), and
// we drive each redirected index from the engine with the same value its billboard
// gets, so skin and billboard flash in lockstep. Registered at XPluginStart so it
// exists before the mesh OBJ binds the name (OBJ datarefs bind at load — gotcha #1);
// sized to cover every redirected index (A339 uses 12..16), unused entries read 0.
static const char* kGlowArrayDataRef   = "ToLissPhoton/exterior/glow";
static const int   kGlowArraySize      = 24;

// Superseded files this native .xpl replaces and deletes on startup (see
// RemoveSupersededFiles): the retired FlyWithLua script(s) (the waveform engine used
// to live in Lua) and the retired Python plugin PI_ToLissPhoton.py (this .xpl now
// supersedes it). Any leftover copy would register the same datarefs and drive the
// same per-frame beacon/strobe writes as this plugin, and the two would fight.
static const char* kLegacyLuaNames[]     = {"ToLissPhoton.lua", "photon_toliss_lights.lua"};
static const char* kRetiredPythonPlugin  = "PI_ToLissPhoton.py";

// Everything this plugin says goes to Log.txt AND to a small in-memory ring.
// The ring is what the Dev window's Log tab shows: during a tuning pass the
// interesting line is usually the one just written, and alt-tabbing to a text
// editor to find it is exactly the friction the Dev window exists to remove.
// It costs a few KB in a shipping build and buys a support answer that does not
// start with "please send me your Log.txt".
static const size_t kLogRingMax = 500;
static std::vector<std::string> gLogRing;

static void Log(const std::string& msg) {
    XPLMDebugString(("ToLiss Photon: " + msg + "\n").c_str());
    gLogRing.push_back(msg);
    if (gLogRing.size() > kLogRingMax) gLogRing.erase(gLogRing.begin());
}

static std::vector<std::string> LogRingSnapshot() { return gLogRing; }
static void ClearLogRing() { gLogRing.clear(); }

// ================================== JSON =====================================
// Minimal self-contained reader/writer (keeps the plugin dependency-free). Only
// what ToLissPhoton_profiles.json needs: objects, strings, ints, true/false/null;
// arrays are parsed and ignored. Same on-disk format the Python plugin uses.
namespace json {

struct Value {
    enum Type { Null, Int, Str, Obj, Arr } type = Null;
    long i = 0;
    // ⚠ `d` is set on EVERY number, alongside `i`, and the type stays Int. The
    // debug-light manifest is full of non-integers (positions, colors, cones)
    // and this reader used to truncate them all to a long; adding a Double type
    // instead would have meant every `type == Int` test in the profile reader
    // silently stopping at the first fractional value it met.
    double d = 0.0;
    std::string s;
    std::map<std::string, Value> obj;
    std::vector<Value> arr;
    const Value* find(const std::string& k) const {
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
    // Convenience for the manifest: numbers, with a default.
    double num(double dflt = 0.0) const { return type == Int ? d : dflt; }
    const Value* at(size_t k) const { return k < arr.size() ? &arr[k] : nullptr; }
};

struct Parser {
    const char* p;
    const char* end;
    explicit Parser(const std::string& src) : p(src.data()), end(src.data() + src.size()) {}

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

    bool value(Value& v) {
        ws();
        if (p >= end) return false;
        char c = *p;
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v.type = Value::Str; return str(v.s); }
        if (c == 't' || c == 'f') return boolean(v);
        if (c == 'n') return null(v);
        return number(v);
    }

    bool object(Value& v) {
        v.type = Value::Obj;
        ++p; ws();
        if (p < end && *p == '}') { ++p; return true; }
        while (p < end) {
            ws();
            if (p >= end || *p != '"') return false;
            std::string key;
            if (!str(key)) return false;
            ws();
            if (p >= end || *p != ':') return false;
            ++p;
            Value child;
            if (!value(child)) return false;
            v.obj[key] = child;
            ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; return true; }
            return false;
        }
        return false;
    }

    bool array(Value& v) {
        v.type = Value::Arr;
        ++p; ws();
        if (p < end && *p == ']') { ++p; return true; }
        while (p < end) {
            Value child;
            if (!value(child)) return false;
            v.arr.push_back(child);
            ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; return true; }
            return false;
        }
        return false;
    }

    bool str(std::string& out) {   // assumes *p == '"'
        ++p;
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p >= end) return false;
            char e = *p++;
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (end - p < 4) return false;
                    int cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = *p++;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                        else return false;
                    }
                    if (cp < 0x80) {
                        out += (char)cp;
                    } else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool number(Value& v) {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        bool any = false;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' ||
                           *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
            ++p; any = true;
        }
        if (!any) return false;
        const std::string text(start, p);
        v.type = Value::Int;
        v.i = std::strtol(text.c_str(), nullptr, 10);
        v.d = std::strtod(text.c_str(), nullptr);
        return true;
    }

    bool boolean(Value& v) {
        if (end - p >= 4 && !std::strncmp(p, "true", 4))  { p += 4; v.type = Value::Int; v.i = 1; v.d = 1.0; return true; }
        if (end - p >= 5 && !std::strncmp(p, "false", 5)) { p += 5; v.type = Value::Int; v.i = 0; v.d = 0.0; return true; }
        return false;
    }

    bool null(Value& v) {
        if (end - p >= 4 && !std::strncmp(p, "null", 4)) { p += 4; v.type = Value::Null; return true; }
        return false;
    }
};

inline std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); o += buf; }
                else o += (char)c;
        }
    }
    return o;
}

} // namespace json

// ============================== WAVEFORM ENGINE ==============================
// Faithful port of the Python engine (itself ported from ToLissPhoton.lua). See
// the Python module docstring for the invariants (onset = exact peak; drift-free
// advance; normalized Decay; clean low-FPS collapse). Do not regress these.

static const double kPi = 3.14159265358979323846;   // M_PI isn't standard C++

enum class Shape { Square, Decay, Triangle, Sine, RampUp, RampDown };

struct Pulse {
    double at = 0.0;
    double duration = 0.0;
    Shape  shape = Shape::Square;
    double hold = 0.0;
    double tau = 0.30;
    double peak = 1.0;
};

struct Segment {
    double duration = 0.0;
    bool   isOn = false;
    Shape  shape = Shape::Square;
    double tau = 0.30;
    double hold = 0.0;
    double peak = 1.0;
};

static double ClampBrightness(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// ---- the display brightness response curve ----------------------------------
//
// A display's brightness knob reads 0..1 linearly, but the glow it throws into
// the cockpit does not follow it linearly. Feeding the raw value straight into an
// alpha brings the screen-glow lights and the panel-FX tint up almost at once and
// then leaves them barely changing across the top half of the knob, which is the
// opposite of how the real thing behaves.
//
// So both go through this one shared curve, specified as knots (asked for
// 2026-08-01, x = knob, y = opacity):
//
//     0.0 -> 0.0     dead
//     0.2 -> 0.0     the glow starts here
//     0.5 -> 0.2     half a turn is still dim
//     1.0 -> 1.0
//
// THE KNOTS ARE THE SPEC. To reshape the fade, edit them and nothing else: no
// exponent, gain or dead-zone constant is derived from them anywhere. They must
// stay sorted by x and non-decreasing in y; tests/test_screen_curve.py pins both
// the values and the shape they produce.
//
// ⚠ Interpolation is MONOTONE cubic (Fritsch-Carlson), and the choice is
// load-bearing in both directions:
//   * plain Catmull-Rom/Hermite through these knots UNDERSHOOTS BELOW ZERO just
//     past the dead zone, because the flat run into 0.2 gives the middle knot a
//     tangent that dips. A negative alpha clamps, and a clamped dip is a visible
//     step in the middle of a fade - the one artifact this curve exists to remove.
//   * piecewise LINEAR has a 4x slope break at the middle knot, which reads as a
//     pop rather than a fade.
// Fritsch-Carlson cannot overshoot either bound by construction, and it flattens
// the take-off at the dead-zone edge for free: a zero secant pins both of its
// tangents to zero, so the curve leaves 0.2 horizontally instead of with a corner.
struct CurveKnot { float x, y; };

// The most knots any curve in this file may carry. The built-in table below is
// four and will stay four; the cap is for the AUTHORED curves (FxCurve, further
// down), which a hand drag in the Dev window and a hand-typed panelfx.txt line
// both feed, so both need somewhere bounded to land. It also lets the tangent
// builder keep its scratch on the stack.
static const int kFxCurveMaxKnots = 12;

// ---- the interpolator, over ANY knot table ----------------------------------
// Split out of kBrightnessCurve's own pair below because the shape is no longer
// only that table's: a panel-FX LAYER can carry knots of its own (FxCurve, and
// docs/fcu_tint_plan.md §8i). ONE implementation serves both, deliberately — a
// custom curve that read even slightly differently from the built-in one through
// the same knots would be indistinguishable in-sim from having placed a knot
// wrong, and the whole point of an editor is that what you draw is what you get.
//
// Two passes, in this order: flat and reversing segments pin their tangents to
// zero first, then the limiter scales what is left. Interleaving them would let
// the limiter operate on a tangent the next segment is about to zero anyway —
// harmless for the built-in knots, but only by accident of those knots.
static void BuildMonotoneTangents(const CurveKnot* k, int n, float* tangent) {
    if (n > kFxCurveMaxKnots) n = kFxCurveMaxKnots;
    if (n < 2) { if (n == 1) tangent[0] = 0.0f; return; }

    float secant[kFxCurveMaxKnots] = { 0.0f };   // secant[i] spans i -> i+1
    for (int i = 0; i < n - 1; ++i) {
        const float dx = k[i + 1].x - k[i].x;
        secant[i] = dx > 0.0f ? (k[i + 1].y - k[i].y) / dx : 0.0f;
    }
    tangent[0]     = secant[0];
    tangent[n - 1] = secant[n - 2];
    for (int i = 1; i < n - 1; ++i)
        tangent[i] = 0.5f * (secant[i - 1] + secant[i]);

    for (int i = 0; i < n - 1; ++i)
        if (secant[i] == 0.0f) { tangent[i] = 0.0f; tangent[i + 1] = 0.0f; }
    // A knot the curve turns around at. Never fires for kBrightnessCurve, whose
    // secants are all >= 0 — it is here for an authored curve that comes back
    // down, where a shared tangent across the turn is what would overshoot.
    for (int i = 1; i < n - 1; ++i)
        if (secant[i - 1] * secant[i] < 0.0f) tangent[i] = 0.0f;

    for (int i = 0; i < n - 1; ++i) {
        if (secant[i] == 0.0f) continue;
        const float a = tangent[i]     / secant[i];
        const float b = tangent[i + 1] / secant[i];
        const float s = a * a + b * b;
        if (s > 9.0f) {                       // outside the monotonicity region
            const float f = 3.0f / std::sqrt(s);
            tangent[i]     = f * a * secant[i];
            tangent[i + 1] = f * b * secant[i];
        }
    }
}

static float EvalMonotoneCurve(const CurveKnot* k, const float* tangent, int n,
                               float x) {
    if (n > kFxCurveMaxKnots) n = kFxCurveMaxKnots;
    if (n < 2) return n == 1 ? k[0].y : x;
    if (x <= k[0].x)     return k[0].y;
    if (x >= k[n - 1].x) return k[n - 1].y;

    int i = 0;
    while (i < n - 2 && x > k[i + 1].x) ++i;
    const float h = k[i + 1].x - k[i].x;
    if (h <= 0.0f) return k[i + 1].y;

    const float t  = (x - k[i].x) / h;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float y  =  ( 2.0f * t3 - 3.0f * t2 + 1.0f) * k[i].y
                   +  (        t3 - 2.0f * t2 + t   ) * h * tangent[i]
                   +  (-2.0f * t3 + 3.0f * t2       ) * k[i + 1].y
                   +  (        t3 -        t2       ) * h * tangent[i + 1];
    // Monotone interpolation cannot leave [y_i, y_i+1], so this only matters for
    // a table edited into a non-monotone shape - which an authored curve may
    // legitimately be. Cheaper than the bug.
    return y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
}

static const CurveKnot kBrightnessCurve[] = {
    { 0.0f, 0.0f },
    { 0.2f, 0.0f },
    { 0.5f, 0.2f },
    { 1.0f, 1.0f },
};
static const int kBrightnessCurveCount =
    (int)(sizeof(kBrightnessCurve) / sizeof(kBrightnessCurve[0]));

static_assert(kBrightnessCurveCount <= kFxCurveMaxKnots,
              "kBrightnessCurve must fit the shared tangent scratch");

static float gCurveTangent[kBrightnessCurveCount] = { 0.0f };
static bool  gCurveBuilt = false;

// Tangents depend only on the constant table, so this runs once.
static void BuildBrightnessCurve() {
    BuildMonotoneTangents(kBrightnessCurve, kBrightnessCurveCount, gCurveTangent);
    gCurveBuilt = true;
}

// Normalized 0..1 brightness in, 0..1 opacity out, monotone in between.
static float BrightnessResponse(float x) {
    if (!gCurveBuilt) BuildBrightnessCurve();
    return EvalMonotoneCurve(kBrightnessCurve, gCurveTangent, kBrightnessCurveCount, x);
}

// ---- a curve someone AUTHORED ----------------------------------------------
//
// kBrightnessCurve is the spec for how a rheostat becomes an opacity, and it is
// right for what it was measured on: a backlit LCD dimmed by its own knob. It is
// not right for everything a layer does. A bleed that should only appear over
// the top third of the knob, a wash that has to be there the moment the screen
// lights at all, a correction that fades out early so the readout is not tinted
// twice at full brightness — every one of those is the same driver read through
// a different shape, and before this the only choices were that one curve or a
// straight line.
//
// So a LAYER may carry knots of its own. Not a target: two layers of one stack
// routinely want opposite shapes (that is the same argument `lfollow` is built
// on), and a curve on the target would force the more visible one to win.
//
// ⚠ THIS NEVER REACHES THE SPILL LIGHTS. They run through BrightnessResponse
// unconditionally, which is what keeps a tint and the glow under it coming up
// together; an authored curve is a property of one painted layer.
//
// `on` false means "no opinion" — the layer takes its target's response switch,
// which takes the master's. The knots are still kept while it is off, so
// unticking the box to compare against the default does not throw the shape
// away.
struct FxCurve {
    bool on = false;
    std::vector<CurveKnot> knots;      // >= 2, sorted by x, x spanning 0..1

    // Rebuilt on demand rather than on every edit: the editor moves a knot every
    // frame a drag is held, and there is nothing to gain by recomputing four
    // tangents at the moment of the edit rather than at the moment of the read.
    mutable float tangent[kFxCurveMaxKnots] = { 0.0f };
    mutable bool  built = false;
};

// The built-in shape, as an FxCurve, so that "which curve is in force" is one
// pointer everywhere instead of a pointer plus a bool. Its knots ARE
// kBrightnessCurve, so it evaluates identically to BrightnessResponse.
static const FxCurve& FxBuiltInCurve() {
    static FxCurve c = [] {
        FxCurve k;
        k.on = true;
        k.knots.assign(kBrightnessCurve, kBrightnessCurve + kBrightnessCurveCount);
        return k;
    }();
    return c;
}

// Put a dragged or hand-typed table into the shape the interpolator assumes:
// inside the unit square, sorted by x, spanning the whole 0..1 input range, at
// least two knots and no more than the cap.
//
// ⚠ It does NOT force y to be non-decreasing. A curve that comes back down is a
// legitimate authored look (a wash that peaks mid-travel and eases off as the
// screen gets bright enough not to need it), and BuildMonotoneTangents handles
// the turn — see the local-extremum pass there. What is forced is the DOMAIN,
// because a curve whose first knot sits at x = 0.3 would silently hold its own
// first y across the bottom third of every knob.
static void FxCurveNormalize(FxCurve& c) {
    for (size_t i = 0; i < c.knots.size(); ++i) {
        CurveKnot& k = c.knots[i];
        k.x = k.x < 0.0f ? 0.0f : (k.x > 1.0f ? 1.0f : k.x);
        k.y = k.y < 0.0f ? 0.0f : (k.y > 1.0f ? 1.0f : k.y);
    }
    std::stable_sort(c.knots.begin(), c.knots.end(),
                     [](const CurveKnot& a, const CurveKnot& b) { return a.x < b.x; });
    if (c.knots.size() > (size_t)kFxCurveMaxKnots) c.knots.resize(kFxCurveMaxKnots);
    if (c.knots.size() < 2) {
        c.knots.assign(kBrightnessCurve, kBrightnessCurve + kBrightnessCurveCount);
    } else {
        c.knots.front().x = 0.0f;
        c.knots.back().x  = 1.0f;
    }
    c.built = false;
}

static float FxCurveEval(const FxCurve& c, float x) {
    const int n = (int)c.knots.size();
    if (n < 2) return x;                       // nothing authored: identity
    if (!c.built) {
        BuildMonotoneTangents(c.knots.data(), n, c.tangent);
        c.built = true;
    }
    return EvalMonotoneCurve(c.knots.data(), c.tangent, n, x);
}

static double Envelope(Shape shape, double progress, double tau) {
    switch (shape) {
        case Shape::Square: return 1.0;
        case Shape::Decay: {
            // normalized exponential: exactly 1.0 at progress 0, 0.0 at progress 1
            double t = tau > 0.0 ? tau : 0.30;
            double tail = std::exp(-1.0 / t);
            return (std::exp(-progress / t) - tail) / (1.0 - tail);
        }
        case Shape::RampUp:   return progress;
        case Shape::RampDown: return 1.0 - progress;
        case Shape::Triangle: return 1.0 - std::fabs(2.0 * progress - 1.0);
        case Shape::Sine:     return std::sin(progress * kPi);
    }
    return 1.0;
}

static std::vector<Segment> BuildSegments(std::vector<Pulse> pulses, double period) {
    std::sort(pulses.begin(), pulses.end(),
              [](const Pulse& a, const Pulse& b) { return a.at < b.at; });
    std::vector<Segment> segments;
    double cursor = 0.0;
    for (const Pulse& pulse : pulses) {
        if (pulse.at > cursor + 1e-6) {
            Segment off; off.duration = pulse.at - cursor; off.isOn = false;
            segments.push_back(off);
        }
        Segment on;
        on.duration = pulse.duration; on.isOn = true;
        on.shape = pulse.shape; on.tau = pulse.tau; on.hold = pulse.hold; on.peak = pulse.peak;
        segments.push_back(on);
        cursor = std::max(cursor, pulse.at) + pulse.duration;
    }
    if (cursor < period - 1e-6) {
        Segment off; off.duration = period - cursor; off.isOn = false;
        segments.push_back(off);
    } else if (cursor > period + 1e-3) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "WARNING pulses total %.3fs exceed period %.3fs", cursor, period);
        Log(buf);
    }
    return segments;
}

class Channel {
public:
    std::vector<Segment> segments;
    double period = 0.0, offset = 0.0, totalDuration = 0.0;
    int    segmentIndex = 0;
    double segmentElapsed = 0.0, renderAge = 0.0;

    Channel() {}
    Channel(const std::vector<Pulse>& pulses, double period_, double offset_)
        : period(period_), offset(offset_) {
        segments = BuildSegments(pulses, period_);
        for (const Segment& s : segments) totalDuration += s.duration;
    }

    void Seek(double targetTime) {
        if (totalDuration <= 0.0) { segmentIndex = 0; segmentElapsed = 0.0; renderAge = 0.0; return; }
        targetTime = std::fmod(targetTime, totalDuration);
        if (targetTime < 0.0) targetTime += totalDuration;
        double accumulated = 0.0;
        for (int index = 0; index < (int)segments.size(); ++index) {
            if (targetTime < accumulated + segments[index].duration) {
                segmentIndex = index;
                segmentElapsed = targetTime - accumulated;
                renderAge = segmentElapsed;
                return;
            }
            accumulated += segments[index].duration;
        }
        segmentIndex = (int)segments.size() - 1;
        segmentElapsed = 0.0;
        renderAge = 0.0;
    }

    double Tick(double deltaTime) {
        if (segments.empty()) return 0.0;
        Segment& segment = segments[segmentIndex];
        double brightness = 0.0;
        if (segment.isOn && segment.duration > 0.0 && renderAge < segment.duration) {
            double progress = renderAge / segment.duration;
            if (progress < 0.0) progress = 0.0;
            else if (progress > 1.0) progress = 1.0;
            double hold = segment.hold;
            if (hold >= 1.0) progress = 0.0;
            else if (progress > hold) progress = (progress - hold) / (1.0 - hold);
            else progress = 0.0;
            brightness = Envelope(segment.shape, progress, segment.tau) * segment.peak;
        }
        segmentElapsed += deltaTime;
        renderAge += deltaTime;
        if (segmentElapsed >= segment.duration) {
            segmentElapsed -= segment.duration;
            if (++segmentIndex >= (int)segments.size()) segmentIndex = 0;
            renderAge = 0.0;
        }
        return brightness;
    }
};

static const double kBeaconPeriod = 1.0, kBeaconOffset = 0.5;
static const double kStrobePeriod = 1.0, kStrobeOffset = 0.0;

static std::vector<Pulse> BeaconPulses(bool led) {
    Pulse p;
    if (led) { p.duration = 0.11; p.shape = Shape::Square; }
    else     { p.duration = 0.06; p.hold = 0.40; p.shape = Shape::Decay; p.tau = 0.30; }
    return {p};
}

static std::vector<Pulse> StrobePulses(bool led, int index) {
    auto sq = [](double at, double dur) { Pulse p; p.at = at; p.duration = dur; p.shape = Shape::Square; return p; };
    // xenon strike-and-decay flash: bright hold then a quick fade (same envelope as the tail)
    auto decay = [](double at) {
        Pulse p;
        p.at = at;
        p.duration = 0.06;
        p.hold = 0.40;
        p.shape = Shape::Decay;
        p.tau = 0.30;
        return p;
    };
    if (led) {
        switch (index) {
            case 0: case 1: return {sq(0.00, 0.05), sq(0.11, 0.05)};   // double wingtips
            case 2:         return {sq(0.00, 0.11)};                    // single tail
            case 3:         return {sq(0.00, 0.11)};                    // single tail
        }
    } else {
        switch (index) {
            case 0:
            case 1: return {decay(0.00), decay(0.12)};   // double wingtips, each strike-and-decay
            case 2:         return {decay(0.00)};
            case 3:         return {sq(0.00, 0.04)};
        }
    }
    return {};
}

// ================================== state ====================================
static int gValues[NCAT] = {0};          // backing ints for the 9 datarefs (0/1)
static int gProfile = PROFILE_AUTO;
static int gBeaconAlwaysOn = 0;
static int gStrobeAlwaysOn = 0;

// interior: backing ints for the 4 datarefs (0 old / 1 new / 2 LED)
static int gIntValues[NINT] = {0};
static int gIntProfile = INT_PROFILE_AUTO;
// What Auto currently resolves to on each axis, refreshed by AutoLoop at 1 Hz
// whatever profile is selected. Read by the settings window to label the Auto row
// "Auto (Hybrid LED)"; never used to DRIVE anything — ApplyProfileToValues
// re-resolves for itself, so a stale cache can only mislabel, never mis-light.
static int gAutoExtResolved = PROFILE_CLASSIC;
static int gAutoIntResolved = INT_PROFILE_OLD;
// Live alpha for the map spill light, refreshed each frame from ckpt/lights/map.
static float gMapSpillAlpha = 0.0f;
// Live SIZE FACTOR per screen-glow light (0..1 of the OBJ's authored size),
// refreshed each frame from AirbusFBW/DUBrightness. Not an alpha: the DU knob
// drives how far a screen's wash reaches, not how opaque it is.
static float gScreenSizeFactor[kScreenCount] = {0.0f};
// The OBJ's authored `size:` per screen light, captured from the pre-filled
// parameter buffer the FIRST time X-Plane asks for that slot, and reset on every
// aircraft load (a rebuilt OBJ may bake a different one).
//
// ⚠ Captured rather than multiplied in place, and rather than duplicated as a
// constant here. Multiplying would compound to zero if X-Plane ever fed our own
// output back in — an assumption this project has not measured — and a constant
// would put the authored size in two files, which is exactly the drift
// kScreenDU already has to be tested against. Reading once and assigning
// thereafter is correct under either behavior, and keeps the .phdsl the only
// place a size is written.
static float gScreenBakedSize[kScreenCount] = {0.0f};
static bool  gScreenBakedSeen[kScreenCount] = {false};

static XPLMDataRef gCategoryRefs[NCAT] = {nullptr};
static XPLMDataRef gIntCategoryRefs[NINT] = {nullptr};
static XPLMDataRef gIntOptimizedRef = nullptr;
static XPLMDataRef gScreensDisabledRef = nullptr;
static XPLMDataRef gSpillMapRef = nullptr;
static XPLMDataRef gSpillScreenRefs[kScreenCount] = {nullptr};
// The names must outlive registration — XPLMRegisterDataAccessor keeps the char*.
static std::string gScreenDataRefNames[kScreenCount];
static XPLMDataRef gDcduBrightnessRef = nullptr;
static XPLMDataRef gIsLedRef = nullptr, gBeaconDbgRef = nullptr, gStrobeDbgRef = nullptr;
static XPLMDataRef gLiveryIndexRef = nullptr;

// persistence. The interior fields are ADDITIVE: an old prefs file has neither,
// and an absent interior_profile means Auto — exactly as an absent entry means
// Auto for the exterior. So old files keep loading unchanged.
struct Entry {
    int  profile = PROFILE_AUTO;
    bool hasCats = false;
    int  cats[NCAT] = {0};
    int  intProfile = INT_PROFILE_AUTO;
    bool hasIntCats = false;
    int  intCats[NINT] = {0};
};
static std::map<std::string, Entry> gProfiles;   // liveryKey -> entry

// Whether THIS aircraft carries the cockpit mod (see kInteriorObjNeedle).
// Re-detected on every aircraft load; false for any non-ToLiss.
static bool gInteriorInstalled = false;

// menu
static bool       gMenuCreated = false;
// What gInteriorInstalled was when the menu was BUILT. The Cockpit submenu is
// decided at build time, so switching between two ToLiss aircraft that disagree
// about the cockpit mod has to rebuild — see UpdateMenuVisibility.
static bool       gMenuHasInterior = false;
static XPLMMenuID gMenuID = nullptr;          // level 1: ToLiss Photon
static XPLMMenuID gExtMenuID = nullptr;       // level 2: Exterior
static XPLMMenuID gIntMenuID = nullptr;       // level 2: Cockpit — absent when
                                              // the cockpit mod is not installed
static int        gPluginsMenuItem = -1;
static int        gProfileItemIndex[NPROFILE];
static int        gIntProfileItemIndex[NINTPROFILE];
static int        gExtCustomItemIndex = -1;   // "Custom..." at the foot of each submenu
static int        gIntCustomItemIndex = -1;

// waveform engine
static XPLMFlightLoopID gEngineLoopID = nullptr;
static bool       gAutoLoopRegistered = false;
static XPLMDataRef gSimTimeRef = nullptr, gBeaconRatioRef = nullptr, gStrobeRatioRef = nullptr,
                   gOverrideRef = nullptr, gStrobeGateRef = nullptr;
static double     gLastSimTime = -1.0;
static bool       gOverrideWarned = false;
static Channel    gBeaconChannel;
static bool       gBeaconBuilt = false;
static Channel    gStrobeChannel[4];
static bool       gStrobeBuilt = false;
static int        gCurrentBeaconLed = -1, gCurrentStrobeLed = -1;
static double     gLastActiveBeacon[4] = {-1e9, -1e9, -1e9, -1e9};

// skin-glow sync — see kGlowArrayDataRef. A GlowMap says which glow-array index
// each engine channel drives; it is airframe-specific (the array's per-index
// meaning matches ToLiss's own convention, which differs by model), so one is only
// selected for a matching ICAO. kGlowNone = channel unmapped (that index left at 0).
// The indices here MUST match build/patch_glow.py's REDIRECT table — the installer
// only repoints exactly these OBJ regions at our array, and we only drive exactly
// these entries; a mismatch would leave a redirected region reading a 0 we never set.
static const int   kGlowNone = -1;
struct GlowMap { int strobe[4]; int beacon[4]; };
static XPLMDataRef gGlowArrayRef = nullptr;
static float       gGlow[kGlowArraySize] = {0};   // backing store the OBJ reads
static GlowMap     gGlowMap = { {kGlowNone,kGlowNone,kGlowNone,kGlowNone},
                                {kGlowNone,kGlowNone,kGlowNone,kGlowNone} };
static bool        gGlowMapActive = false;

// forward decls
static void UpdateChecks();
static void ApplyProfileToValues();
static void LoadProfileForCurrentLivery();
static void UpdateMenuVisibility();

// ============================= dataref accessors =============================
static int   ReadCategoryInt(void* refcon)   { return gValues[(int)(intptr_t)refcon]; }
static float ReadCategoryFloat(void* refcon) { return (float)gValues[(int)(intptr_t)refcon]; }

// Interior categories. BOTH int and float readers, same as the exterior nine
// (gotcha #4): the OBJ reads them as float, and registering only one type makes
// X-Plane advertise the type wrong so the OBJ silently reads 0 forever.
static int   ReadIntCategoryInt(void* refcon)   { return gIntValues[(int)(intptr_t)refcon]; }
static float ReadIntCategoryFloat(void* refcon) { return (float)gIntValues[(int)(intptr_t)refcon]; }

// The reduced-light-count switch. Same int|float pairing as the categories, and
// for the same reason: the OBJ reads it as a float, and registering one type
// makes X-Plane advertise the type wrong and the gate read 0 forever — which
// here would look like the switch simply not working.
static int   ReadIntOptimizedInt(void*)   { return gCockpit.optimized ? 1 : 0; }
static float ReadIntOptimizedFloat(void*) { return gCockpit.optimized ? 1.0f : 0.0f; }

// The screens' master gate. DERIVED, not stored: it is exactly the state of the
// user's "backlight illumination" switch under the Displays master, so keeping a
// second copy in sync per frame would be one more thing to forget. Read through
// DisplayFeatureOn like every other consumer of that switch.
//
// Inverted, because the dataref is named for the OFF state (kScreensDisabledRef).
static int   ReadScreensDisabledInt(void*)   { return DisplayFeatureOn(gDisplays.spill) ? 0 : 1; }
static float ReadScreensDisabledFloat(void*) { return DisplayFeatureOn(gDisplays.spill) ? 0.0f : 1.0f; }

// The map spot's 9-float custom-light dataref.
//
// X-Plane runs the OBJ's BAKED parameters THROUGH this dataref: it pre-fills the
// buffer with them and the accessor modifies in place (the same
// "getDatavf fills in place" behavior noted at the top of this file). So we
// touch ONLY the alpha slot and leave the other eight exactly as the OBJ baked
// them — color keeps coming from the visible ANIM_hide branch, geometry from the DSL.
// Writing all nine here would duplicate the DSL's values in C++ and create a
// third place that has to agree, which is precisely the failure mode the
// skin-glow triple already documents.
//
// If this plugin is absent the dataref is simply not found and X-Plane draws the
// nine baked parameters unmodified — the light still appears, at its baked alpha,
// just no longer following the map rheostat. That is why the DSL bakes alpha
// conservatively low: the degraded state is "dim", never "glaring".
static int ReadSpillMap(void* /*refcon*/, float* out, int offset, int max) {
    if (!out) return kSpillParamCount;          // size query
    if (offset < 0) offset = 0;
    int n = 0;
    for (int i = 0; i < max && offset + i < kSpillParamCount; ++i) {
        if (offset + i == kSpillAlphaSlot) out[i] = gMapSpillAlpha;
        ++n;                                     // every other slot passes through
    }
    return n;
}

// The six screen-glow lights' 9-float custom-light datarefs — one accessor shared
// by all of them, the light's slot arriving as the refcon.
//
// Same modify-in-place contract as ReadSpillMap above — X-Plane pre-fills the
// buffer with the OBJ's baked parameters — but the driven slot is SIZE, not
// alpha: the DU knob sets how far a screen's wash reaches, and `alpha:` in the
// DSL is a constant nothing here overwrites. With this plugin absent the dataref
// is simply not found and every baked parameter is drawn unmodified, so the
// degraded state is a faint glow at full reach that ignores the DU knobs, never
// nothing at all.
//
// ⚠ The authored size is CAPTURED on the first read of that slot, then assigned
// from thereafter (see gScreenBakedSize). Until it has been captured the slot is
// left alone rather than written: an uncaptured size would otherwise multiply
// out to 0 and make six lights vanish, which reads in-sim as a failed .acf
// attachment — the one wrong answer this whole feature keeps having to design
// against.
static int ReadSpillScreen(void* refcon, float* out, int offset, int max) {
    if (!out) return kSpillParamCount;          // size query
    if (offset < 0) offset = 0;
    const int slot = (int)(intptr_t)refcon;
    if (slot < 0 || slot >= kScreenCount) return 0;
    int n = 0;
    for (int i = 0; i < max && offset + i < kSpillParamCount; ++i) {
        if (offset + i == kSpillSizeSlot) {
            if (!gScreenBakedSeen[slot] && out[i] > 0.0f) {
                gScreenBakedSize[slot] = out[i];
                gScreenBakedSeen[slot] = true;
            }
            if (gScreenBakedSeen[slot])
                out[i] = gScreenBakedSize[slot] * gScreenSizeFactor[slot];
        }
        ++n;                                     // every other slot passes through
    }
    return n;
}

// The inferred DCDU brightness levels, published so the FX driver mechanism can
// read them by name like any of ToLiss's own sources — and so they can be watched
// in a dataref inspector while pressing the buttons, which is how kDcduLevelMax
// and kDcduLevelDefault are meant to be settled.
//
// ⚠ Unlike the spill accessors above, this one does NOT modify a pre-filled
// buffer: it is a plain value, not a custom-light parameter block, so every
// element in range is written.
static int ReadDcduBrightness(void* /*refcon*/, float* out, int offset, int max) {
    if (!out) return kDcduCount;                // size query
    if (offset < 0) offset = 0;
    int n = 0;
    for (int i = 0; i < max && offset + i < kDcduCount; ++i)
        out[n++] = (float)gDcduLevel[offset + i];
    return n;
}

// Float-array reader for ToLissPhoton/exterior/glow. Read-only to the sim: the
// engine owns gGlow and updates it every frame; the redirected OBJ ATTR_light_level
// regions read it back here. Follows the XPLMGetDatavf_f contract — out==null is a
// size query.
static int ReadGlowArray(void* /*refcon*/, float* out, int offset, int max) {
    if (!out) return kGlowArraySize;
    if (offset < 0) offset = 0;
    int n = 0;
    for (int i = 0; i < max && offset + i < kGlowArraySize; ++i) {
        out[i] = gGlow[offset + i];
        ++n;
    }
    return n;
}

static int AllLed() {
    for (int i = 0; i < NCAT; ++i) if (gValues[i] != 1) return 0;
    return 1;
}
static int   ReadIsLedInt(void*)   { return AllLed(); }
static float ReadIsLedFloat(void*) { return (float)AllLed(); }

static int   ReadBeaconDbgInt(void*)             { return gBeaconAlwaysOn; }
static float ReadBeaconDbgFloat(void*)           { return (float)gBeaconAlwaysOn; }
static void  WriteBeaconDbgInt(void*, int v)     { gBeaconAlwaysOn = v ? 1 : 0; }
static void  WriteBeaconDbgFloat(void*, float v) { gBeaconAlwaysOn = v >= 0.5f ? 1 : 0; }

static int   ReadStrobeDbgInt(void*)             { return gStrobeAlwaysOn; }
static float ReadStrobeDbgFloat(void*)           { return (float)gStrobeAlwaysOn; }
static void  WriteStrobeDbgInt(void*, int v)     { gStrobeAlwaysOn = v ? 1 : 0; }
static void  WriteStrobeDbgFloat(void*, float v) { gStrobeAlwaysOn = v >= 0.5f ? 1 : 0; }

// ============================ profile resolution =============================
static void CategoriesForNamedProfile(int profile, int out[NCAT]) {
    for (int i = 0; i < NCAT; ++i) {
        if (profile == PROFILE_FULL_LED)   out[i] = 1;
        else if (profile == PROFILE_HYBRID) out[i] = IsHybridLed(i) ? 1 : 0;
        else                                out[i] = 0;   // Classic / anything unexpected
    }
}

static std::string AircraftIcao() {
    XPLMDataRef ref = XPLMFindDataRef(kIcaoDataRef);
    if (!ref) return "";
    char buf[64] = {0};
    XPLMGetDatab(ref, buf, 0, (int)sizeof(buf) - 1);
    std::string s(buf);   // stops at first NUL
    // trim + upper
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    s = s.substr(a, b - a + 1);
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// Which glow-array indices this airframe's engine channels drive. Applies only to
// the matching ICAO; any other aircraft gets no map (feature off, ToLiss's own glow
// fade untouched). Indices keep ToLiss's own numbering so the installer's OBJ patch
// is a minimal per-region token swap (AirbusFBW/ExternalLightBrightnesses[N] ->
// ToLissPhoton/exterior/glow[N], same N). A339 indices confirmed in-sim (live values
// watched): strobe [12]=left wingtip, [13]=right wingtip, [14]=tail; beacon
// [15],[16] = top/bottom (in ExteriorGlass.obj — they ramp with the beacon fade
// envelope, unlike a strobe's sharp on/off). Keep in sync with build/patch_glow.py.
// ⚠ THE INDEX LIST COMES FROM `core/constants.h`, WHICH THE INSTALLER'S PATCHER
// READS TOO. It used to live here and, separately, in build/patch_glow.py's
// REDIRECT — and a redirected index Photon does NOT drive goes DARK, because our
// array reads 0 where we never write. "Keep these two in sync" was the rule; one
// table is the fix. The ORDER within that list is this function's own contract
// (strobe ch0/ch1/ch2 then beacon top/bottom) and is asserted below.
static bool GlowMapForIcao(const std::string& icao, GlowMap& out) {
    out = GlowMap{ {kGlowNone,kGlowNone,kGlowNone,kGlowNone},
                   {kGlowNone,kGlowNone,kGlowNone,kGlowNone} };
    // The installer keys the table by airframe; the plugin only has the ICAO.
    const char* key = (icao == "A339") ? "a339" : nullptr;
    if (key == nullptr) return false;
    const std::vector<int>* idx = photon::GlowIndicesFor(key);
    if (idx == nullptr || idx->size() < 5) return false;
    out.strobe[0] = (*idx)[0];   // engine strobe ch0 = left wingtip
    out.strobe[1] = (*idx)[1];   // ch1 = right wingtip
    out.strobe[2] = (*idx)[2];   // ch2 = tail
    out.beacon[0] = (*idx)[3];   // top + bottom beacon glow (both on the beacon
    out.beacon[1] = (*idx)[4];   // waveform; live values ramp 0..1 with the fade)
    return true;
}

// Select the glow map for the loaded aircraft. Our glow array is registered at
// XPluginStart and always present, so there is nothing to bind here — just pick the
// per-ICAO index map (or none). When no map applies, clear our array so a previous
// aircraft's values don't linger (nothing reads them, but keep it tidy).
static void ResolveGlowMap() {
    std::string icao = AircraftIcao();
    gGlowMapActive = GlowMapForIcao(icao, gGlowMap);
    if (gGlowMapActive) {
        Log("skin-glow sync active (" + icao + "); driving " + kGlowArrayDataRef);
    } else {
        for (float& g : gGlow) g = 0.0f;
    }
}

// Read any numeric dataref as a double, regardless of how it is typed. Per
// XPLMDataAccess.h: "if there is a type mismatch, the functions that read data
// will return 0" — so XPLMGetDataf on an INT-typed dataref (e.g. ToLiss's
// AirbusFBW/LandingLightLocation) silently yields 0.0 and never trips a gate.
// This is gotcha #4 in reverse: there the OBJ read our int-backed dataref as
// float; here we read someone else's dataref without knowing its type. So detect
// the real type and use the matching getter. Returns false when the path is
// empty/unset, the dataref is missing, or it is non-numeric (byte/string data).
// Read an ALREADY-RESOLVED dataref using the getter matching its real type. Split
// out from ReadDataRefDouble so a caller on the per-frame path can cache the
// handle and type once instead of paying for a string lookup every frame.
static bool ReadDataRefTyped(XPLMDataRef ref, XPLMDataTypeID types, double& out) {
    if (!ref) return false;
    if (types & xplmType_Int)        { out = (double)XPLMGetDatai(ref); return true; }
    if (types & xplmType_Float)      { out = (double)XPLMGetDataf(ref); return true; }
    if (types & xplmType_Double)     { out = XPLMGetDatad(ref); return true; }
    // arrays: read element [0] (a scalar flag is the expected shape here)
    if (types & xplmType_IntArray)   { int v = 0;   if (XPLMGetDatavi(ref, &v, 0, 1) >= 1) { out = (double)v; return true; } }
    if (types & xplmType_FloatArray) { float v = 0; if (XPLMGetDatavf(ref, &v, 0, 1) >= 1) { out = (double)v; return true; } }
    return false;
}

static bool ReadDataRefDouble(const char* path, double& out) {
    if (!path || !*path) return false;
    XPLMDataRef ref = XPLMFindDataRef(path);
    if (!ref) return false;
    return ReadDataRefTyped(ref, XPLMGetDataRefTypes(ref), out);
}

// ckpt/lights/map, resolved ONCE rather than per frame. The engine loop runs
// every frame, so calling ReadDataRefDouble (which does an XPLMFindDataRef
// string lookup plus a type query) from there would repeat that work 60-120x a
// second for a value that only needs one handle. Resolved on aircraft load and
// retried on the 1 Hz Auto loop while still unresolved, because ToLiss's own
// datarefs only appear once its plugin has started — which can be after ours.
static XPLMDataRef    gMapRheostat     = nullptr;
static XPLMDataTypeID gMapRheostatType = 0;

static void ResolveMapRheostat() {
    gMapRheostat = XPLMFindDataRef(kMapRheostatRef);
    gMapRheostatType = gMapRheostat ? XPLMGetDataRefTypes(gMapRheostat) : 0;
    if (gMapRheostat)
        Log(std::string("bound ") + kMapRheostatRef + " (type mask "
            + std::to_string((int)gMapRheostatType) + ")");
}

// AirbusFBW/DUBrightness — the per-display brightness array behind the screen
// glow. Cached on aircraft load exactly like the map rheostat, and for the same
// reason: a per-frame XPLMFindDataRef would repeat a string lookup 60-120x a
// second. ToLiss's datarefs appear only once its plugin has started, which can be
// after ours, so an unresolved handle is retried on the 1 Hz loop.
//
// The array LENGTH is queried once and kept: kScreenDU is a seed (see the note
// there), and reading past the end of a float array is undefined. An index that
// does not exist leaves its light dark rather than reading whatever follows.
static XPLMDataRef gDUBrightness      = nullptr;
static int         gDUBrightnessCount = 0;

static void ResolveDUBrightness() {
    gDUBrightness = XPLMFindDataRef(kDUBrightnessRef);
    gDUBrightnessCount = gDUBrightness
        ? XPLMGetDatavf(gDUBrightness, nullptr, 0, 0) : 0;
    if (gDUBrightness) {
        Log(std::string("bound ") + kDUBrightnessRef + " ("
            + std::to_string(gDUBrightnessCount) + " displays)");
        for (int i = 0; i < kScreenCount; ++i) {
            if (kScreenDU[i] >= gDUBrightnessCount) {
                Log("NOTE screen glow slot " + std::to_string(i) + " wants "
                    + kDUBrightnessRef + "[" + std::to_string(kScreenDU[i])
                    + "] but the array holds only "
                    + std::to_string(gDUBrightnessCount)
                    + " - that light will stay dark. Fix kScreenDU.");
            }
        }
    }
}

// Cached on aircraft load beside AirbusFBW/DUBrightness, and for the same reason:
// this is read once per frame from the flight loop and once per rect per layer
// from the panel pass, and XPLMFindDataRef is a string lookup.
//
// ⚠ THE TYPE IS QUERIED, NOT ASSUMED. XPLMGetDatavf on an INT array returns 0 —
// no error, no log line — so assuming float would leave gDcBusCount at 0 and
// DcBusLive() answering "unknown, therefore live" forever. That is this project's
// own "read a dataref with the matching getter" tripwire, and it produces exactly
// the symptom it always produces: a gate that silently never fires.
static void ResolveDcBus() {
    gDcBusRef   = XPLMFindDataRef(kDcBusRef);
    gDcBusTypes = gDcBusRef ? XPLMGetDataRefTypes(gDcBusRef) : 0;
    gDcBusCount = 0;
    if (gDcBusRef) {
        if (gDcBusTypes & xplmType_FloatArray)    gDcBusCount = XPLMGetDatavf(gDcBusRef, nullptr, 0, 0);
        else if (gDcBusTypes & xplmType_IntArray) gDcBusCount = XPLMGetDatavi(gDcBusRef, nullptr, 0, 0);
    }

    // ⚠ Logged ONCE per aircraft, not once per attempt: this is retried from the
    // 1 Hz loop, so an unconditional note would put a line a second in Log.txt
    // for the whole flight on any aircraft that genuinely lacks the dataref.
    if (gDcBusLogged) return;
    if (!gDcBusRef) return;                  // still waiting on ToLiss's plugin
    gDcBusLogged = true;
    if (!gDcBusCount) {
        Log(std::string("NOTE ") + kDcBusRef + " exists but is neither a float "
            "nor an int array (type mask " + std::to_string((int)gDcBusTypes)
            + ") - the displays will not be gated on bus voltage. They stay lit, "
            "which is the safe answer.");
    } else if (kDcBusIndex >= gDcBusCount) {
        Log(std::string("NOTE ") + kDcBusRef + "[" + std::to_string(kDcBusIndex)
            + "] is past the end of the array (" + std::to_string(gDcBusCount)
            + " buses) - the displays will not be gated. Fix kDcBusIndex.");
    } else {
        Log(std::string("bound ") + kDcBusRef + " (" + std::to_string(gDcBusCount)
            + " buses, type mask " + std::to_string((int)gDcBusTypes)
            + "); displays are live above "
            + std::to_string((int)kDcBusLiveVolts) + " V on ["
            + std::to_string(kDcBusIndex) + "]");
    }
}

// ---- the DCDU brightness buttons --------------------------------------------
// One row per command. `unit` is the DCDU (0 = captain, 1 = FO) and `step` is
// what a press does; the row's own address is the handler's refcon, so there is
// no packing to get wrong and no parallel table to keep in step.
struct DcduCommand { const char* name; int unit; int step; };
static const DcduCommand kDcduCommands[] = {
    { "AirbusFBW/CPDLC1/BrightnessUp",   0, +1 },
    { "AirbusFBW/CPDLC1/BrightnessDown", 0, -1 },
    { "AirbusFBW/CPDLC2/BrightnessUp",   1, +1 },
    { "AirbusFBW/CPDLC2/BrightnessDown", 1, -1 },
};
static const int kDcduCommandCount =
    (int)(sizeof(kDcduCommands) / sizeof(kDcduCommands[0]));
static XPLMCommandRef gDcduCmdRefs[kDcduCommandCount] = { nullptr };
static int  gDcduBound  = 0;      // how many of the four are currently registered
static bool gDcduLogged = false;  // one line per aircraft, not one per retry

// ⚠ inBefore = 1: we run BEFORE the aircraft's own handler, and that is the
// robust half of a pair of choices that only work together.
//
// It was 0 ("we are only a monitor, so stay out of the way"), which reads well
// and does not work: a *before* handler that returns 0 stops the command dead,
// and later handlers are never called at all. ToLiss owns these commands and
// implements them in a before-handler, so whether we ever hear a press was left
// resting on its return value — a fact about someone else's plugin that we
// cannot see and that can change in any update. Running first removes the
// dependency entirely.
//
// ⚠ Which is safe ONLY because it RETURNS 1. Returning 0 from here would eat
// the press: ToLiss would never dim, while our count says it did. The screen and
// its tint drift apart one press at a time and neither looks broken alone.
//
// It steps on BEGIN ONLY. A held button delivers Begin, then Continue every
// frame, then End; counting Continue would run 0..10 in a sixth of a second.
// One step per press is also the behavior that can be checked by pressing once
// and reading the dataref, which is how the two assumed constants get settled.
static const int kDcduHandlerBefore = 1;
static int DcduBrightnessHandler(XPLMCommandRef, XPLMCommandPhase phase, void* refcon) {
    if (phase == xplm_CommandBegin && refcon) {
        const DcduCommand* c = (const DcduCommand*)refcon;
        if (c->unit >= 0 && c->unit < kDcduCount) {
            int v = gDcduLevel[c->unit] + c->step;
            if (v < kDcduLevelMin) v = kDcduLevelMin;
            if (v > kDcduLevelMax) v = kDcduLevelMax;
            gDcduLevel[c->unit] = v;
        }
    }
    return 1;
}

static void UnbindDcduCommands() {
    for (int i = 0; i < kDcduCommandCount; ++i) {
        if (!gDcduCmdRefs[i]) continue;
        XPLMUnregisterCommandHandler(gDcduCmdRefs[i], DcduBrightnessHandler,
                                     kDcduHandlerBefore, (void*)&kDcduCommands[i]);
        gDcduCmdRefs[i] = nullptr;
    }
    gDcduBound = 0;
}

// Bind whichever of the four are not bound yet, and report only when the answer
// CHANGES. Safe to call repeatedly: a slot already holding a ref is skipped, so
// no command can pick up two registrations and count a press twice.
//
// ⚠ This is the retry half of the same start race that killed the bus gate, and
// it is worse here, because a command is not a dataref: no later read can heal
// it. XPLMFindCommand at aircraft-load time returned null for all four on a
// stock install — the log shows our line at 3882 and "ToLiss aircraft systems
// plugin starting up" at 3900 — so the buttons were unwatched for the entire
// session with one NOTE, twenty lines above the evidence, to say so.
static void RetryDcduCommands() {
    for (int i = 0; i < kDcduCommandCount; ++i) {
        if (gDcduCmdRefs[i]) continue;
        gDcduCmdRefs[i] = XPLMFindCommand(kDcduCommands[i].name);
        if (!gDcduCmdRefs[i]) continue;
        XPLMRegisterCommandHandler(gDcduCmdRefs[i], DcduBrightnessHandler,
                                   kDcduHandlerBefore, (void*)&kDcduCommands[i]);
        ++gDcduBound;
    }
    if (gDcduBound == kDcduCommandCount) {
        if (gDcduLogged) return;
        gDcduLogged = true;
        Log("bound the " + std::to_string(gDcduBound) + " DCDU brightness "
            "commands; level starts at " + std::to_string(kDcduLevelDefault)
            + " of " + std::to_string(kDcduLevelMax));
    }
}

static void BindDcduCommands() {
    UnbindDcduCommands();
    // The level cannot be read back, so a load is the only moment we know what
    // it is — or rather, the only moment our guess is as good as it will get.
    for (int u = 0; u < kDcduCount; ++u) gDcduLevel[u] = kDcduLevelDefault;
    gDcduLogged = false;
    RetryDcduCommands();   // usually finds nothing; AutoLoop finishes the job
}

// -1 = gate not configured/present/unreadable; 0/1 = below/above threshold
static int GateActive(const char* path, double threshold) {
    double v;
    if (!ReadDataRefDouble(path, v)) return -1;
    return v > threshold ? 1 : 0;
}

static int ResolveAutoProfile() {
    std::string icao = AircraftIcao();
    if (icao.rfind("A34", 0) == 0) return PROFILE_CLASSIC;      // A340: always Classic
    if (GateActive(kMfrlGate, kMfrlThreshold) == 1) return PROFILE_FULL_LED;
    if (icao == "A339" || icao == "A338") return PROFILE_HYBRID; // A330neo
    int shark = GateActive(kSharkletGate, kSharkletThreshold);
    if (shark == 1) return PROFILE_HYBRID;                       // sharklets
    if (shark == 0) return PROFILE_CLASSIC;                      // wingtip fences
    return kAutoFallback;
}

static void ApplyProfileToValues() {
    if (gProfile == PROFILE_CUSTOM) return;   // Custom keeps its explicit values
    int effective = (gProfile == PROFILE_AUTO) ? ResolveAutoProfile() : gProfile;
    CategoriesForNamedProfile(effective, gValues);
}

// Interior Auto MIRRORS THE RESOLVED EXTERIOR ERA (decision #15) — it needs no
// detection gate of its own, because the thing that tells you a cockpit is LED-lit
// is the same airframe generation that tells you the exterior is:
//     Classic -> Old Halogen, Hybrid LED -> New Halogen, Full LED -> LED
// An exterior on Custom has no single era to mirror, so it falls back to Old
// Halogen — the same conservative default kAutoFallback uses.
static int ResolveIntAutoProfile() {
    int ext = (gProfile == PROFILE_AUTO) ? ResolveAutoProfile() : gProfile;
    if (ext == PROFILE_FULL_LED) return INT_PROFILE_LED;
    if (ext == PROFILE_HYBRID)   return INT_PROFILE_NEW;
    return INT_PROFILE_OLD;      // Classic, Custom, anything unexpected
}

// profile enum -> the value category `idx` should carry. PER-CATEGORY, because the
// value spaces need not agree: a ternary category takes the profile at face value,
// while a two-valued one folds BOTH halogen profiles onto 0 and LED onto 1. No
// category is two-valued today (the dome was, until 2026-08-03), but routing every
// non-Custom write through here is what makes that safe to reintroduce — and it is
// what keeps an out-of-range value off a two-valued dataref, where the gate
// encoding leaves every branch drawing at once.
static int IntValueForProfile(int idx, int profile) {
    if (kIntCategories[idx].values == 2)
        return profile == INT_PROFILE_LED ? 1 : 0;
    if (profile == INT_PROFILE_NEW) return 1;
    if (profile == INT_PROFILE_LED) return 2;
    return 0;                                  // Old Halogen
}

static void ApplyIntProfileToValues() {
    if (gIntProfile == INT_PROFILE_CUSTOM) return;   // Custom keeps its values
    int effective = (gIntProfile == INT_PROFILE_AUTO) ? ResolveIntAutoProfile()
                                                      : gIntProfile;
    for (int i = 0; i < NINT; ++i) gIntValues[i] = IntValueForProfile(i, effective);
}

// What the four interior datarefs now read. The OBJ's ANIM_hide branches are the
// only consumer, and a wrong value there fails SILENTLY (the wrong branch simply
// draws), so this is the one place the truth is recorded.
static void LogIntValues(const char* why) {
    std::string s = std::string(why) + ": profile=" + std::to_string(gIntProfile);
    for (int i = 0; i < NINT; ++i)
        s += std::string(" ") + kIntCategories[i].key + "=" + std::to_string(gIntValues[i]);
    Log(s);
}

// ============================ per-livery persistence =========================
static std::string CurrentLiveryKey() {
    char fileName[512] = {0}, path[512] = {0};
    XPLMGetNthAircraftModel(0, fileName, path);
    int liveryIndex = 0;
    if (gLiveryIndexRef) liveryIndex = XPLMGetDatai(gLiveryIndexRef);
    std::string p = path[0] ? path : "?";
    return p + "#" + std::to_string(liveryIndex);
}

static fs::path ProfilesFilePath() {
    char root[512] = {0};
    XPLMGetSystemPath(root);
    return fs::path(root) / "Output" / "preferences" / "ToLissPhoton_profiles.json";
}

// migrate one parsed value into an Entry; returns false to drop it (== Auto)
static bool EntryFromJson(const json::Value& v, Entry& out) {
    if (v.type == json::Value::Obj) {
        const json::Value* prof = v.find("profile");
        out.profile = prof && prof->type == json::Value::Int ? (int)prof->i : PROFILE_AUTO;
        if (out.profile == PROFILE_CUSTOM) {
            const json::Value* cats = v.find("categories");
            out.hasCats = true;
            for (int i = 0; i < NCAT; ++i) {
                out.cats[i] = 0;
                if (cats && cats->type == json::Value::Obj) {
                    const json::Value* cv = cats->find(kCategories[i].key);
                    if (cv && cv->type == json::Value::Int && cv->i == 1) out.cats[i] = 1;
                }
            }
        }
        // Interior — additive. A file written before interior support simply has
        // neither key, and an absent interior_profile means Auto, so every old
        // prefs file loads unchanged with the interior on Auto.
        const json::Value* iprof = v.find("interior_profile");
        out.intProfile = iprof && iprof->type == json::Value::Int ? (int)iprof->i
                                                                  : INT_PROFILE_AUTO;
        if (out.intProfile == INT_PROFILE_CUSTOM) {
            const json::Value* icats = v.find("interior_categories");
            const json::Value* isch  = v.find("interior_schema");
            int schema = isch && isch->type == json::Value::Int ? (int)isch->i : 1;
            out.hasIntCats = true;
            bool haveMainPnl = false;
            for (int i = 0; i < NINT; ++i) {
                out.intCats[i] = 0;
                if (icats && icats->type == json::Value::Obj) {
                    const json::Value* cv = icats->find(kIntCategories[i].key);
                    if (cv && cv->type == json::Value::Int && cv->i >= 0 && cv->i <= 2) {
                        out.intCats[i] = (int)cv->i;
                        if (i == kIntMainPnlIndex) haveMainPnl = true;
                    }
                }
            }
            // MIGRATION: `mainpnl` split out of `map`, so a prefs file written
            // before the split has no such key. Inheriting map's value keeps the
            // cockpit looking exactly as the user left it; defaulting to 0 would
            // silently reset their MAIN PNL flood to old halogen.
            if (!haveMainPnl) out.intCats[kIntMainPnlIndex] = out.intCats[kIntMapIndex];
            // MIGRATION schema 2 -> 3, the dome going back to ternary: on the v2
            // scale 0 was fluorescent and 1 was LED, so a stored 1 read on the
            // ternary scale would come back as NEW HALOGEN. Only v2 moves — v1 is
            // the same value space as v3 and is read as it stands.
            if (schema == 2)
                out.intCats[kIntDomeIndex] = out.intCats[kIntDomeIndex] >= 1 ? 2 : 0;
            // Whatever the file said, nothing leaves here out of range.
            for (int i = 0; i < NINT; ++i)
                out.intCats[i] = ClampIntValue(i, out.intCats[i]);
        }
        return true;
    }
    // legacy bare int / numeric string (old scheme: 0 Auto, 1 LED, 2 Classic)
    long old = 0; bool have = false;
    if (v.type == json::Value::Int) { old = v.i; have = true; }
    else if (v.type == json::Value::Str) {
        const std::string& s = v.s;
        std::string t = s; size_t a = t.find_first_not_of(" \t");
        if (a != std::string::npos) t = t.substr(a);
        bool digits = !t.empty();
        for (size_t k = (t[0] == '-' ? 1 : 0); k < t.size(); ++k)
            if (!std::isdigit((unsigned char)t[k])) { digits = false; break; }
        if (digits) { old = std::strtol(t.c_str(), nullptr, 10); have = true; }
    }
    if (have && old == 1) { out.profile = PROFILE_FULL_LED; return true; }
    if (have && old == 2) { out.profile = PROFILE_CLASSIC;  return true; }
    return false;   // old 0 (Auto) or anything else => no entry
}

// The Displays settings live in the SAME file but not in a livery entry: they are
// one global choice about an effect, not a per-paint-scheme lighting profile, and
// a user who set them once would not expect to set them again on the next livery.
//
// ⚠ The key is deliberately not a legal livery key. A livery key is
// "<aircraft path>#<index>", so a leading '$' cannot collide with one — and
// LoadProfilesFile must SKIP it explicitly, because EntryFromJson would happily
// read an object with no "profile" member as a valid Auto entry and then write it
// straight back as a phantom aircraft.
static const char* kDisplaysKey = "$displays";

static void DisplaysFromJson(const json::Value& v) {
    if (v.type != json::Value::Obj) return;
    auto flag = [&](const char* key, bool& out) {
        const json::Value* p = v.find(key);
        if (p && p->type == json::Value::Int) out = p->i != 0;
    };
    auto gain = [&](const char* key, float& out) {
        const json::Value* p = v.find(key);
        // ⚠ `d`, never `i`. This parser gives EVERY number type Int and fills both
        // fields; reading `i` would truncate 0.35 to 0 and read as "the user set
        // the strength to zero", which is indistinguishable from them meaning it.
        if (p && p->type == json::Value::Int) out = ClampGain((float)p->d);
    };
    flag("enabled",    gDisplays.enabled);
    flag("spill",      gDisplays.spill);
    gain("spill_gain", gDisplays.spillGain);
    flag("screen_fx",      gDisplays.screenFx);
    gain("screen_fx_gain", gDisplays.screenFxGain);
    flag("other_fx",   gDisplays.otherFx);
}

// The cockpit light COUNT — same global treatment as the Displays block above,
// and for a sharper version of the same reason. It is a performance choice about
// this machine: a user who turned it on for a 40-plane livery folder did not
// mean "on the Air France A320 only", and per-livery it would appear to forget
// itself every time they repainted.
//
// Its own key rather than a member of "$displays" because that block is about
// the SCREENS and this is about the cockpit lamps; a reader should not have to
// know that "displays" grew a cockpit setting.
static const char* kCockpitKey = "$cockpit";

static void CockpitFromJson(const json::Value& v) {
    if (v.type != json::Value::Obj) return;
    const json::Value* p = v.find("optimized");
    if (p && p->type == json::Value::Int) gCockpit.optimized = p->i != 0;
}

static void LoadProfilesFile() {
    gProfiles.clear();
    std::ifstream f(ProfilesFilePath(), std::ios::binary);
    if (!f) return;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json::Value root;
    json::Parser parser(src);
    if (!parser.value(root) || root.type != json::Value::Obj) return;
    for (const auto& kv : root.obj) {
        if (kv.first == kDisplaysKey) { DisplaysFromJson(kv.second); continue; }
        if (kv.first == kCockpitKey)  { CockpitFromJson(kv.second);  continue; }
        Entry e;
        if (EntryFromJson(kv.second, e)) gProfiles[kv.first] = e;
    }
}

static void SaveProfilesFile() {
    fs::path path = ProfilesFilePath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // atomic-ish: write to temp then rename
    fs::path tmp = path; tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { Log("could not save profiles (open failed)"); return; }
        f << "{\n";
        bool first = true;
        // The one global block, written FIRST so it is at the top of a file that
        // may hold dozens of livery entries. Always written, unlike the livery
        // entries: "absent means default" works for a profile because Auto is a
        // real selectable state, but a Displays switch turned off has no such
        // reading — omitting it would restore it to on.
        {
            first = false;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "  \"%s\": {\"enabled\": %d, \"spill\": %d, \"spill_gain\": %.3f, "
                "\"screen_fx\": %d, \"screen_fx_gain\": %.3f, \"other_fx\": %d}",
                kDisplaysKey, gDisplays.enabled ? 1 : 0,
                gDisplays.spill ? 1 : 0, gDisplays.spillGain,
                gDisplays.screenFx ? 1 : 0, gDisplays.screenFxGain,
                gDisplays.otherFx ? 1 : 0);
            f << buf;
        }
        // The other global block. Always written, like the Displays one: "absent
        // means default" is only safe for a setting whose default is a state the
        // user can also choose on purpose, and a switch has no such reading.
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), ",\n  \"%s\": {\"optimized\": %d}",
                          kCockpitKey, gCockpit.optimized ? 1 : 0);
            f << buf;
        }
        for (const auto& kv : gProfiles) {
            if (!first) f << ",\n";
            first = false;
            const Entry& e = kv.second;
            f << "  \"" << json::escape(kv.first) << "\": {\"profile\": " << e.profile;
            if (e.profile == PROFILE_CUSTOM && e.hasCats) {
                f << ", \"categories\": {";
                for (int i = 0; i < NCAT; ++i)
                    f << (i ? ", " : "") << "\"" << kCategories[i].key << "\": " << e.cats[i];
                f << "}";
            }
            // Interior keys are written only when NOT Auto, so an exterior-only
            // user's prefs file stays byte-for-byte what it was before.
            if (e.intProfile != INT_PROFILE_AUTO) {
                f << ", \"interior_profile\": " << e.intProfile;
                if (e.intProfile == INT_PROFILE_CUSTOM && e.hasIntCats) {
                    f << ", \"interior_schema\": " << kIntPrefsSchema;
                    f << ", \"interior_categories\": {";
                    for (int i = 0; i < NINT; ++i)
                        f << (i ? ", " : "") << "\"" << kIntCategories[i].key << "\": "
                          << e.intCats[i];
                    f << "}";
                }
            }
            f << "}";
        }
        f << "\n}\n";
    }
    fs::rename(tmp, path, ec);
    if (ec) { fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec); fs::remove(tmp, ec); }
}

static void LoadProfileForCurrentLivery() {
    std::string key = CurrentLiveryKey();
    auto it = gProfiles.find(key);
    if (it == gProfiles.end()) {
        gProfile = PROFILE_AUTO;
        gIntProfile = INT_PROFILE_AUTO;
        ApplyProfileToValues();
        ApplyIntProfileToValues();
    } else {
        gProfile = it->second.profile;
        if (gProfile == PROFILE_CUSTOM && it->second.hasCats) {
            for (int i = 0; i < NCAT; ++i) gValues[i] = it->second.cats[i] == 1 ? 1 : 0;
        } else {
            ApplyProfileToValues();
        }
        gIntProfile = it->second.intProfile;
        if (gIntProfile == INT_PROFILE_CUSTOM && it->second.hasIntCats) {
            for (int i = 0; i < NINT; ++i)
                gIntValues[i] = ClampIntValue(i, it->second.intCats[i]);
        } else {
            // ApplyIntProfileToValues resolves Auto against the exterior, which
            // was set just above — so order matters here.
            ApplyIntProfileToValues();
        }
    }
    UpdateChecks();
    LogIntValues("livery loaded");
}

// Exterior and interior are INDEPENDENTLY installable and independently
// selectable, so each save must preserve the other side's fields rather than
// overwrite the whole entry. These helpers read-modify-write one entry.
// That also keeps a saved Cockpit choice intact on an aircraft without the cockpit
// mod: with no Cockpit submenu nothing can change those fields, and every exterior
// save carries them through untouched.
static Entry& CurrentEntry() {
    return gProfiles[CurrentLiveryKey()];   // default-constructs if absent
}

// Drop the entry once BOTH axes are back on Auto — absence of a key is what
// "Auto" means on disk, so leaving an all-Auto entry behind would be noise.
static void PruneIfAllAuto() {
    auto it = gProfiles.find(CurrentLiveryKey());
    if (it == gProfiles.end()) return;
    if (it->second.profile == PROFILE_AUTO && it->second.intProfile == INT_PROFILE_AUTO)
        gProfiles.erase(it);
}

static void SaveNamed(int profile) {
    Entry& e = CurrentEntry();
    e.profile = profile;
    e.hasCats = false;
    PruneIfAllAuto();
    SaveProfilesFile();
}

static void SaveCustom() {
    Entry& e = CurrentEntry();
    e.profile = PROFILE_CUSTOM;
    e.hasCats = true;
    for (int i = 0; i < NCAT; ++i) e.cats[i] = gValues[i];
    SaveProfilesFile();
}

static void SaveIntNamed(int profile) {
    Entry& e = CurrentEntry();
    e.intProfile = profile;
    e.hasIntCats = false;
    PruneIfAllAuto();
    SaveProfilesFile();
}

static void SaveIntCustom() {
    Entry& e = CurrentEntry();
    e.intProfile = INT_PROFILE_CUSTOM;
    e.hasIntCats = true;
    for (int i = 0; i < NINT; ++i) e.intCats[i] = gIntValues[i];
    SaveProfilesFile();
}

// Exterior back to Auto: clear only the exterior fields. The entry survives if
// the interior is still on an explicit profile.
static void ClearSaved() {
    auto it = gProfiles.find(CurrentLiveryKey());
    if (it == gProfiles.end()) return;
    it->second.profile = PROFILE_AUTO;
    it->second.hasCats = false;
    PruneIfAllAuto();
    SaveProfilesFile();
}

// The Displays block has no per-livery half and no pruning to do, so this is the
// whole of its save path.
static void SaveDisplays() {
    SaveProfilesFile();
    char buf[192];
    std::snprintf(buf, sizeof(buf),
        "displays: enabled=%d spill=%d (%.2f) screen fx=%d (%.2f) other fx=%d",
        gDisplays.enabled ? 1 : 0,
        gDisplays.spill ? 1 : 0, gDisplays.spillGain,
        gDisplays.screenFx ? 1 : 0, gDisplays.screenFxGain,
        gDisplays.otherFx ? 1 : 0);
    Log(buf);
}

// The cockpit light-count switch: one flag, no per-livery half, so this is the
// whole of its save path too.
//
// ⚠ EVERY writer goes through here — the menu item and the Cockpit tab's
// checkbox both — so the dataref, the prefs file and the menu check can never
// disagree. The dataref itself needs no write: its accessor reads gCockpit.
static void SetCockpitOptimized(bool on) {
    if (gCockpit.optimized == on) return;
    gCockpit.optimized = on;
    SaveProfilesFile();
    UpdateChecks();
    Log(std::string("cockpit lights: ")
        + (on ? "optimized (one light per position)" : "full stack"));
}

// Defined with the menu, below — the config windows drive profile selection
// through the same entry points the menu items do, so the two can never
// disagree about what is selected.
static void SelectProfile(int profile);
static void SelectIntProfile(int profile);

// ============================== ImGui windows ================================
// Every window is Dear ImGui on the X-Plane backend in imgui_xplm.{h,cpp}. The
// hand-rolled XPLMDrawString/XPLMDrawTranslucentDarkBox windows this replaced
// carried ~900 lines of column measuring, hit-testing and press-state tracking
// that ImGui does for free; the layouts below are the same layouts.

// A lazily created window plus its UI callback. Creation is deferred to first
// show so a session that never opens a window pays nothing, and a failed
// creation degrades to "no window" instead of a crash.
struct UiWindow {
    const char*           title;
    int                   w, h;
    std::function<void()> build;
    ImguiWindow*          win = nullptr;
};

static void ShowUiWindow(UiWindow& u) {
    if (!u.win) {
        u.win = new ImguiWindow(u.w, u.h, u.title);
        if (!u.win->Ok()) {
            Log(std::string("could not create window: ") + u.title);
            delete u.win;
            u.win = nullptr;
            return;
        }
        u.win->BuildUi = u.build;
    }
    u.win->SetVisible(true);
    u.win->BringToFront();
}

static void DestroyUiWindow(UiWindow& u) {
    delete u.win;
    u.win = nullptr;
}

// ---- the panel surface ------------------------------------------------------
// ⚠ THE ROOT WINDOW IS TRANSPARENT (ImGuiCol_WindowBg, see imgui_xplm.cpp), and
// the opaque #23282e the UI reads against is painted HERE — a child window
// filling whatever is left of the pane, one per tab.
//
// That split is the whole point: a fill on the root also covers the strip behind
// the tab bar, and the tab row is X-Plane's chrome rather than ours. Wrapping the
// CONTENT leaves the tabs sitting on the host window and gives the page under
// them a surface of its own.
//
// The bool is ImGui's "this child is entirely clipped, you may skip its
// contents". ⚠ UiEndPanel is called either way — EndChild is not optional.
static bool UiBeginPanel(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, PhotonImgui::PanelBg());
    // AlwaysUseWindowPadding: a child window is padding-less by default, and the
    // fill would then run right up to the text.
    const bool open = ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                                        ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();   // the fill is resolved by BeginChild, not at EndChild
    return open;
}
static void UiEndPanel() { ImGui::EndChild(); }

// ⚠ THE GAP UNDER A TAB BAR IS ItemSpacing.y, AND ImGui CAPTURES IT INSIDE
// BeginTabBar. `tab_bar->ItemSpacingY` is read off the style once, at BeginTabBar,
// and the cursor for every tab's content is then placed at
// `BarRect.Max.y + ItemSpacingY` — so a PushStyleVar around the tab ITEM, or
// anywhere after the bar has begun, moves nothing at all and looks like the gap
// coming from somewhere else. It has to be zeroed around this one call.
//
// Zeroed because the tab bar's 1 px underline is the top edge of the page: the
// panel meeting it is what makes the fill read as the tabs' own content rather
// than as a card floating below them. The x spacing is left alone — that one is
// ordinary widget spacing and is wanted.
static bool UiBeginTabBar(const char* id) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    const bool open = ImGui::BeginTabBar(id);
    ImGui::PopStyleVar();
    return open;
}

// ⚠ EVERY RadioButton IN THIS PLUGIN GOES THROUGH HERE, and ImGui::RadioButton is
// not to be called directly. `tests/test_ui.py` enforces it.
//
// ImGui draws the radio DOT and the checkbox TICK with the same style color,
// ImGuiCol_CheckMark — there is no separate slot for the dot. The tick is white,
// because it sits on the accent-filled ImGuiCol_CheckboxSelectedBg where an
// accent-derived mark is one blue on another; the dot sits on an ordinary
// FrameBg and wants the accent. One theme value cannot be both, so the dot's is
// pushed here rather than stated in ApplyPhotonStyle.
//
// It comes from PhotonImgui::AccentLight() — the same expression the theme's
// `light` shade uses — so retuning kUiBlue moves the dot with everything else.
static bool UiRadioButton(const char* label, bool active) {
    ImGui::PushStyleColor(ImGuiCol_CheckMark, PhotonImgui::AccentLight());
    const bool clicked = ImGui::RadioButton(label, active);
    ImGui::PopStyleColor();
    return clicked;
}

// Would a RadioButton submitted at the current cursor be under the mouse?
//
// ⚠ THE ANSWER HAS TO ARRIVE BEFORE THE WIDGET IS SUBMITTED. A label's color is
// a PushStyleColor around the call, and `IsItemHovered` only answers afterwards
// — by which time the text is drawn. The obvious workaround, remembering last
// frame's hover per row, is a frame late and tints the wrong row whenever the
// grid moves under a stationary mouse. Predicting the rect is exact and has no
// lag.
//
// ⚠ The math is RadioButton's OWN bounding box, copied from imgui_widgets.cpp,
// and it has to track it: the circle at frame height, then ItemInnerSpacing,
// then the label. It is a rect ImGui will compute again a line later — if the
// highlight ever lands beside the button rather than on it, this drifted.
//
// IsMouseHoveringRect clips to the current clip rect (here, the table cell) but
// does NOT test which window is hovered, so a popup lying over the grid can
// light a label underneath it. Cosmetic, and the popup is drawn on top anyway.
static bool UiRadioHovered(const char* label) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 ls = ImGui::CalcTextSize(label);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + ImGui::GetFrameHeight()
                        + (ls.x > 0.0f ? st.ItemInnerSpacing.x + ls.x : 0.0f),
                    p0.y + ls.y + st.FramePadding.y * 2.0f);
    return ImGui::IsMouseHoveringRect(p0, p1);
}

// A dim explanatory line. Wrapped, because these are sentences and a tool window
// gets resized.
static void UiHint(const char* fmt, ...) IM_FMTARGS(1);
static void UiHint(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextV(fmt, args);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    va_end(args);
}

// ⚠ EVERY tooltip in this plugin goes through here, and ImGui::SetTooltip is not
// to be called directly.
//
// A tooltip window auto-sizes to its content, and a paragraph with no wrap
// position is ONE line however long it is. ImGui then clamps that window to the
// host viewport — which, for us, is the X-Plane window — so the sentence is not
// re-flowed, it is CUT OFF at whichever edge it ran into. Tooltips near the right
// of the screen lose their ends, the same tooltip reads fine two hundred pixels
// to the left, and nothing anywhere reports a problem. Every explanatory string
// in this file is a paragraph, so this is not an occasional case.
//
// PushTextWrapPos inside the tooltip fixes it: the wrap position is measured from
// the window's own left edge, so the tooltip sizes itself to the wrap width and
// wraps at it.
//
// ⚠ THE WIDTH IS ALSO PART OF THE FIX, not just the wrapping. 35 em — ImGui's own
// demo value — is wider than a settings window and wide enough to run off the
// screen from a control near the right of it, at which point the clamp cuts the
// ends off again despite the wrap. 24 em is narrower than the shipping window, so
// a tooltip fits wherever its control is.
//
// The `...` overload takes a format string so callers keep reading the way they
// did; the common case passes "%s" or a plain literal.
static const float kUiTooltipEm = 24.0f;

static void UiTooltipV(const char* fmt, va_list args) {
    if (!ImGui::BeginTooltip()) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * kUiTooltipEm);
    ImGui::TextV(fmt, args);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

static void UiTooltip(const char* fmt, ...) IM_FMTARGS(1);
static void UiTooltip(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    UiTooltipV(fmt, args);
    va_end(args);
}

// The IsItemHovered + tooltip idiom, which is how nearly every call site here
// used it. Kept separate rather than folded in, because a handful of tooltips are
// attached to something other than the preceding item.
static void UiItemTooltip(const char* fmt, ...) IM_FMTARGS(1);
static void UiItemTooltip(const char* fmt, ...) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    va_list args;
    va_start(args, fmt);
    UiTooltipV(fmt, args);
    va_end(args);
}

// ========================= the settings window ===============================
// ONE window, four tabs — Exterior / Cockpit / Displays / About. It was four
// separate windows (two Custom grids and an About box), and the split cost more
// than it bought: three menu paths that each opened a different floating window,
// none of which could be compared side by side, and an About box that had to
// repeat what the others already said.
//
// The tabs are not symmetrical, deliberately:
//
//   * Exterior and Cockpit are PROFILE axes — a dropdown plus a grid of
//     per-category overrides. They share one implementation (ConfigAxis) so the
//     two cannot drift apart.
//   * Displays is not a profile axis at all: one look per effect, so it is
//     switches and strengths. See DisplaySettings.
//   * About is static text.
//
// ⚠ The COCKPIT TAB APPEARS ONLY WHERE THE COCKPIT MOD DOES, on exactly the same
// gInteriorInstalled test that decides the submenu. A tab that is present but
// inert reads as "the mod is broken"; an absent one reads as "not installed",
// which is the truth.
//
// Buttons sit in a TABLE — one column per look, measured by ImGui — and a row
// may carry fewer buttons than there are columns, in which case its first button
// absorbs the spare ones and the LAST column stays aligned down the window. No
// row does that today; the dome did while it was two-valued. See RowButtons.
static const int kMaxCols = 3;
static const int kMaxBtns = kMaxCols;   // a row can never have more buttons than columns

// Which tab the window opens on. The menu's "Custom..." / "Displays..." /
// "About..." items all open the SAME window, so each has to be able to say where.
//
// The performance tool is NOT in here: it is its own floating window, opened
// from a button at the bottom of the Displays tab. A measurement wants the
// cockpit visible and the window out of the way, which a tab in the settings
// window cannot be.
enum { kTabExterior = 0, kTabCockpit, kTabDisplays, kTabAbout, kTabCount };
// -1 = "leave whichever tab is showing alone", which is what an already-open
// window gets when the user clicks the menu item for the tab they are on.
static int gWantTab = -1;

// One profile axis. No UiWindow member any more — there is one window, and this
// describes a TAB in it.
struct ConfigAxis {
    bool        interior;    // which axis this tab edits
    int         rows;        // NCAT or NINT
    int         cols;        // grid width = the MOST looks any one row offers
};
static const ConfigAxis kExtAxis = { false, NCAT, 2 };
static const ConfigAxis kIntAxis = { true,  NINT, 3 };

// One clickable button: the dataref value it selects, its label, and the grid
// columns it covers (colFirst == colLast for the ordinary case).
struct RowButton { int value; const char* label; int colFirst, colLast; };

// The buttons of `row`, left to right. Returns how many.
static int RowButtons(const ConfigAxis& w, int row, RowButton out[kMaxBtns]) {
    if (!w.interior) {
        out[0] = {0, kCategories[row].classic, 0, 0};
        out[1] = {1, "LED", 1, 1};
        return 2;
    }
    const IntCategory& c = kIntCategories[row];
    int n = c.values < w.cols ? c.values : w.cols;
    int span = w.cols - n;              // extra columns the first button absorbs
    out[0] = {0, c.valueLabel[0], 0, span};
    for (int i = 1; i < n; ++i) {
        int col = span + i;
        out[i] = {i, c.valueLabel[i], col, col};
    }
    return n;
}

static const char* RowLabel(const ConfigAxis& w, int row) {
    return w.interior ? kIntCategories[row].label : kCategories[row].label;
}

static int RowCurrentValue(const ConfigAxis& w, int row) {
    return w.interior ? gIntValues[row] : gValues[row];
}

// Toggling ANY category switches that axis to Custom and persists all of its
// categories — but only that axis. Changing a cockpit light must not knock the
// exterior off Auto.
static void SetCategoryValue(const ConfigAxis& w, int row, int want) {
    if (want == RowCurrentValue(w, row)) return;
    if (w.interior) {
        gIntValues[row] = ClampIntValue(row, want);
        gIntProfile     = INT_PROFILE_CUSTOM;
        SaveIntCustom();
        Log(std::string("custom cockpit: ") + kIntCategories[row].key
            + "=" + std::to_string(want));
    } else {
        gValues[row] = want;            // 0 = classic (Halogen/Xenon), 1 = LED
        gProfile     = PROFILE_CUSTOM;
        SaveCustom();
        Log(std::string("custom: ") + kCategories[row].key + "=" + std::to_string(want));
    }
    UpdateChecks();
}

// ---- the profile dropdown ---------------------------------------------------
// A combo rather than the row of radio buttons this replaced. Five choices across
// the top of a 430-px window left no room for the "* Custom" state marker, and a
// dropdown also gives the Auto row somewhere to say what it resolved to.
static const char* ProfileName(bool interior, int value) {
    const ProfileItem* items = interior ? kIntProfileItems : kProfileItems;
    const int n = interior ? NINTPROFILE : NPROFILE;
    for (int i = 0; i < n; ++i) if (items[i].value == value) return items[i].label;
    return "Custom";
}

// "Auto (Hybrid LED)". The parenthetical is what Auto WOULD choose right now, read
// from the 1 Hz cache — so it is live: load an airframe with sharklets and the
// label follows within a second, whether or not Auto is the selected profile.
// That is the whole point of showing it.
static std::string ProfileComboLabel(bool interior, int value) {
    const char* name = ProfileName(interior, value);
    const bool isAuto = value == (interior ? INT_PROFILE_AUTO : PROFILE_AUTO);
    if (!isAuto) return name;
    const int resolved = interior ? gAutoIntResolved : gAutoExtResolved;
    return std::string(name) + " (" + ProfileName(interior, resolved) + ")";
}

static void BuildProfileCombo(const ConfigAxis& w) {
    const int  cur  = w.interior ? gIntProfile : gProfile;
    const int  n    = w.interior ? NINTPROFILE : NPROFILE;
    const int  customValue = w.interior ? INT_PROFILE_CUSTOM : PROFILE_CUSTOM;
    const bool isCustom    = cur == customValue;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Profile");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);

    const std::string preview = ProfileComboLabel(w.interior, cur);
    if (ImGui::BeginCombo("##profile", preview.c_str())) {
        for (int i = 0; i < n; ++i) {
            const int value = w.interior ? kIntProfileItems[i].value : kProfileItems[i].value;
            const std::string label = ProfileComboLabel(w.interior, value);
            if (ImGui::Selectable(label.c_str(), cur == value) && cur != value) {
                if (w.interior) SelectIntProfile(value);
                else            SelectProfile(value);
            }
            if (cur == value) ImGui::SetItemDefaultFocus();
        }
        // Custom is listed ONLY while it is the current state. It is not a look
        // you can ask for — it is what changing a row below puts the axis into —
        // and an always-present "Custom" row would raise "custom of what?" and
        // then do nothing when clicked. Listed here so the closed combo is never
        // blank and never names a profile that is not in force.
        if (isCustom) {
            ImGui::Separator();
            ImGui::Selectable("Custom", true);
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (isCustom) {
        ImGui::SameLine();
        ImGui::TextDisabled("(from the rows below)");
    }
}

// The cockpit's one performance control. Scoped like every other row helper —
// an ImGui item's ID is its label hashed with the ID stack, and this pane is
// built from two places (the settings window and the Dev window's Cockpit tab).
static void BuildCockpitPerformance() {
    ImGui::PushID("cockpit-perf");
    ImGui::Spacing();
    ImGui::SeparatorText("Performance");
    ImGui::Spacing();
    bool on = gCockpit.optimized;
    if (ImGui::Checkbox("Use simplified lighting", &on)) SetCockpitOptimized(on);
    UiItemTooltip(
        "Lowers lighting fidelity for slightly better GPU performance. "
        "Results may vary.");
    ImGui::PopID();
}

static void BuildConfigTab(const ConfigAxis& w) {
    ImGui::Spacing();
    BuildProfileCombo(w);
    ImGui::Spacing();
    // No hint under this heading. "Changing any row below switches this axis to
    // Custom" described the mechanism rather than the choice, and the combo
    // already shows it happening — it flips to Custom with "(from the rows
    // below)" beside it the moment a row is touched.
    ImGui::SeparatorText("Configure");
    ImGui::Spacing();

    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchSame
                                | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_BordersInnerV;
    if (!ImGui::BeginTable("cats", 1 + w.cols, flags)) return;

    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    for (int c = 0; c < w.cols; ++c) ImGui::TableSetupColumn("");

    for (int row = 0; row < w.rows; ++row) {
        ImGui::PushID(row);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(RowLabel(w, row));

        RowButton btn[kMaxBtns];
        const int n   = RowButtons(w, row, btn);
        const int cur = RowCurrentValue(w, row);
        for (int i = 0; i < n; ++i) {
            ImGui::TableSetColumnIndex(1 + btn[i].colFirst);
            ImGui::PushID(i);
            // ⚠ The LABEL carries the selection too, not just the dot. Which look
            // each row is on is the only thing this grid exists to say, and with
            // every label at full brightness the answer is a filled circle the
            // eye has to hunt for row by row. Dimming the alternatives puts the
            // current state on the words. TextDisabled rather than a color of its
            // own — it is the same "not in play" gray as every other pane, and it
            // follows the theme.
            //
            // Hover lights a label as well, so a dimmed option answers "is this
            // clickable?" the moment the mouse is on it. See UiRadioHovered for
            // why that takes a predicted rect and not IsItemHovered.
            const bool on  = cur == btn[i].value;
            const bool lit = on || UiRadioHovered(btn[i].label);
            if (!lit)
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            if (UiRadioButton(btn[i].label, on))
                SetCategoryValue(w, row, btn[i].value);
            // Popped before the tooltip below, which wants ordinary text.
            if (!lit) ImGui::PopStyleColor();
            if (btn[i].colFirst != btn[i].colLast && ImGui::IsItemHovered())
                UiTooltip("One look covering more than one era - this fitting "
                          "does not distinguish them.");
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();

    // The light-COUNT switch, on the Cockpit tab only. It sits under its own
    // heading rather than as a sixth row of the grid above, because it is not a
    // look: every row up there answers "which era?", and this one answers "how
    // many lights may that cost?". Put in the grid it would read as a profile
    // the user had never heard of.
    if (w.interior) BuildCockpitPerformance();
}

// ---- the Displays tab -------------------------------------------------------
// Not a profile axis: each effect has one look, so the only controls are on/off
// and strength. Written straight to gDisplays and persisted immediately, the same
// way toggling a category row is.
static void SaveDisplays();          // defined with the rest of persistence, above
// The performance pane, defined far below with the levers it moves — the Displays
// switches, the FX compositor and the waveform engine are all declared after this
// point. `dev` adds the internal levers and the full sweep.
static void BuildPerfTab(bool dev);
// Its window, defined with the settings window just below. The only way in is the
// button at the bottom of this tab.
static void ShowPerfWindow();
// "Is there anything for the two effect switches to draw?" — a function rather
// than the two globals it reads (gFxEnabled, gFxStacks), because those live with
// the compositor a couple of thousand lines below and gFxStacks is a vector of a
// type that is not declared yet.
static bool PanelFxLoaded();

// A checkbox with a strength slider that is only live while it is ticked. Grayed
// rather than hidden: a control that vanishes makes the rows below it jump under
// the mouse, and it hides the value the user set before turning the effect off.
//
// ⚠ PushID(label) is load-bearing. Every row's slider is called "Strength", and an
// ImGui widget's ID is its label hashed with the current ID stack — so two visible
// rows collide, ImGui raises "2 visible items with conflicting ID", and dragging
// one slider moves the other. Scoping on the CHECKBOX's label keeps the sliders
// distinct without putting a different word in front of each of them.
static bool DisplayToggleRow(const char* label, const char* help,
                             bool* on, float* gain) {
    ImGui::PushID(label);
    bool dirty = ImGui::Checkbox(label, on);
    if (help) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) UiTooltip("%s", help);
    }
    if (gain) {
        ImGui::Indent();
        ImGui::BeginDisabled(!*on);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Strength", gain, 0.0f, 1.0f, "%.2f")) {
            *gain = ClampGain(*gain);
            dirty = true;
        }
        ImGui::EndDisabled();
        ImGui::Unindent();
    }
    ImGui::PopID();
    return dirty;
}

static void BuildDisplaysTab() {
    // Scoped because this pane is built from TWO places — the settings window and
    // the Dev window's Displays tab — and the master checkbox below carries a
    // literal label rather than going through DisplayToggleRow's PushID. Cheap
    // insurance whatever the call sites are: see tests/test_ui.py.
    ImGui::PushID("displays");
    ImGui::Spacing();
    UiHint("Adjust the appearance and effects used on displays and screens in "
           "the cockpit.");
    ImGui::Spacing();

    bool dirty = false;
    // The master switch, ABOVE the first separator so it reads as covering the
    // page rather than as another Screens row. Everything below is grayed while
    // it is off — grayed rather than hidden, for the same reason a strength
    // slider is: a page that empties out looks broken, and it hides the settings
    // the user will want back when they switch it on again.
    if (ImGui::Checkbox("Enable display effects", &gDisplays.enabled)) dirty = true;
    UiItemTooltip("Master switch for everything on this page. Off is the stock "
                  "ToLiss cockpit: no glow from the screens, no tinting on any "
                  "readout. The settings below are remembered.");

    ImGui::BeginDisabled(!gDisplays.enabled);
    ImGui::Spacing();
    ImGui::SeparatorText("Screens");
    dirty |= DisplayToggleRow(
        "Enable backlight illumination",
        "Backlight illumination casts light from the screens into the cockpit. "
        "Note: this may have a slight effect on performance.",
        &gDisplays.spill, &gDisplays.spillGain);
    ImGui::Spacing();
    dirty |= DisplayToggleRow(
        "Enable backlight visual effect",
        "The backlight visual effect makes displays slightly bluish-gray and "
        "adds a backlight bleed effect on LCD screens. It's most visible in dark "
        "cockpit environments.",
        &gDisplays.screenFx, &gDisplays.screenFxGain);

    ImGui::Spacing();
    ImGui::SeparatorText("Other displays");
    dirty |= DisplayToggleRow(
        "Enable modified display colors",
        "Modifies the color of all other digital screens.",
        &gDisplays.otherFx, nullptr);
    ImGui::EndDisabled();

    if (!PanelFxLoaded()) {
        ImGui::Spacing();
        UiHint("No panel effect definition is loaded, so the two effect switches "
               "above have nothing to draw. Reinstalling Photon restores "
               "panelfx.txt in the plugin folder.");
    }

    // The way in to the performance tool. It sits here rather than on the menu
    // because what it measures is this page: the switches above are the levers
    // it moves. NOT inside the BeginDisabled block — a user who has turned the
    // master off is entitled to measure that.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Performance analysis...")) ShowPerfWindow();
    UiItemTooltip("Opens a separate window that times the cockpit with these "
                  "effects on and off, so you can see what they cost on this "
                  "machine.");

    ImGui::PopID();
    if (dirty) SaveDisplays();
}

// ================================ About tab ==================================
// Static text plus three link buttons. The links are the same ones the installer
// offers on its final screen (installer/constants.py) — keep them in step.
//
// There is no XPLM call to open a URL, so this hands off to the platform's own
// opener. ShellExecute is linked here with a #pragma rather than in CMakeLists so
// the dependency stays next to its only use.
#if defined(_WIN32)
  #include <shellapi.h>
  #pragma comment(lib, "shell32.lib")
#endif

static void OpenUrl(const char* url) {
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = std::string("open \"") + url + "\" &";
    std::system(cmd.c_str());
#else
    std::string cmd = std::string("xdg-open \"") + url + "\" >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#endif
    Log(std::string("opened URL: ") + url);
}

struct AboutLink { const char* label; const char* url; };
static const AboutLink kAboutLinks[] = {
    {"X-Plane.org Page",             kOrgUrl},
    {"GitHub Project",               kGitHubUrl},
    {"Gus Rodrigues on X-Plane.org", kGusUrl},
};
static const int NABOUTLINK = (int)(sizeof(kAboutLinks) / sizeof(kAboutLinks[0]));

// The people who flew this before anyone else could. Alphabetical, case-insensitively
// — it is a thank-you list and not a ranking, and any other order invites the
// question of what the order means.
static const char* const kTesters[] = {
    "aaronfromib", "blackbirdsmall", "konur", "Lucinaro",
    "Ryjano", "teropa", "The Almanac",
};
static const int NTESTER = (int)(sizeof(kTesters) / sizeof(kTesters[0]));

static void BuildAboutTab() {
    ImGui::Spacing();
    ImGui::TextUnformatted("ToLiss Photon Lighting");
    ImGui::SameLine();
    ImGui::TextDisabled("v%s", kPhotonVersion);
    ImGui::TextUnformatted("A comprehensive lighting plugin for ToLiss aircraft.");

    ImGui::Spacing();
    ImGui::SeparatorText("Credits");
    ImGui::TextUnformatted("Photon project created by Schmal.");
    ImGui::TextUnformatted("Cockpit lighting created by Gus Rodrigues. (Thank you, Gus!)");

    ImGui::Spacing();
    ImGui::TextUnformatted("Special thanks to our testers and contributors:");
    // TWO COLUMNS, and BulletText rather than a bullet character: the UI font is
    // built with ImGui's default (Latin-1) glyph range, so U+2022 would come out
    // as a missing-glyph box. BulletText draws the dot as geometry instead.
    // Seven names down one column also made this tab the tallest of the four and
    // pushed the link buttons out of a 430-px window.
    if (ImGui::BeginTable("testers", 2, ImGuiTableFlags_SizingStretchSame)) {
        for (int i = 0; i < NTESTER; ++i) {
            if (i % 2 == 0) ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(i % 2);
            ImGui::BulletText("%s", kTesters[i]);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    // STACKED, not in a row. Side by side these three set the window's minimum
    // useful width all on their own — wider than any other tab needs — so the
    // window could not be narrowed to sit beside a cockpit view.
    for (int i = 0; i < NABOUTLINK; ++i)
        if (ImGui::Button(kAboutLinks[i].label, ImVec2(280.0f, 0.0f)))
            OpenUrl(kAboutLinks[i].url);

    ImGui::Spacing();
    UiHint("Not affiliated with or endorsed by ToLiss.");
}

// ---- the window -------------------------------------------------------------
// A tab is opened programmatically by setting gWantTab and letting the frame that
// follows pass ImGuiTabItemFlags_SetSelected. There is no SetSelectedTab call to
// use instead, which is why this is a one-shot flag rather than a stored mode: a
// mode would re-select the tab every frame and the user could not leave it.
static ImGuiTabItemFlags TabFlags(int tab) {
    return gWantTab == tab ? ImGuiTabItemFlags_SetSelected : 0;
}

static void BuildMainUi() {
    if (!UiBeginTabBar("photon_tabs")) return;

    // Each tab's content sits on its own panel — see UiBeginPanel. The tab bar
    // itself deliberately does not.
    if (ImGui::BeginTabItem("Exterior", nullptr, TabFlags(kTabExterior))) {
        if (UiBeginPanel("##pane")) BuildConfigTab(kExtAxis);
        UiEndPanel();
        ImGui::EndTabItem();
    }
    // Present only where the cockpit mod is — see the block comment above.
    if (gInteriorInstalled
        && ImGui::BeginTabItem("Cockpit", nullptr, TabFlags(kTabCockpit))) {
        if (UiBeginPanel("##pane")) BuildConfigTab(kIntAxis);
        UiEndPanel();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Displays", nullptr, TabFlags(kTabDisplays))) {
        if (UiBeginPanel("##pane")) BuildDisplaysTab();
        UiEndPanel();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("About", nullptr, TabFlags(kTabAbout))) {
        if (UiBeginPanel("##pane")) BuildAboutTab();
        UiEndPanel();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();

    // Cleared after the whole bar, not inside a tab: the flag has to survive
    // until the BeginTabItem that wants it, and that may be the last one.
    gWantTab = -1;
}

static UiWindow gMainWin = { "ToLiss Photon", 520, 430, BuildMainUi, nullptr };

// `tab` may be -1 for "wherever it was". Every menu item that opens this window
// names a tab, so -1 is only for a caller that genuinely has no opinion.
static void ShowMainWindow(int tab) {
    if (tab >= 0 && tab < kTabCount) gWantTab = tab;
    ShowUiWindow(gMainWin);
}
static void DestroyMainWindow() { DestroyUiWindow(gMainWin); }

// ---- the performance window -------------------------------------------------
// Its own floating window rather than a fifth tab, because a run is watched
// against the cockpit: the settings window can be closed or moved aside while
// this one stays where the frame counter is readable. It is opened from the
// button at the bottom of the Displays tab and from nowhere else — there is no
// menu item, for the same reason there was no menu item when it was a tab.
//
// Untabbed, so it paints its own panel: with ImGuiCol_WindowBg transparent, a
// pane that does not call UiBeginPanel is text floating over the cockpit.
static UiWindow gPerfWin = {
    "ToLiss Photon - Performance", 560, 640,
    [] { if (UiBeginPanel("##pane")) BuildPerfTab(false); UiEndPanel(); },
    nullptr,
};
static void ShowPerfWindow()    { ShowUiWindow(gPerfWin); }
static void DestroyPerfWindow() { DestroyUiWindow(gPerfWin); }


// The Dev submenu is built by the dev-tooling block further down. In a shipping
// build these are the no-op versions, so the menu code below needs no #if of its
// own and there is no Dev row at all.
#if PHOTON_DEV
static void AppendDebugMenu(XPLMMenuID parent);
static void DestroyDebugMenu();
static void DestroyDebugImguiWindows();
#else
static void AppendDebugMenu(XPLMMenuID) {}
static void DestroyDebugMenu() {}
static void DestroyDebugImguiWindows() {}
#endif

// ================================== menu =====================================
static void SelectProfile(int profile) {
    gProfile = profile;
    ApplyProfileToValues();
    if (profile == PROFILE_AUTO) ClearSaved();   // Auto is never persisted
    else SaveNamed(profile);
    UpdateChecks();
}

static void SelectIntProfile(int profile) {
    gIntProfile = profile;
    ApplyIntProfileToValues();
    SaveIntNamed(profile);   // SaveIntNamed prunes the entry when both axes are Auto
    UpdateChecks();
    LogIntValues("interior profile selected");
}

// One handler per submenu, so the refcon is just an index into that submenu's
// item table — plus one sentinel for the "Custom..." item at the bottom. The
// sentinel is well clear of any real index, so a stray refcon can't select a
// profile by accident.
static const intptr_t kMenuCustom = 1000;

static void ExteriorMenuHandler(void*, void* itemRef) {
    intptr_t i = (intptr_t)itemRef;
    if (i == kMenuCustom) { ShowMainWindow(kTabExterior); return; }
    if (i < 0 || i >= NPROFILE) return;
    SelectProfile(kProfileItems[i].value);
}

static void InteriorMenuHandler(void*, void* itemRef) {
    intptr_t i = (intptr_t)itemRef;
    if (i == kMenuCustom)    { ShowMainWindow(kTabCockpit); return; }
    if (i < 0 || i >= NINTPROFILE) return;
    SelectIntProfile(kIntProfileItems[i].value);
}

// The top level's two clickable items — Displays... and About... — both open the
// one settings window, on their own tab. The refcon carries the tab index
// directly; there is no submenu here to index into, so nothing else needs it.
static void MenuHandler(void*, void* itemRef) {
    const intptr_t tab = (intptr_t)itemRef;
    ShowMainWindow(tab >= 0 && tab < kTabCount ? (int)tab : kTabAbout);
}

static void UpdateChecks() {
    if (!gMenuCreated) return;
    if (gExtMenuID) {
        for (int i = 0; i < NPROFILE; ++i)
            XPLMCheckMenuItem(gExtMenuID, gProfileItemIndex[i],
                              kProfileItems[i].value == gProfile ? xplm_Menu_Checked
                                                                 : xplm_Menu_Unchecked);
        // Custom is a real profile, so its menu item carries the check when the
        // axis is on it — otherwise selecting Custom would leave the submenu
        // showing nothing checked at all.
        if (gExtCustomItemIndex >= 0)
            XPLMCheckMenuItem(gExtMenuID, gExtCustomItemIndex,
                              gProfile == PROFILE_CUSTOM ? xplm_Menu_Checked
                                                         : xplm_Menu_Unchecked);
    }
    if (gIntMenuID) {
        for (int i = 0; i < NINTPROFILE; ++i)
            XPLMCheckMenuItem(gIntMenuID, gIntProfileItemIndex[i],
                              kIntProfileItems[i].value == gIntProfile ? xplm_Menu_Checked
                                                                       : xplm_Menu_Unchecked);
        if (gIntCustomItemIndex >= 0)
            XPLMCheckMenuItem(gIntMenuID, gIntCustomItemIndex,
                              gIntProfile == INT_PROFILE_CUSTOM ? xplm_Menu_Checked
                                                                : xplm_Menu_Unchecked);
    }
    // the Custom windows read gValues/gIntValues live each frame — nothing to sync
}

static void CreatePhotonMenu() {   // not CreateMenu: collides with the Win32 API
    // Plugins > ToLiss Photon > Exterior > Auto
    //                         |            ─────────
    //                         |            Classic / Hybrid LED / Full LED
    //                         |            ─────────
    //                         |            Custom...
    //                         > Cockpit  > Auto
    //                         |            ─────────
    //                         |            Old Halogen / New Halogen / LED
    //                         |            ─────────
    //                         |            Custom...
    //                         > Displays...
    //                         ─────────
    //                         > About...
    //
    // The three AXES sit together — Exterior, Cockpit and Displays are the same
    // kind of thing, and the separator belongs under them rather than between
    // them. About is what is left over. (A dev build appends its own separator
    // and Dev submenu below About; the shipping build ends at About, which is
    // why that separator lives in AppendDebugMenu and not here.)
    //
    // The separator under Auto sets it apart as the only ADAPTIVE choice: it
    // picks an era from the airframe rather than naming one, so it isn't a peer
    // of the three fixed looks below it.
    //
    // ⚠ DISPLAYS IS A FLAT ITEM, not a submenu, because it has no profiles to
    // list: its controls are switches and strengths, which is a window shape and
    // not a menu shape. Same reason Custom is a window.
    //
    // Nesting is deliberate: CLAUDE.md's "no menu level 2+" rule is an XPPython3
    // 4.7a1 defect and binds PI_PhotonDevReload.py, not this native plugin.
    gPluginsMenuItem = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "ToLiss Photon", nullptr, 0);
    gMenuID = XPLMCreateMenu("ToLiss Photon", XPLMFindPluginsMenu(), gPluginsMenuItem,
                             MenuHandler, nullptr);

    int extItem = XPLMAppendMenuItem(gMenuID, "Exterior", nullptr, 0);
    gExtMenuID = XPLMCreateMenu("Exterior", gMenuID, extItem, ExteriorMenuHandler, nullptr);
    for (int i = 0; i < NPROFILE; ++i) {
        gProfileItemIndex[i] = XPLMAppendMenuItem(gExtMenuID, kProfileItems[i].label,
                                                 (void*)(intptr_t)i, 0);
        if (kProfileItems[i].value == PROFILE_AUTO) XPLMAppendMenuSeparator(gExtMenuID);
    }
    XPLMAppendMenuSeparator(gExtMenuID);
    gExtCustomItemIndex = XPLMAppendMenuItem(gExtMenuID, "Custom...",
                                             (void*)kMenuCustom, 0);

    // The Cockpit submenu exists ONLY where the cockpit mod does — omitted, not
    // grayed out. Everything downstream keys off gIntMenuID staying null:
    // UpdateChecks skips the interior block, and with no menu item there is no
    // path to InteriorMenuHandler or the Cockpit Custom window.
    gMenuHasInterior = gInteriorInstalled;
    if (gMenuHasInterior) {
        int intItem = XPLMAppendMenuItem(gMenuID, "Cockpit", nullptr, 0);
        gIntMenuID = XPLMCreateMenu("Cockpit", gMenuID, intItem, InteriorMenuHandler, nullptr);
        for (int i = 0; i < NINTPROFILE; ++i) {
            gIntProfileItemIndex[i] = XPLMAppendMenuItem(gIntMenuID, kIntProfileItems[i].label,
                                                        (void*)(intptr_t)i, 0);
            if (kIntProfileItems[i].value == INT_PROFILE_AUTO) XPLMAppendMenuSeparator(gIntMenuID);
        }
        XPLMAppendMenuSeparator(gIntMenuID);
        gIntCustomItemIndex = XPLMAppendMenuItem(gIntMenuID, "Custom...",
                                                 (void*)kMenuCustom, 0);
        // The simplified-lighting toggle is NOT here: it lives on the Cockpit
        // tab's Performance section only (2026-08-09). A checkable row below
        // Custom... read as a sixth profile nobody had heard of.
    }

    // Both go to MenuHandler, whose refcon IS the tab index.
    XPLMAppendMenuItem(gMenuID, "Displays...", (void*)(intptr_t)kTabDisplays, 0);
    XPLMAppendMenuSeparator(gMenuID);
    XPLMAppendMenuItem(gMenuID, "About...",    (void*)(intptr_t)kTabAbout, 0);

    // Dev builds only — a no-op in the shipping .xpl, so there is no Dev row to
    // hide or gray out. Same rule that keeps the magenta out of a release. It
    // brings its OWN separator: a release must not end on a trailing divider.
    AppendDebugMenu(gMenuID);

    gMenuCreated = true;
    UpdateChecks();
    Log(gMenuHasInterior ? "menu built (Exterior / Cockpit submenus)"
                         : "menu built (Exterior only - cockpit mod not installed)");
}

static void DestroyPhotonMenu() {   // not DestroyMenu: collides with the Win32 API
    // ⚠ The settings window goes with the menu, and that is what makes the
    // Cockpit tab honest: the menu is REBUILT when the cockpit mod's presence
    // changes between two ToLiss aircraft (UpdateMenuVisibility), so the window
    // is recreated with it and cannot linger showing a tab for a mod that is no
    // longer there.
    DestroyMainWindow();
    DestroyPerfWindow();
    DestroyDebugImguiWindows();
    // Destroy the children before the parent: XPLMDestroyMenu on the parent
    // invalidates the submenu IDs it owns.
    if (gExtMenuID) { XPLMDestroyMenu(gExtMenuID); gExtMenuID = nullptr; }
    if (gIntMenuID) { XPLMDestroyMenu(gIntMenuID); gIntMenuID = nullptr; }
    DestroyDebugMenu();
    if (gMenuID) { XPLMDestroyMenu(gMenuID); gMenuID = nullptr; }
    // stale indices must not be re-checked
    gExtCustomItemIndex = gIntCustomItemIndex = -1;
    if (gPluginsMenuItem >= 0) {
        XPLMRemoveMenuItem(XPLMFindPluginsMenu(), gPluginsMenuItem);
        gPluginsMenuItem = -1;
    }
    gMenuCreated = false;
    gMenuHasInterior = false;
}

// =========================== ToLiss detection ================================
static bool IsToLiss() {
    char fileName[512] = {0}, path[512] = {0};
    XPLMGetNthAircraftModel(0, fileName, path);
    std::string blob = std::string(path) + "|" + fileName;
    for (char& c : blob) c = (char)std::tolower((unsigned char)c);
    return blob.find("toliss") != std::string::npos;
}

// ======================= cockpit-mod ("Gus Mod") detection ===================
// Does <aircraft>/objects/lights_inn.obj bind our interior datarefs? See
// kInteriorObjNeedle for why that is the test. The OBJ is ~13 KB either way, so
// this reads it whole; the cap only guards against an absurd file.
static bool InteriorObjIsModded(const fs::path& obj) {
    std::error_code ec;
    if (!fs::is_regular_file(obj, ec)) return false;   // clears ec on success
    uintmax_t size = fs::file_size(obj, ec);
    if (ec || size == 0 || size > kInteriorObjMaxBytes) return false;
    std::ifstream f(obj, std::ios::binary);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return text.find(kInteriorObjNeedle) != std::string::npos;
}

// Re-detected per aircraft load, never per frame. XPLM_USE_NATIVE_PATHS is
// enabled in XPluginStart, so the model path is a real filesystem path.
static void DetectInterior() {
    char fileName[512] = {0}, path[512] = {0};
    XPLMGetNthAircraftModel(0, fileName, path);
    gInteriorInstalled = false;
    if (path[0]) {
        fs::path obj = fs::path(path).parent_path() / "objects" / kInteriorObjName;
        gInteriorInstalled = InteriorObjIsModded(obj);
    }
    Log(gInteriorInstalled ? "cockpit mod detected (lights_inn.obj binds our datarefs)"
                           : "cockpit mod NOT installed - hiding the Cockpit menu");
}

// Defined far below, with the panel-FX drivers, and called from the aircraft-load
// path, which is far above. See the call for why it must happen there.
//
// ⚠ NOT dev-only, and it was for one release cycle by accident: the compositor
// moved into the shipping build but this call site stayed behind the guard, so a
// release held ToLiss dataref handles across an aircraft change. Every symptom of
// that is a crash in someone else's plugin.
static void FxForgetDrivers();
// The DCDU brightness the buttons have been pressed to; rebound with the aircraft
// because the commands belong to ToLiss's plugin.
static void BindDcduCommands();
// AirbusFBW/DisplayResolutionFallback — which of the two DU rect sets to paint.
// A ToLiss handle, so it is rebound with the aircraft like the ones above.
//
// ⚠ The two globals live HERE, with the aircraft-lifecycle code that drops and
// rebinds them, rather than down beside PanelRects() where they are read. That
// is the whole point: they were declared next to their reader, which is why
// nobody looking at the aircraft-load path ever noticed they belonged in it.
static XPLMDataRef gDisplayFallbackRef = nullptr;
static bool        gDisplayFallbackLogged = false;
static void ResolveDisplayFallback();
#if PHOTON_DEV
static void DbgLoadManifest();
// Defined with the rest of the tuning file, far below the two places that must
// call it: the aircraft load (which reseeds every light and would otherwise
// discard the session) and XPluginDisable.
static void DbgAutoSaveTuning();
static void DbgLoadTuning();
#endif

static void UpdateMenuVisibility() {
    if (IsToLiss()) {
        DetectInterior();
        // The submenu is decided at menu-BUILD time, so switching between two
        // ToLiss aircraft that disagree about the cockpit mod has to rebuild —
        // otherwise it lingers on one without the mod or stays missing on one with.
        if (gMenuCreated && gMenuHasInterior != gInteriorInstalled) DestroyPhotonMenu();
        if (!gMenuCreated) CreatePhotonMenu();
        ResolveGlowMap();      // pick the skin-glow index map for this airframe
        // Cached whether or not the mod is installed: gating it would turn a
        // detection false negative from "no menu" into "the map spot is black".
        ResolveMapRheostat();  // cache ckpt/lights/map off the per-frame path
        ResolveDUBrightness(); // and AirbusFBW/DUBrightness, for the screen glow
        // ⚠ The screen glow's authored sizes are captured from the OBJ that is
        // being replaced right now, so they have to go with it: a rebuilt
        // lights_screens.obj may bake a different `size:`, and a stale capture
        // would scale every screen against the previous build's reach with
        // nothing to show for it but a glow that is quietly the wrong size.
        for (int i = 0; i < kScreenCount; ++i) gScreenBakedSeen[i] = false;
        // Each aircraft gets one line about the bus, so clear the sticky flag
        // before resolving rather than after: the 1 Hz retry below shares it.
        gDcBusLogged = false;
        ResolveDcBus();        // and AirbusFBW/DCBusVoltages, which gates both
        // ⚠ Same rule, and it is a ToLiss handle for the same reason: kept across
        // a reload it points into an unloaded plugin, and what it reads back
        // decides which rect set the six DUs paint from. Sticky log flag cleared
        // first, exactly like the bus above, because the 1 Hz retry shares it.
        gDisplayFallbackLogged = false;
        ResolveDisplayFallback();
        // ⚠ Every panel-FX driver handle points into the PREVIOUS aircraft's
        // plugin. ToLiss's datarefs are unregistered with it, so a handle kept
        // across the swap is the one way this feature could take the sim down
        // rather than merely look wrong.
        FxForgetDrivers();
        // Same hazard, same fix, for the four CPDLC brightness COMMANDS: an
        // XPLMCommandRef into an unloaded plugin is a dangling handler
        // registration, not merely a stale read.
        BindDcduCommands();
#if PHOTON_DEV
        // ⚠ BEFORE the reload below, which reseeds every light from the manifest
        // and is therefore the moment a session's edits would vanish. Writes
        // nothing when nothing was touched, so it cannot blank an earlier file.
        DbgAutoSaveTuning();
        // The debug-light manifest belongs to the aircraft that just loaded, and
        // its OBJ starts reading those datarefs immediately — with the Dev window
        // shut. Loading it lazily when the Lights tab opens would leave every
        // debug light black until someone happened to click that tab.
        DbgLoadManifest();
#endif
    } else {
        if (gMenuCreated) DestroyPhotonMenu();
        gInteriorInstalled = false;
        gGlowMapActive = false;
        gMapRheostat = nullptr;
        gMapRheostatType = 0;
        gDUBrightness = nullptr;
        gDUBrightnessCount = 0;
        gDcBusRef = nullptr;
        gDcBusTypes = 0;
        gDcBusCount = 0;
        gDcBusLogged = false;
        gDisplayFallbackRef = nullptr;   // null = full-res, which is the safe set
        gDisplayFallbackLogged = false;
        // Dropped on the way OUT of a ToLiss too. Everything above is a stale
        // READ if it is kept; these two are handles into a plugin that is being
        // unloaded, and the command handlers are registered against them.
        FxForgetDrivers();
        UnbindDcduCommands();
    }
}

// ============================== waveform engine ==============================
static void RebuildBeacon(int led, double simTime) {
    gCurrentBeaconLed = led;
    gBeaconChannel = Channel(BeaconPulses(led >= 1), kBeaconPeriod, kBeaconOffset);
    gBeaconChannel.Seek(simTime - gBeaconChannel.offset);
    gBeaconBuilt = true;
    Log(std::string("beacon waveform -> ") + (led >= 1 ? "LED" : "Classic"));
}

static void RebuildStrobe(int led, double simTime) {
    gCurrentStrobeLed = led;
    for (int i = 0; i < 4; ++i) {
        gStrobeChannel[i] = Channel(StrobePulses(led >= 1, i), kStrobePeriod, kStrobeOffset);
        gStrobeChannel[i].Seek(simTime - gStrobeChannel[i].offset);
    }
    gStrobeBuilt = true;
    Log(std::string("strobe waveform -> ") + (led >= 1 ? "LED" : "Classic"));
}

static double SimTime() {
    return gSimTimeRef ? (double)XPLMGetDataf(gSimTimeRef) : 0.0;
}

static int ReadRatios(XPLMDataRef ref, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    return XPLMGetDatavf(ref, out, 0, 4);
}

// Set one element of Photon's own glow array — what the redirected OBJ
// ATTR_light_level regions read. No-op for an unmapped channel (kGlowNone) or an
// out-of-range index.
static void WriteGlow(int idx, float value) {
    if (idx < 0 || idx >= kGlowArraySize) return;
    gGlow[idx] = value;
}

static bool BeaconActive(int index, double sampledValue, double simTime) {
    if (sampledValue > kActivityThreshold) gLastActiveBeacon[index] = simTime;
    return (simTime - gLastActiveBeacon[index]) < kActivityWindowSeconds;
}

static bool StrobeGateOn() {
    if (!gStrobeGateRef) return false;
    bool on = XPLMGetDatai(gStrobeGateRef) > (int)kGateThreshold;
    return kInvertStrobeGate ? !on : on;
}

static float EngineLoop(float, float, int, void*) {
    // Only drive lights on a ToLiss aircraft (gMenuCreated tracks that).
    if (!gMenuCreated) { gLastSimTime = -1.0; return -1.0f; }

    double simTime = SimTime();
    if (gLastSimTime < 0) gLastSimTime = simTime;
    double deltaTime = simTime - gLastSimTime;
    gLastSimTime = simTime;
    if (deltaTime < 0) deltaTime = 0.0;
    else if (deltaTime > kMaxDeltaTime) deltaTime = kMaxDeltaTime;

    if (!gOverrideWarned && gOverrideRef) {
        if (XPLMGetDatai(gOverrideRef) < 1) {
            Log("NOTE override_beacons_and_strobes is 0 - effect may not show.");
            gOverrideWarned = true;
        }
    }

    // Interior: the ONLY per-frame interior work in the whole plugin — one
    // dataref read plus one float, for one light group. Read through the
    // type-matching getter because ckpt/lights/map's type is not statically
    // knowable (it appears only in the four .acf files, never in an OBJ), and
    // reading an int-typed dataref with XPLMGetDataf silently yields 0 (gotcha
    // #4 in reverse). The handle is cached (ResolveMapRheostat), so this costs a
    // single get, not a string lookup. Missing => alpha 0 rather than a guess.
    {
        double mapRheostat = 0.0;
        if (ReadDataRefTyped(gMapRheostat, gMapRheostatType, mapRheostat))
            gMapSpillAlpha = (float)ClampBrightness(mapRheostat);
        else
            gMapSpillAlpha = 0.0f;
    }

    // Screen glow: ONE array read for all six lights. Unconditional for the same
    // reason the map read is — the screens OBJ is an independent opt-in and this
    // plugin cannot tell whether it is attached, so gating on a detection flag
    // would turn a false negative into six dark lights instead of nothing at all.
    // With the OBJ absent nobody reads gScreenSizeFactor and this is a wasted
    // array fetch; with it present and the array missing, every screen reads 0
    // (no reach, i.e. dark), which is the honest answer rather than a guess.
    {
        float du[16] = {0.0f};
        int got = 0;
        if (gDUBrightness && gDUBrightnessCount > 0) {
            const int want = gDUBrightnessCount < (int)(sizeof(du) / sizeof(du[0]))
                           ? gDUBrightnessCount : (int)(sizeof(du) / sizeof(du[0]));
            got = XPLMGetDatavf(gDUBrightness, du, 0, want);
        }
        // Through the shared response curve, not straight: see kBrightnessCurve.
        // The knob's bottom fifth is dead and half a turn is still dim, which is
        // what a screen actually does and what the FX tint follows too.
        // The user's own switch is the LAST multiplier, applied after the curve —
        // it scales the finished look rather than reshaping the response, so
        // turning it down pulls the glow in without moving where the knob's dead
        // zone ends. Off writes 0 rather than skipping the loop: a stale factor
        // left in the array would keep the lights lit at whatever they last were.
        const float userGain = DisplayFeatureOn(gDisplays.spill) ? gDisplays.spillGain : 0.0f;
        // ⚠ THE BUS GATE IS NOT A MULTIPLIER, it is a hard zero, and it is read
        // ONCE for all six rather than per light — every one of them is on the
        // same bus, and this is the per-frame path. A dead bus means the screens
        // are off, not dim, so there is nothing for the knob or the user's gain
        // to scale: an "80% of nothing" spill is still a lit cockpit.
        const bool busLive = DcBusLive();
        for (int i = 0; i < kScreenCount; ++i) {
            const int idx = kScreenDU[i];
            // A FRACTION OF THE AUTHORED SIZE, not an alpha (2026-08-09). Zero is
            // a pool of no radius, i.e. the light contributes nothing — the same
            // "off" a zero alpha used to mean.
            gScreenSizeFactor[i] = (busLive && idx >= 0 && idx < got)
                ? BrightnessResponse((float)ClampBrightness(du[idx])) * kScreenGain * userGain
                : 0.0f;
        }
    }

    int beaconLed = gValues[5] >= 1 ? 1 : 0;   // index 5 = beacon
    int strobeLed = gValues[7] >= 1 ? 1 : 0;   // index 7 = strobe
    if (beaconLed != gCurrentBeaconLed) RebuildBeacon(beaconLed, simTime);
    if (strobeLed != gCurrentStrobeLed) RebuildStrobe(strobeLed, simTime);

    bool beaconAlwaysOn = gBeaconAlwaysOn >= 1;
    bool strobeAlwaysOn = gStrobeAlwaysOn >= 1;

    // beacon: one waveform on all 4 indices, each auto-gated independently
    if (gBeaconBuilt && gBeaconRatioRef) {
        double brightness = gBeaconChannel.Tick(deltaTime);   // tick to keep phase
        float out[4];
        int n = ReadRatios(gBeaconRatioRef, out);
        float glowVal = 0.0f;   // 0 = beacon off this frame -> skin dark (we own it)
        for (int i = 0; i < n && i < 4; ++i) {
            if (beaconAlwaysOn) { out[i] = 1.0f; glowVal = 1.0f; }
            else if (BeaconActive(i, out[i], simTime)) {
                out[i] = (float)ClampBrightness(brightness);
                glowVal = out[i];
            }
        }
        XPLMSetDatavf(gBeaconRatioRef, out, 0, 4);
        // Drive the beacon skin glow(s) with the SAME value the billboards got —
        // including 0 when off, since our array is the only thing feeding these
        // regions now (nothing else drives it back down).
        if (gGlowMapActive)
            for (int b = 0; b < 4 && gGlowMap.beacon[b] != kGlowNone; ++b)
                WriteGlow(gGlowMap.beacon[b], glowVal);
    }

    // strobe: per-index waveforms, gated on the strobe switch. Tick every channel
    // every frame (even when gated off) so phase stays correct when it turns on.
    if (gStrobeBuilt && gStrobeRatioRef) {
        float out[4];
        ReadRatios(gStrobeRatioRef, out);
        bool gateOn = StrobeGateOn();
        float glow[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // 0 = off -> skin dark (we own it)
        for (int index = 0; index < 4; ++index) {
            double value = gStrobeChannel[index].Tick(deltaTime);
            if (strobeAlwaysOn) { out[index] = 1.0f; glow[index] = 1.0f; }
            else if (gateOn)    { out[index] = (float)ClampBrightness(value); glow[index] = out[index]; }
        }
        XPLMSetDatavf(gStrobeRatioRef, out, 0, 4);
        // Skin-glow sync: drive each mapped glow index with the SAME value its
        // billboard got, so the redirected LIT-texture region flashes in lockstep
        // instead of keeping ToLiss's soft xenon fade. Unmapped channels/aircraft
        // are skipped, so no other glow region is disturbed.
        if (gGlowMapActive)
            for (int i = 0; i < 4; ++i)
                if (gGlowMap.strobe[i] != kGlowNone) WriteGlow(gGlowMap.strobe[i], glow[i]);
    }
    return -1.0f;   // every frame
}

static void StartEngine() {
    if (gEngineLoopID) return;
    gSimTimeRef     = XPLMFindDataRef(kSimTimeDataRef);
    gBeaconRatioRef = XPLMFindDataRef(kBeaconRatioDataRef);
    gStrobeRatioRef = XPLMFindDataRef(kStrobeRatioDataRef);
    gOverrideRef    = XPLMFindDataRef(kOverrideDataRef);
    gStrobeGateRef  = XPLMFindDataRef(kStrobeGateDataRef);
    // After-flight-model phase runs late, so our write lands after ToLiss's own
    // per-frame write and wins (gotcha #5, verified in-sim).
    XPLMCreateFlightLoop_t params;
    params.structSize = sizeof(params);
    params.phase = xplm_FlightLoop_Phase_AfterFlightModel;
    params.callbackFunc = EngineLoop;
    params.refcon = nullptr;
    gEngineLoopID = XPLMCreateFlightLoop(&params);
    XPLMScheduleFlightLoop(gEngineLoopID, -1.0f, 1);   // -1 = every frame
    Log("waveform engine started (after-flight-model, every frame)");
}

static void StopEngine() {
    if (gEngineLoopID) { XPLMDestroyFlightLoop(gEngineLoopID); gEngineLoopID = nullptr; }
}

// ====================== Auto re-resolution (1 Hz) ============================
static float AutoLoop(float, float, int, void*) {
    if (!gMenuCreated) return 1.0f;
    // ToLiss's own datarefs AND COMMANDS appear when ITS plugin starts, which can
    // be after our aircraft-loaded hook ran. Retry here (1 Hz) rather than from
    // the per-frame engine loop.
    //
    // ⚠ EVERY CACHED ToLiss HANDLE BELONGS IN THIS LIST — and this has now been
    // learned twice, from opposite ends. gDcBusRef was added to ResolveDcBus and
    // to the aircraft-load path but not to here: the handle stayed null,
    // DcBusLive() answered "unknown, therefore live" for the whole session, and
    // nothing in the log said so. The four DCDU commands then repeated it
    // exactly, one round-trip later. A resolver that is only called once is only
    // correct if ToLiss always wins the start race, which it does not — on a
    // stock install it loses it every time.
    //
    // The panel-FX drivers are the reason this asymmetry took so long to see:
    // they resolve LAZILY, on the first frame the compositor draws, so they are
    // immune to the race by construction and looked like proof the datarefs were
    // fine. Anything that caches at load time needs a line here instead.
    if (!gMapRheostat) ResolveMapRheostat();
    if (!gDUBrightness) ResolveDUBrightness();
    if (!gDcBusRef) ResolveDcBus();
    // ⚠ The third instance of this exact bug, found 2026-08-02 from "the panel FX
    // works on the MCDUs but not the PFDs". Only the six DUs are
    // resolution-switched, so a stale or unbound handle here is invisible on
    // every other face in the cockpit — see ResolveDisplayFallback.
    if (!gDisplayFallbackRef) ResolveDisplayFallback();
    if (gDcduBound < kDcduCommandCount) RetryDcduCommands();
    // What Auto WOULD pick, resolved whichever profile is selected, so the
    // settings window's "Auto (Hybrid LED)" label is right even while the axis is
    // on Classic. Cached rather than resolved from the UI: ResolveAutoProfile
    // does two XPLMFindDataRef string lookups, and the window would repeat them
    // every frame it is open for a value that cannot change faster than this loop.
    gAutoExtResolved = ResolveAutoProfile();
    gAutoIntResolved = ResolveIntAutoProfile();
    if (gProfile == PROFILE_AUTO) {
        int before[NCAT];
        std::memcpy(before, gValues, sizeof(before));
        ApplyProfileToValues();
        if (std::memcmp(before, gValues, sizeof(before)) != 0) {
            UpdateChecks();
            Log("Auto re-resolved");
        }
    }
    // Interior Auto re-resolves on the same 1 Hz tick, and AFTER the exterior:
    // it mirrors the resolved exterior era, so it must see this tick's value.
    // Deliberately NOT gated on gInteriorInstalled: five int writes a second keep
    // a detection false negative costing only the menu. Gating it here would
    // freeze a cockpit that IS modded at whatever it last loaded.
    if (gIntProfile == INT_PROFILE_AUTO) {
        int before[NINT];
        std::memcpy(before, gIntValues, sizeof(before));
        ApplyIntProfileToValues();
        if (std::memcmp(before, gIntValues, sizeof(before)) != 0) {
            UpdateChecks();
            Log("interior Auto re-resolved");
        }
    }
    return 1.0f;
}

// ========================= superseded-file removal ==========================
// Delete files this native .xpl supersedes so none of them runs alongside it and
// fights over the same per-frame beacon/strobe writes (and the same datarefs):
//   1. the retired FlyWithLua script(s) — the waveform engine used to live in Lua;
//   2. the retired Python plugin PI_ToLissPhoton.py (+ its __pycache__ bytecode) —
//      this .xpl replaces it, so XPPython3 must stop loading it.
// Best-effort: missing folders and permission errors are just logged. NOTE: if
// XPPython3 / FlyWithLua already loaded a superseded file THIS session, deleting it
// now doesn't unload the running copy — it stops loading on the next reload / sim
// restart, so an upgrader may need one restart for the switchover to be clean.
static void RemoveSupersededFiles() {
    char root[512] = {0};
    XPLMGetSystemPath(root);
    if (!root[0]) return;
    fs::path plugins = fs::path(root) / "Resources" / "plugins";

    // 1) retired FlyWithLua scripts
    fs::path fwl = plugins / "FlyWithLua";
    fs::path luaDirs[] = {fwl / "Scripts", fwl / "Scripts (Quarantine)"};
    for (const fs::path& dir : luaDirs) {
        for (const char* name : kLegacyLuaNames) {
            fs::path path = dir / name;
            std::error_code ec;
            if (fs::is_regular_file(path, ec)) {
                if (fs::remove(path, ec) && !ec) Log("removed legacy FlyWithLua script: " + path.string());
                else if (ec) Log("could NOT remove legacy script " + path.string());
            }
        }
    }

    // 2) retired Python plugin + its cached bytecode
    fs::path pyDir    = plugins / "PythonPlugins";
    fs::path pyPlugin = pyDir / kRetiredPythonPlugin;
    std::error_code ec;
    if (fs::is_regular_file(pyPlugin, ec)) {
        if (fs::remove(pyPlugin, ec) && !ec) Log("removed retired Python plugin: " + pyPlugin.string());
        else if (ec) Log("could NOT remove retired Python plugin " + pyPlugin.string());
    }
    // __pycache__ holds e.g. PI_ToLissPhoton.cpython-313.pyc; remove only THIS plugin's
    // cached bytecode (prefix "PI_ToLissPhoton.") so other Python plugins' caches — e.g.
    // the separate dev-reload tool's — are left untouched.
    std::string prefix = kRetiredPythonPlugin;               // "PI_ToLissPhoton.py"
    prefix = prefix.substr(0, prefix.rfind('.') + 1);        // -> "PI_ToLissPhoton."
    fs::path pycache = pyDir / "__pycache__";
    std::error_code dec;
    if (fs::is_directory(pycache, dec)) {
        for (fs::directory_iterator it(pycache, dec), end; !dec && it != end; it.increment(dec)) {
            const fs::path& p = it->path();
            if (p.filename().string().rfind(prefix, 0) == 0 && p.extension() == ".pyc") {
                std::error_code rmec;
                if (fs::remove(p, rmec) && !rmec) Log("removed stale bytecode: " + p.string());
            }
        }
    }
}

// ======================== the panel pass and its probe =======================
// Everything that draws into the cockpit's panel framebuffer. TWO features share
// this machinery and they ship differently:
//
//   * the Panel FX COMPOSITOR — the shipped effect. Layer stacks over the
//     readout rects, composited every lit-pass frame. Unconditional.
//   * the panel PROBE — a dev diagnostic that floods the panel magenta. Behind
//     `#if PHOTON_DEV` and must never ship: it paints over the captain's PFD.
//
// So the rect table, the target/group resolution, the GL entry points, the blend
// save/restore, the texture pool and the compositor itself are all compiled
// unconditionally; only the probe's own datarefs, draw methods, state dumps and
// UI are guarded. When reading the guards below, the question they answer is
// always "is this the diagnostic, or the effect?".
//
// ---- the probe (dev-only) ---------------------------------------------------
// Paints a flat magenta quad into the cockpit's panel framebuffer, so that "do
// our pixels land on top of ToLiss's?" is answered by looking rather than by
// reasoning. Draw order BETWEEN plugins within one phase is unspecified by the
// SDK, so nothing else settles it. Contract, measurements and history:
// docs/fcu_tint_plan.md §0-§1.
//
// Reading the result — START WITH THE FLOOD (rect -2), not the DU rects:
//   whole cockpit magenta  -> we are in the panel FBO and our pixels survive
//   nothing at all         -> wrong render target; read the "GL target" line in
//                             Log.txt. Rect tuning cannot help until that is fixed
//   part of the cockpit    -> right target, coordinates are off
// Then with the rects: a flat magenta PFD means we draw after ToLiss.
//
// ⚠ Mode 3 ("before gauges") is NOT a sanity check that we draw at all — it is
// registered ahead of every gauge, so ToLiss is guaranteed to paint over it. The
// flood is the sanity check.
//
// ⚠ COORDINATES ARE RAW PANEL PIXELS, never remapped through panel_total_pnl_*.
// Those report 1024 on the A320 because the aircraft's 2-D panel is empty, so
// scaling through them puts every quad 4x too small in a corner no cockpit
// geometry samples — invisible in every mode. They are still logged, as
// information only.
// Read every panel-pass frame by both features, so it is not dev-only.
static const char* kPanelRenderType = "sim/graphics/view/panel_render_type";

#if PHOTON_DEV

static const char* kPanelProbeMode  = "ToLissPhoton/debug/panel_probe";
static const char* kPanelProbeRect  = "ToLissPhoton/debug/panel_probe_rect";
static const char* kPanelProbeDraw  = "ToLissPhoton/debug/panel_probe_draw";
static const char* kPanelProbeType  = "ToLissPhoton/debug/panel_probe_pass";
static const char* kPanelProbeCycle = "ToLissPhoton/debug/panel_probe_cycle";
// The multiply method's color, one dataref per channel so it can be dialled in
// from any dataref editor. Three scalars rather than one array deliberately: an
// array editor makes "nudge green down by 0.02" awkward, and that is the entire
// tuning motion here. See docs/fcu_tint_plan.md §5 step 4.
static const char* kPanelTintR      = "ToLissPhoton/debug/panel_tint_r";
static const char* kPanelTintG      = "ToLissPhoton/debug/panel_tint_g";
static const char* kPanelTintB      = "ToLissPhoton/debug/panel_tint_b";

// HOW the pixels are put down. Four methods, ordered so each eliminates a
// different cause of "nothing appears":
//
//   Clear     — glClear through a scissor box. Touches NO vertex or fragment
//               stage. If even this shows nothing, our GL is not reaching the
//               FBO at all and the approach needs rethinking, not tuning.
//   Immediate — glBegin/glEnd. Depends on X-Plane's fixed-function matrices and
//               on no shader program being bound.
//   Forced    — immediate, plus glUseProgram(0), culling and depth off, and our
//               own glOrtho. The fix if the state dump shows a bound program.
//   Multiply  — NOT a diagnostic: the FCU-tint effect itself, blended
//               glBlendFunc(GL_DST_COLOR, GL_ZERO). Multiply is the right
//               primitive because the FCU background is BLACK and black x
//               anything is black, so it masks itself and recolors only the lit
//               digits — and it self-scales with the brightness knob for free.
//
// ⚠ Multiply and Forced cannot be combined: Forced disables GL_BLEND, which is
// exactly what multiply needs on.
enum {
    kDrawClear       = 0,   // the decisive one — default
    kDrawImmediate   = 1,
    kDrawForced      = 2,
    kDrawMultiply    = 3,   // the effect, not a diagnostic
    kDrawMethodCount = 4,
};
static const char* kDrawMethodName[kDrawMethodCount] =
    { "Clear", "Immediate", "Forced", "Multiply" };

#endif  // PHOTON_DEV — the probe's draw methods

// Which callback slot draws. In a dev build each is registered separately and only
// the one matching gPanelProbeMode paints, so a single dataref walks all three
// without a rebuild. A SHIPPING build registers only kProbeAfterGauges — the slot
// the compositor runs from — but the enum stays whole so the slot refcon means the
// same thing in both builds.
enum {
    kProbeOff          = 0,
    kProbeAfterGauges  = 1,   // magenta — the slot the FX compositor draws from
    kProbeAfterPanel   = 2,   // green   — earlier; try if ToLiss draws in Gauges
    kProbeBeforeGauges = 3,   // blue    — sanity check that we draw at all
    kProbeModeCount    = 4,
};

// The panel texture is a 4096x4096 atlas, and a face's UV bounding box IS its
// pixel rect in the FBO — cockpit_INN.obj carries no TEXTURE line and declares
// GLOBAL_cockpit_lit, so UV 0..1 maps to the whole atlas. Pixels are kept
// verbatim from build/derive_panel_rects.py rather than rounded, so they diff
// cleanly against a re-run of it.
//
// ⚠ THE DU RECT SET IS RUNTIME-GATED. Every DU face exists TWICE in the OBJ
// under `ANIM_hide … AirbusFBW/DisplayResolutionFallback`: ~748 px at 0, ~499 px
// at 1, same origin. Draw the wrong set and the overlay is two-thirds size on a
// correctly-aimed screen. The dataref is read per frame.
//
// A3xx-wide: re-derived on A319/A320/A321 on 2026-07-31 and byte-identical on
// all three. The A339 has its own atlas and must be derived separately.
static const float kPanelAtlas = 4096.0f;

// `du` is the AirbusFBW/DUBrightness index that drives this screen's brightness
// — needed by any overlay that should track the knob. -1 = not a display unit.
// The order below is CA PFD, CA ND, E/WD, SD, FO ND, FO PFD, which is why the
// indices read {0,1,4,5,3,2}; that matches kScreenDU in the screen-glow feature.
struct PanelRect { const char* name; float u0, u1, v0, v1; int du; };

// ⚠ APPEND ONLY. The index into this table is persisted (the Debug window's
// prefs file) and is the value of ToLissPhoton/debug/panel_probe_rect, so
// inserting a row silently re-aims a saved target at a different screen.
//
// DisplayResolutionFallback == 0 (the normal case)
static const PanelRect kPanelRectsHi[] = {
    { "capt PFD",     8.0f,  755.5f, 2832.5f, 3583.7f, 0 },
    { "capt ND",    763.9f, 1514.4f, 2831.5f, 3585.5f, 1 },
    { "E/WD",      1522.6f, 2145.9f, 2425.5f, 3051.9f, 4 },
    { "SD",        2152.9f, 2777.6f, 2426.6f, 3054.3f, 5 },
    { "FO ND",      764.0f, 1512.6f, 2078.0f, 2830.2f, 3 },
    { "FO PFD",       6.5f,  755.0f, 2076.8f, 2828.9f, 2 },
    { "FCU strip", 1046.1f, 1863.9f,  954.0f, 1020.2f, -1 },
    { "capt baro",  896.5f, 1023.7f,  954.0f, 1020.2f, -1 },
    { "FO baro",   1890.4f, 2017.6f,  954.0f, 1020.2f, -1 },
    // --- the rest of the cockpit's readouts, derived 2026-07-31 ---------------
    // Every one of these is BYTE-IDENTICAL on A319, A320 and A321 (checked with
    // derive_panel_rects.py --all against all three), so the table is A3xx-wide
    // rather than per-airframe. None is resolution-switched — only the six DUs
    // have a DisplayResolutionFallback variant — so these rows repeat unchanged
    // in kPanelRectsLo.
    //
    // ⚠ NAMES BELOW ARE DERIVED FROM GEOMETRY, NOT CONFIRMED IN-SIM. ToLiss
    // labels none of these faces, so each name is an inference from position,
    // size and the company each one keeps. The ISIS and the clock triple are
    // safe; DCDU is the project's best reading and nothing more. (The two center
    // readouts WERE on that list and were confirmed as the DME windows in-sim
    // 2026-08-02.) Confirm one the same way anything here gets confirmed —
    // Dev > Probe, pick it as the target, and look at what turns magenta. Then
    // fix the name here rather than working around it (the "identify a light by
    // its position, never by an inherited name" rule, applied to faces).
    { "ISIS",      3266.6f, 3670.7f, 2417.7f, 2823.7f, -1 },  // 6.4x6.1cm  x-0.143 y+0.110
    { "DCDU capt", 3675.6f, 3944.9f, 2618.4f, 2820.0f, -1 },  // 9.6x6.9cm  x-0.181 y-0.085
    { "DCDU FO",   3675.6f, 3944.9f, 2410.1f, 2611.7f, -1 },  // 9.6x6.9cm  x+0.194 y-0.085
    // The clock's three windows, stacked. The middle one is widest because UTC
    // is six digits where CHR and ET are four.
    { "clock CHR",   22.1f,  100.8f,  943.2f,  973.8f, -1 },  // 3.8x1.4cm  x+0.166 y+0.129
    { "clock UTC",    9.8f,  115.6f,  896.9f,  927.4f, -1 },  // 5.1x1.4cm  x+0.167 y+0.108
    { "clock ET",    24.5f,   98.4f,  850.4f,  880.9f, -1 },  // 3.5x1.4cm  x+0.166 y+0.087
    // A pair of 2.8x0.8cm windows directly below the ISIS. They are the DME
    // windows, identified in-sim 2026-08-02 with the target tree's ? button;
    // their positional names are kept because a rect NAME is wire identity in
    // panelfx.txt, and the group over them carries the identification instead
    // (kPanelGroups, "DME"), which is what the tree and the FX file address.
    { "ctr readout L", 727.6f,  855.7f, 889.7f, 930.5f, -1 },  // x-0.168 y+0.040
    { "ctr readout R", 872.9f, 1000.9f, 889.7f, 930.5f, -1 },  // x-0.127 y+0.040
    // ⚠ FOUR pedestal faces share this ONE atlas island (x -0.159/-0.142 and
    // +0.226/+0.244, all at y-0.175 z-4.178). They therefore cannot be tinted
    // separately at all — one rect is not a simplification, it is the truth.
    { "ped strips",  990.1f, 1055.6f, 3607.1f, 3714.4f, -1 },  // 1.8x3.4cm each
    // --- the two EFB tablets, derived 2026-07-31 ------------------------------
    // On the side-window rails, animated by AirbusFBW/CockpitWindowPosition[n].
    // Each tablet has TWO faces, gated by AirbusFBW/EFBShowsAvitab[n]: ToLiss's
    // own EFB at 0 and AviTab at 1. Only one draws at a time, so tinting the
    // hidden one is harmless — its atlas region is sampled by nothing.
    //
    // ⚠ BOTH TABLETS SHARE THE AviTab ISLAND, so AviTab cannot be tinted per
    // side at all. Same shape as "ped strips": one rect is the truth.
    //
    // ⚠ THE SIDE COMES FROM THE ANIM INDEX, NOT FROM THE VERTEX POSITIONS. These
    // faces sit inside two levels of ANIM, so their vertices are in a LOCAL frame
    // (all four read x≈0, z≈-0.1) and the survey's position column says nothing
    // about which side they are on. Captain is the [0] chain, whose ANIM_trans is
    // -1.27387 0.15733 -3.63657 — negative x, i.e. left. Reading the position
    // column instead is what nearly filed both of these as cabin geometry.
    //
    // The v range stops short of each screen's own UV extent (1885.5 / 1886.3):
    // the tablet's bezel strip starts at 1878.1 / 1884.7 and the screen quad
    // overlaps it by ~7 px. Losing 7 px off the bottom of an 800 px screen is
    // invisible; tinting 7 px of a 75 px bezel is a colored sliver of frame.
    { "EFB capt",     4.7f, 1330.7f, 1076.2f, 1878.1f, -1 },  // 12.0x17.2cm
    { "EFB FO",    1336.7f, 2662.3f, 1077.7f, 1884.7f, -1 },  // 10.2x17.9cm
    { "EFB AviTab",2120.0f, 3142.1f,    8.7f,  645.0f, -1 },  // 12.1x17.2cm, BOTH sides
    // --- the two MCDUs, derived 2026-07-31 -----------------------------------
    // Directly ABOVE the ISIS in the atlas, one island per side, ungated and not
    // resolution-switched. 9.4 x 11.4 cm each, lying almost flat on the pedestal
    // (normal 0.00 0.99 0.14) at x -0.194 / +0.194, z -4.442.
    //
    // ⚠ THESE WERE MISSED ONCE AND THE REASON IS A MEASUREMENT TRAP, not a
    // subtle one: derive_panel_rects.py used to report each island's AXIS-ALIGNED
    // x and y extents, so a screen lying 8 degrees off horizontal measured "11.4
    // x 1.4 cm" and read as a label strip. Its `--all` filter also required
    // n[2] > 0.2, which drops a face whose normal is 0.14 — i.e. the whole
    // pedestal. Both are fixed there now (in_plane_size, and n[1] > 0.5 as an
    // alternative), so a re-derivation lists them.
    //
    // ⚠ v0 is trimmed from 2822.0 to 2824.0 to clear the ISIS, whose island ends
    // at 2823.7 — the two are packed adjacent with ~1.7 px of overlap. Losing
    // 1.7 px off a 257 px screen edge is invisible; bleeding MCDU color onto
    // the top of the ISIS is a hairline someone would eventually chase.
    //
    // The UV mapping is near-uniform and NOT rotated: u runs with +x (rightward)
    // and v with -z (forward, i.e. away from the pilot), 24.2 px/cm against
    // 27.6 px/cm. So the top of an overlay image lands at the FAR edge of the
    // screen. Corroboration that these really are the MCDUs, beyond position:
    // the atlas aspect is 276/257 = 1.07, and the screen cut-out in ToLiss's own
    // MCDU art (cockpit/OpenGLTextures/MCDU_Popup.png) is ~295x270 = 1.09.
    { "MCDU capt", 3265.5f, 3541.3f, 2824.0f, 3079.1f, -1 },  // 9.4x11.4cm
    { "MCDU FO",   3541.8f, 3817.6f, 2824.0f, 3079.3f, -1 },  // 9.4x11.4cm
    // --- the radios, the transponder and the trim, derived 2026-08-01 --------
    // Found by taking the WHOLE TRIS BATCH the FCU strip lives in rather than by
    // filtering geometry: ToLiss draws every readout face in the cockpit from
    // one batch (line 54323 on all three A3xx), so "everything in that batch"
    // is a complete list and nothing size-based can drop a member of it. It
    // holds 22 unique islands, byte-identical on A319/A320/A321.
    //
    // These are NAMED FROM THE KNOBS AROUND THEM, not from the faces, which is
    // as close to told-by-ToLiss as this gets: each panel's manipulators carry
    // command names (AirbusFBW/RMP2LargeKnob, AirbusFBW/XPDRPower, ...), so the
    // nearest manipulator to a face names the panel it sits on. ⚠ A knob inside
    // an ANIM has LOCAL coordinates and reads as sitting at the origin — the
    // same trap as the EFB faces below — so the enclosing ANIM_trans offsets
    // have to be summed before comparing distances. Without that every knob in
    // the cockpit looks equidistant and the method silently gives nothing.
    //
    // Each RMP has two windows, ACTIVE left and STANDBY right, with the
    // frequency selector under the standby one — which is how the two are told
    // apart here: RMP1's knobs are at x-0.167, i.e. under the x-0.156 window.
    { "RMP1 active",  12.1f,  103.9f,  988.6f, 1016.4f, -1 },  // 4.4x1.3cm x-0.229
    { "RMP1 stby",   121.9f,  213.7f,  988.6f, 1016.4f, -1 },  // 4.4x1.3cm x-0.156
    { "RMP2 active", 229.8f,  321.6f,  988.9f, 1016.7f, -1 },  // 4.4x1.3cm x+0.157
    { "RMP2 stby",   346.4f,  438.2f,  988.9f, 1016.7f, -1 },  // 4.4x1.3cm x+0.230
    // RMP3 is on the OVERHEAD, not the pedestal (AirbusFBW/RMP3LargeKnob is at
    // x+0.274 y+0.970 z-3.716, 2.9 cm from the standby window). Same layout.
    { "RMP3 active", 454.7f,  556.2f,  980.1f, 1019.1f, -1 },  // 4.9x1.9cm x+0.212
    { "RMP3 stby",   571.8f,  673.3f,  980.1f, 1019.1f, -1 },  // 4.9x1.9cm x+0.283
    // The transponder code window: XPDRPower/XPDRSystem/XPDRTCASMode surround it
    // at 2.4-9.8 cm, and nothing else is within 20.
    { "XPDR code",   690.9f,  789.0f,  977.8f, 1027.3f, -1 },  // 4.0x2.0cm x+0.226
    // The rudder (yaw) trim readout. No manipulator names it — ToLiss animates
    // that knob without a command — so this one is placed by NEIGHBORS: it sits
    // on the centerline between the speedbrake lever (11.1 cm) and the flap
    // lever (16.9 cm), just aft of the ENG mode switch (11.6 cm), which is
    // exactly where the A320's RUD TRIM panel is.
    { "rudder trim", 814.8f,  882.4f,  988.7f, 1016.4f, -1 },  // 3.2x1.3cm x-0.020
    // --- five aft-overhead readouts, derived 2026-08-01 ----------------------
    // They are in the readout batch, they are readout-sized, and they are up on
    // the overhead (y +0.93 to +1.08, z -3.51 to -3.88, i.e. above and behind
    // the RMP3/ACP3 cluster) — but ToLiss puts NO manipulator within 40 cm of
    // four of them, so the method that named everything above had nothing to say
    // here and all five started out named by POSITION.
    //
    // THREE ARE NOW IDENTIFIED IN-SIM (2026-08-02), each by flooding the row
    // magenta with the FX target tree's ? button and looking: the two forward
    // windows are the BAT 1 / BAT 2 voltmeters and the wide aft-right one is the
    // FMS load readout. `ovhd aft L1`/`L2` are still unidentified and keep their
    // positional names — do not guess in the name field, flood and look.
    { "BAT1",        123.1f,  197.9f,  941.1f,  980.1f, -1 },  // 3.6x1.9cm x-0.057
    { "BAT2",        211.3f,  286.1f,  941.1f,  980.1f, -1 },  // 3.6x1.9cm x+0.032
    { "ovhd aft L1", 381.4f,  457.0f,  890.6f,  929.6f, -1 },  // 3.6x1.9cm x-0.294
    { "ovhd aft L2", 474.9f,  550.6f,  890.6f,  929.6f, -1 },  // 3.6x1.9cm x-0.216
    { "FMS Load",    565.9f,  702.7f,  890.6f,  929.6f, -1 },  // 6.5x1.9cm x+0.259
};

// DisplayResolutionFallback == 1. Same faces, same origins, ~499 px instead of
// ~748. Only the six DUs are resolution-switched; every other row has one
// variant, so the rest of this table is identical to the one above.
static const PanelRect kPanelRectsLo[] = {
    { "capt PFD",     8.0f,  506.3f, 2832.5f, 3333.3f, 0 },
    { "capt ND",    763.9f, 1264.2f, 2831.5f, 3334.2f, 1 },
    { "E/WD",      1522.6f, 2021.2f, 2425.5f, 2926.6f, 4 },
    { "SD",        2152.9f, 2652.6f, 2426.6f, 2928.8f, 5 },
    { "FO ND",      764.0f, 1263.1f, 2078.0f, 2579.5f, 3 },
    { "FO PFD",       6.5f,  505.5f, 2076.8f, 2578.2f, 2 },
    { "FCU strip", 1046.1f, 1863.9f,  954.0f, 1020.2f, -1 },
    { "capt baro",  896.5f, 1023.7f,  954.0f, 1020.2f, -1 },
    { "FO baro",   1890.4f, 2017.6f,  954.0f, 1020.2f, -1 },
    { "ISIS",      3266.6f, 3670.7f, 2417.7f, 2823.7f, -1 },
    { "DCDU capt", 3675.6f, 3944.9f, 2618.4f, 2820.0f, -1 },
    { "DCDU FO",   3675.6f, 3944.9f, 2410.1f, 2611.7f, -1 },
    { "clock CHR",   22.1f,  100.8f,  943.2f,  973.8f, -1 },
    { "clock UTC",    9.8f,  115.6f,  896.9f,  927.4f, -1 },
    { "clock ET",    24.5f,   98.4f,  850.4f,  880.9f, -1 },
    { "ctr readout L", 727.6f,  855.7f, 889.7f, 930.5f, -1 },
    { "ctr readout R", 872.9f, 1000.9f, 889.7f, 930.5f, -1 },
    { "ped strips",  990.1f, 1055.6f, 3607.1f, 3714.4f, -1 },
    { "EFB capt",     4.7f, 1330.7f, 1076.2f, 1878.1f, -1 },
    { "EFB FO",    1336.7f, 2662.3f, 1077.7f, 1884.7f, -1 },
    { "EFB AviTab",2120.0f, 3142.1f,    8.7f,  645.0f, -1 },
    { "MCDU capt", 3265.5f, 3541.3f, 2824.0f, 3079.1f, -1 },
    { "MCDU FO",   3541.8f, 3817.6f, 2824.0f, 3079.3f, -1 },
    { "RMP1 active",  12.1f,  103.9f,  988.6f, 1016.4f, -1 },
    { "RMP1 stby",   121.9f,  213.7f,  988.6f, 1016.4f, -1 },
    { "RMP2 active", 229.8f,  321.6f,  988.9f, 1016.7f, -1 },
    { "RMP2 stby",   346.4f,  438.2f,  988.9f, 1016.7f, -1 },
    { "RMP3 active", 454.7f,  556.2f,  980.1f, 1019.1f, -1 },
    { "RMP3 stby",   571.8f,  673.3f,  980.1f, 1019.1f, -1 },
    { "XPDR code",   690.9f,  789.0f,  977.8f, 1027.3f, -1 },
    { "rudder trim", 814.8f,  882.4f,  988.7f, 1016.4f, -1 },
    { "BAT1",        123.1f,  197.9f,  941.1f,  980.1f, -1 },
    { "BAT2",        211.3f,  286.1f,  941.1f,  980.1f, -1 },
    { "ovhd aft L1", 381.4f,  457.0f,  890.6f,  929.6f, -1 },
    { "ovhd aft L2", 474.9f,  550.6f,  890.6f,  929.6f, -1 },
    { "FMS Load",    565.9f,  702.7f,  890.6f,  929.6f, -1 },
};
static const int kPanelRectCount = (int)(sizeof(kPanelRectsHi) / sizeof(kPanelRectsHi[0]));
static_assert(sizeof(kPanelRectsLo) == sizeof(kPanelRectsHi),
              "the two resolution variants must hold the same faces in the same order - "
              "a target index selects a row in whichever table is live");

// --- groups ------------------------------------------------------------------
// A named set of rects addressed as one target, because the thing a person wants
// to recolor is often not one UV island.
//
// ⚠ A group is NOT a shortcut for one wide rect. The FCU row's three faces have
// 22 and 26 px of atlas between them that other cockpit geometry samples, so a
// single 896..2017 quad would recolor something unrelated. Three quads cost
// nothing and touch only our faces.
//
// Members are named, not indexed: re-deriving the table must not silently
// retarget a group.
//
// Groups NEST by containment, not by declaration — see PanelTargetMask below.
// Nothing states that "Glass displays" contains "capt PFD"; it falls out of the
// rect sets, so adding a group is enough to place it in the hierarchy and no
// group can claim a rect it does not actually paint. The tree, top down:
//
//   Whole panel
//   ├── Glass displays          the CRT/LCD screens
//   │   ├── Main displays       the six DUs
//   │   ├── DCDU                the two datalink screens
//   │   ├── EFB                 the two side-rail tablets
//   │   ├── MCDU                the two pedestal CDUs
//   │   └── ISIS                (one rect, so no group of its own)
//   └── Segment readouts        the digit windows
//       ├── FCU row
//       ├── Clock
//       ├── DME                 the two center readouts
//       ├── Radios              the three RMPs
//       │   ├── RMP1            active + standby
//       │   ├── RMP2
//       │   └── RMP3            (on the OVERHEAD, not the pedestal)
//       ├── Overhead readouts   BAT 1/2, FMS load and the two still unidentified
//       ├── XPDR code           (one rect)
//       ├── rudder trim         (one rect)
//       └── ped strips          (one rect)
//
// Glass vs. segment is the split that matters for tinting: a backlit LCD and a
// seven-segment window start from different colors and take a multiply
// differently, so they are the two things you most often want to move as one.
//
// ⚠ A single-member group is pointless and slightly harmful — its mask EQUALS
// its member's, so containment (which is strict) puts the two side by side as
// siblings instead of nesting them. Hence no "Standby" wrapping ISIS alone.
struct PanelGroup { const char* name; const char* members[48]; };
static const PanelGroup kPanelGroups[] = {
    { "Main displays",   { "capt PFD", "capt ND", "E/WD", "SD", "FO ND", "FO PFD",
                           nullptr } },
    { "DCDU",            { "DCDU capt", "DCDU FO", nullptr } },
    { "EFB",             { "EFB capt", "EFB FO", "EFB AviTab", nullptr } },
    { "MCDU",            { "MCDU capt", "MCDU FO", nullptr } },
    { "Glass displays",  { "capt PFD", "capt ND", "E/WD", "SD", "FO ND", "FO PFD",
                           "ISIS", "DCDU capt", "DCDU FO",
                           "EFB capt", "EFB FO", "EFB AviTab",
                           "MCDU capt", "MCDU FO", nullptr } },
    { "FCU row",         { "capt baro", "FCU strip", "FO baro", nullptr } },
    { "Clock",           { "clock CHR", "clock UTC", "clock ET", nullptr } },
    { "DME",             { "ctr readout L", "ctr readout R", nullptr } },
    { "RMP1",            { "RMP1 active", "RMP1 stby", nullptr } },
    { "RMP2",            { "RMP2 active", "RMP2 stby", nullptr } },
    { "RMP3",            { "RMP3 active", "RMP3 stby", nullptr } },
    { "Radios",          { "RMP1 active", "RMP1 stby", "RMP2 active", "RMP2 stby",
                           "RMP3 active", "RMP3 stby", nullptr } },
    { "Overhead readouts", { "BAT1", "BAT2",
                             "ovhd aft L1", "ovhd aft L2", "FMS Load", nullptr } },
    { "Segment readouts",{ "capt baro", "FCU strip", "FO baro",
                           "clock CHR", "clock UTC", "clock ET",
                           "ctr readout L", "ctr readout R", "ped strips",
                           "RMP1 active", "RMP1 stby", "RMP2 active", "RMP2 stby",
                           "RMP3 active", "RMP3 stby", "XPDR code", "rudder trim",
                           "BAT1", "BAT2",
                           "ovhd aft L1", "ovhd aft L2", "FMS Load", nullptr } },
    { "Whole panel",     { "capt PFD", "capt ND", "E/WD", "SD", "FO ND", "FO PFD",
                           "FCU strip", "capt baro", "FO baro",
                           "ISIS", "DCDU capt", "DCDU FO",
                           "clock CHR", "clock UTC", "clock ET",
                           "ctr readout L", "ctr readout R", "ped strips",
                           "EFB capt", "EFB FO", "EFB AviTab",
                           "MCDU capt", "MCDU FO",
                           "RMP1 active", "RMP1 stby", "RMP2 active", "RMP2 stby",
                           "RMP3 active", "RMP3 stby", "XPDR code", "rudder trim",
                           "BAT1", "BAT2",
                           "ovhd aft L1", "ovhd aft L2", "FMS Load", nullptr } },
};
static const int kPanelGroupCount = (int)(sizeof(kPanelGroups) / sizeof(kPanelGroups[0]));

// Targets share one index space so a single int selects either: [0, rectCount)
// is a rect, [rectCount, rectCount+groupCount) is a group.
static const int kPanelTargetCount = kPanelRectCount + kPanelGroupCount;

// ⚠ A ToLiss HANDLE, so it belongs to the aircraft: dropped when one goes away,
// rebound when one loads, and RETRIED at 1 Hz — its line in AutoLoop's list is
// not optional. It was resolved exactly ONCE, in StartPanelPass (i.e. from
// XPluginEnable), and never again (fixed 2026-08-02). Both halves of that were
// wrong, and the second is the dangerous one:
//
//  * On a cold start our XPluginEnable can run BEFORE ToLiss's plugin registers
//    anything, so the lookup returned null and PanelRects() answered "full-res"
//    for the whole session — silently right, until ToLiss actually falls back.
//  * On an AIRCRAFT RELOAD — which the dev loop does all day — the handle
//    survived into a ToLiss plugin that had been UNLOADED. Reading a destroyed
//    dataref is undefined, not merely stale, and whatever it returns is what
//    decides which rect set the six DUs paint from.
//
// ⚠ AND THE BLAST RADIUS IS EXACTLY THE SIX DUs, which is what makes this hard
// to recognise from the symptom. They are the only resolution-switched rows —
// every other rect is byte-identical in the two tables — so the whole rest of
// the cockpit keeps tinting perfectly while the displays alone go wrong. "The FX
// works on the MCDUs but not the PFDs" does not read as a dangling dataref
// handle, and it took a report of exactly that to find this.
//
// Null is the SAFE state (PanelRects falls back to full-res), so dropping it on
// the way out costs nothing and the retry rebinds within a second.
static void ResolveDisplayFallback() {
    gDisplayFallbackRef = XPLMFindDataRef("AirbusFBW/DisplayResolutionFallback");
    if (gDisplayFallbackRef && !gDisplayFallbackLogged) {
        gDisplayFallbackLogged = true;
        Log("bound AirbusFBW/DisplayResolutionFallback (the six DU rects follow it)");
    }
}

// Which of the two sets is on screen right now.
static const PanelRect* PanelRects() {
    const bool lo = gDisplayFallbackRef && XPLMGetDatai(gDisplayFallbackRef) != 0;
    return lo ? kPanelRectsLo : kPanelRectsHi;
}

// Name lookups. Everything that aims at a particular face does it by name and
// not by a literal index, so that re-deriving or reordering the tables cannot
// silently point the tint at a display unit instead.
static int RectIndexByName(const char* name) {
    for (int i = 0; i < kPanelRectCount; ++i)
        if (std::strcmp(kPanelRectsHi[i].name, name) == 0) return i;
    return -1;
}
static int GroupTargetByName(const char* name) {
    for (int g = 0; g < kPanelGroupCount; ++g)
        if (std::strcmp(kPanelGroups[g].name, name) == 0) return kPanelRectCount + g;
    return -1;
}

// --- containment -------------------------------------------------------------
// Which rects a target paints, as a bitmask. This is the whole hierarchy: a
// target CONTAINS another when its mask is a superset, and its BREADTH is the
// popcount. Everything else — the tree in the FX editor, the order the layer
// stacks composite in — is derived from these two, so the hierarchy can never
// disagree with what is actually painted. Declaring parent/child links by hand
// instead would let a group claim a rect it does not draw.
//
// ⚠ The tree assumes no two groups PARTIALLY overlap. Sibling groups must be
// disjoint or nested (they are: FCU row and Screens partition the panel, and
// Whole panel contains both). A group spanning half of each — "everything on the
// captain's side" — is legitimate for the compositor, which only sorts by
// breadth, but has no single place in a tree; it would need a rect to appear
// under two parents. Add one only alongside that handling.
// ⚠ 64-BIT, and the static_assert below is not decoration. This was `unsigned`
// while the table held 23 rects; the pedestal and overhead readouts took it to
// 36, and `1u << 35` is undefined behavior that in practice wraps — every rect
// past the 32nd would have aliased one of the first four, so a group would claim
// rects it does not paint and the containment TREE, which is derived from these
// masks, would have quietly rearranged itself. Nothing about that failure looks
// like an overflow from the UI.
typedef std::uint64_t PanelMask;
static_assert(kPanelRectCount <= 64,
              "the rect table has outgrown the containment mask - widen PanelMask "
              "(and everything derived from it) before appending more rows");

static PanelMask PanelTargetMask(int t) {
    if (t >= 0 && t < kPanelRectCount) return (PanelMask)1 << t;
    if (t >= kPanelRectCount && t < kPanelTargetCount) {
        const PanelGroup& g = kPanelGroups[t - kPanelRectCount];
        const int maxMembers = (int)(sizeof(g.members) / sizeof(g.members[0]));
        PanelMask mask = 0;
        for (int m = 0; m < maxMembers && g.members[m]; ++m) {
            const int i = RectIndexByName(g.members[m]);
            if (i >= 0) mask |= (PanelMask)1 << i;
        }
        return mask;
    }
    return 0;
}

static int PanelTargetBreadth(int t) {
    PanelMask m = PanelTargetMask(t);
    int n = 0;
    for (; m; m &= m - 1) ++n;
    return n;
}

// Strict containment: a contains b, and they are not the same set.
static bool PanelTargetContains(int a, int b) {
    const PanelMask ma = PanelTargetMask(a), mb = PanelTargetMask(b);
    return ma != mb && (ma & mb) == mb;
}

// The immediate parent — the SMALLEST target that strictly contains t. Smallest,
// so "capt PFD" nests under "Screens" and not directly under "Whole panel".
static int PanelTargetParent(int t) {
    int best = -1, bestBreadth = 0;
    for (int c = kPanelRectCount; c < kPanelTargetCount; ++c) {
        if (!PanelTargetContains(c, t)) continue;
        const int b = PanelTargetBreadth(c);
        if (best < 0 || b < bestBreadth) { best = c; bestBreadth = b; }
    }
    return best;
}

// The FCU readout row — the target the tint aims at. A group, not a rect: the
// captain's and F/O's baro windows are part of the same readout row and want the
// same treatment, but are separate UV islands (see kPanelGroups).
static const char* kFcuRowName = "FCU row";
static int FcuRowTarget() { return GroupTargetByName(kFcuRowName); }

// Name of a target in the shared index space, for logs and menus.
static const char* PanelTargetName(int t) {
    if (t == -1) return "all";
    if (t >= 0 && t < kPanelRectCount) return kPanelRectsHi[t].name;
    if (t >= kPanelRectCount && t < kPanelTargetCount)
        return kPanelGroups[t - kPanelRectCount].name;
    return "FLOOD (whole panel)";
}

// The whole panel in one quad — deliberately oversized so a coordinate-space
// guess that is off by 4x either way still covers something. This is the only
// mode whose negative result is meaningful: if the cockpit does not go magenta
// under the flood, no rect is going to appear either.
static const int kProbeFloodRect = -2;
static const PanelRect kFloodRect = { "FLOOD", -kPanelAtlas, 2.0f * kPanelAtlas,
                                               -kPanelAtlas, 2.0f * kPanelAtlas };

static XPLMDataRef  gPanelRenderTypeRef = nullptr;
static bool gPanelProbeRegistered = false;
static int  gPanelTypesSeen = 0;                  // bitmask of panel_render_type values

#if PHOTON_DEV
static XPLMDataRef  gPanelProbeModeRef = nullptr;
static XPLMDataRef  gPanelProbeRectRef = nullptr;
static XPLMDataRef  gPanelProbeDrawRef = nullptr;
static XPLMDataRef  gPanelProbeTypeRef = nullptr;
static XPLMDataRef  gPanelTintRefs[3] = { nullptr, nullptr, nullptr };
static XPLMDataRef  gPanelExtentRefs[4] = { nullptr, nullptr, nullptr, nullptr }; // l b r t
static XPLMCommandRef gPanelProbeCycleCmd = nullptr;

static int  gPanelProbeMode = kProbeAfterGauges;  // on by default: this build exists to look
static int  gPanelProbeRect = kProbeFloodRect;    // -2 = flood, -1 = all rects, else index
static int  gPanelProbeDrawMethod = kDrawClear;   // the one that bypasses the most
// Latches are per panel_render_type, not global. X-Plane runs the 3-d panel
// through TWO passes — 1 = non-lit, 2 = lit — and they need not share a
// framebuffer. A single bool logged whichever pass happened to run first and
// silently hid the other, which is exactly the question "is this the LIT fbo?".
static int  gPanelStateLoggedMask = 0;            // bit N = type N dumped; cleared on any change
static int  gPanelErrLoggedMask = 0;              // same, for the post-draw glGetError
static int  gPanelProbeType = -1;                 // -1 = both passes, else only that type

// The multiply color. Green-suppressing, to pull ToLiss's yellow-green FCU
// readouts toward the orange the real unit shows. This is a SEED, not a tuned
// value — it has never been looked at in-sim. Tune it through the datarefs and
// copy the accepted numbers back here; never pick it off a screenshot, for the
// same reason the light tuner exists.
static constexpr float kPanelTintDefault[3] = { 1.00f, 0.82f, 0.90f };
static float gPanelTint[3] = { kPanelTintDefault[0], kPanelTintDefault[1],
                               kPanelTintDefault[2] };

static int ReadPanelProbeMode(void*)          { return gPanelProbeMode; }
static void WritePanelProbeMode(void*, int v) {
    gPanelProbeMode = (v < 0 || v >= kProbeModeCount) ? kProbeOff : v;
    gPanelStateLoggedMask = gPanelErrLoggedMask = 0;   // re-log for the slot we switched to
    Log("panel probe mode = " + std::to_string(gPanelProbeMode));
}
static int ReadPanelProbeRect(void*)          { return gPanelProbeRect; }
static void WritePanelProbeRect(void*, int v) {
    gPanelProbeRect = (v < kProbeFloodRect || v >= kPanelTargetCount) ? -1 : v;
    gPanelStateLoggedMask = gPanelErrLoggedMask = 0;
    Log(std::string("panel probe rect = ") + PanelTargetName(gPanelProbeRect));
}
static int ReadPanelProbeType(void*)          { return gPanelProbeType; }
static void WritePanelProbeType(void*, int v) {
    gPanelProbeType = (v == 1 || v == 2) ? v : -1;
    gPanelStateLoggedMask = gPanelErrLoggedMask = 0;
    Log(std::string("panel probe pass = ")
        + (gPanelProbeType == 1 ? "3-d NON-LIT only"
         : gPanelProbeType == 2 ? "3-d LIT only (the 'lit panel fbo' target)"
         : "both 3-d passes"));
}
static int ReadPanelProbeDraw(void*)          { return gPanelProbeDrawMethod; }
static void WritePanelProbeDraw(void*, int v) {
    gPanelProbeDrawMethod = (v < 0 || v >= kDrawMethodCount) ? kDrawClear : v;
    gPanelStateLoggedMask = gPanelErrLoggedMask = 0;
    Log(std::string("panel probe draw method = ")
        + kDrawMethodName[gPanelProbeDrawMethod]);
}

// Tint channels. refcon carries the channel index, so one accessor pair serves
// all three. Every write is logged as the whole triple: the point of tuning is
// to end up with three numbers to paste into gPanelTint, and a log of isolated
// single-channel edits does not give you that.
static float ReadPanelTint(void* refcon) {
    return gPanelTint[(int)(intptr_t)refcon];
}
static void WritePanelTint(void* refcon, float v) {
    const int ch = (int)(intptr_t)refcon;
    gPanelTint[ch] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);   // multiply, so 0..1
    char buf[96];
    std::snprintf(buf, sizeof(buf), "panel tint = %.3f %.3f %.3f",
                  gPanelTint[0], gPanelTint[1], gPanelTint[2]);
    Log(buf);
}

#endif  // PHOTON_DEV — the probe's own datarefs and their accessors

// ---- the quad batch ---------------------------------------------------------
// One glBegin/glEnd around all of a target's quads, and the winding they are
// emitted in. Both live here rather than in EmitRectQuad because a target is
// usually a group — see the note there.
//
// The winding is a GLOBAL rather than a parameter for the same reason gFxAlpha
// and gFxTile are: EmitRectQuad is reached through PanelPaintFn, a plain function
// pointer the probe shares, which has no room for it. Keeping it as a stale
// global would be the classic version of this codebase's worst failure mode — a
// wrong winding is SILENTLY CULLED, i.e. it looks exactly like the compositor not
// running — so it is only ever written by the batch, and a dev build says so
// loudly if a quad is emitted outside one.
static GLint gQuadFront     = GL_CCW;
static bool  gQuadBatchOpen = false;

struct PanelQuadBatch {
    PanelQuadBatch() {
        // Queried per batch, not per pass: X-Plane owns this state, and the probe
        // and the compositor can both run in one frame with the sim's own drawing
        // in between. Once per layer is cheap; once per quad was 61 driver
        // queries a frame.
        gQuadFront = GL_CCW;
        glGetIntegerv(GL_FRONT_FACE, &gQuadFront);
        glBegin(GL_QUADS);
        gQuadBatchOpen = true;
    }
    ~PanelQuadBatch() {
        gQuadBatchOpen = false;
        glEnd();
    }
    PanelQuadBatch(const PanelQuadBatch&) = delete;
    PanelQuadBatch& operator=(const PanelQuadBatch&) = delete;
};

// Panel drawing coordinates ARE the 3-d panel's pixel grid, so the rects go out
// verbatim. See the coordinate warning in the block comment above for why this
// no longer rescales through panel_total_pnl_* — those extents describe the
// (empty) 2-d panel and rescaling by them was what made the first run invisible.
// X-Plane leaves GL_CULL_FACE ENABLED in the panel pass (measured: cull=1), so a
// quad wound the wrong way is silently culled and looks exactly like "drawing
// does not work here". Rather than fight that with a global glDisable — which
// leaks into the sim's own rendering — emit whichever winding the CURRENT
// glFrontFace setting considers front-facing. The real feature must do the same.
// Texture coordinates go out on every vertex whether or not a texture is bound —
// glTexCoord2f with the texture unit off is a no-op, and the alternative is two
// near-identical emitters. `tile` is how many times the image repeats across the
// rect, so 1 stretches it to fit.
//
// ⚠ THE QUADS OF ONE LAYER ARE ONE BATCH — glBegin and glEnd are PanelQuadBatch's,
// not this function's, and the winding query moved there with them. Two reasons,
// and the second is not an optimization:
//
//  * A target is usually a GROUP: "Main displays" is six quads that differ only in
//    color, and glColor4f is legal BETWEEN glBegin and glEnd (it sets the current
//    vertex attribute). Six begin/end pairs were paying six state transitions to
//    emit twenty-four vertices.
//  * glGetIntegerv is NOT legal inside a begin/end block. Keeping the query here
//    while batching outside would raise GL_INVALID_OPERATION on every quad and
//    read as "the tint stopped drawing" — so the two halves of this change are one
//    change, and the winding is queried once per batch instead of once per quad.
static void EmitRectQuad(const PanelRect& r, float cr, float cg, float cb, float ca,
                         float tile) {
    // Vertices outside a begin/end block are GL_INVALID_OPERATION and draw
    // NOTHING — the same shape of silent failure as a wrongly wound quad, and the
    // one way this split can be got wrong. Say it once rather than leaving a
    // blank cockpit and a clean log.
    if (!gQuadBatchOpen) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log("panel fx: EmitRectQuad called with no PanelQuadBatch open - the "
                "quad is being DROPPED. Every caller must open one around its "
                "PaintPanelTarget call.");
        }
        return;
    }

    const float t = tile;
    glColor4f(cr, cg, cb, ca);
    if (gQuadFront == GL_CCW) {
        glTexCoord2f(0.0f, 0.0f); glVertex2f(r.u0, r.v0);
        glTexCoord2f(t,    0.0f); glVertex2f(r.u1, r.v0);
        glTexCoord2f(t,    t);    glVertex2f(r.u1, r.v1);
        glTexCoord2f(0.0f, t);    glVertex2f(r.u0, r.v1);
    } else {
        glTexCoord2f(0.0f, 0.0f); glVertex2f(r.u0, r.v0);
        glTexCoord2f(0.0f, t);    glVertex2f(r.u0, r.v1);
        glTexCoord2f(t,    t);    glVertex2f(r.u1, r.v1);
        glTexCoord2f(t,    0.0f); glVertex2f(r.u1, r.v0);
    }
}

// The probe's own fills are opaque and untextured; the FX compositor needs
// per-layer alpha and tiling, and PanelPaintFn's signature has no room for
// either. Both are set immediately before each PaintPanelTarget call.
static float gFxAlpha = 1.0f;
static float gFxTile  = 1.0f;

// Forward declaration: the FX rect painter needs the brightness drivers, which
// are declared with the layer stacks further down. Defined there.
static void DrawFxRect(const PanelRect& r, int rectIndex, float cr, float cg, float cb);

#if PHOTON_DEV
static void DrawProbeRect(const PanelRect& r, int, float cr, float cg, float cb) {
    EmitRectQuad(r, cr, cg, cb, 1.0f, 1.0f);
}

// glClear through a scissor box. The value of this path is everything it does
// NOT touch: no vertices, no shader, no matrix stack, no culling, no blending.
// A negative result here is the strongest possible one.
static void ClearProbeRect(const PanelRect& r, int, float cr, float cg, float cb) {
    const float lo_x = r.u0 < r.u1 ? r.u0 : r.u1, hi_x = r.u0 < r.u1 ? r.u1 : r.u0;
    const float lo_y = r.v0 < r.v1 ? r.v0 : r.v1, hi_y = r.v0 < r.v1 ? r.v1 : r.v0;

    GLboolean hadScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint     oldBox[4];   glGetIntegerv(GL_SCISSOR_BOX, oldBox);
    GLfloat   oldClear[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, oldClear);

    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)lo_x, (GLint)lo_y, (GLsizei)(hi_x - lo_x), (GLsizei)(hi_y - lo_y));
    glClearColor(cr, cg, cb, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Leave the shared state as we found it — this runs mid-frame, inside
    // X-Plane's own panel pass, and a stray scissor box would corrupt it.
    glClearColor(oldClear[0], oldClear[1], oldClear[2], oldClear[3]);
    glScissor(oldBox[0], oldBox[1], oldBox[2], oldBox[3]);
    if (!hadScissor) glDisable(GL_SCISSOR_TEST);
}
#endif  // PHOTON_DEV — the probe's two rect painters

// Paint whatever the shared target index selects: the flood quad (-2), every
// rect (-1), one rect, or every member of a group. The group members are
// resolved by name against the CURRENT set, so a group follows the resolution
// switch for free — members that are resolution-gated get their low-res rect and
// members that are not (the whole FCU row) get their only one.
//
// The paint function is handed the rect's INDEX as well as its geometry, which
// the probe ignores and the compositor needs: brightness is per screen, so a
// group has to be able to dim each member by that member's own knob rather than
// by one factor for the whole group. Deriving the index from the PanelRect&
// (pointer arithmetic against the live table) would work and would be a landmine
// the first time someone paints a rect that is not an element of it — kFloodRect
// already is not.
typedef void (*PanelPaintFn)(const PanelRect&, int, float, float, float);

// Every group's members as INDICES, resolved once. The names are what the tables
// are authored and read in — that is the house rule and it is not changing — but
// resolving them is a strcmp walk of the whole rect table, and this ran per
// member per layer per PASS: "Main displays" alone did 24 walks a frame to
// rediscover the same six numbers.
//
// ⚠ Safe only because a NAME MEANS THE SAME INDEX IN BOTH RECT TABLES. The two
// are the same length (static_assert) and the same order — only the six DUs'
// VALUES differ between them — which is why the resolved index can be looked up
// once and then applied to whichever set PanelRects() hands back. The old code
// already relied on this: it searched kPanelRectsHi for the name and indexed the
// live set with the answer. A member that does not resolve keeps its -1 and is
// skipped exactly as before; kPanelGroups and kPanelRectsHi are both static const,
// so there is nothing that could later invalidate this.
static const int kPanelGroupMaxMembers =
    (int)(sizeof(kPanelGroups[0].members) / sizeof(kPanelGroups[0].members[0]));

struct PanelGroupMembers {
    int idx[kPanelGroupMaxMembers];
    int count = 0;
};

static const PanelGroupMembers& GroupMembers(int group) {
    static PanelGroupMembers table[kPanelGroupCount];
    static bool built = false;
    if (!built) {
        built = true;
        for (int g = 0; g < kPanelGroupCount; ++g) {
            const PanelGroup& src = kPanelGroups[g];
            PanelGroupMembers& dst = table[g];
            for (int m = 0; m < kPanelGroupMaxMembers && src.members[m]; ++m) {
                const int i = RectIndexByName(src.members[m]);
                // A miss is a member naming a rect that no longer exists, i.e. a
                // rename that did not update this table. Say so once — silently
                // painting a smaller group is the failure this table's naming
                // rule exists to prevent.
                if (i < 0) Log(std::string("panel fx: group '") + src.name
                               + "' names no rect '" + src.members[m]
                               + "' - that member is being skipped");
                else dst.idx[dst.count++] = i;
            }
        }
    }
    return table[group];
}

static void PaintPanelTarget(int target, PanelPaintFn paint,
                             float cr, float cg, float cb) {
    const PanelRect* rects = PanelRects();
    if (target == kProbeFloodRect) { paint(kFloodRect, -1, cr, cg, cb); return; }
    if (target < 0) {
        for (int i = 0; i < kPanelRectCount; ++i) paint(rects[i], i, cr, cg, cb);
        return;
    }
    if (target < kPanelRectCount) { paint(rects[target], target, cr, cg, cb); return; }
    if (target >= kPanelTargetCount) return;

    const PanelGroupMembers& g = GroupMembers(target - kPanelRectCount);
    for (int m = 0; m < g.count; ++m) {
        const int i = g.idx[m];
        paint(rects[i], i, cr, cg, cb);
    }
}

// ---- "which render target are we actually on?" -----------------------------
// The whole point of the rebuild: the first probe never asked. A quad that goes
// nowhere and a quad that goes somewhere invisible look identical from the
// cockpit, and only the framebuffer id distinguishes them.
//
// Expected if we are on the panel target: a NON-ZERO draw framebuffer whose
// viewport is the panel texture size (4096x4096 here), with a color attachment
// of type TEXTURE. Framebuffer 0, or a viewport matching the monitor resolution,
// means we are drawing into the main 3-d scene and never touching the panel.
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#endif

// glGetFramebufferAttachmentParameteriv is GL 3.0; the Windows GL/gl.h stops at
// 1.1, so it is resolved at run time. Not fatal if it fails — the framebuffer id
// and viewport already answer the main question; the texture name is the bonus
// that lets the result be matched against the sim's texture browser.
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif

// The blend func is queried as four separate factors (GL 1.4 split RGB from
// alpha). Not to be confused with GL_SRC_ALPHA / GL_DST_COLOR, which are blend
// FACTORS and live in the 0x03xx range; these are the query enums.
#ifndef GL_BLEND_DST_RGB
#define GL_BLEND_DST_RGB   0x80C8
#endif
#ifndef GL_BLEND_SRC_RGB
#define GL_BLEND_SRC_RGB   0x80C9
#endif
#ifndef GL_BLEND_DST_ALPHA
#define GL_BLEND_DST_ALPHA 0x80CA
#endif
#ifndef GL_BLEND_SRC_ALPHA
#define GL_BLEND_SRC_ALPHA 0x80CB
#endif

// The blend EQUATION — the operator between the two weighted terms, where the
// func supplies the weights. GL 1.4 / EXT_blend_minmax + EXT_blend_subtract.
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD              0x8006
#endif
#ifndef GL_MIN
#define GL_MIN                   0x8007
#endif
#ifndef GL_MAX
#define GL_MAX                   0x8008
#endif
#ifndef GL_FUNC_SUBTRACT
#define GL_FUNC_SUBTRACT         0x800A
#endif
#ifndef GL_FUNC_REVERSE_SUBTRACT
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#endif
#ifndef GL_BLEND_EQUATION_RGB
#define GL_BLEND_EQUATION_RGB    0x8009
#endif
#ifndef GL_BLEND_EQUATION_ALPHA
#define GL_BLEND_EQUATION_ALPHA  0x883D
#endif

#if IBM
typedef void (APIENTRY *PfnGetFbAttachParam)(GLenum, GLenum, GLenum, GLint*);
typedef void (APIENTRY *PfnUseProgram)(GLuint);
typedef void (APIENTRY *PfnBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void (APIENTRY *PfnBlendEquation)(GLenum);
typedef void (APIENTRY *PfnBlendEquationSeparate)(GLenum, GLenum);
#else
typedef void (*PfnGetFbAttachParam)(GLenum, GLenum, GLenum, GLint*);
typedef void (*PfnUseProgram)(GLuint);
typedef void (*PfnBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void (*PfnBlendEquation)(GLenum);
typedef void (*PfnBlendEquationSeparate)(GLenum, GLenum);
#endif

// GL 2.0+, so not in the Windows gl.h. Resolved once, lazily; null is survivable
// everywhere it is used.
static PfnUseProgram GlUseProgram() {
#if IBM
    static PfnUseProgram fn = (PfnUseProgram)wglGetProcAddress("glUseProgram");
    return fn;
#else
    return nullptr;
#endif
}

// GL 1.4. Only ever used to RESTORE a split RGB/alpha blend func exactly; the
// multiply itself sets the same factors on both halves, so plain glBlendFunc
// (GL 1.1, always present) is a complete substitute on the setting side.
static PfnBlendFuncSeparate GlBlendFuncSeparate() {
#if IBM
    static PfnBlendFuncSeparate fn =
        (PfnBlendFuncSeparate)wglGetProcAddress("glBlendFuncSeparate");
    return fn;
#else
    return nullptr;
#endif
}

// GL 1.4. Unlike the func, there is no substitute: the four non-additive blend
// modes (Subtract, Burn, Darken, Lighten) ARE an equation change and nothing in
// GL 1.1 approximates them, so a layer that needs one is skipped rather than
// drawn additively. Null here is therefore survivable but not free — SetFxBlend
// says so in Log.txt, once.
static PfnBlendEquation GlBlendEquation() {
#if IBM
    static PfnBlendEquation fn = (PfnBlendEquation)wglGetProcAddress("glBlendEquation");
    return fn;
#else
    return nullptr;
#endif
}
static PfnBlendEquationSeparate GlBlendEquationSeparate() {
#if IBM
    static PfnBlendEquationSeparate fn =
        (PfnBlendEquationSeparate)wglGetProcAddress("glBlendEquationSeparate");
    return fn;
#else
    return nullptr;
#endif
}

#if PHOTON_DEV
static void LogGlTarget(const char* what, int renderType) {
    GLint fbo = -1, vp[4] = { -1, -1, -1, -1 };
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, vp);

    GLint attType = -1, attName = -1;
#if IBM
    static PfnGetFbAttachParam getAtt =
        (PfnGetFbAttachParam)wglGetProcAddress("glGetFramebufferAttachmentParameteriv");
    if (getAtt && fbo > 0) {
        getAtt(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
               GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attType);
        getAtt(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
               GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attName);
    }
#endif

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "panel probe: GL target (%s, render_type=%d %s) fbo=%d viewport=%d,%d %dx%d "
        "color0_type=0x%X color0_tex=%d",
        what, renderType,
        renderType == 1 ? "3-d NON-LIT" : renderType == 2 ? "3-d LIT" : "?",
        (int)fbo, (int)vp[0], (int)vp[1], (int)vp[2], (int)vp[3],
        (unsigned)attType, (int)attName);
    Log(buf);
    if (fbo == 0)
        Log("panel probe: ^ framebuffer 0 = the MAIN SCENE, not the panel FBO. "
            "Nothing drawn here can ever land on the panel texture.");

    // Everything that can silently swallow a correctly-aimed quad. A bound
    // program is the prime suspect: with one active, glColor/glVertex feed that
    // shader's attributes instead of the fixed-function pipeline, and the result
    // is whatever it happens to compute — usually nothing visible.
    GLint prog = -1, scissorBox[4] = { -1, -1, -1, -1 };
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    GLboolean mask[4] = { 2, 2, 2, 2 };
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    GLint front = 0, cullMode = 0;
    glGetIntegerv(GL_FRONT_FACE, &front);
    glGetIntegerv(GL_CULL_FACE_MODE, &cullMode);
    std::snprintf(buf, sizeof(buf),
        "panel probe: GL state program=%d cull=%d(front=%s mode=0x%X) depth=%d "
        "blend=%d tex2d=%d scissor=%d box=%d,%d %dx%d writemask=%d%d%d%d",
        (int)prog, (int)glIsEnabled(GL_CULL_FACE),
        front == GL_CCW ? "CCW" : front == GL_CW ? "CW" : "?", (unsigned)cullMode,
        (int)glIsEnabled(GL_DEPTH_TEST),
        (int)glIsEnabled(GL_BLEND), (int)glIsEnabled(GL_TEXTURE_2D),
        (int)glIsEnabled(GL_SCISSOR_TEST),
        (int)scissorBox[0], (int)scissorBox[1], (int)scissorBox[2], (int)scissorBox[3],
        (int)mask[0], (int)mask[1], (int)mask[2], (int)mask[3]);
    Log(buf);
    if (prog > 0)
        Log("panel probe: ^ a shader program is BOUND — immediate-mode color will "
            "not reach the framebuffer as written. Try draw method 2 (Forced).");

    // If the projection is identity, X-Plane never set up panel coordinates for
    // us and our 0..4096 vertices are far outside clip space.
    GLfloat proj[16] = { 0 }, mv[16] = { 0 };
    glGetFloatv(GL_PROJECTION_MATRIX, proj);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    std::snprintf(buf, sizeof(buf),
        "panel probe: proj diag %.4f %.4f %.4f %.4f tx,ty %.3f %.3f | "
        "modelview diag %.4f %.4f %.4f %.4f",
        proj[0], proj[5], proj[10], proj[15], proj[12], proj[13],
        mv[0], mv[5], mv[10], mv[15]);
    Log(buf);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::snprintf(buf, sizeof(buf),
            "panel probe: glGetError after state query = 0x%X "
            "(0x502 GL_INVALID_OPERATION here usually means a CORE profile "
            "context, in which immediate mode does not exist at all)",
            (unsigned)err);
        Log(buf);
    }
}

// Strip the pipeline back to something an immediate-mode quad can survive, and
// impose our own panel-pixel projection instead of trusting the sim's.
//
// ⚠ EVERY toggle here MUST be restored. The first version popped only the two
// matrices and left the enables changed, which meant one run of Forced disabled
// GL_CULL_FACE for the whole rest of the frame — corrupting X-Plane's own
// rendering, and making the Immediate method appear to start working once
// Forced had been used at least once. That is not a probe artifact to shrug at:
// it silently invalidated the comparison the probe exists to make. Confirmed in
// the log by every state dump reading cull=1 before the first Forced run and
// cull=0 forever after.
static struct {
    GLboolean cull, depth, blend, tex2d, mask[4];
    GLint     program;
} gForcedSaved;

static void PushForcedState() {
    gForcedSaved.cull  = glIsEnabled(GL_CULL_FACE);
    gForcedSaved.depth = glIsEnabled(GL_DEPTH_TEST);
    gForcedSaved.blend = glIsEnabled(GL_BLEND);
    gForcedSaved.tex2d = glIsEnabled(GL_TEXTURE_2D);
    glGetBooleanv(GL_COLOR_WRITEMASK, gForcedSaved.mask);
    gForcedSaved.program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &gForcedSaved.program);

    if (PfnUseProgram up = GlUseProgram()) up(0);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (GLdouble)kPanelAtlas, 0.0, (GLdouble)kPanelAtlas, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
}

static void PopForcedState() {
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    if (gForcedSaved.cull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
    if (gForcedSaved.depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (gForcedSaved.blend) glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (gForcedSaved.tex2d) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
    glColorMask(gForcedSaved.mask[0], gForcedSaved.mask[1],
                gForcedSaved.mask[2], gForcedSaved.mask[3]);
    if (gForcedSaved.program > 0)
        if (PfnUseProgram up = GlUseProgram()) up((GLuint)gForcedSaved.program);
}
#endif  // PHOTON_DEV — LogGlTarget and the Forced draw method's state stack

// Blending is ALREADY enabled in this pass (measured), so only the func changes
// — but the func is shared state on the same footing as the enables above, and
// leaking it corrupts every blended thing X-Plane draws for the rest of the pass.
//
// ⚠ Push AFTER XPLMSetGraphicsState, never before. That call is X-Plane's own
// state tracker and sets a blend func of its own when it enables blending; what
// we want to restore is the func the TRACKER believes is current. Saving first
// puts the tracker and the real GL state out of step.
//
// ⚠ The EQUATION is saved and restored on exactly the same footing, and it
// matters more than the func: leaking GL_MIN or a reverse subtract turns every
// subsequent blended draw in the pass into a darkening operation, which is a
// whole-cockpit artifact rather than a wrong-looking rect. It is saved here
// rather than around each layer because the compositor sets it per layer and
// only the value in force before we ran is worth restoring.
static struct { GLint srcRGB, dstRGB, srcA, dstA, eqRGB, eqA; } gBlendSaved;

// Save the func without imposing one. The FX compositor sets its own per layer;
// the probe's multiply mode is this plus one glBlendFunc.
static void PushBlendFunc() {
    gBlendSaved.srcRGB = gBlendSaved.srcA = GL_ONE;
    gBlendSaved.dstRGB = gBlendSaved.dstA = GL_ZERO;
    gBlendSaved.eqRGB  = gBlendSaved.eqA  = GL_FUNC_ADD;
    glGetIntegerv(GL_BLEND_SRC_RGB,   &gBlendSaved.srcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB,   &gBlendSaved.dstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &gBlendSaved.srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &gBlendSaved.dstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB,   &gBlendSaved.eqRGB);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &gBlendSaved.eqA);
}

static void PopBlendFunc() {
    // Prefer the exact restore. Without glBlendFuncSeparate a split RGB/alpha
    // func collapses to its RGB half — imperfect, but strictly better than
    // leaving our own func in place, and X-Plane's panel pass has not been seen
    // using a split func.
    if (PfnBlendFuncSeparate sep = GlBlendFuncSeparate())
        sep((GLenum)gBlendSaved.srcRGB, (GLenum)gBlendSaved.dstRGB,
            (GLenum)gBlendSaved.srcA,   (GLenum)gBlendSaved.dstA);
    else
        glBlendFunc((GLenum)gBlendSaved.srcRGB, (GLenum)gBlendSaved.dstRGB);

    if (PfnBlendEquationSeparate seps = GlBlendEquationSeparate())
        seps((GLenum)gBlendSaved.eqRGB, (GLenum)gBlendSaved.eqA);
    else if (PfnBlendEquation eq = GlBlendEquation())
        eq((GLenum)gBlendSaved.eqRGB);
}

#if PHOTON_DEV
// The probe's Multiply draw method. The compositor does not use this — it sets a
// func per LAYER through SetFxBlend, of which multiply is one of eight.
static void PushMultiplyBlend() {
    PushBlendFunc();
    glBlendFunc(GL_DST_COLOR, GL_ZERO);
}
static void PopMultiplyBlend() { PopBlendFunc(); }
#endif

// ===================== panel FX: the overlay texture pool ====================
//
// A layer can be an IMAGE instead of a flat color: a gradient, a vignette, a
// scanline pattern, a hand-painted glow. The image modulates the layer color
// (fixed-function GL_MODULATE, i.e. src = texel x color), so every control that
// worked on a solid layer still works — the color tints the image and the
// opacity fades it out.
//
// Images live in ONE folder next to the plugin and are picked by name. No file
// dialog: there is no portable one in ImGui, a path typed into a text box is a
// support burden, and a folder the user drops PNGs into is both simpler and what
// makes a look reproducible on another machine.
//
// ⚠ The name is what gets SAVED, so renaming a file breaks the layers using it —
// deliberately, and reported: LoadPanelFx already logs dropped targets, and a
// missing texture is logged the same way rather than silently drawing a solid.
//
// ⚠ REFRESH IS MANUAL, and one operation (2026-08-04). The pool used to stat()
// every file once a second FROM THE COMPOSITOR, so a save in a paint program
// reached the cockpit with nothing clicked. That is a nice inner loop for
// authoring a look and it is the only thing it is good for: a shipped install's
// overlays change when the installer replaces them and at no other time, so every
// user was paying a per-second filesystem walk on the render thread for a
// developer's convenience. ScanFxTextures already compared stamps and marked
// changed files stale, so the tab's "Rescan images" button was always doing the
// whole job — adding, renaming AND editing — and the split into two operations
// was the part that needed explaining, not the part that earned its keep.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

// What a file looked like when we last read it. Size AND mtime, because either
// one alone misses a real edit: a paint program that rewrites the same pixel
// count keeps the size, and a few tools (and every unzip) preserve the mtime.
struct FxStamp {
    std::uintmax_t     size = 0;
    fs::file_time_type mtime{};
    bool operator==(const FxStamp& o) const { return size == o.size && mtime == o.mtime; }
    bool operator!=(const FxStamp& o) const { return !(*this == o); }
};

static FxStamp FxStampOf(const fs::path& p) {
    FxStamp s;
    std::error_code ec;
    s.size  = fs::file_size(p, ec);       if (ec) s.size = 0;
    s.mtime = fs::last_write_time(p, ec); if (ec) s.mtime = fs::file_time_type{};
    return s;
}

struct FxTexture {
    std::string name;                  // file name, as shown and as saved
    int  id     = 0;                   // X-Plane texture number; 0 = not uploaded yet
    int  w      = 0, h = 0;
    bool failed = false;               // tried and could not decode — do not retry
    // Live reload: the stamp of the bytes currently ON the card, and the flag
    // that says the file has moved on since. Editing an overlay in a paint
    // program and saving it is the inner loop this whole folder exists for, and
    // an alt-tab that shows the OLD image is indistinguishable from an edit that
    // did nothing.
    FxStamp stamp;
    bool    stale = false;
};
static std::vector<FxTexture> gFxTextures;
static bool gFxTexturesScanned = false;

// The compositor draws every frame, so its complaints are logged once — but
// "the file is not there" is the one a rescan is expected to fix, and a warning
// that never comes back cannot tell you whether it did. Reset by ScanFxTextures.
static bool gFxMissingWarned = false;

// The stand-in bound for layers that have no image. Binding a 1x1 opaque white
// texture makes an untextured layer arithmetically identical to a textured one
// (white x color = color), which is what lets the compositor set the graphics
// state ONCE per pass instead of toggling the texture unit per layer — and
// toggling it per layer would mean calling XPLMSetGraphicsState inside the loop,
// which re-enters X-Plane's blend-func tracker under a func we set ourselves.
static int gFxWhiteTex = 0;

// <plugin>/overlays/, i.e. Resources/plugins/ToLissPhoton/overlays/. Derived
// from our own .xpl path rather than assembled from XPLMGetSystemPath, so it
// follows the plugin if the folder is renamed; the system path is the fallback
// for the case where that lookup returns something unusable.
// Resolved once: the UI shows this path every frame, and it cannot change while
// the plugin is loaded.
// Our own plugin folder — Resources/plugins/ToLissPhoton/. Derived from the .xpl
// path rather than assembled from XPLMGetSystemPath, so it follows the plugin if
// the folder is renamed; the system path is the fallback for the case where that
// lookup returns something unusable.
//
// Resolved once. It cannot change while the plugin is loaded, and both callers
// (the overlay pool and the FX layer definition) read it from a per-frame path.
static const fs::path& PhotonPluginDir() {
    static fs::path dir;
    if (!dir.empty()) return dir;

    char name[512] = {0}, path[512] = {0}, sig[512] = {0}, desc[512] = {0};
    XPLMGetPluginInfo(XPLMGetMyID(), name, path, sig, desc);
    const fs::path xpl(path);
    // .../ToLissPhoton/<arch>/ToLissPhoton.xpl -> .../ToLissPhoton/
    if (xpl.has_parent_path() && xpl.parent_path().has_parent_path()) {
        dir = xpl.parent_path().parent_path();
        return dir;
    }

    char root[512] = {0};
    XPLMGetSystemPath(root);
    dir = fs::path(root) / "Resources" / "plugins" / "ToLissPhoton";
    return dir;
}

static const fs::path& FxOverlayDir() {
    static fs::path dir;
    if (dir.empty()) dir = PhotonPluginDir() / "overlays";
    return dir;
}

static bool FxIsImageName(const fs::path& p) {
    std::string ext = p.extension().string();
    for (size_t i = 0; i < ext.size(); ++i) ext[i] = (char)std::tolower((unsigned char)ext[i]);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg"
        || ext == ".bmp" || ext == ".tga";
}

// Rescan the folder, keeping the texture numbers of files that are still there.
// Uploaded textures are never deleted: X-Plane hands out the numbers and the
// ImGui backend takes the same line with its font atlas.
//
// A file whose bytes have changed keeps its number too and is marked stale — the
// next draw re-uploads it INTO THE SAME number. ⚠ Never allocate a fresh one on
// reload: the old number leaks (nothing here can free one), and every layer, the
// thumbnail and the compositor would have to be told the new value.
static void ScanFxTextures() {
    const fs::path dir = FxOverlayDir();
    std::error_code ec;
    fs::create_directories(dir, ec);       // first run: make the drop folder

    const fs::path readme = dir / "README.txt";
    if (!fs::exists(readme, ec)) {
        std::ofstream f(readme, std::ios::binary);
        if (f) f <<
            "Drop PNG / JPG / BMP / TGA images here and they appear in the Panel FX\r\n"
            "layer editor (Dev window > Panel FX > a layer's Image box).\r\n"
            "\r\n"
            "The image MODULATES the layer color: the layer color tints it and the\r\n"
            "opacity fades it out, exactly as for a solid layer. Alpha is respected --\r\n"
            "a transparent texel leaves the pixel under it alone in every blend mode.\r\n"
            "\r\n"
            "Layers are saved by FILE NAME, so renaming a file here orphans the layers\r\n"
            "that used it (it is reported in Log.txt, never silently ignored).\r\n"
            "\r\n"
            "After ADDING, renaming or EDITING a file here, click the tab's\r\n"
            "\"Rescan images\" button: a changed image is re-read and re-uploaded, and\r\n"
            "the cockpit follows immediately. Nothing is picked up on its own.\r\n"
            "\r\n"
            "Square power-of-two images tile cleanly; anything stretches to fill the\r\n"
            "target unless the layer's Tile value is above 1.\r\n";
    }

    std::vector<FxTexture> found;
    int changed = 0;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || !FxIsImageName(it->path())) continue;
        FxTexture t;
        t.name = it->path().filename().string();
        for (size_t i = 0; i < gFxTextures.size(); ++i)      // keep what is uploaded
            if (gFxTextures[i].name == t.name) { t = gFxTextures[i]; break; }
        const FxStamp now = FxStampOf(it->path());
        if (t.id != 0 && now != t.stamp) {
            t.stale = true;
            ++changed;
        } else if (t.failed && now != t.stamp) {
            // The fix for an undecodable file is to edit it, so a changed file
            // gets another chance. Without this, one bad save poisons the name
            // until the sim restarts.
            t.failed = false;
            ++changed;
        }
        found.push_back(t);
    }
    std::sort(found.begin(), found.end(),
              [](const FxTexture& a, const FxTexture& b) { return a.name < b.name; });
    gFxTextures = found;
    gFxTexturesScanned = true;
    gFxMissingWarned   = false;
    Log("panel fx: " + std::to_string(gFxTextures.size()) + " overlay image(s) in "
        + dir.string()
        + (changed ? " - " + std::to_string(changed) + " changed on disk, reloading"
                   : ""));
}

// Force every image to be re-read from disk on the next draw, stamps ignored.
// The escape hatch for the case the stamp cannot see: a tool that rewrites a
// file to the same size and restores the mtime.
static void FxReloadAllTextures() {
    for (size_t i = 0; i < gFxTextures.size(); ++i) {
        gFxTextures[i].stale  = gFxTextures[i].id != 0;
        gFxTextures[i].failed = false;
    }
    gFxMissingWarned = false;
    Log("panel fx: forcing a re-read of all " + std::to_string(gFxTextures.size())
        + " overlay image(s)");
}

// ⚠ There is deliberately NO PollFxTextureChanges (removed 2026-08-04). Its whole
// body is a subset of ScanFxTextures, which the "Rescan images" button already
// calls: both stat() the folder and mark a file whose bytes have moved stale, and
// the scan additionally notices files added and removed. Reintroducing a poll
// would mean two code paths deciding what "changed" means, and the reason it was
// removed is that it ran from the compositor — see the note at the top of this
// section.

static FxTexture* FxTextureByName(const std::string& name) {
    if (name.empty()) return nullptr;
    if (!gFxTexturesScanned) ScanFxTextures();
    for (size_t i = 0; i < gFxTextures.size(); ++i)
        if (gFxTextures[i].name == name) return &gFxTextures[i];
    return nullptr;
}

// ⚠ Uploads PREMULTIPLIED alpha. Every blend mode below carries the layer's
// contribution in the color channels and its coverage in alpha, so a texel with
// alpha 0 has to contribute nothing to EITHER — and a straight-alpha PNG happily
// stores bright rgb under a zero alpha, which would then be added or multiplied
// in at full strength. Premultiplying on upload is the one place that can be
// fixed once for all eight modes.
//
// ⚠ Must run with a GL context current, i.e. from a draw callback. Both callers
// are draw callbacks (the panel pass and the ImGui window), so this is lazy on
// first use rather than done at XPluginStart.
static bool UploadFxTexture(FxTexture& t) {
    const bool reload = t.stale && t.id != 0;
    if (!reload && (t.id != 0 || t.failed)) return t.id != 0;
    // Cleared here, not after the upload: a file that now fails to decode must
    // not be re-read every frame forever. It keeps drawing the pixels already on
    // the card, which is the least surprising thing a half-written PNG can do.
    t.stale  = false;
    t.failed = true;                                  // pessimistic: set on success

    const fs::path file = FxOverlayDir() / t.name;
    // Stamped BEFORE the read and on every path out, failures included: what the
    // stamp records is "these are the bytes we last looked at", so a file that
    // cannot be decoded is retried when it changes and not once a second forever.
    const FxStamp before = FxStampOf(file);
    t.stamp = before;
    std::ifstream in(file, std::ios::binary);
    if (!in) { Log("panel fx: cannot open overlay " + file.string()); return false; }
    std::string bytes((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    if (bytes.empty()) { Log("panel fx: overlay is empty - " + t.name); return false; }

    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(),
                                        &w, &h, &comp, 4);
    if (!px) {
        Log("panel fx: could not decode overlay " + t.name + " - "
            + (stbi_failure_reason() ? stbi_failure_reason() : "unknown format"));
        return false;
    }

    for (int i = 0; i < w * h; ++i) {                 // premultiply — see above
        const unsigned a = px[i * 4 + 3];
        px[i * 4 + 0] = (stbi_uc)((px[i * 4 + 0] * a + 127) / 255);
        px[i * 4 + 1] = (stbi_uc)((px[i * 4 + 1] * a + 127) / 255);
        px[i * 4 + 2] = (stbi_uc)((px[i * 4 + 2] * a + 127) / 255);
    }

    if (t.id == 0) XPLMGenerateTextureNumbers(&t.id, 1);   // reload reuses it
    XPLMBindTexture2d(t.id, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // GL_REPEAT so a layer's Tile value above 1 actually repeats. A tile of 1
    // samples 0..1 exactly, where repeat and clamp agree.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);

    t.w = w; t.h = h; t.failed = false;

    // The file may have been written WHILE we read it — a paint program's save
    // is not atomic, and the poll below fires on the first stat that differs.
    // A changed stamp means the bytes on the card are half of something, so ask
    // for one more pass rather than leaving a torn image up.
    const FxStamp after = FxStampOf(file);
    if (after != before) { t.stamp = after; t.stale = true; }

    char buf[256];
    std::snprintf(buf, sizeof(buf), "panel fx: overlay '%s' %s - %dx%d, texture %d",
                  t.name.c_str(), reload ? "reloaded" : "uploaded", w, h, t.id);
    Log(buf);
    return true;
}

static void EnsureFxWhiteTexture() {
    if (gFxWhiteTex != 0) return;
    const unsigned char white[4] = { 255, 255, 255, 255 };
    XPLMGenerateTextureNumbers(&gFxWhiteTex, 1);
    XPLMBindTexture2d(gFxWhiteTex, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
}

// ======================== panel FX: the layer stacks ==========================
//
// A TARGET (a rect or a group) carries an ordered stack of LAYERS, each a
// color, a blend mode and an opacity; the compositor walks the stacks every
// lit-pass frame. Multiply alone lands the FCU hue but cannot brighten, so the
// accepted look needs a multiply and an additive pass together
// (docs/fcu_tint_plan.md §3) — hence a stack rather than a second hard-coded pass.
//
// Layer order is bottom-first: layers[0] composites first. The editor displays
// the list reversed, top-down, the way a paint program does.
// --- blend modes -------------------------------------------------------------
// Everything a fixed-function blend func + equation can express, which is the
// whole budget: the blender only ever computes `src*sf OP dst*df` with factors
// chosen per DRAW, not per pixel.
//
// ⚠ Not possible here, and not worth faking: Overlay, Soft/Hard Light, Color
// Burn/Dodge, the HSL modes, and anything gamma- or contrast-shaped. Each needs
// the destination pixel as a shader INPUT (a branch or a divide on dst), i.e.
// the panel FBO bound as a texture and a fragment shader — a different feature,
// and one X-Plane gives no hook for.
//
// ⚠ Order here is free — the save file stores modes by NAME. But names must stay
// SINGLE WORDS: the loader reads them with `>>`.
enum {
    kFxMultiply = 0, kFxAdd, kFxScreen, kFxNormal,
    kFxSubtract, kFxBurn, kFxDarken, kFxLighten,
    kFxBlendCount
};
static const char* kFxBlendName[kFxBlendCount] = {
    "Multiply", "Add", "Screen", "Normal",
    "Subtract", "Burn", "Darken", "Lighten",
};
static const char* kFxBlendHelp[kFxBlendCount] = {
    "dst x src. Darkens only. Black masks itself, so a whole-rect multiply "
    "recolors the lit pixels and leaves the background alone.",
    "dst + src. Brightens; cannot suppress a channel. The partner to multiply.",
    "dst + src x (1-dst). Brightens, but rolls off instead of clipping.",
    "Straight alpha blend. Flat and opaque-looking — it ignores what is under it.",
    "dst - src. Removes a color rather than scaling it: subtract a little blue "
    "to warm a readout without touching how bright it is.",
    "dst + src - 1 (linear burn). Deepens harder than Multiply and reaches black "
    "sooner. Use it when Multiply leaves the background washed out.",
    "min(src, dst) per channel. A CEILING — it caps each channel at the color "
    "and leaves anything already darker alone. Clamps a blown-out white.",
    "max(src, dst) per channel. A FLOOR — sets a minimum glow without touching "
    "anything already brighter. The counterpart to Darken.",
};

// ⚠ Two modes CANNOT carry an image, and it is a property of fixed-function GL
// rather than an unfinished corner:
//
//   Burn    emits the COMPLEMENT of the layer color (that is how dst + src - 1
//           is folded into one reverse subtract), and 1 - texel is not something
//           GL_MODULATE can produce.
//   Darken  is GL_MIN, whose equation ignores the blend factors entirely, so its
//           opacity has to be carried by fading the emitted color toward white
//           — per PIXEL once a texture is involved, which again MODULATE cannot
//           express. A 0-opacity textured Darken layer would still clamp the
//           panel to the image instead of doing nothing.
//
// Both would need ARB_texture_env_combine or a fragment shader. Drawing them
// approximately would be worse than not drawing them, so a layer in this state
// is skipped and says so in the editor — the same call SetFxBlend already makes
// for a driver with no glBlendEquation.
static bool FxBlendTakesTexture(int blend) {
    return blend != kFxBurn && blend != kFxDarken;
}

// A switch that can defer to the level above it. Declared here rather than down
// with FxStack because `follow` now exists at three levels — the master switch,
// the target, and the layer — and each one's Inherit resolves to the next one
// out. See the block comment above FxStack for why Inherit TRACKS rather than
// copies, and why the master still wins when it is off.
enum { kFxInherit = -1, kFxOff = 0, kFxOn = 1 };

static bool FxTriState(int tri, bool inherited) {
    return tri < 0 ? inherited : (tri != 0);
}

struct FxLayer {
    bool  enabled  = true;
    int   blend    = kFxMultiply;
    float color[3] = { 1.00f, 0.82f, 0.90f };
    float opacity  = 1.00f;
    // Overlay image, by file name in the overlays folder. Empty = a solid fill,
    // which is the same code path with a 1x1 white texture bound.
    std::string tex;
    float tile     = 1.00f;            // UV repeats across the target

    // ---- the DAYTIME half ---------------------------------------------------
    // Absent (`hasDay` false) means this layer looks the same at noon as at
    // midnight, which is what every layer authored before the ambient blend
    // existed does — so an old panelfx.txt keeps its look exactly.
    //
    // ⚠ Only the COLOR and the OPACITY vary. The blend mode and the image do
    // not, and that is a deliberate limit rather than an unfinished corner:
    // interpolating between two modes is not defined (what is halfway between
    // GL_MIN and additive?), and cross-fading two textures would need a second
    // texture unit inside a loop this file goes to some length to keep flat.
    //
    // It costs nothing, because opacity 0 is a real state on both sides. A layer
    // that should only exist at night is one with dayOpacity 0; one that should
    // only exist in daylight is one with `opacity` 0 and a dayOpacity above it.
    // So "additive bleed after dark, a subtractive wash at noon" is two layers
    // with opposite day settings, not a second stack — and the two still
    // composite in the authored order while both are partly present at dusk.
    bool  hasDay      = false;
    float dayColor[3] = { 1.00f, 1.00f, 1.00f };
    float dayOpacity  = 0.00f;

    // ---- the COLOR RAMP: what this layer looks like at a DIM knob ----------
    // Absent (`hasRamp` false) means one color at every knob position, which is
    // what every layer authored before this existed does.
    //
    // `rampColor` is the color at brightness 0 and the layer's own `color` is
    // the color at 1 — so the END of the ramp is the color that was already
    // there, and ticking the box on an existing layer cannot change what full
    // brightness looks like. That asymmetry is the whole point: a look is
    // authored at the top of the knob and the ramp says where it CAME from.
    //
    // It is a different question from opacity, and the two do not substitute for
    // each other. A dim LCD is not the same color more faintly — it is warmer,
    // its backlight is redder, and its black is less lifted. Fading the authored
    // color toward nothing (which is all `follow` can do) never produces that.
    //
    // ⚠ It interpolates on the RAW dataref position — 0..1 after the driver's
    // lo/hi and BEFORE the response curve and the floor — not on the factor the
    // opacity is scaled by. Those are different quantities: the factor is an
    // EFFECT INTENSITY carrying our model of the backlight and an authored
    // "never fully off", while a ramp is a statement about the SCREEN, which
    // reads warmer at a quarter knob whatever we have decided to paint there.
    // On the factor, the ramp also moved whenever a curve was edited and put its
    // midpoint wherever that curve crossed 0.5 rather than at half a turn.
    //
    // ⚠ And it is NOT gated on `follow`: `follow` answers "does the opacity
    // track the knob", this answers "does the color". A color correction that
    // must stay at full strength while powered (§8f) is exactly the layer most
    // likely to want a ramp, so tying the two together would make the feature
    // unavailable in its own primary case. The master switch still outranks
    // both — with it off, every layer draws at knob 1, which is the color it was
    // authored as.
    //
    // ⚠ The start color has NO daylight half, the same deliberate limit the
    // blend mode and the image have (§8e): the ramp interpolates toward the
    // ambient-resolved color, so the day/night blend still moves the END. Two
    // eras of ramp is two layers with opposite day opacities, not four colors
    // on one layer.
    bool  hasRamp      = false;
    float rampColor[3] = { 1.00f, 1.00f, 1.00f };

    // ---- does THIS layer dim with the screen's brightness knob? -------------
    // The same tri-state as FxStack::follow, one level further in, and it reads
    // the stack's setting as its inherited value.
    //
    // It exists because the two things a layer does are not the same kind of
    // thing. A bleed or a backlight wash IS the screen's light and has to track
    // the knob. A color correction — "this LCD is greener than the texture
    // says" — is a property of the GLASS, and dimming it toward nothing as the
    // knob comes down means the readout drifts back to the untinted color
    // exactly where the tint is most visible. Before this, one setting had to
    // cover both and the stack was tuned to whichever mattered more.
    //
    // ⚠ Off does NOT mean ungated: the power, bus and breaker gates still apply,
    // the same way they do for a target with follow = Off. "Persistent while
    // powered" is the state this offers; "painted over a dead screen" is not.
    int   follow = kFxInherit;

    // ---- and through WHAT SHAPE does it dim? --------------------------------
    // Off (the default) means the target's response switch decides, which means
    // kBrightnessCurve or a straight line. On means this layer's own knots
    // replace both, for this layer alone. See the FxCurve block for why the
    // shape belongs to a layer rather than to a target, and note that `follow`
    // is still the outer question: a layer that does not dim has no curve to
    // apply, whatever is authored here.
    FxCurve curve;

};

// ==================== panel FX: brightness and power drivers =================
//
// A tint that stays at full strength while the screen behind it is dimmed to
// nothing is the single most obvious way this feature looks fake: at 3 a.m. with
// the PFD knob at its stop, the display is black and the tint is still there,
// glowing on its own. So a target's layers can FOLLOW a dataref — the same knob
// the pilot turns — and a second one can switch them off entirely.
//
//   brightness  scales every layer's opacity by lo..hi mapped to 0..1
//   power       gates the target off below a threshold
//
// Both are the same struct because they read the same way and because a driver
// that is set to something wrong must be visible in the same UI either way.
//
// Brightness is then reshaped by kBrightnessCurve, the same curve the screen-glow
// spill lights run through, so a tint and the glow it sits under come up
// together. gFxFollowBrightness (further down) is the master switch for the whole
// mechanism; this one only removes the curve.
//
// ⚠ Both of these are DEFAULTS, not the last word: a target can override either
// (FxStack::follow / FxStack::curve). See the tri-state block above FxStack for
// why the master still wins when it is OFF.
static bool gFxCurve = true;
//
// ⚠ Resolved by NAME at run time, never bound at XPluginStart. These are ToLiss's
// datarefs, not ours: they exist only once its plugin has started and only on an
// aircraft that has them. A missing one is reported and treated as "no driver"
// (factor 1), never as zero — the failure mode of "everything went black and the
// log says nothing" is much worse than an un-dimmed tint.
struct FxDriver {
    std::string dref;                 // empty = nothing drives this
    int   index = -1;                 // array element; -1 = scalar / element 0
    float lo    = 0.0f;               // dataref value that maps to factor 0
    float hi    = 1.0f;               // dataref value that maps to factor 1
    float floorF = 0.0f;              // never scale below this (power: threshold)

    // resolution cache — a per-frame XPLMFindDataRef is a string lookup 60-120
    // times a second, and this runs inside the panel pass.
    XPLMDataRef   ref     = nullptr;
    XPLMDataTypeID types  = 0;
    bool          tried   = false;
    bool          missing = false;

    bool set() const { return !dref.empty(); }
};

// Resolve once, remember the answer, and retry only when the name changes or the
// aircraft is reloaded (FxForgetDrivers). `missing` is sticky so the log gets one
// line per dataref rather than one per frame.
static bool FxResolveDriver(FxDriver& d) {
    if (!d.set()) return false;
    if (!d.tried) {
        d.tried = true;
        d.ref   = XPLMFindDataRef(d.dref.c_str());
        d.types = d.ref ? XPLMGetDataRefTypes(d.ref) : 0;
        if (!d.ref) {
            d.missing = true;
            Log("panel fx: driver dataref '" + d.dref + "' does not exist - that "
                "target is not being dimmed. Wrong name, or the aircraft's plugin "
                "has not started yet (Rebind sources).");
        } else {
            d.missing = false;
            Log("panel fx: bound driver " + d.dref
                + (d.index >= 0 ? "[" + std::to_string(d.index) + "]" : "")
                + " (type mask " + std::to_string((int)d.types) + ")");
        }
    }
    return d.ref != nullptr;
}

// ⚠ Prefer the WIDEST advertised type — unlike ReadDataRefTyped, which tries int
// first and is right to, because its callers read flags and enumerations.
//
// A driver reads a KNOB. A dataref registered with both an int and a float reader
// (ToLiss does this on some of its own, exactly as this plugin does on the
// category refs) read through the int one TRUNCATES: a 0..1 brightness is 0 for
// the whole travel of the knob bar its very top, and the symptom is a tint that
// simply never appears — no error, nothing in the log, and the dataref inspector
// showing a perfectly healthy value the whole time. Nothing a driver reads is a
// count, so nothing here wants int while a float is on offer.
static bool FxReadScalar(XPLMDataRef ref, XPLMDataTypeID types, double& out) {
    if (!ref) return false;
    if (types & xplmType_Double) { out = XPLMGetDatad(ref);        return true; }
    if (types & xplmType_Float)  { out = (double)XPLMGetDataf(ref); return true; }
    if (types & xplmType_Int)    { out = (double)XPLMGetDatai(ref); return true; }
    if (types & xplmType_FloatArray) { float v = 0.0f; if (XPLMGetDatavf(ref, &v, 0, 1) >= 1) { out = (double)v; return true; } }
    if (types & xplmType_IntArray)   { int   v = 0;    if (XPLMGetDatavi(ref, &v, 0, 1) >= 1) { out = (double)v; return true; } }
    return false;
}

// ---- the per-pass value cache -----------------------------------------------
// One compositor pass reads the SAME datarefs many times over: every layer of
// every target asks each of its rects for bus, power and brightness, so the
// shipped look read ~200 values a frame to learn about 15 distinct numbers.
// "Main displays" alone did 24 bus reads and 24 brightness reads — and an
// indexed read costs TWO cross-plugin calls, because the length probe below is a
// separate one — to fetch six knob positions and one voltage.
//
// ⚠ KEYED ON THE RESOLVED (ref, index), NOT ON THE FxDriver. Six rects on the
// same bus are six distinct FxDriver objects all naming DCBusVoltages[0]; keying
// on the object would collapse the per-layer repetition and leave the per-rect
// repetition untouched, which is most of it.
//
// ⚠ ACTIVE ONLY INSIDE A PASS. The Dev panes call FxRectFactor and FxDriverState
// to SHOW a live value — that display is the whole reason a mis-bound driver is
// distinguishable from a mis-reading one — so a cache they read through would
// freeze the numbers the pane exists to move. Outside DrawPanelFx this is off and
// every read goes to the sim.
//
// The side benefit is worth as much as the calls saved: every layer in a pass now
// sees ONE electrical snapshot, so a bus voltage crossing the threshold mid-pass
// cannot gate one layer of a screen and not the next.
struct FxValueCacheEntry {
    XPLMDataRef ref   = nullptr;
    int         index = 0;
    double      value = 0.0;
    bool        ok    = false;
};
static const int kFxValueCacheMax = 32;
static FxValueCacheEntry gFxValueCache[kFxValueCacheMax];
static int  gFxValueCacheCount = 0;
static bool gFxValueCacheOn    = false;

static void FxValueCacheBegin() { gFxValueCacheCount = 0; gFxValueCacheOn = true; }
static void FxValueCacheEnd()   { gFxValueCacheOn = false; gFxValueCacheCount = 0; }

static bool FxDriverReadUncached(FxDriver& d, double& out);

// The raw dataref value, honoring the array index. Returns false when there is
// nothing to read — the caller then behaves as if no driver were set.
static bool FxDriverRaw(FxDriver& d, double& out) {
    if (!FxResolveDriver(d)) return false;
    if (!gFxValueCacheOn) return FxDriverReadUncached(d, out);

    for (int i = 0; i < gFxValueCacheCount; ++i) {
        const FxValueCacheEntry& e = gFxValueCache[i];
        if (e.ref == d.ref && e.index == d.index) {
            out = e.value;
            return e.ok;
        }
    }

    double v = 0.0;
    const bool ok = FxDriverReadUncached(d, v);
    // A full cache falls through to reading every time rather than evicting: the
    // shipped look needs ~15 slots and a look needing more than 32 distinct
    // sources is still CORRECT here, just no faster than it was.
    if (gFxValueCacheCount < kFxValueCacheMax) {
        FxValueCacheEntry& e = gFxValueCache[gFxValueCacheCount++];
        e.ref = d.ref; e.index = d.index; e.value = v; e.ok = ok;
    }
    out = v;
    return ok;
}

// ⚠ A FAILED read is cached too. Without that, a driver naming an index past the
// end of its array retries the length probe once per rect per layer for the whole
// pass — the most expensive path in here, taken by the one case that can never
// succeed.
static bool FxDriverReadUncached(FxDriver& d, double& out) {
    if (d.index >= 0) {
        // ⚠ An array read past the end is undefined, and XPLMGetDatav* reports
        // the length when handed a null buffer. Cheap enough here, and the
        // alternative is reading whatever follows the array in ToLiss's memory.
        if (d.types & xplmType_FloatArray) {
            if (XPLMGetDatavf(d.ref, nullptr, 0, 0) <= d.index) return false;
            float v = 0.0f;
            if (XPLMGetDatavf(d.ref, &v, d.index, 1) < 1) return false;
            out = (double)v;
            return true;
        }
        if (d.types & xplmType_IntArray) {
            if (XPLMGetDatavi(d.ref, nullptr, 0, 0) <= d.index) return false;
            int v = 0;
            if (XPLMGetDatavi(d.ref, &v, d.index, 1) < 1) return false;
            out = (double)v;
            return true;
        }
        return false;                  // an index on a scalar is a mistake
    }
    return FxReadScalar(d.ref, d.types, out);
}

// 0..1, or 1 when nothing drives it. lo may be ABOVE hi — that is how an
// inverted source (a "dim" knob, a fault flag) is expressed, and it falls out of
// the arithmetic rather than needing a flag.
//
// Three stages, in this order and for a reason each: lo/hi normalizes whatever
// the source counts in onto 0..1; BrightnessResponse reshapes that into the fade
// a screen really has (kBrightnessCurve — the same curve the screen-glow lights
// use, so a tint and the spill under it come up together); the floor then lifts
// the result, so "never fully off" still means never fully off after the curve
// has flattened the bottom of the knob.
//
// ⚠ The SHAPE is a PARAMETER rather than a read of gFxCurve, because it is not
// a property of the driver or of the global: it is per target (two stacks can
// share one driver and want different shapes) and now per LAYER as well. Three
// states, carried as one pointer so no call site can express a fourth:
//
//     nullptr             linear, straight from lo to hi
//     &FxBuiltInCurve()   kBrightnessCurve, the shape the spill lights use
//     &layer.curve        knots this layer authored

// ⚠ Dev-only manual override of every BRIGHTNESS driver, as a value rather than
// a second switch: -1 is off, 0..1 pins the knob. Exactly the shape
// gFxAmbientManual has, and it is here for the same reason — authoring the dim
// end of a look otherwise means finding the rheostat for the face being edited,
// holding it down, and letting go to reach the editor.
//
// ⚠ It pins the NORMALIZED value, not the factor, so the curve and the floor
// still act on it: the whole point is watching what the authored shape does
// across the knob's travel, and pinning past the curve would show a straight
// line through it.
//
// ⚠ BRIGHTNESS ONLY. Power, bus and breaker read through FxDriverRaw and are
// deliberately untouched — those are facts about the aircraft, and a slider that
// could fake them would be a way to author a look against an electrical state
// the cockpit is not in. The tint is what this pins; a screen with no power is
// still off.
//
// A shipping build never writes it (the control is dev-only), so this is -1 for
// the whole life of a release and costs one compare per driver read.
static float gFxDriveManual = -1.0f;

#if PHOTON_DEV
// ⚠ The pin is HELD, not latched — it is in force only while the mouse is down
// inside the slider — and that needs a watchdog, not just an else branch. The
// pane that clears it is not drawn when the Dev window is closed or another tab
// is selected, so a drag interrupted by anything that hides the window (a click
// on the sim's own UI, a popped-out window closing, a reload) would otherwise
// leave every brightness driver pinned for the rest of the session, with the
// control that did it out of sight. That is the "restore every lever" rule the
// perf tool follows, in the one shape available to a control with no off state.
//
// Half a second: long enough that a frame hitch mid-drag does not drop the pin,
// short enough that letting go and looking at the cockpit shows the real knob.
static const double kFxDriveHoldSeconds = 0.5;
static double gFxDriveManualStamp = 0.0;

static void FxHoldDriveOverride(float value) {
    gFxDriveManual      = value;
    gFxDriveManualStamp = (double)XPLMGetElapsedTime();
}

static void FxReleaseDriveOverride() { gFxDriveManual = -1.0f; }

// Called once per compositor pass. Cheap by construction — one elapsed-time read
// per frame, in a dev build only.
static void FxExpireDriveOverride() {
    if (gFxDriveManual < 0.0f) return;
    if ((double)XPLMGetElapsedTime() - gFxDriveManualStamp > kFxDriveHoldSeconds)
        FxReleaseDriveOverride();
}
#endif  // PHOTON_DEV

// The knob position a driver is reporting, 0..1, BEFORE any curve. Returns false
// when there is nothing to read.
//
// ⚠ ONE function, because two places want this number — the factor below and the
// curve editor's live marker — and a marker sitting somewhere other than where
// the compositor is reading would make the plot a lie in exactly the way §8i
// exists to prevent. The override is applied HERE for the same reason: one place
// to pin, and everything downstream agrees about what is being fed in.
static bool FxDriverNorm(FxDriver& d, float* out) {
    double v = 0.0;
    // ⚠ The read happens whether or not the pin is on. Its value is discarded
    // while pinned, but FxDriverState prints it, and a readout that freezes the
    // moment you touch the slider is the readout you needed to check the slider
    // against. Same rule the ambient pin follows.
    const bool ok = FxDriverRaw(d, v);
    if (gFxDriveManual >= 0.0f) {
        *out = gFxDriveManual > 1.0f ? 1.0f : gFxDriveManual;
        return true;
    }
    if (!ok) return false;
    // A zero span is a step at `hi` rather than a division. It now goes through
    // the curve and the floor like every other value, where it used to be
    // returned raw — a driver with lo == hi AND a floor was the one case that
    // could report below its own floor. Nothing authored has a zero span.
    const float span = d.hi - d.lo;
    float f = span == 0.0f ? (v >= d.hi ? 1.0f : 0.0f)
                           : (float)((v - d.lo) / span);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    *out = f;
    return true;
}

// The factor a NORMALIZED reading produces: the shape, then the floor.
//
// Split out of FxDriverFactor so that one rect can get BOTH things it needs from
// a single read of the knob — the opacity's factor, which is shaped, and the
// color ramp's position, which is not. Reading twice would be cheap (the pass
// caches), but it would also be two chances for the two to describe different
// frames of a knob being turned.
static float FxShapeFactor(const FxDriver& d, const FxCurve* shape, float f) {
    if (shape) f = FxCurveEval(*shape, f);
    return d.floorF + f * (1.0f - d.floorF);
}

static float FxDriverFactor(FxDriver& d, const FxCurve* shape) {
    float f = 0.0f;
    if (!FxDriverNorm(d, &f)) return 1.0f;
    return FxShapeFactor(d, shape, f);
}

// Power is a threshold, not a ramp: floorF carries the value it must exceed.
static bool FxDriverPowered(FxDriver& d) {
    double v = 0.0;
    if (!FxDriverRaw(d, v)) return true;      // unknown = on; never blank silently
    return v > (double)d.floorF;
}

// Everything a driver is doing right now, as one string: name, index, live value
// and what that resolves to. Shared by the per-rect table, the override row and
// the log dump, so the three cannot disagree about what a driver is reading.
//
// ⚠ The VALUE is the point, not the name. Every built-in source except the six DU
// indices is a name read off ToLiss's binary rather than measured, so "is this the
// right dataref" is a question only a moving number answers — and this pane showed
// nothing but the name until 2026-08-01, which made a driver that resolved
// perfectly and read the wrong thing indistinguishable from one that never bound.
static std::string FxDriverState(FxDriver& d, bool isPower, const FxCurve* shape) {
    if (!d.set()) return "-";
    std::string s = d.dref;
    if (d.index >= 0) s += "[" + std::to_string(d.index) + "]";
    double v = 0.0;
    if (!FxDriverRaw(d, v)) return s + "  UNREADABLE";
    char buf[96];
    if (isPower)
        std::snprintf(buf, sizeof(buf), " = %.3f -> %s", v,
                      v > (double)d.floorF ? "ON" : "OFF (target skipped)");
    else
        std::snprintf(buf, sizeof(buf), " = %.3f -> x%.2f", v,
                      FxDriverFactor(d, shape));
    return s + buf;
}

// ==================== panel FX: the ambient day/night blend ==================
//
// A screen tint authored at night is wrong at noon and the other way round, and
// not by a little: the effects that sell a backlit LCD in the dark — bleed, a
// lifted black, a cool cast — are exactly what a sunlit screen does NOT do. It
// washes out, its contrast collapses, and the glass in front of it starts
// reflecting the cockpit rather than glowing.
//
// So a layer can carry a SECOND color and opacity for full daylight, and this
// is the number the compositor blends between them with: 0 = night, 1 = day.
//
// ⚠ THIS IS AN INTERPOLATION, NOT A SWITCH, and that is the whole point. Dusk is
// where every cockpit lighting effect is judged, it lasts twenty minutes, and a
// threshold would pop mid-approach.
//
// ---- where the number comes from -------------------------------------------
//
// sim/graphics/misc/cockpit_light_level_{r,g,b} — "the level for the cockpit
// 'night' tinting, from 0 to 1". This is the sim's own answer to "how much
// daylight is in this cockpit", it already accounts for the sun being behind
// terrain or cloud, and it is what ToLiss reads: those three names are in the
// AirbusFBW plugin binary alongside outside_light_level_*. Reading the same
// quantity the aircraft reads is the reason the two will not disagree.
//
// The three channels are collapsed with MAX rather than an average. "Is there
// light in here" is answered by the brightest channel: the night tint is
// strongly blue-shifted, so an average reads a lit-by-moonlight cockpit as
// darker than it looks, and max degrades gracefully if a future sim version
// stops writing one of them.
//
// ⚠ Resolved LAZILY on first draw, like the FX drivers and unlike anything bound
// at aircraft load — so this needs no line in AutoLoop's 1 Hz retry list. These
// are X-Plane's own datarefs rather than ToLiss's: they exist before any
// aircraft loads and do not go away with one, which is also why they are not
// dropped in FxForgetDrivers. The OVERRIDE below can name anything, so that one
// is.
static const char* const kFxAmbientRefs[3] = {
    "sim/graphics/misc/cockpit_light_level_r",
    "sim/graphics/misc/cockpit_light_level_g",
    "sim/graphics/misc/cockpit_light_level_b",
};
static XPLMDataRef gFxAmbientRef[3]  = { nullptr, nullptr, nullptr };
static bool        gFxAmbientTried   = false;

// An escape hatch, not the normal path: point the blend at outside_light_level,
// or at a ToLiss dataref, without a rebuild. Empty = the three channels above.
static FxDriver gFxAmbientSource;

// The reading that means full night and the one that means full day. Not 0 and 1:
// the cockpit tint sits well above 0 under any moon and reaches its top long
// before the sun is overhead, so the raw value spends almost none of its travel
// in the range that matters. These two are what a tuning session actually moves.
// lo ABOVE hi inverts, exactly as on an FxDriver, and for the same reason.
static float gFxAmbientLo = 0.15f;
static float gFxAmbientHi = 0.70f;

// Seconds to close ~63% of a step. ⚠ NOT cosmetic. The raw value moves the
// instant the camera passes into a shadow, a cloud crosses the sun, or the
// aircraft banks — and an un-smoothed blend makes every screen in the cockpit
// shift color with it. Slow enough that nothing pops, fast enough that a
// sunrise is not still catching up at cruise.
static float gFxAmbientTau = 2.5f;

static float gFxAmbientRaw    = 0.0f;   // last reading, before lo/hi — for the pane
static float gFxAmbient       = 0.0f;   // smoothed 0..1 — what the compositor uses
static double gFxAmbientClock = -1.0;   // XPLMGetElapsedTime of the last sample

// ⚠ Dev-only manual override, as a value rather than a second switch: -1 is off,
// 0..1 pins the blend. Authoring the daytime half of a look at 02:00 local
// otherwise means changing the sim's time of day and waiting for the tint to
// catch up, twice per edit.
static float gFxAmbientManual = -1.0f;

static float FxAmbientRawNow() {
    if (gFxAmbientSource.set()) {
        double v = 0.0;
        return FxDriverRaw(gFxAmbientSource, v) ? (float)v : gFxAmbientRaw;
    }
    if (!gFxAmbientTried) {
        gFxAmbientTried = true;
        int found = 0;
        for (int i = 0; i < 3; ++i) {
            gFxAmbientRef[i] = XPLMFindDataRef(kFxAmbientRefs[i]);
            if (gFxAmbientRef[i]) ++found;
        }
        if (found < 3)
            Log("panel fx: only " + std::to_string(found) + " of the 3 "
                "cockpit_light_level channels exist - the day/night blend is "
                "reading what it can. Set an ambient source override if this "
                "sim version renamed them.");
    }
    float best = -1.0f;
    for (int i = 0; i < 3; ++i)
        if (gFxAmbientRef[i]) {
            const float v = XPLMGetDataf(gFxAmbientRef[i]);
            if (v > best) best = v;
        }
    // ⚠ Nothing readable holds the LAST value rather than collapsing to 0. Same
    // house rule as the drivers: a missing source must not silently repaint the
    // whole cockpit in its other look.
    return best < 0.0f ? gFxAmbientRaw : best;
}

// Sampled once per compositor pass rather than from a flight loop, so the value
// is fresh exactly when it is used and there is nothing to keep in step. The
// step is taken against the WALL CLOCK, not a fixed per-frame constant, so the
// smoothing runs at the same speed at 20 fps as at 120 and a paused sim does not
// bank up a jump.
static void FxSampleAmbient() {
    const double now  = (double)XPLMGetElapsedTime();
    const bool  first = gFxAmbientClock < 0.0;
    float dt = first ? 0.0f : (float)(now - gFxAmbientClock);
    gFxAmbientClock = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 1.0f) dt = 1.0f;          // a loading pause is not four hours of dusk

    gFxAmbientRaw = FxAmbientRawNow();

    const float span = gFxAmbientHi - gFxAmbientLo;
    float target = span == 0.0f
        ? (gFxAmbientRaw >= gFxAmbientHi ? 1.0f : 0.0f)
        : (gFxAmbientRaw - gFxAmbientLo) / span;
    if (target < 0.0f) target = 0.0f;
    if (target > 1.0f) target = 1.0f;

    // First sample of the session snaps: fading up from "night" over the first
    // two seconds of every flight would be a visible artifact on the runway at
    // noon, and there is nothing to smooth away yet.
    if (first || dt <= 0.0f || gFxAmbientTau <= 0.0f) {
        gFxAmbient = target;
        return;
    }
    const float k = 1.0f - std::exp(-dt / gFxAmbientTau);
    gFxAmbient += (target - gFxAmbient) * k;
}

// What the compositor blends with. The manual override is read HERE rather than
// in the sampler so the raw value keeps updating behind it — otherwise pinning
// the blend would freeze the readout that tells you what to pin it to.
static float FxAmbientNow() {
    if (gFxAmbientManual >= 0.0f)
        return gFxAmbientManual > 1.0f ? 1.0f : gFxAmbientManual;
    return gFxAmbient;
}

// ---- what drives each face by default ---------------------------------------
// Keyed by rect NAME, like the groups, and for the same reason: the rect table
// is append-only, but a table keyed by index still silently re-aims if a row is
// ever reordered by accident.
//
// ⚠ THE DATAREF NAMES BELOW ARE READ OFF ToLiss's PLUGIN BINARY, not measured.
// The DU array is settled (kScreenDU, in-sim 2026-07-29); everything else is a
// name that exists and looks right. What each one MEANS — 0..1 or 0..100, the
// screen's own brightness or its panel's integral lighting — is unverified, so
// every one of these is editable in the FX editor and the editor shows the live
// value next to the factor it produces. Fix them there, then here.
//
// Empty means "nothing found for this face". Most of the segment windows have no
// brightness dataref of their own at all — on the real aircraft they are lit by
// the integral lighting of the panel they sit in, so the rule here is THE PANEL
// THEY ARE ON, not the unit they belong to: everything on the main panel and the
// pedestal takes kFxPanelIntegral, everything on the overhead takes
// kFxOhpIntegral. That is what put the pedestal radios and the rudder trim on the
// panel knob and RMP3 — which is on the OVERHEAD, not the pedestal — on the OHP
// knob alongside the BAT windows (2026-08-02, user).
struct PanelSource {
    const char* rect;
    const char* bright;  int brightIdx;  float brightLo, brightHi;
    const char* power;   int powerIdx;   float powerAbove;
};
static const char* kFxPanelIntegral = "AirbusFBW/PanelBrightnessLevel";
static const char* kFxOhpIntegral   = "AirbusFBW/OHPBrightnessLevel";
// The two battery voltmeters read their own battery, which is also the only
// thing that makes them legible: a BAT window with a flat battery behind it is
// blank, not dim. 3 V is well under any charged reading and well over a dead
// bus, so it separates "there is a battery here" from "there is not".
static const char* kFxBatVolts      = "AirbusFBW/BatVolts";
static const PanelSource kPanelSources[] = {
    // the six DUs — each on its own knob. The indices are kScreenDU's, which is
    // the one thing in this table that IS settled in-sim.
    { "capt PFD",      "AirbusFBW/DUBrightness", 0, 0.0f, 1.0f,  "", -1, 0.0f },
    { "capt ND",       "AirbusFBW/DUBrightness", 1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "E/WD",          "AirbusFBW/DUBrightness", 4, 0.0f, 1.0f,  "", -1, 0.0f },
    { "SD",            "AirbusFBW/DUBrightness", 5, 0.0f, 1.0f,  "", -1, 0.0f },
    { "FO ND",         "AirbusFBW/DUBrightness", 3, 0.0f, 1.0f,  "", -1, 0.0f },
    { "FO PFD",        "AirbusFBW/DUBrightness", 2, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ISIS",          "AirbusFBW/ISIBrightness", -1, 0.0f, 1.0f,
                       "AirbusFBW/ISIAvailable", -1, 0.5f },
    { "MCDU capt",     "AirbusFBW/MCDUScreenBrightnessArray", 0, 0.0f, 1.0f, "", -1, 0.0f },
    { "MCDU FO",       "AirbusFBW/MCDUScreenBrightnessArray", 1, 0.0f, 1.0f, "", -1, 0.0f },
    // The DCDUs have BrightnessUp/Down COMMANDS and no level dataref anyone can
    // read, so Photon counts the presses itself and publishes the result — see
    // kDcduBrightnessRef. The name and the top of the range are taken FROM THOSE
    // CONSTANTS rather than written out again: the range is an assumption that
    // in-sim testing is expected to correct, and a literal 10 here would be a
    // second place to remember when it moves. The power gate stays "does this
    // aircraft have CPDLC at all", which is a different question from "is it lit".
    { "DCDU capt",     kDcduBrightnessRef, 0, (float)kDcduLevelMin, (float)kDcduLevelMax,
                       "AirbusFBW/HasCPDLC", -1, 0.5f },
    { "DCDU FO",       kDcduBrightnessRef, 1, (float)kDcduLevelMin, (float)kDcduLevelMax,
                       "AirbusFBW/HasCPDLC", -1, 0.5f },
    // The FCU row is integral-lit as one unit, baro windows included — one knob
    // for all three faces.
    //
    // ⚠ NOT in 0..1. This is the RAW KNOB ANIMATION value and it runs 1..270,
    // which is why lo/hi exist at all: every other row here happens to be 0..1
    // and it would be easy to assume the mechanism only ever normalizes a
    // fraction. If the tint turns out to fade the wrong way round, the fix is to
    // SWAP these two — lo above hi is the supported way to say "this source is
    // inverted", not a mistake.
    //
    // ⚠ Reported by the user 2026-08-01 and NOT FOUND in this install: no
    // ckpt/*/lights/*/anim dataref on any of the four ToLiss airframes here is
    // named .../fcu/lights/right/anim, and every ckpt light-knob animation that
    // does exist keys 0..1 or 0..2, never 1..270. It may be a newer aircraft
    // build or another add-on. Kept as asked, because the failure is safe and
    // self-announcing: an unresolvable driver logs itself, reads as factor 1
    // (undimmed, never black) and shows UNREADABLE in red in the source pane.
    // If it stays red, the working names are ckpt/fcu/integLight/anim (the FCU's
    // own integral-light knob) and AirbusFBW/FCUIntegralBrightness, both 0..1.
    { "FCU strip",     "ckpt/fcu/lights/right/anim", -1, 1.0f, 270.0f,
                       "AirbusFBW/FCUAvail", -1, 0.5f },
    { "capt baro",     "ckpt/fcu/lights/right/anim", -1, 1.0f, 270.0f,
                       "AirbusFBW/FCUAvail", -1, 0.5f },
    { "FO baro",       "ckpt/fcu/lights/right/anim", -1, 1.0f, 270.0f,
                       "AirbusFBW/FCUAvail", -1, 0.5f },
    // The radios. Each RMP has its own AirbusFBW/RMP<n>Lights, which was what
    // these rows used until 2026-08-02 — but these faces are integral-lit like
    // every other segment window, and RMP1/2 sit on the PEDESTAL while RMP3 is on
    // the OVERHEAD, so they follow two different rheostats. `RMP<n>Available`
    // stays as the power gate: that one really is per unit.
    { "RMP1 active",   kFxPanelIntegral, -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP1Available", -1, 0.5f },
    { "RMP1 stby",     kFxPanelIntegral, -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP1Available", -1, 0.5f },
    { "RMP2 active",   kFxPanelIntegral, -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP2Available", -1, 0.5f },
    { "RMP2 stby",     kFxPanelIntegral, -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP2Available", -1, 0.5f },
    { "RMP3 active",   kFxOhpIntegral, -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP3Available", -1, 0.5f },
    { "RMP3 stby",     kFxOhpIntegral, -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP3Available", -1, 0.5f },
    // The transponder window. ⚠ NO power source, deliberately (2026-08-02, user).
    // It carried AirbusFBW/XPDRPower, which is the wrong quantity: that dataref
    // says whether the transponder is INTERROGATING — the STBY/AUTO/ON selector —
    // not whether the unit is alive. The code window stays lit and readable in
    // STBY, so gating on it blanked the tint on a screen a pilot can plainly see.
    // This is the failure mode this table keeps producing: a name that resolves
    // perfectly and reports a neighbouring fact. The bus gate below still covers
    // it, which is the gate that actually answers "is this thing on".
    { "XPDR code",     kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    // Everything integral-lit by the main panel rheostat. `ped strips` and
    // `rudder trim` were on AirbusFBW/PedestalFloodBrightnessLevel until
    // 2026-08-02: that is the pedestal FLOOD lamp, which washes the pedestal from
    // above, not the integral lighting inside these windows.
    { "clock CHR",     kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "clock UTC",     kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "clock ET",      kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ctr readout L", kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ctr readout R", kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ped strips",    kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "rudder trim",   kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    // The overhead windows. The two BAT voltmeters gate on their OWN battery —
    // one each, and note this is the face's own `power` row, so a stack in
    // panelfx.txt can override it. The bus gate below is the one that cannot be.
    { "BAT1",          kFxOhpIntegral, -1, 0.0f, 1.0f,  kFxBatVolts, 0, 3.0f },
    { "BAT2",          kFxOhpIntegral, -1, 0.0f, 1.0f,  kFxBatVolts, 1, 3.0f },
    { "ovhd aft L1",   kFxOhpIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ovhd aft L2",   kFxOhpIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "FMS Load",      kFxOhpIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    // The EFB tablets are on their own screen brightness, up/down buttons only.
};
static const int kPanelSourceCount = (int)(sizeof(kPanelSources) / sizeof(kPanelSources[0]));

// ---- the two ELECTRICAL gates ------------------------------------------------
// A face's own `power` source above answers "does this unit exist and is it
// switched on". These two answer "does electricity reach it", and they are kept
// SEPARATE from it for a reason that is not tidiness: `power` is overridable by a
// stack in panelfx.txt, because it is part of the LOOK being authored. Bus
// voltage and a pulled breaker are facts about the aircraft, not tuning, and a
// look that could opt out of them would eventually be authored by accident.
//
// Both are ordinary FxDrivers evaluated as thresholds, so everything the driver
// machinery already does — lazy resolution, one log line per missing dataref,
// dropped handles on aircraft change, "unreadable means ON" — applies unchanged.
//
// Both tables are keyed by rect NAME, like kPanelSources and kPanelGroups and for
// the same reason. Absence from a table means "no such gate", which is what makes
// the breaker optional without needing a flag.

// Which DC bus feeds each face. Only the faces that go DARK with the bus are
// listed; a face that is merely integral-lit keeps its tint, because the tint is
// then a property of the lighting rather than of the readout being alive.
//
// The five rows below the DCDUs were added 2026-08-02 (user): the transponder
// code, the rudder trim, the FMS load window and the two unidentified aft
// overhead readouts are all driven displays, not lit legends, so they blank with
// the bus exactly as a DU does. Same index and same threshold as the DUs — one
// bus, one number, and nothing here gets its own.
struct PanelBus { const char* rect; int index; };
static const PanelBus kPanelBuses[] = {
    { "capt PFD",    kDcBusIndex },
    { "capt ND",     kDcBusIndex },
    { "E/WD",        kDcBusIndex },
    { "SD",          kDcBusIndex },
    { "FO ND",       kDcBusIndex },
    { "FO PFD",      kDcBusIndex },
    { "DCDU capt",   kDcBusIndex },
    { "DCDU FO",     kDcBusIndex },
    { "XPDR code",   kDcBusIndex },
    { "rudder trim", kDcBusIndex },
    { "FMS Load",    kDcBusIndex },
    { "ovhd aft L1", kDcBusIndex },
    { "ovhd aft L2", kDcBusIndex },
};
static const int kPanelBusCount = (int)(sizeof(kPanelBuses) / sizeof(kPanelBuses[0]));

// Which circuit breaker, for the faces that have a dedicated one.
// AirbusFBW/CBArray is 1 = pushed in (connected), 0 = pulled.
//
// ⚠ DELIBERATELY EMPTY. The A320's CBArray holds 363 elements and NOTHING names
// them — not the OBJ (knobs.obj animates CBArrayAnim[n] with no label), not the
// plugin binary, not the manuals shipped with the aircraft. Every index here
// would therefore be a guess, and a wrong guess is the worst kind of failure
// this table can have: the tint on one readout tracks an unrelated breaker, so
// it looks correct until the day someone pulls that one.
//
// TO FILL A ROW: in a dev build open Dev ▸ Probe and watch the "CB watch" line,
// pull the breaker, and it names the index that changed. Then add the row here.
// That is the "probe one slot at a time before hypothesizing" rule, which this
// project has already paid for once.
//
// The single sentinel row is there because a zero-length array is not valid
// C++; BuildRectDrivers skips a null rect.
struct PanelBreaker { const char* rect; int index; };
static const char* kFxCbArray = "AirbusFBW/CBArray";
static const PanelBreaker kPanelBreakers[] = {
    { nullptr, -1 },
};
static const int kPanelBreakerCount =
    (int)(sizeof(kPanelBreakers) / sizeof(kPanelBreakers[0]));

// The per-rect drivers, built once from the tables above. Mutable because a
// driver caches its resolved handle; the CONFIG is the constant part.
static FxDriver gRectBright[64];
static FxDriver gRectPower[64];
static FxDriver gRectBus[64];
static FxDriver gRectCb[64];
static bool     gRectDriversBuilt = false;

static void BuildRectDrivers() {
    for (int i = 0; i < kPanelRectCount && i < 64; ++i) {
        gRectBright[i] = FxDriver();
        gRectPower[i]  = FxDriver();
        gRectBus[i]    = FxDriver();
        gRectCb[i]     = FxDriver();
    }
    for (int s = 0; s < kPanelSourceCount; ++s) {
        const PanelSource& p = kPanelSources[s];
        const int i = RectIndexByName(p.rect);
        if (i < 0 || i >= 64) {
            Log(std::string("panel fx: source table names '") + p.rect
                + "', which is not a rect - ignored");
            continue;
        }
        if (*p.bright) {
            gRectBright[i].dref  = p.bright;
            gRectBright[i].index = p.brightIdx;
            gRectBright[i].lo    = p.brightLo;
            gRectBright[i].hi    = p.brightHi;
        }
        if (*p.power) {
            gRectPower[i].dref   = p.power;
            gRectPower[i].index  = p.powerIdx;
            gRectPower[i].floorF = p.powerAbove;
        }
    }
    for (int b = 0; b < kPanelBusCount; ++b) {
        const PanelBus& p = kPanelBuses[b];
        const int i = RectIndexByName(p.rect);
        if (i < 0 || i >= 64) {
            Log(std::string("panel fx: bus table names '") + p.rect
                + "', which is not a rect - ignored");
            continue;
        }
        gRectBus[i].dref   = kDcBusRef;
        gRectBus[i].index  = p.index;
        gRectBus[i].floorF = kDcBusLiveVolts;
    }
    for (int b = 0; b < kPanelBreakerCount; ++b) {
        const PanelBreaker& p = kPanelBreakers[b];
        if (!p.rect) continue;                 // the sentinel; see the table
        const int i = RectIndexByName(p.rect);
        if (i < 0 || i >= 64) {
            Log(std::string("panel fx: breaker table names '") + p.rect
                + "', which is not a rect - ignored");
            continue;
        }
        gRectCb[i].dref   = kFxCbArray;
        gRectCb[i].index  = p.index;
        // 1 = in, 0 = pulled, so "above 0.5" is the threshold. Written as a half
        // rather than as 0 so a float array that reads 0.999 while animating
        // still counts as connected.
        gRectCb[i].floorF = 0.5f;
    }
    gRectDriversBuilt = true;
}

// Drop every cached handle. ToLiss's datarefs go away with its plugin, so a
// handle held across an aircraft change is a pointer into an unloaded plugin —
// the one way this feature could crash the sim rather than just look wrong.
static void FxForgetDrivers();

// Per-target overrides of the two global switches, as a tri-state.
//
// ⚠ kFxInherit is NOT "the master's value, copied at the time you set it" — a
// target left on Inherit tracks the master afterwards. That is the whole reason
// the third state exists: gFxFollowBrightness is a tuning escape hatch you flip
// while picking a color and flip back, and if setting one target's switch
// silently froze every other target's, the hatch would stop working.
//
// The states are deliberately NOT symmetric with the master:
//
//   master OFF     nothing follows and nothing is power-gated. It is the "show
//                  me the raw colors" switch and it outranks every target,
//                  including one set to On — otherwise a target you set months
//                  ago is the reason the hatch appears broken today.
//   follow = Off   this target is NOT dimmed by its knob, but it IS still gated
//                  by its power driver, its bus and its breaker. "Always on
//                  whenever the thing is powered", which is a real look on a face
//                  whose integral lighting does not track the readout brightness.
//   curve  = Off   linear from lo..hi instead of kBrightnessCurve. Only affects
//                  this target's tint; the screen-glow spill lights are never
//                  switchable (docs/screens_plan.md §1).
//
// The enum itself is declared up with FxLayer, which needs it: `follow` now
// exists at BOTH levels and a layer's inherited value is its stack's.

struct FxStack {
    int                  target = -1;   // index in the shared rect/group space
    std::vector<FxLayer> layers;
    // Per-target overrides. Empty means "use whatever drives each rect", which
    // is what makes a GROUP dim per member: "Main displays" follows six knobs,
    // not one. Setting them here is for the faces the built-in table gets wrong.
    FxDriver bright;
    FxDriver power;
    int      follow = kFxInherit;       // dim with the brightness driver at all
    int      curve  = kFxInherit;       // shape that dimming with kBrightnessCurve
};

static std::vector<FxStack> gFxStacks;
static bool gFxEnabled = false;
// The master switch for the whole mechanism. On by default: a tint that ignores
// the brightness knob is the bug, not the feature. Turning it off is for tuning
// a color without having to hold a knob up.
static bool gFxFollowBrightness = true;

// Declared up with the Displays tab, which asks this to decide whether its two
// effect switches have anything to act on.
static bool PanelFxLoaded() { return gFxEnabled && !gFxStacks.empty(); }

static void FxForgetDrivers() {
    for (int i = 0; i < kPanelRectCount && i < 64; ++i) {
        gRectBright[i].tried = false; gRectBright[i].ref = nullptr;
        gRectPower[i].tried  = false; gRectPower[i].ref  = nullptr;
        gRectBus[i].tried    = false; gRectBus[i].ref    = nullptr;
        gRectCb[i].tried     = false; gRectCb[i].ref     = nullptr;
    }
    for (size_t s = 0; s < gFxStacks.size(); ++s) {
        gFxStacks[s].bright.tried = false; gFxStacks[s].bright.ref = nullptr;
        gFxStacks[s].power.tried  = false; gFxStacks[s].power.ref  = nullptr;
    }
    // The ambient override can name a ToLiss dataref, so it goes with the rest.
    // The three cockpit_light_level channels do NOT — they are the sim's own and
    // outlive every aircraft.
    gFxAmbientSource.tried = false; gFxAmbientSource.ref = nullptr;
}

// The factor for ONE rect of the stack being drawn: the stack's own driver if it
// has one, else the rect's built-in one. Set up per layer by DrawFxLayer.
//
// Both are globals rather than parameters because PanelPaintFn is a plain
// function pointer that the probe shares — the same reason gFxAlpha and gFxTile
// are. The LAYER is here too now: `follow` is per layer, so the per-rect factor
// cannot be computed without knowing which layer is being painted.
static FxStack* gFxDrawStack = nullptr;
static const FxLayer* gFxDrawLayer = nullptr;

// The effective switches for the stack being drawn. Split out so the UI, the log
// dump and the compositor cannot disagree about what is in force — a pane that
// says "curve on" over a target drawing linearly is worse than no pane.
static bool FxStackFollows(const FxStack* s) {
    return gFxFollowBrightness && FxTriState(s ? s->follow : kFxInherit, true);
}
static bool FxStackCurves(const FxStack* s) {
    return FxTriState(s ? s->curve : kFxInherit, gFxCurve);
}

// One level further in: a layer's Inherit is its STACK's setting, which is in
// turn the master's. The master is re-tested here rather than left to
// FxStackFollows because the rule that it outranks everything has to hold for a
// layer set to On as much as for a target set to On.
static bool FxLayerFollows(const FxLayer* l, const FxStack* s) {
    return gFxFollowBrightness
        && FxTriState(l ? l->follow : kFxInherit, FxStackFollows(s));
}

// Which shape a knob is read through, as one pointer — nullptr is linear. The
// two entry points differ only in whether there is a layer to ask, because the
// per-target panes describe a stack rather than any one of its layers.
//
// ⚠ A LAYER'S OWN KNOTS OUTRANK THE TARGET'S RESPONSE SWITCH, including "Linear".
// That is the same precedence every other per-layer setting has, and the reading
// that makes the editor honest: a curve you drew and can see plotted must be the
// curve being applied, or the pane is lying about the thing it exists to show.
static const FxCurve* FxShapeForStack(const FxStack* s) {
    return FxStackCurves(s) ? &FxBuiltInCurve() : nullptr;
}
static const FxCurve* FxShapeFor(const FxLayer* l, const FxStack* s) {
    if (l && l->curve.on && l->curve.knots.size() >= 2) return &l->curve;
    return FxShapeForStack(s);
}

// Which rects the Displays tab's "backlight visual effect" switch owns. The six
// DUs are the ones with a DUBrightness index, and beyond them the MCDUs, the
// DCDUs and the ISIS — they are backlit screens the pilot reads the same way, so
// a switch labeled "backlight visual effect" that left them tinted while the PFDs
// went flat would read as the switch half-working. (The ISIS joined them
// 2026-08-02; it is a screen by every test except having a DUBrightness index,
// which it does not because ToLiss gives it its own ISIBrightness knob.)
//
// ⚠ BY NAME, not by index, and that is the house rule for this table (see
// RectIndexByName): panelfx.txt already names its targets, so a rect's name is
// the wire identity and renaming one is already a breaking change. An index
// literal here would survive a rename silently and re-aim the switch.
static const char* const kScreenFxExtraRects[] = {
    "MCDU capt", "MCDU FO", "DCDU capt", "DCDU FO", "ISIS",
};

// Resolved once. The lookup is a strcmp walk of the whole table and this runs
// per rect per layer per frame.
static bool PanelRectIsScreen(int rectIndex) {
    if (rectIndex < 0 || rectIndex >= kPanelRectCount) return false;
    if (kPanelRectsHi[rectIndex].du >= 0) return true;

    static PanelMask extras = 0;
    static bool built = false;
    if (!built) {
        built = true;
        const int n = (int)(sizeof(kScreenFxExtraRects) / sizeof(kScreenFxExtraRects[0]));
        for (int e = 0; e < n; ++e) {
            const int i = RectIndexByName(kScreenFxExtraRects[e]);
            // A miss means the rect was renamed. Say so — the symptom otherwise
            // is one readout quietly answering to the wrong switch.
            if (i < 0) Log(std::string("panel fx: no rect named '")
                           + kScreenFxExtraRects[e] + "' - it will follow the "
                           "other-displays switch instead");
            else extras |= (PanelMask)1 << i;
        }
    }
    return (extras >> rectIndex) & 1;
}

// The USER's contribution to one rect's factor, from the Displays tab. Split by
// rect rather than by stack — see DisplaySettings — so a group spanning a DU and
// the FCU obeys both switches at once, which is the only reading of it that is
// not arbitrary.
//
// A rect index of -1 means the flood quad, which only the probe paints; it can
// never reach here from the compositor, and treating it as "other" is the
// harmless answer if it ever does.
static float FxDisplayUserFactor(int rectIndex) {
    if (PanelRectIsScreen(rectIndex))
        return DisplayFeatureOn(gDisplays.screenFx) ? gDisplays.screenFxGain : 0.0f;
    return DisplayFeatureOn(gDisplays.otherFx) ? 1.0f : 0.0f;
}

// `driveOut`, when asked for, receives the brightness value this rect resolves
// to — 0..1, through whatever shape is in force, and 1 when nothing readable
// drives it. It is what the layer's color RAMP interpolates on, and it is a
// second output rather than a second function because it comes from the same
// four-line lookup: computing it separately would mean the color and the
// opacity could end up reading two different frames of the same knob.
//
// ⚠ It is NOT the return value scaled back up. The returned factor folds in the
// Displays-tab gain and is zero whenever a gate fails; the drive is the knob
// alone, which is the only thing a color ramp can sensibly follow.
static float FxRectFactor(int rectIndex, float* driveOut = nullptr) {
    // Set up front so every early return below leaves it defined at the "nothing
    // is dimming this" value. A ramp on a gated rect is moot — the rect draws
    // nothing at all — but an uninitialized float would not stay moot.
    if (driveOut) *driveOut = 1.0f;
    const float user = FxDisplayUserFactor(rectIndex);
    if (user <= 0.0f) return 0.0f;              // switched off: draw nothing at all
    if (!gFxFollowBrightness) return user;
    if (!gRectDriversBuilt) BuildRectDrivers();

    FxDriver* bright = nullptr;
    FxDriver* power  = nullptr;
    if (gFxDrawStack && gFxDrawStack->bright.set()) bright = &gFxDrawStack->bright;
    else if (rectIndex >= 0 && rectIndex < kPanelRectCount) bright = &gRectBright[rectIndex];
    if (gFxDrawStack && gFxDrawStack->power.set())  power  = &gFxDrawStack->power;
    else if (rectIndex >= 0 && rectIndex < kPanelRectCount) power  = &gRectPower[rectIndex];

    // ⚠ THE GATES COME FIRST and are not what `follow` switches off. A target
    // with follow = Off means "full strength whenever this is powered", so
    // skipping them with it would leave the tint painted over a dead screen —
    // the exact artifact the whole driver mechanism exists to remove.
    //
    // Three of them, and the difference between the first and the other two is
    // not cosmetic. `power` may be OVERRIDDEN BY A STACK, because it is part of
    // the authored look. The bus and the breaker are always the RECT's own: they
    // are facts about the aircraft's electrical state, and a look that could opt
    // out of them would eventually be authored by accident. Any one of the three
    // failing is a hard zero — a screen with no power is off, not dim.
    if (power && power->set() && !FxDriverPowered(*power)) return 0.0f;
    if (rectIndex >= 0 && rectIndex < kPanelRectCount) {
        if (gRectBus[rectIndex].set() && !FxDriverPowered(gRectBus[rectIndex])) return 0.0f;
        if (gRectCb[rectIndex].set()  && !FxDriverPowered(gRectCb[rectIndex]))  return 0.0f;
    }
    // ⚠ Per LAYER, not per stack. A color correction that should stay put while
    // the knob comes down and a backlight wash that must follow it are two layers
    // of the same target, and this is the line that lets them differ.
    const bool follows = FxLayerFollows(gFxDrawLayer, gFxDrawStack);

    // ⚠ Read the knob when EITHER question wants it — dimming, or a color ramp.
    // A layer that does not follow but does ramp still needs the value, which is
    // the case §8k exists for; a layer that does neither must not pay for a read
    // it will not use, which is what this condition buys back.
    const bool wantsDrive = follows || (gFxDrawLayer && gFxDrawLayer->hasRamp);
    float norm     = 1.0f;
    bool  haveNorm = false;
    if (wantsDrive && bright && bright->set())
        haveNorm = FxDriverNorm(*bright, &norm);
    if (!haveNorm) norm = 1.0f;               // no readable knob = full brightness

    // ⚠ `driveOut` is the RAW dataref position, 0..1 after lo/hi and BEFORE the
    // response curve and the floor — deliberately not the factor returned below.
    //
    // The two are different quantities and the ramp wants this one. The factor
    // is an EFFECT INTENSITY: it carries the curve, which is Photon's model of
    // how a backlight responds, and the floor, which is an authored "never fully
    // off". A color ramp is a statement about the SCREEN — this display reads
    // warmer at a quarter knob than at full — so it has to follow where the knob
    // is, not how strongly we have decided to paint there. Feeding it the factor
    // also made the ramp move whenever a curve was edited, and put its midpoint
    // wherever the curve happened to cross 0.5 rather than at half a turn.
    if (driveOut) *driveOut = norm;

    if (!follows) return user;
    if (!haveNorm) return user;
    return user * FxShapeFactor(*bright, FxShapeFor(gFxDrawLayer, gFxDrawStack), norm);
}

// The lit pass. Not a knob: pass 1 puts down nothing visible on these faces and
// pass 2 replaces the digits, measured in-sim 2026-07-31.
static const int kFxPass = 2;

// Set the func and equation for one layer, holding the ALPHA channel at "keep
// destination". Some of these modes would otherwise write alpha — additive would
// saturate it, Darken would floor it — and nothing here has any business changing
// the panel's alpha channel. Without the Separate entry points the alpha rides
// the RGB half, which is the pre-existing behavior and not worse than refusing
// to draw; each mode below picks a source alpha that survives that fallback.
//
// Returns false when the layer cannot be drawn as asked — only possible for the
// four equation-based modes on a driver with no glBlendEquation. Drawing them
// additively instead would be worse than not drawing them: a mode named
// "Darken" that brightens is a bug report about the wrong thing.
static bool SetFxBlend(GLenum src, GLenum dst, GLenum eq) {
    if (PfnBlendEquationSeparate seps = GlBlendEquationSeparate()) {
        seps(eq, GL_FUNC_ADD);             // alpha always plain add — see above
    } else if (PfnBlendEquation one = GlBlendEquation()) {
        one(eq);
    } else if (eq != GL_FUNC_ADD) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log("panel fx: glBlendEquation is unavailable on this driver — "
                "Subtract, Burn, Darken and Lighten layers are being SKIPPED. "
                "The other four modes are unaffected.");
        }
        return false;
    }

    if (PfnBlendFuncSeparate sep = GlBlendFuncSeparate())
        sep(src, dst, GL_ZERO, GL_ONE);
    else
        glBlendFunc(src, dst);
    return true;
}

// Every mode fades to a NO-OP at opacity 0, which means fading toward that
// mode's own identity and not toward black. Getting this wrong does not look
// like a wrong number — it looks like the opacity slider doing the opposite of
// what it says.
//
// Six of the eight carry it the same way: the quad emits color x opacity, and
// the identity is black, so opacity scales plainly. That includes MULTIPLY,
// which pays for it in the blend func instead of in the color —
//
//     src=DST_COLOR, dst=ONE_MINUS_SRC_ALPHA, quad = (C x o, o)
//     => dst' = dst x C x o + dst x (1 - o) = dst x (1 - o + o x C)
//
// which is exactly the old "fade the color toward white" arithmetic, expressed
// per PIXEL. That is what lets multiply take an image: with a premultiplied
// texture the same expression becomes dst x (1 - a x o + a x o x texel x C), so a
// transparent texel is a no-op and a solid one behaves as before. The two modes
// that cannot be written this way are Burn and Darken — see FxBlendTakesTexture.
//
// ⚠ Split into "the color for one opacity" and "draw the layer" because the
// opacity is no longer one number per layer: a brightness driver scales it PER
// RECT, so a group whose members are on different knobs needs this arithmetic
// re-run per quad. The blend func and equation do NOT vary with opacity — every
// case below picks them from layer.blend alone — which is what keeps the state
// change outside the loop where it belongs.
struct FxQuadColor { float r, g, b, a; GLenum src, dst, eq; };

// ---- a layer as it looks RIGHT NOW ------------------------------------------
// The authored color and opacity are the NIGHT half. What the compositor draws
// is that lerped toward the day half by the ambient blend, so this is the one
// place the two halves are combined and everything below it works in terms of
// the result.
//
// ⚠ A layer with no day half is not lerped toward anything — it returns its own
// values untouched. That is what keeps a panelfx.txt written before the ambient
// blend existed looking exactly as it did.
struct FxLayerNow { float color[3]; float opacity; };

static FxLayerNow FxLayerAtAmbient(const FxLayer& l) {
    FxLayerNow n = { { l.color[0], l.color[1], l.color[2] }, l.opacity };
    if (!l.hasDay) return n;
    const float a = FxAmbientNow();
    for (int i = 0; i < 3; ++i) n.color[i] = l.color[i] + (l.dayColor[i] - l.color[i]) * a;
    n.opacity = l.opacity + (l.dayOpacity - l.opacity) * a;
    return n;
}

// The color to paint at one rect's brightness. `base` is the ambient-resolved
// color — the END of the ramp, i.e. the layer as authored at a full knob — and
// `drive` walks it back toward the start color as the knob comes down.
//
// ⚠ It takes a COLOR rather than an FxLayerNow for the same reason
// FxQuadColorFor does: the only correct base is the day-blended one, and a
// signature that accepted the layer would let `l.color` be passed by reflex and
// silently pin the ramp's top end to the night color at noon.
//
// ⚠ Shared with the editor's preview swatches. Two implementations of one
// interpolation is how a preview starts lying about the cockpit.
static void FxLayerColorAtDrive(const FxLayer& l, const float base[3],
                                float drive, float out[3]) {
    if (!l.hasRamp) {
        for (int i = 0; i < 3; ++i) out[i] = base[i];
        return;
    }
    float t = drive;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    for (int i = 0; i < 3; ++i)
        out[i] = l.rampColor[i] + (base[i] - l.rampColor[i]) * t;
}

// ⚠ Takes the blend mode and the color SEPARATELY rather than the layer, because
// the color it must use is the ambient-blended one and not the layer's own.
// Passing the layer and reaching into `.color` here is the mistake this signature
// exists to make impossible: it would compile, and it would render the night
// color at every hour with only the opacity following the sun.
static FxQuadColor FxQuadColorFor(int blend, const float color[3], float o) {
    float r = color[0], g = color[1], b = color[2];
    GLenum src = GL_ONE, dst = GL_ZERO, eq = GL_FUNC_ADD;
    float a = 1.0f;
    switch (blend) {
        case kFxMultiply:
            // Opacity rides the ALPHA channel, not the color — see above.
            r *= o; g *= o; b *= o;
            a = o;
            src = GL_DST_COLOR; dst = GL_ONE_MINUS_SRC_ALPHA;
            break;
        case kFxAdd:
            r *= o; g *= o; b *= o;
            a = 0.0f;                      // additive: contribute no alpha
            src = GL_ONE; dst = GL_ONE;
            break;
        case kFxScreen:
            r *= o; g *= o; b *= o;
            a = 0.0f;
            src = GL_ONE_MINUS_DST_COLOR; dst = GL_ONE;
            break;
        case kFxSubtract:
            // dst - src, so the identity is black and opacity scales plainly.
            r *= o; g *= o; b *= o;
            a = 0.0f;
            src = GL_ONE; dst = GL_ONE; eq = GL_FUNC_REVERSE_SUBTRACT;
            break;
        case kFxBurn:
            // Linear burn, dst + src - 1, rewritten as dst - (1 - src) so the
            // same reverse subtract does it. The quad therefore carries the
            // COMPLEMENT of the layer color, which is why opacity scales that
            // complement — at 0 it emits black and subtracts nothing. It is also
            // why Burn cannot take an image: 1 - texel is not a modulate.
            r = (1.0f - r) * o; g = (1.0f - g) * o; b = (1.0f - b) * o;
            a = 0.0f;
            src = GL_ONE; dst = GL_ONE; eq = GL_FUNC_REVERSE_SUBTRACT;
            break;
        case kFxDarken:
            // min/max ignore the blend factors entirely — the equation is the
            // whole operation — so src/dst below are set only to leave the func
            // in a defined state, and opacity has nowhere to go but the color.
            // Identity is white; source alpha 1 keeps min(1, dst.a) = dst.a if
            // the alpha half falls back to the RGB one.
            r = 1.0f + o * (r - 1.0f);
            g = 1.0f + o * (g - 1.0f);
            b = 1.0f + o * (b - 1.0f);
            a = 1.0f;
            src = GL_ONE; dst = GL_ZERO; eq = GL_MIN;
            break;
        case kFxLighten:
            // Identity is black, and source alpha 0 keeps max(0, dst.a).
            r *= o; g *= o; b *= o;
            a = 0.0f;
            src = GL_ONE; dst = GL_ZERO; eq = GL_MAX;
            break;
        default:                           // kFxNormal
            // Premultiplied: the texture already carries color x alpha, so the
            // source factor is ONE rather than SRC_ALPHA. For a solid layer the
            // white texel makes this identical to the straight-alpha form.
            r *= o; g *= o; b *= o;
            a = o;
            src = GL_ONE; dst = GL_ONE_MINUS_SRC_ALPHA;
            break;
    }
    return FxQuadColor{ r, g, b, a, src, dst, eq };
}

// One quad of the layer being drawn. The common path is the old one — a rect
// with no driver, or the master switch off, costs one compare. When something
// DOES drive it, the layer's color is recomputed at the scaled opacity rather
// than the quad being faded toward black: at opacity 0 each mode has to reach
// its OWN identity (white for Multiply and Darken, black for the rest), which is
// the same reason the opacity slider works the way it does.
//
// The ambient blend is resolved ONCE per layer, into gFxDrawNow, rather than per
// quad: it is the same number for every rect of the target, and re-lerping it
// inside the loop would put a dataref read behind each one.
static FxLayerNow gFxDrawNow = { { 1.0f, 1.0f, 1.0f }, 1.0f };

static void DrawFxRect(const PanelRect& r, int rectIndex, float cr, float cg, float cb) {
    float drive = 1.0f;
    const float f = gFxDrawLayer ? FxRectFactor(rectIndex, &drive) : 1.0f;
    if (f <= 0.0f) return;                 // dark screen: draw nothing at all

    // ⚠ The fast path is now "nothing to recompute", not "f is 1". A layer with
    // a color ramp and `follow` off sits at f == 1 with the knob at a quarter —
    // taking the precomputed quad there would draw the full-brightness color and
    // the ramp would look like it does nothing on exactly the layers §8k is for.
    const bool ramped = gFxDrawLayer && gFxDrawLayer->hasRamp && drive < 0.999f;
    if (!ramped && f >= 0.999f) {
        EmitRectQuad(r, cr, cg, cb, gFxAlpha, gFxTile);
        return;
    }

    float col[3];
    FxLayerColorAtDrive(*gFxDrawLayer, gFxDrawNow.color, drive, col);
    const FxQuadColor q = FxQuadColorFor(gFxDrawLayer->blend, col,
                                         gFxDrawNow.opacity * f);
    EmitRectQuad(r, q.r, q.g, q.b, q.a, gFxTile);
}

static void DrawFxLayer(const FxLayer& layer, int target) {
    // ⚠ The DAY-BLENDED opacity decides whether there is anything to draw, not
    // the authored one. A layer that is invisible at night and appears at noon
    // has opacity 0 and a dayOpacity above it, and testing the authored value
    // would skip it every frame — the daytime-only case would silently not work.
    const FxLayerNow now = FxLayerAtAmbient(layer);
    if (now.opacity <= 0.0f) return;

    const FxTexture* tex = nullptr;
    if (!layer.tex.empty()) {
        if (!FxBlendTakesTexture(layer.blend)) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                Log(std::string("panel fx: a ") + kFxBlendName[layer.blend]
                    + " layer carries an image, which fixed-function GL cannot "
                      "express - the layer is being SKIPPED. Use Multiply or "
                      "Subtract, or clear the image.");
            }
            return;
        }
        FxTexture* t = FxTextureByName(layer.tex);
        if (!t || !UploadFxTexture(*t)) {
            if (!gFxMissingWarned) {
                gFxMissingWarned = true;
                Log("panel fx: layer image '" + layer.tex + "' is missing or "
                    "undecodable - the layer is being SKIPPED rather than drawn "
                    "as a solid. Panel FX tab > Rescan images after adding the file.");
            }
            return;
        }
        tex = t;
    }
    XPLMBindTexture2d(tex ? tex->id : gFxWhiteTex, 0);
    gFxTile = tex ? layer.tile : 1.0f;

    const FxQuadColor q = FxQuadColorFor(layer.blend, now.color, now.opacity);
    if (!SetFxBlend(q.src, q.dst, q.eq)) return;
    gFxAlpha     = q.a;
    gFxDrawNow   = now;                    // read back by DrawFxRect per quad
    gFxDrawLayer = &layer;
    {
        // ⚠ Opened AFTER the texture bind and the blend func, and closed before
        // the next layer sets its own. Neither is legal inside a begin/end block,
        // and both are per LAYER — which is what makes one batch per layer the
        // natural granularity rather than one per pass.
        PanelQuadBatch batch;
        PaintPanelTarget(target, DrawFxRect, q.r, q.g, q.b);
    }
    gFxDrawLayer = nullptr;
}

// ⚠ Stacks composite BROADEST FIRST, not in the order they were added. That is
// what makes the hierarchy mean anything: a layer on "Screens" is a base every
// display receives, and a layer on "SD" refines what the base already put down.
// Creation order would make it depend on which target the user happened to click
// first, so the same two stacks would look different between two sessions that
// built them in a different order.
//
// Stable, so two targets of equal breadth keep a predictable order. The log dump
// uses this too — reading a tuning log in a different order than the compositor
// applied it is how a stack gets transcribed back into source wrong.
// ⚠ The BUFFER is reused; the SORT still runs every call. Returning by value put
// a heap allocation in the panel pass, which is worth removing — but caching the
// ORDER itself is not, and deliberately so: it would need invalidating at every
// site that adds, removes or re-targets a stack, and the editor has many. A stale
// order is a silent, visual-only bug (a base layer compositing over its own
// refinement) of exactly the kind this file keeps getting bitten by, and sorting
// ten ints costs nanoseconds. The allocation was the cost; the sort is not.
//
// Returned by reference, so the caller must not hold it across another call. Both
// callers — the compositor and the log dump — iterate it immediately, and they
// cannot interleave: the Dev window does not draw from inside the panel pass.
static const std::vector<int>& FxDrawOrder() {
    static std::vector<int> order;
    order.resize(gFxStacks.size());
    for (size_t s = 0; s < gFxStacks.size(); ++s) order[s] = (int)s;
    std::stable_sort(order.begin(), order.end(), [](int a, int b) {
        return PanelTargetBreadth(gFxStacks[a].target)
             > PanelTargetBreadth(gFxStacks[b].target);
    });
    return order;
}

static void DrawPanelFx() {
    if (!gFxEnabled || gFxStacks.empty()) return;

    // ⚠ The overlay folder is NOT polled from here (2026-08-04). Picking up a
    // saved image on its own was a dev convenience — the inner loop of authoring
    // a look — and it was paying for it with a stat() of every overlay file, on
    // the render thread, once a second, in everybody's release build. It lives in
    // the Panel FX tab now, which is where the editing it serves happens; a
    // shipped install's overlays only change when the installer replaces them.
    // The initial scan moved to LoadPanelFx, off this thread entirely.

    // How much daylight is in this cockpit, for the day/night blend. One read
    // and one exponential step per pass, ahead of everything that uses it.
    FxSampleAmbient();

#if PHOTON_DEV
    // Drop a held brightness pin whose pane stopped being drawn. See
    // FxHoldDriveOverride — a pin with no off state needs somewhere that always
    // runs to take it back, and this is the only per-frame path the pin affects.
    FxExpireDriveOverride();
#endif

    // Every driver value this pass reads, read once. Scoped to the pass — see
    // FxDriverRaw for why the Dev panes must keep reading through to the sim.
    FxValueCacheBegin();

    // ONE texture unit for the whole pass, whether or not any layer has an
    // image: a solid layer binds the 1x1 white texture instead, which modulates
    // to the same color. Toggling the unit per layer would mean calling
    // XPLMSetGraphicsState inside the loop, i.e. re-entering X-Plane's blend
    // tracker while our own func is set — the ordering trap PushBlendFunc exists
    // to avoid.
    EnsureFxWhiteTexture();
    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
    PushBlendFunc();                       // AFTER XPLMSetGraphicsState — see above

    // The texture env is not part of XPLMSetGraphicsState and is shared state
    // like the blend func, so it gets the same save/restore. MODULATE is the GL
    // default and is what makes "the image tints with the layer color" true.
    GLint savedEnv = GL_MODULATE;
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &savedEnv);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    const std::vector<int>& order = FxDrawOrder();
    for (size_t n = 0; n < order.size(); ++n) {
        FxStack& stack = gFxStacks[order[n]];
        // Which stack is being drawn, for the per-rect brightness lookup. Not a
        // parameter because PanelPaintFn is a plain function pointer and the
        // probe shares it — the same reason gFxAlpha and gFxTile are globals.
        gFxDrawStack = &stack;
        for (size_t i = 0; i < stack.layers.size(); ++i) {
            const FxLayer& layer = stack.layers[i];
            // The opacity test moved INTO DrawFxLayer, which is the only place
            // that knows the day-blended value — see the note there.
            if (layer.enabled) DrawFxLayer(layer, stack.target);
        }
        gFxDrawStack = nullptr;
    }

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, savedEnv);
    PopBlendFunc();
    FxValueCacheEnd();                     // the Dev panes read live again
    gFxAlpha = 1.0f;
    gFxTile  = 1.0f;
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

// The one draw callback in the panel pass. It carries BOTH features: the shipped
// FX compositor, and — in a dev build only — the probe. Reading it top to bottom,
// everything before the `#if` runs in a release `.xpl`.
static int PanelPassDraw(XPLMDrawingPhase, int, void* refcon) {
    const int slot = (int)(intptr_t)refcon;

    // Record which passes actually run. 0 = 2-d panel, 1 = 3-d non-lit,
    // 2 = 3-d lit — the last is the one the texture browser names "lit panel fbo".
    const int type = gPanelRenderTypeRef ? XPLMGetDatai(gPanelRenderTypeRef) : -1;
    if (type >= 0 && type < 8 && !(gPanelTypesSeen & (1 << type))) {
        gPanelTypesSeen |= (1 << type);
        Log("panel pass: saw panel_render_type " + std::to_string(type));
    }
    if (type == 0) return 1;              // the 2-d panel / popups are not our target

    // The FX compositor runs from the after-Gauges slot in the lit pass, and is
    // deliberately NOT gated on the probe's mode/rect/draw knobs. Turning the
    // probe off to look at the effect must not turn the effect off with it —
    // that is the whole difference between a diagnostic and a feature. In a
    // shipping build this is the only thing the callback does.
    if (slot == kProbeAfterGauges && type == kFxPass) DrawPanelFx();

#if PHOTON_DEV
    if (gPanelProbeMode != slot) return 1;
    if (gPanelProbeType >= 0 && type != gPanelProbeType) return 1;   // pass filter

    const int typeBit = (type >= 0 && type < 8) ? (1 << type) : 0;
    if (!(gPanelStateLoggedMask & typeBit)) {
        gPanelStateLoggedMask |= typeBit;
        char buf[224];
        std::snprintf(buf, sizeof(buf),
            "panel probe: extents l=%.1f b=%.1f r=%.1f t=%.1f "
            "(logged for information ONLY — no longer applied to the rects)",
            XPLMGetDataf(gPanelExtentRefs[0]), XPLMGetDataf(gPanelExtentRefs[1]),
            XPLMGetDataf(gPanelExtentRefs[2]), XPLMGetDataf(gPanelExtentRefs[3]));
        Log(buf);
        LogGlTarget(slot == kProbeAfterPanel ? "after Panel"
                  : slot == kProbeBeforeGauges ? "before Gauges" : "after Gauges", type);
        Log(std::string("panel probe: rect set = ")
            + (PanelRects() == kPanelRectsLo ? "LOW-RES (DisplayResolutionFallback=1)"
                                             : "full-res (DisplayResolutionFallback=0)"));
    }

    const int method = gPanelProbeDrawMethod;
    const bool multiply = (method == kDrawMultiply);

    // Opaque, unblended, no depth: for the diagnostics the point is "did
    // anything of ours survive", and a translucent tint would be ambiguous at
    // exactly the wrong moment. Multiply is the exception — it is an effect
    // rather than a probe, and blending is the whole mechanism.
    XPLMSetGraphicsState(0, 0, 0, 0, multiply ? 1 : 0, 0, 0);

    float cr = 1.0f, cg = 0.0f, cb = 1.0f;                 // after gauges  — magenta
    if (slot == kProbeAfterPanel)   { cr = 0.0f; cg = 1.0f; cb = 0.0f; }   // green
    if (slot == kProbeBeforeGauges) { cr = 0.0f; cg = 0.3f; cb = 1.0f; }   // blue
    // The slot colors identify which callback painted; a multiply wants the
    // tint being tuned, and would read as "black" under a magenta probe color.
    if (multiply) { cr = gPanelTint[0]; cg = gPanelTint[1]; cb = gPanelTint[2]; }

    if (method == kDrawForced) PushForcedState();
    if (multiply)              PushMultiplyBlend();
    while (glGetError() != GL_NO_ERROR) {}   // drain, so what we log next is ours

    // ⚠ Only the QUAD method batches. ClearProbeRect is glScissor + glClear, and
    // neither is legal between glBegin and glEnd — wrapping it would turn the
    // probe's strongest diagnostic (the one that touches no vertices at all) into
    // a GL error that draws nothing, which is the exact answer it exists to rule
    // out.
    if (method == kDrawClear) {
        PaintPanelTarget(gPanelProbeRect, ClearProbeRect, cr, cg, cb);
    } else {
        PanelQuadBatch batch;
        PaintPanelTarget(gPanelProbeRect, DrawProbeRect, cr, cg, cb);
    }

    if (!(gPanelErrLoggedMask & typeBit)) {
        gPanelErrLoggedMask |= typeBit;
        GLenum err = glGetError();
        char buf[224];
        std::snprintf(buf, sizeof(buf),
            "panel probe: draw method %s (render_type=%d) -> glGetError 0x%X%s",
            kDrawMethodName[method], type, (unsigned)err,
            err == GL_NO_ERROR ? " (no error — the calls were accepted)" : "");
        Log(buf);
        if (multiply) {
            std::snprintf(buf, sizeof(buf),
                "panel probe: multiply tint %.3f %.3f %.3f, restoring blend func "
                "src/dst RGB 0x%X/0x%X alpha 0x%X/0x%X",
                gPanelTint[0], gPanelTint[1], gPanelTint[2],
                (unsigned)gBlendSaved.srcRGB, (unsigned)gBlendSaved.dstRGB,
                (unsigned)gBlendSaved.srcA,   (unsigned)gBlendSaved.dstA);
            Log(buf);
        }
    }

    if (multiply)              PopMultiplyBlend();
    if (method == kDrawForced) PopForcedState();

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);    // leave the shared GL color as we found it
#endif  // PHOTON_DEV — the probe half of the panel-pass callback
    return 1;
}

// Registration order within a phase is the lever we have on draw order: a plugin
// registering later is called later. So this is deliberately re-run on every
// aircraft load, AFTER the aircraft's own plugin has had its XPluginStart — which
// is the whole trick if the first result comes back "ToLiss draws over us".
//
// A SHIPPING build registers ONE callback, the after-Gauges slot the compositor
// draws from. The other two exist only so the probe can compare slots; registering
// them in a release would put two extra per-frame callbacks in the panel pass that
// can never draw anything.
static void RegisterPanelPass() {
    const int a = XPLMRegisterDrawCallback(PanelPassDraw, xplm_Phase_Gauges, 0,
                                           (void*)(intptr_t)kProbeAfterGauges);
    gPanelProbeRegistered = true;
#if PHOTON_DEV
    const int b = XPLMRegisterDrawCallback(PanelPassDraw, xplm_Phase_Panel, 0,
                                           (void*)(intptr_t)kProbeAfterPanel);
    const int c = XPLMRegisterDrawCallback(PanelPassDraw, xplm_Phase_Gauges, 1,
                                           (void*)(intptr_t)kProbeBeforeGauges);
    Log("panel pass: registered draw callbacks (after-gauges=" + std::to_string(a)
        + " after-panel=" + std::to_string(b) + " before-gauges=" + std::to_string(c)
        + "; 0 would mean the phase is unsupported)");
#else
    Log("panel pass: registered the after-Gauges draw callback (" + std::to_string(a)
        + "; 0 would mean the phase is unsupported)");
#endif
}

static void UnregisterPanelPass() {
    if (!gPanelProbeRegistered) return;
    XPLMUnregisterDrawCallback(PanelPassDraw, xplm_Phase_Gauges, 0, (void*)(intptr_t)kProbeAfterGauges);
#if PHOTON_DEV
    XPLMUnregisterDrawCallback(PanelPassDraw, xplm_Phase_Panel,  0, (void*)(intptr_t)kProbeAfterPanel);
    XPLMUnregisterDrawCallback(PanelPassDraw, xplm_Phase_Gauges, 1, (void*)(intptr_t)kProbeBeforeGauges);
#endif
    gPanelProbeRegistered = false;
}

#if PHOTON_DEV
static int PanelProbeCycleHandler(XPLMCommandRef, XPLMCommandPhase phase, void*) {
    if (phase == xplm_CommandBegin)
        WritePanelProbeMode(nullptr, (gPanelProbeMode + 1) % kProbeModeCount);
    return 0;
}

// Defined with the dev window below, which needs the accessors above.
static void BuildDbgRectOpts();
static void LoadPanelDebug();
static void LoadDevPrefs();
#endif

// Defined with the FX persistence block below; called from the start-up path.
static void LoadPanelFx();

// ========================== the performance tool =============================
//
// "Which of these is costing me frames?" — measured, rather than argued about,
// by turning one thing off at a time and timing the result. It sits here, below
// the compositor, because the levers it moves are declared above it: the three
// Displays switches, the FX compositor as a whole, and the waveform engine.
//
// TWO PANES, ONE IMPLEMENTATION (BuildPerfPane):
//   * SHIPPING — its own Performance window, opened from the Displays tab. A live frame-rate
//     readout, an A/B between Photon's display effects on and off, and a report
//     that can be pasted into a bug thread.
//   * DEV — the Dev window's Perf tab. The same, plus the internal levers and a
//     one-at-a-time sweep of all of them.
//
// ⚠ THE SHIPPING PANE HAS NO MANUAL SWITCHES, deliberately. A checkbox here that
// is not saved (and it must not be — a measurement is not a preference) would
// look exactly like the Displays tab's and quietly forget itself on the next
// launch. So the shipping pane only ever moves a lever inside a run, which puts
// it back. The Dev pane does offer them, and says the same thing in a hint.
//
// ⚠ THE RUN IS DRIVEN FROM A FLIGHT LOOP, NOT FROM THE DRAW CALLBACK. A draw
// callback stops being called the moment its window is hidden, popped out or
// covered, and a run abandoned halfway would leave the levers it moved wherever
// the last step put them, with nothing on screen to say so.
//
// ⚠ AND IT RESTORES WHAT IT MOVED FROM THREE PLACES — the end of a run, Abort,
// and XPluginDisable. A benchmark that leaves the user's settings changed is a
// bug report about the effect it was measuring.

// ---- the levers -------------------------------------------------------------
// One entry per thing that can be switched off and timed. `dev` marks the ones a
// user has no way to reach anyway (the compositor as a whole, the engine), which
// is exactly why they are worth having here: "it is not the panel effects" is an
// answer, and without a lever it takes a rebuild to get.
enum {
    kLeverDisplayFx = 0,   // gDisplays.enabled — the Displays master
    kLeverSpill,           // gDisplays.spill
    kLeverScreenFx,        // gDisplays.screenFx
    kLeverOtherFx,         // gDisplays.otherFx
    kLeverCompositor,      // gFxEnabled — the whole panel pass
    kLeverEngine,          // the waveform engine's per-frame flight loop
    kLeverCockpitLights,   // !gCockpit.optimized — the cockpit's full light stack
    kLeverCount
};

struct PerfLever { const char* name; const char* help; bool dev; };
static const PerfLever kLevers[kLeverCount] = {
    {"Display effects (all)",
     "The master switch on the Displays tab. Off is the stock ToLiss cockpit: no "
     "glow from the screens and no tinting on any readout.", false},
    {"Backlight illumination",
     "The six spill lights the screens throw into the cockpit. Real lights, so "
     "this is the one on the list that costs the renderer rather than us.", false},
    {"Backlight visual effect",
     "The tint stacks painted over the six displays, the MCDUs, the DCDUs and the "
     "standby instrument.", false},
    {"Modified display colors",
     "The tint stacks over every other readout - FCU, radios, clock, DME, "
     "overhead.", false},
    {"Panel FX compositor",
     "The whole panel pass, not just its output: with this off the stacks are "
     "not composited at all. The difference between this and the two switches "
     "above IS the cost of the compositor itself.", true},
    {"Waveform engine",
     "Our per-frame flight loop - the beacon and strobe shaping. Expected to be "
     "far too small to see; it is here so that can be shown rather than assumed.\n"
     "While it is off the exterior flash ratios stay wherever the last frame left "
     "them.", true},
    {"Cockpit flood light stacks",
     "The main panel floods as authored - two or three overlapping lights each, "
     "ten in all. Off is the Cockpit tab's 'Use simplified lighting': four, one "
     "per lamp. Real lights again, so this is the renderer's cost, not ours.\n"
     "Dev-only because the A/B run does not move it - it is a cockpit setting "
     "rather than a display effect, and it is measured by the sweep.", true},
};

static bool PerfLeverGet(int i) {
    switch (i) {
        case kLeverDisplayFx:  return gDisplays.enabled;
        case kLeverSpill:      return gDisplays.spill;
        case kLeverScreenFx:   return gDisplays.screenFx;
        case kLeverOtherFx:    return gDisplays.otherFx;
        case kLeverCompositor: return gFxEnabled;
        case kLeverEngine:     return gEngineLoopID != nullptr;
        // Inverted, so that "on" means the expensive state here as it does on
        // every other row: the full stack is what the reduced set switches OFF.
        case kLeverCockpitLights: return !gCockpit.optimized;
        default:               return false;
    }
}

// ⚠ NOTHING HERE PERSISTS. Not SaveDisplays, not SavePanelFx — see the pane's
// note above. The run restores every lever it touched, and a value that had been
// written to disk in between would survive that restore.
static void PerfLeverSet(int i, bool on) {
    switch (i) {
        case kLeverDisplayFx:  gDisplays.enabled  = on; break;
        case kLeverSpill:      gDisplays.spill    = on; break;
        case kLeverScreenFx:   gDisplays.screenFx = on; break;
        case kLeverOtherFx:    gDisplays.otherFx  = on; break;
        case kLeverCompositor: gFxEnabled         = on; break;
        // ⚠ Creating and destroying a flight loop, not setting a flag. Legal from
        // inside a DIFFERENT flight loop (which is where a run drives it from) and
        // not from inside its own — which is the reason the perf loop is a second
        // loop rather than a branch inside EngineLoop.
        case kLeverEngine:     if (on) StartEngine(); else StopEngine(); break;
        // ⚠ The FIELD, not SetCockpitOptimized — that one saves to disk, and a
        // value written there mid-run would survive the restore. Same rule as
        // every lever above (see this function's note).
        case kLeverCockpitLights: gCockpit.optimized = !on; break;
        default: break;
    }
}

// ---- frame sampling ---------------------------------------------------------
// Frame times come from the flight loop's own "seconds since I last ran" — it is
// scheduled every frame, so that IS the frame interval, and it needs no dataref
// and no assumption about which of the sim's several time bases is which.
static const int   kPerfRing     = 480;      // ~8 s at 60 fps; also the plot width
static const float kPerfMaxFrame = 1.0f;     // longer than this is a load, not a frame

static float gPerfRing[kPerfRing] = {0.0f};
static int   gPerfRingHead = 0, gPerfRingCount = 0;

static float gPerfLiveFps = 0.0f, gPerfLiveMs = 0.0f, gPerfLiveLow = 0.0f;
static float gPerfBucketSec = 0.0f;
static int   gPerfBucketFrames = 0;
static int   gPerfDropped = 0;               // frames rejected as hitches

static XPLMFlightLoopID gPerfLoopID = nullptr;

struct PerfStats {
    float fps = 0.0f, ms = 0.0f, low1 = 0.0f;
    int   frames = 0;
};

// The 1% low is the AVERAGE OF THE SLOWEST ONE PERCENT of frames, expressed as an
// fps — the usual reading of the term, and the number that says whether a change
// costs smoothness rather than average rate. A mean alone hides exactly the kind
// of cost this tool is looking for: a per-second pass that stalls one frame in
// sixty leaves the average almost untouched.
static PerfStats PerfStatsOf(std::vector<float> ms) {
    PerfStats s;
    if (ms.empty()) return s;
    s.frames = (int)ms.size();
    double sum = 0.0;
    for (float v : ms) sum += v;
    s.ms  = (float)(sum / ms.size());
    s.fps = s.ms > 0.0f ? 1000.0f / s.ms : 0.0f;

    std::sort(ms.begin(), ms.end());
    size_t n = ms.size() / 100;
    if (n < 1) n = 1;                        // a short sample still has a worst frame
    double worst = 0.0;
    for (size_t i = ms.size() - n; i < ms.size(); ++i) worst += ms[i];
    worst /= (double)n;
    s.low1 = worst > 0.0 ? (float)(1000.0 / worst) : 0.0f;
    return s;
}

static std::vector<float> PerfRingSamples() {
    std::vector<float> out;
    out.reserve(gPerfRingCount);
    for (int i = 0; i < gPerfRingCount; ++i) {
        int idx = gPerfRingHead - 1 - i;
        while (idx < 0) idx += kPerfRing;
        out.push_back(gPerfRing[idx]);
    }
    return out;
}

static void PerfResetSampling() {
    gPerfRingHead = gPerfRingCount = 0;
    gPerfBucketSec = 0.0f;
    gPerfBucketFrames = 0;
    gPerfDropped = 0;
    gPerfLiveFps = gPerfLiveMs = gPerfLiveLow = 0.0f;
}

// ---- a run ------------------------------------------------------------------
// A step is a COMPLETE lever mask, never a delta from whatever was set before it.
// A sweep that toggled one lever per step would carry every earlier step's state
// forward, so a mistake in step 2 would show up as a cost in steps 3..n and read
// as several separate findings.
struct PerfStep {
    std::string label;
    bool        lever[kLeverCount];
};
struct PerfResult {
    std::string label;
    PerfStats   stats;
};

// One result as a line of text, for Log.txt and for the report button. Written
// with snprintf rather than std::to_string, which renders 59.7 as "59.700000" and
// makes a log of six of these unreadable.
static std::string PerfResultLine(const PerfResult& r) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: %.1f fps avg, %.1f fps 1%% low, "
                                    "%.2f ms, %d frames",
                  r.label.c_str(), (double)r.stats.fps, (double)r.stats.low1,
                  (double)r.stats.ms, r.stats.frames);
    return buf;
}

static std::vector<PerfStep>   gPerfSteps;
static std::vector<PerfResult> gPerfResults;
static std::vector<float>      gPerfStepSamples;
static int   gPerfStep       = -1;           // -1 = idle
static bool  gPerfSettling   = false;
static float gPerfPhaseLeft  = 0.0f;
static float gPerfSettleSec  = 2.0f;
static float gPerfMeasureSec = 8.0f;
static bool  gPerfSaved[kLeverCount] = {false};
static bool  gPerfHaveSaved = false;
static std::string gPerfLastNote;

// The settle wait is not politeness. Switching the compositor off leaves a frame
// or two of already-queued GL work, X-Plane's own frame pacing takes a moment to
// find its level again, and a texture the pass had been touching can fall out of
// cache — measure immediately and the first configuration in a run always looks
// worse than the same configuration measured second.
static void PerfApplyStep(const PerfStep& s) {
    for (int i = 0; i < kLeverCount; ++i) PerfLeverSet(i, s.lever[i]);
}

static void PerfRestoreLevers() {
    if (!gPerfHaveSaved) return;
    for (int i = 0; i < kLeverCount; ++i) PerfLeverSet(i, gPerfSaved[i]);
    gPerfHaveSaved = false;
}

static void PerfStopRun(const char* why) {
    gPerfStep = -1;
    gPerfSettling = false;
    gPerfStepSamples.clear();
    PerfRestoreLevers();
    if (why) {
        gPerfLastNote = why;
        Log(std::string("perf: run ") + why);
    }
}

static void PerfEnsureLoop();

static void PerfStartRun(std::vector<PerfStep> steps) {
    if (steps.empty()) return;
    PerfEnsureLoop();
    if (gPerfStep >= 0) PerfStopRun(nullptr);

    for (int i = 0; i < kLeverCount; ++i) gPerfSaved[i] = PerfLeverGet(i);
    gPerfHaveSaved = true;

    gPerfSteps = std::move(steps);
    gPerfResults.clear();
    gPerfStepSamples.clear();
    gPerfStep      = 0;
    gPerfSettling  = true;
    gPerfPhaseLeft = gPerfSettleSec;
    gPerfLastNote.clear();
    PerfApplyStep(gPerfSteps[0]);
    Log("perf: run started, " + std::to_string(gPerfSteps.size()) + " step(s)");
}

// Called once per frame from the perf loop while a run is up.
static void PerfAdvanceRun(float dt) {
    if (gPerfStep < 0 || gPerfStep >= (int)gPerfSteps.size()) return;
    gPerfPhaseLeft -= dt;

    if (gPerfSettling) {
        if (gPerfPhaseLeft <= 0.0f) {
            gPerfSettling  = false;
            gPerfPhaseLeft = gPerfMeasureSec;
            gPerfStepSamples.clear();
        }
        return;
    }

    // 8 s at even 200 fps is 1600 samples; the cap is for a runaway measure time,
    // not for the normal case.
    if (gPerfStepSamples.size() < 20000) gPerfStepSamples.push_back(dt * 1000.0f);
    if (gPerfPhaseLeft > 0.0f) return;

    PerfResult r;
    r.label = gPerfSteps[gPerfStep].label;
    r.stats = PerfStatsOf(gPerfStepSamples);
    gPerfResults.push_back(r);
    Log("perf: " + PerfResultLine(r));

    ++gPerfStep;
    if (gPerfStep >= (int)gPerfSteps.size()) {
        PerfStopRun(nullptr);
        gPerfLastNote = "finished";
        Log("perf: run finished");
        return;
    }
    gPerfSettling  = true;
    gPerfPhaseLeft = gPerfSettleSec;
    gPerfStepSamples.clear();
    PerfApplyStep(gPerfSteps[gPerfStep]);
}

static float PerfLoop(float elapsed, float, int, void*) {
    // ⚠ A frame longer than a second is a scenery load, an aircraft reload or an
    // alt-tab — not a frame. Counting one would put a 5000 ms sample into the 1%
    // low and make whichever step it landed in look catastrophic. Counted, though,
    // and shown: "the run had 3 hitches" is why two runs disagree.
    if (elapsed > 0.0f && elapsed < kPerfMaxFrame) {
        const float ms = elapsed * 1000.0f;
        gPerfRing[gPerfRingHead] = ms;
        gPerfRingHead = (gPerfRingHead + 1) % kPerfRing;
        if (gPerfRingCount < kPerfRing) ++gPerfRingCount;

        gPerfBucketSec += elapsed;
        ++gPerfBucketFrames;
        if (gPerfBucketSec >= 0.5f) {
            gPerfLiveFps = (float)gPerfBucketFrames / gPerfBucketSec;
            gPerfLiveMs  = gPerfBucketSec * 1000.0f / (float)gPerfBucketFrames;
            // The 1% low is over the whole ring rather than the half-second
            // bucket: at 60 fps a bucket holds 30 frames, so its "worst 1%" is
            // just its worst frame and the number would flicker wildly.
            gPerfLiveLow = PerfStatsOf(PerfRingSamples()).low1;
            gPerfBucketSec = 0.0f;
            gPerfBucketFrames = 0;
        }
        PerfAdvanceRun(elapsed);
    } else if (elapsed >= kPerfMaxFrame) {
        ++gPerfDropped;
    }
    return -1.0f;   // every frame
}

// Created on first use, never destroyed until the plugin is disabled: a session
// that never opens a performance pane pays for nothing at all, and one that does
// keeps a continuous history rather than restarting it every time the window is
// reopened.
static void PerfEnsureLoop() {
    if (gPerfLoopID) return;
    XPLMCreateFlightLoop_t p;
    p.structSize   = sizeof(p);
    // Same phase as the waveform engine, so the interval measured is the sim's
    // own frame cadence and not the gap between two different phases.
    p.phase        = xplm_FlightLoop_Phase_AfterFlightModel;
    p.callbackFunc = PerfLoop;
    p.refcon       = nullptr;
    gPerfLoopID    = XPLMCreateFlightLoop(&p);
    if (!gPerfLoopID) { Log("perf: could not create the sampling flight loop"); return; }
    XPLMScheduleFlightLoop(gPerfLoopID, -1.0f, 1);
    PerfResetSampling();
    Log("perf: sampling started");
}

// From XPluginDisable. ⚠ The lever restore comes FIRST: destroying the loop while
// a run is up would strand the levers at whatever the last step set.
static void PerfShutdown() {
    PerfStopRun(nullptr);
    if (gPerfLoopID) { XPLMDestroyFlightLoop(gPerfLoopID); gPerfLoopID = nullptr; }
    SysMetrics::Stop();
}

// ---- the step lists ---------------------------------------------------------
static PerfStep PerfStepAll(const char* label, bool on) {
    PerfStep s;
    s.label = label;
    for (int i = 0; i < kLeverCount; ++i) s.lever[i] = on;
    return s;
}

// The shipping A/B. Two steps, and the OFF one drops the master alone: the other
// switches keep their values so that "on" means the user's own configuration,
// which is the one they want measured.
static std::vector<PerfStep> PerfStepsAB() {
    PerfStep on, off;
    on.label  = "Display effects ON";
    off.label = "Display effects OFF";
    for (int i = 0; i < kLeverCount; ++i) {
        on.lever[i]  = PerfLeverGet(i);
        off.lever[i] = PerfLeverGet(i);
    }
    on.lever[kLeverDisplayFx]  = true;
    off.lever[kLeverDisplayFx] = false;
    return {on, off};
}

// The dev sweep: a baseline with everything on, then one step per lever with that
// lever alone off, then everything off. One factor at a time — each row is
// therefore readable as "this is what that one thing costs", and the last row is
// the check that the parts add up to the whole.
static std::vector<PerfStep> PerfStepsSweep() {
    std::vector<PerfStep> steps;
    steps.push_back(PerfStepAll("Everything on (baseline)", true));
    for (int i = 0; i < kLeverCount; ++i) {
        // The master would mask the three switches under it, so its own row is
        // the "all display effects off" one and it goes last of the four.
        PerfStep s = PerfStepAll((std::string("without ") + kLevers[i].name).c_str(), true);
        s.lever[i] = false;
        steps.push_back(s);
    }
    steps.push_back(PerfStepAll("Everything off", false));
    return steps;
}

// ---- the pane ---------------------------------------------------------------
static void PerfDrawLive() {
    ImGui::Text("%.1f fps", (double)gPerfLiveFps);
    ImGui::SameLine();
    ImGui::TextDisabled("| %.2f ms/frame | 1%% low %.1f fps | %d frames sampled",
                        (double)gPerfLiveMs, (double)gPerfLiveLow, gPerfRingCount);
    if (gPerfDropped > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %d hitch(es) ignored", gPerfDropped);
        UiItemTooltip("Frames longer than a second - a scenery load, an aircraft "
                      "reload, an alt-tab. They are left out of every figure on "
                      "this page, and counted here so two runs that disagree have "
                      "somewhere to show why.");
    }

    // Frame TIME, not fps, and that is the point: the plot's height is milliseconds
    // and a spike goes UP. An fps plot compresses everything that matters into the
    // bottom of the graph.
    float peak = 0.0f;
    for (int i = 0; i < gPerfRingCount; ++i) if (gPerfRing[i] > peak) peak = gPerfRing[i];
    if (peak < 20.0f) peak = 20.0f;
    ImGui::PlotLines("##perf_plot", gPerfRing, kPerfRing, gPerfRingHead,
                     "frame time (ms)", 0.0f, peak * 1.1f, ImVec2(0.0f, 60.0f));
}

static void PerfDrawTelemetry() {
    const SysMetrics::Sample s = SysMetrics::Read();
    const std::string note = SysMetrics::Note();

    if (s.cpuValid) {
        ImGui::Text("CPU  X-Plane %.0f%%", (double)s.processCpu);
        ImGui::SameLine();
        ImGui::TextDisabled("| machine %.0f%% | %d cores", (double)s.systemCpu, s.cores);
        UiItemTooltip("X-Plane's own CPU time as a share of the WHOLE machine, so "
                      "100%% would mean every core busy with the simulator. A "
                      "figure near 100/cores means one thread is saturated, which "
                      "is the usual shape of a CPU-bound frame.");
        // One bar per core, wrapped. The per-core view is what distinguishes "the
        // machine is busy" from "one thread is pinned and everything waits for it",
        // and only the second is something a plugin can cause.
        const float w = 26.0f;
        for (int i = 0; i < s.cores && i < SysMetrics::kMaxCores; ++i) {
            ImGui::PushID(i);
            ImGui::ProgressBar(s.core[i] / 100.0f, ImVec2(w, 10.0f), "");
            UiItemTooltip("core %d: %.0f%%", i, (double)s.core[i]);
            ImGui::PopID();
            if ((i % 16) != 15 && i + 1 < s.cores) ImGui::SameLine();
        }
    } else {
        ImGui::TextDisabled("CPU  no reading");
    }

    if (s.gpuValid) {
        ImGui::Text("GPU  %.0f%%", (double)s.gpu);
        UiItemTooltip("The 3D engines of every process on this machine, added up "
                      "and clamped at 100%%. Near 100 with the frame rate low is "
                      "GPU-bound, and a lighting effect is a plausible cause; well "
                      "under it means the frame is waiting on something else.");
    } else if (SysMetrics::Running()) {
        ImGui::TextDisabled("GPU  no reading");
    }

    if (!note.empty()) UiHint("%s", note.c_str());
}

static void PerfDrawResults() {
    if (gPerfResults.empty()) return;
    ImGui::Spacing();
    ImGui::SeparatorText("Results");

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("perf_results", 5, flags)) {
        ImGui::TableSetupColumn("Configuration");
        ImGui::TableSetupColumn("fps");
        ImGui::TableSetupColumn("ms");
        ImGui::TableSetupColumn("1% low");
        ImGui::TableSetupColumn("vs. first");
        ImGui::TableHeadersRow();

        const float base = gPerfResults[0].stats.fps;
        for (size_t i = 0; i < gPerfResults.size(); ++i) {
            const PerfResult& r = gPerfResults[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.label.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f", (double)r.stats.fps);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", (double)r.stats.ms);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f", (double)r.stats.low1);
            ImGui::TableSetColumnIndex(4);
            if (i == 0) {
                ImGui::TextDisabled("baseline");
            } else {
                const float d = r.stats.fps - base;
                // Colored on SIGN only, never on size: a threshold here would be a
                // claim about what counts as significant, and run-to-run scatter on
                // a busy machine is easily a frame or two either way.
                ImGui::TextColored(d >= 0.0f ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)
                                             : ImVec4(0.95f, 0.55f, 0.35f, 1.0f),
                                   "%+.1f fps", (double)d);
            }
        }
        ImGui::EndTable();
    }

    UiHint("Note: weather, traffic, scenery, and time of day all affect "
           "performance. You may also see a slight loss in framerate whenever "
           "this window is open.");

    if (ImGui::Button("Copy report to Log.txt")) {
        Log("---- performance report ----");
        Log(std::string("Photon ") + kPhotonVersion
            + (gInteriorInstalled ? ", cockpit mod installed" : ", exterior only"));
        const SysMetrics::Sample s = SysMetrics::Read();
        if (s.cpuValid) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "machine: CPU %.0f%% over %d cores, X-Plane %.0f%%, GPU %s",
                          (double)s.systemCpu, s.cores, (double)s.processCpu,
                          s.gpuValid ? "" : "not reported");
            if (s.gpuValid) {
                char gpu[32];
                std::snprintf(gpu, sizeof(gpu), "%.0f%%", (double)s.gpu);
                Log(std::string(buf) + gpu);
            } else {
                Log(buf);
            }
        }
        for (const PerfResult& r : gPerfResults) Log("  " + PerfResultLine(r));
        Log("---- end ----");
    }
    UiItemTooltip("Writes the table above into X-Plane's Log.txt, where it can be "
                  "attached to a bug report. Nothing is sent anywhere.");
}

static void PerfDrawRunControls(bool dev) {
    const bool running = gPerfStep >= 0;

    if (running) {
        const int   n     = (int)gPerfSteps.size();
        const float total = gPerfSettleSec + gPerfMeasureSec;
        // What is left of THIS step, settle and measure together — the settle
        // phase still has the whole measure phase behind it.
        const float left  = gPerfSettling ? gPerfPhaseLeft + gPerfMeasureSec : gPerfPhaseLeft;
        const float within = total > 0.0f ? (total - left) / total : 0.0f;
        const float done   = ((float)gPerfStep + within) / (float)n;
        char label[128];
        std::snprintf(label, sizeof(label), "step %d/%d - %s (%s %.0fs)",
                      gPerfStep + 1, n, gPerfSteps[gPerfStep].label.c_str(),
                      gPerfSettling ? "settling" : "measuring",
                      (double)(gPerfPhaseLeft < 0.0f ? 0.0f : gPerfPhaseLeft));
        ImGui::ProgressBar(done < 0.0f ? 0.0f : (done > 1.0f ? 1.0f : done),
                           ImVec2(-1.0f, 0.0f), label);
        if (ImGui::Button("Abort")) PerfStopRun("aborted");
        UiItemTooltip("Stops here and puts every switch back the way it was.");
        UiHint("Leave the view alone while this runs - moving the camera, opening a "
               "menu or changing the weather changes the frame rate far more than "
               "any of these switches, and the run cannot tell the difference.");
        return;
    }

    if (ImGui::Button(dev ? "Run A/B" : "Measure display effects")) PerfStartRun(PerfStepsAB());
    UiItemTooltip("Times the cockpit twice - once with Photon's display effects "
                  "on and once with them off - and puts your settings back "
                  "afterwards. About %.0f seconds.",
                  (double)(2.0f * (gPerfSettleSec + gPerfMeasureSec)));
    if (dev) {
        ImGui::SameLine();
        if (ImGui::Button("Run full sweep")) PerfStartRun(PerfStepsSweep());
        UiItemTooltip("Baseline, then every lever switched off one at a time, then "
                      "everything off. %d steps, about %.0f seconds.",
                      kLeverCount + 2,
                      (double)((kLeverCount + 2) * (gPerfSettleSec + gPerfMeasureSec)));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("settle", &gPerfSettleSec, 0.1f, 0.5f, 10.0f, "%.1fs");
        UiItemTooltip("Dead time after a switch moves, before timing starts. Too "
                      "short and the first configuration in a run always looks the "
                      "worst.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("measure", &gPerfMeasureSec, 0.5f, 2.0f, 60.0f, "%.0fs");
    }
    if (!gPerfLastNote.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", gPerfLastNote.c_str());
    }
}

static void PerfDrawLevers(bool dev) {
    ImGui::SeparatorText(dev ? "Levers" : "Current Settings");
    if (!dev) {
        // Read-only on the shipping pane — see the note at the top of this block.
        //
        // What is ON is drawn in the ordinary text color and what is off stays
        // dim: this is a list the eye scans for "what will the test actually be
        // measuring", and a uniformly gray one answers that only by reading every
        // row's bracket.
        for (int i = 0; i < kLeverCount; ++i) {
            if (kLevers[i].dev) continue;
            const bool on = PerfLeverGet(i);
            if (on) ImGui::Text("[on]   %s", kLevers[i].name);
            else    ImGui::TextDisabled("[off]  %s", kLevers[i].name);
            UiItemTooltip("%s", kLevers[i].help);
        }
        UiHint("This test will temporarily disable the effects you have enabled "
               "to measure the potential performance impact.");
        return;
    }
    for (int i = 0; i < kLeverCount; ++i) {
        ImGui::PushID(i);
        bool on = PerfLeverGet(i);
        if (ImGui::Checkbox(kLevers[i].name, &on)) PerfLeverSet(i, on);
        UiItemTooltip("%s", kLevers[i].help);
        ImGui::PopID();
    }
    UiHint("Dev only, and NOT saved: these move the live state for as long as the "
           "session lasts. A choice that should survive a restart belongs on the "
           "Displays tab.");
}

static void BuildPerfPane(bool dev) {
    ImGui::PushID(dev ? "perf_dev" : "perf_ship");
    PerfEnsureLoop();
    SysMetrics::Start();

    ImGui::Spacing();
    UiHint("You can use this performance analysis tool to determine how your "
           "current ToLiss Photon settings may impact your framerate.");
    ImGui::Spacing();

    PerfDrawLive();
    ImGui::Spacing();
    PerfDrawTelemetry();

    ImGui::Spacing();
    ImGui::SeparatorText("Measure");
    PerfDrawRunControls(dev);

    ImGui::Spacing();
    PerfDrawLevers(dev);

    PerfDrawResults();
    ImGui::PopID();
}

static void BuildPerfTab(bool dev) { BuildPerfPane(dev); }

#if PHOTON_DEV
// ===================== dev: the live light editor ============================
//
// A cockpit light is the one thing in this project that CANNOT be tuned by
// reading source: a LIGHT_SPILL_CUSTOM is invisible in mid-air, only its pool
// shows, and on a close-range lamp the bright part of that pool is nowhere near
// the origin. So the OBJ is built once in a special shape —
//
//     python build/build_objs.py build --target interior --debug --write
//
// — which turns every light into a LIGHT_SPILL_CUSTOM bound to
// ToLissPhoton/debug/light/<n> and drops a `lights_inn.debug.json` manifest
// beside it. This registers those datarefs and feeds them from the window, so
// color, aim, spread, size, brightness and POSITION are live knobs with no
// rebuild and no reload.
//
// ⚠ A debug OBJ has NO category gating, so the Cockpit profile menu does nothing
// while one is installed. Put the real one back with
// `build --target interior --write`.
//
// ⚠ ONE OWNER. These names are also registered by src/plugin/PI_PhotonDevReload.py
// (XPPython3), whose accessors are read-only computed values — writing to them
// from here is impossible, not merely rude. So whoever registers first owns them
// and the other one has to stand down; this build detects that and says so in the
// tab rather than presenting knobs that do nothing.
static const int   kDbgMaxLights   = 64;      // must match DEBUG_MAX_LIGHTS
static const int   kDbgParamCount  = 9;
static const float kDbgPosRange    = 12.0f;   // must match DEBUG_POS_RANGE
static const int   kDbgMarkAxisDots = 5;      // must match DEBUG_MARK_AXIS_DOTS
static const int   kDbgMarkRimDots  = 4;
static const int   kDbgMarkDots     = 1 + kDbgMarkAxisDots + kDbgMarkRimDots;
static const int   kDbgMarkRoles    = 3;

// ⚠ THE DATAREF ORDER IS NOT THE OBJ LINE'S ORDER. Measured in-sim 2026-07-30:
//
//     the OBJ8 line is  ... <size> <dx> <dy> <dz> <semi> <dataref>
//     the dataref wants r g b a size  SEMI  dx dy dz
//
// i.e. the cone comes FIRST in the trailing group of four. Everything in this
// file keeps values in the READABLE order (5..7 direction, 8 cone) because the
// UI, the aim maths and the markers all index them that way; the translation
// happens once, at the wire, in ReadDbgLight. Alpha is index 3 in BOTH orderings,
// which is why the shipping lights were never affected.
static const int kDbgWireOrder[kDbgParamCount] = { 0, 1, 2, 3, 4, 8, 5, 6, 7 };

// Working value layout, readable order:
//   0..2 rgb   3 alpha   4 size   5..7 dir   8 cone
// with position offset and aim kept beside it rather than in the array — the aim
// ANGLES are authoritative and 5..7 are DERIVED from them. A vector -> angles ->
// vector round trip loses yaw at the poles, so editing pitch through 90 degrees
// would silently reset yaw; and an angle pair cannot express a non-unit vector,
// which is the bug class that made the interior's cones inert.
struct DbgLight {
    int         n = 0;
    std::string name, fixture, category, cls, source;
    int         index = 0;
    // >0 marks the SIMPLIFIED (`optimize: boost <f>`) copy of a main panel
    // flood: v[4] holds the authored UNBOOSTED size and the live size is
    // v[4] x gDbgFloodBoost, so one knob drives every simplified flood at once.
    float       boost = 0.0f;
    // Whether THIS light's OBJ baked the ANIM_trans position wrappers. Per
    // light rather than global because two debug OBJs are merged now and only
    // one of them may have been built with them.
    bool        posTunable = false;
    float       pos[3]  = { 0.0f, 0.0f, 0.0f };     // baked, for display only
    float       v[kDbgParamCount]    = { 0.0f };
    float       base[kDbgParamCount] = { 0.0f };
    float       off[3]  = { 0.0f, 0.0f, 0.0f };     // live position offset, meters
    float       baseOff[3] = { 0.0f, 0.0f, 0.0f };
    float       pitch = 0.0f, yaw = 0.0f;
    float       basePitch = 0.0f, baseYaw = 0.0f;
};

static std::vector<DbgLight> gDbgLights;
// ⚠ SLOT is not INDEX. `n` in the manifest is the dataref slot the OBJ baked;
// the vector position is just where it landed in the list. They agree today and
// the generator emits them in order, but a slot lookup that assumes it would
// drive the wrong light with no symptom except "that knob moves the wrong lamp".
// Everything user-facing (the selection, the marker dataref) is in SLOTS.
static int gDbgSlotToIndex[kDbgMaxLights];
static std::string gDbgManifest;         // which file is driving, for the header
static std::string gDbgTarget;           // "cockpit" | "screens"
static std::string gDbgNote;             // last thing that happened
static bool  gDbgPosTunable = false;
static bool  gDbgScanned    = false;     // have we looked for a manifest yet
static int   gDbgMarkerDots = 0;         // 0 = this OBJ has no markers baked in
static int   gDbgSel       = -1;
static bool  gDbgIsolate   = false;      // black out everything but the selection
static bool  gDbgMark      = false;      // paint the selection magenta
static bool  gDbgBlink     = false;
static int   gDbgShowMarkers = 0;        // 0 off, -1 all, n+1 = light n
static int   gDbgCompare   = 1;          // 1 = tunable copies, 0 = the originals
static float gDbgMarkerReach = 0.6f;
static float gDbgMarkerSize  = 0.05f;
static bool  gDbgOwnsRefs  = false;      // false = someone else registered them
// The simplified-flood intensity multiplier (the live form of the DSL's
// `optimize: boost <f>`), and the authored factor the manifest seeded it from.
// Edited on the Dev window's Cockpit tab, applied in DbgParams to every light
// whose manifest row carries `boost`.
static float gDbgFloodBoost     = 1.0f;
static float gDbgFloodBoostSeed = 1.0f;
// Edit the six screen-glow lights as ONE light: while set, every shared
// attribute (color/brightness/size/aim/spread — everything but position) of the
// selected screens light is copied onto the other five each frame.
static bool  gDbgScreensLink = true;

static XPLMDataRef gDbgLightRefs[kDbgMaxLights] = { nullptr };
static std::string gDbgLightNames[kDbgMaxLights];      // must outlive registration
static XPLMDataRef gDbgPosRef = nullptr, gDbgMarkRef = nullptr;
static XPLMDataRef gDbgMarkShowRef = nullptr, gDbgCompareRef = nullptr;
static XPLMDataRef gDbgMarkParamRefs[kDbgMarkRoles] = { nullptr };
static std::string gDbgMarkParamNames[kDbgMarkRoles];

static const char* kDbgLightPrefix   = "ToLissPhoton/debug/light/";
static const char* kDbgPosDataRef    = "ToLissPhoton/debug/pos";
static const char* kDbgMarkDataRef   = "ToLissPhoton/debug/marker";
static const char* kDbgMarkShowRef   = "ToLissPhoton/debug/markers";
static const char* kDbgMarkParamRef  = "ToLissPhoton/debug/markparam";
static const char* kDbgCompareRefName = "ToLissPhoton/debug/compare";

// The rheostat a light's alpha rides, keyed by the manifest's `source`.
// LIGHT_SPILL_CUSTOM has no INDEX slot, so without this every debug light would
// ignore the very knob it is identified by.
static const char* kDbgPanelRheo = "sim/cockpit2/electrical/panel_brightness_ratio";
static const char* kDbgInstRheo  = "sim/cockpit2/electrical/instrument_brightness_ratio";
static const char* kDbgMapRheo   = "ckpt/lights/map";
static const char* kDbgDURheo    = "AirbusFBW/DUBrightness";

// --- angles ------------------------------------------------------------------
// Mirrors aim_to_dir / dir_to_aim / spread_to_cone in build/photon_dsl.py, which
// is where the convention is documented. The two must agree or a number tuned
// here means something else when pasted into the DSL.
//
//   pitch  degrees above horizontal. 0 level, +90 up, -90 down.
//   yaw    0 forward (-Z), 90 right (+X), 180 aft (+Z).
static void DbgAimToDir(float pitch, float yaw, float out[3]) {
    const double p = pitch * 3.14159265358979323846 / 180.0;
    const double y = yaw   * 3.14159265358979323846 / 180.0;
    const double horiz = std::cos(p);
    out[0] = (float)(horiz * std::sin(y));
    out[1] = (float)std::sin(p);
    out[2] = (float)(-horiz * std::cos(y));
}

static void DbgDirToAim(const float d[3], float* pitch, float* yaw) {
    const double len = std::sqrt((double)d[0]*d[0] + (double)d[1]*d[1] + (double)d[2]*d[2]);
    if (len < 1e-9) { *pitch = 0.0f; *yaw = 0.0f; return; }      // omnidirectional
    const double x = d[0] / len, y = d[1] / len, z = d[2] / len;
    const double clamped = y > 1.0 ? 1.0 : (y < -1.0 ? -1.0 : y);
    *pitch = (float)(std::asin(clamped) * 180.0 / 3.14159265358979323846);
    // At the poles yaw is degenerate; report 0 rather than an arbitrary residue.
    *yaw = std::fabs(y) > 1.0 - 1e-9
         ? 0.0f
         : (float)(std::atan2(x, -z) * 180.0 / 3.14159265358979323846);
}

static float DbgConeToSpread(float cone) {
    const float c = cone > 1.0f ? 1.0f : (cone < -1.0f ? -1.0f : cone);
    return (float)(std::acos(c) * 180.0 / 3.14159265358979323846);
}
static float DbgSpreadToCone(float deg) {
    const float d = deg > 180.0f ? 180.0f : (deg < 0.0f ? 0.0f : deg);
    return (float)std::cos(d * 3.14159265358979323846 / 180.0);
}

// --- the values the sim reads ------------------------------------------------
// The four rheostats, resolved once. This runs once per light per frame — 64
// XPLMFindDataRef string lookups a frame is exactly the cost ResolveDUBrightness
// exists to avoid elsewhere in this file.
static XPLMDataRef gDbgRheoRefs[4] = { nullptr };
static bool        gDbgRheoTried   = false;
static void DbgResolveRheostats() {
    gDbgRheoTried = true;
    const char* names[4] = { kDbgPanelRheo, kDbgInstRheo, kDbgDURheo, kDbgMapRheo };
    for (int i = 0; i < 4; ++i) {
        gDbgRheoRefs[i] = XPLMFindDataRef(names[i]);
        if (!gDbgRheoRefs[i])
            Log(std::string("light editor: rheostat ") + names[i] + " NOT FOUND - "
                "lights on it sit at full brightness");
    }
}

static float DbgRheostat(const DbgLight& l) {
    if (!gDbgRheoTried) DbgResolveRheostats();
    int which = 3;                          // map (a scalar)
    bool array = true;
    if (l.source == "panel")     which = 0;
    else if (l.source == "inst") which = 1;
    else if (l.source == "du")   which = 2;
    else                         array = false;

    XPLMDataRef ref = gDbgRheoRefs[which];
    if (!ref) return 1.0f;                 // missing knob: full brightness, not dark
    float v = 1.0f;
    if (array) {
        if (XPLMGetDatavf(ref, nullptr, 0, 0) <= l.index) return 0.0f;
        if (XPLMGetDatavf(ref, &v, l.index, 1) < 1) return 0.0f;
    } else {
        double d = 0.0;
        if (!ReadDataRefTyped(ref, XPLMGetDataRefTypes(ref), d)) return 1.0f;
        v = (float)d;
    }
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// --- the slot probe ----------------------------------------------------------
// WHAT WE THINK the nine floats mean has never been safe to assume: it was read
// off the OBJ8 spec, and in-sim it did not hold — `aim Fwd` pointed a light
// straight down, editing pitch swung it horizontally, and the cone interacted
// with a direction component. Two explanations (an axis permutation, then
// non-unit `dir` lengths) each fitted part of the evidence and were wrong.
//
// So this stops guessing and MEASURES. Probing slot k sends the baseline with
// slot k alone set to 1, so whatever the sim does is caused by that slot and
// nothing else. Nine clicks map the array onto the sim's parameters with no
// inference — which is how the wire order was settled on 2026-07-30, after two
// sim restarts of hypothesizing found nothing.
//
// Baseline choices, each load-bearing:
//   dir 0 0 0   omnidirectional, so a probe of 5/6/7 introduces the ONLY
//               direction present and there is nothing to disentangle it from.
//   cone 0      cos(90 deg). If slot 8 is a direction component after all, a
//               baseline of 0 contributes nothing to it, keeping 5..7 clean.
//   size 4 m    big enough that the pool lands on real geometry rather than on
//               the panel two centimeters away — which is what made cone edits
//               look inert.
//   white, a=1  no rheostat, no palette, nothing to misread as a color bug.
static const float kDbgProbeBaseline[kDbgParamCount] =
    { 1.0f, 1.0f, 1.0f, 1.0f, 4.0f, 0.0f, 0.0f, 0.0f, 0.0f };
static const float kDbgProbeValue = 1.0f;
// What each slot SHOULD do, shown beside the probe so the answer is a comparison
// rather than a memory test.
static const char* kDbgProbeExpect[kDbgParamCount] = {
    "red only", "green only", "blue only", "alpha (already 1)", "bigger",
    "aim RIGHT (+x)", "aim UP (+y)", "aim AFT (+z)",
    "NOTHING - the baseline dir is 0 0 0, so there is no cone to narrow",
};
static int gDbgProbe = -1;               // -1 off, else the slot under test

// The nine floats a probe sends, or false when not probing. Deliberately
// bypasses EVERYTHING else — rheostat, Isolate, Blink, Mark: a probe is only
// worth anything if the values reaching X-Plane are exactly the ones on screen,
// and each of those would be a second variable in an experiment designed to have
// one. Every other light is blacked out for the same reason.
static bool DbgProbeParams(int slot, float out[kDbgParamCount]) {
    if (gDbgProbe < 0) return false;
    for (int i = 0; i < kDbgParamCount; ++i) out[i] = 0.0f;
    if (slot != gDbgSel) return true;                 // the others draw nothing
    for (int i = 0; i < kDbgParamCount; ++i) out[i] = kDbgProbeBaseline[i];
    out[gDbgProbe] = kDbgProbeValue;
    return true;
}

// The light in dataref SLOT n, or null when that slot is empty.
static DbgLight* DbgBySlot(int slot) {
    if (slot < 0 || slot >= kDbgMaxLights) return nullptr;
    const int i = gDbgSlotToIndex[slot];
    if (i < 0 || i >= (int)gDbgLights.size()) return nullptr;
    return &gDbgLights[(size_t)i];
}

// The nine floats slot n currently means, in READABLE order.
static void DbgParams(int n, float out[kDbgParamCount]) {
    if (DbgProbeParams(n, out)) return;               // probing: nothing else applies
    for (int i = 0; i < kDbgParamCount; ++i) out[i] = 0.0f;
    const DbgLight* lp = DbgBySlot(n);
    if (!lp) return;                                      // empty slot: draws nothing
    const DbgLight& l = *lp;
    for (int i = 0; i < kDbgParamCount; ++i) out[i] = l.v[i];
    // The simplified floods carry their authored size in v[4]; the shared
    // multiplier is applied here, at the wire, so the knob moves all of them.
    if (l.boost > 0.0f) out[4] *= gDbgFloodBoost;

    const bool selected = (n == gDbgSel);
    if (gDbgMark && selected) { out[0] = 1.0f; out[1] = 0.0f; out[2] = 1.0f; }

    // ⚠ A SCREEN-GLOW LIGHT IS DRIVEN ON SIZE, every other light on alpha
    // (2026-08-09) — the shipping plugin scales the DU knob into reach rather
    // than opacity, and the tuner has to do the same or a tuning pass judges a
    // look the release will not produce. Isolate and Blink stay on alpha for
    // both kinds: they are identification aids, not part of the look.
    const float rheo = DbgRheostat(l);
    const bool sizeDriven = (l.source == "du");
    if (sizeDriven) out[4] *= rheo;
    float alpha = sizeDriven ? out[3] : out[3] * rheo;
    if (gDbgIsolate && !selected) alpha = 0.0f;
    if (gDbgBlink && selected) {
        // A square wave, not a fade: a hard on/off is easier to pick out of a
        // cockpit where several lights already glow at similar levels.
        const float phase = XPLMGetElapsedTime() * 2.0f;
        alpha *= (phase - std::floor(phase)) < 0.5f ? 1.0f : 0.0f;
    }
    out[3] = alpha;
}

// ⚠ The reorder happens HERE, before the slice: X-Plane asking for [5:9] must
// get semi/dx/dy/dz, not dx/dy/dz/cone.
static int ReadDbgLight(void* refcon, float* out, int offset, int max) {
    if (!out) return kDbgParamCount;                        // size query
    if (offset < 0) offset = 0;
    float readable[kDbgParamCount];
    DbgParams((int)(intptr_t)refcon, readable);
    int n = 0;
    for (int i = 0; i < max && offset + i < kDbgParamCount; ++i) {
        out[i] = readable[kDbgWireOrder[offset + i]];
        ++n;
    }
    return n;
}

static int ReadDbgPos(void* /*refcon*/, float* out, int offset, int max) {
    const int total = kDbgMaxLights * 3;
    if (!out) return total;
    if (offset < 0) offset = 0;
    int n = 0;
    for (int i = 0; i < max && offset + i < total; ++i) {
        const int slot = offset + i;
        const DbgLight* l = DbgBySlot(slot / 3);
        out[i] = l ? l->off[slot % 3] : 0.0f;
        ++n;
    }
    return n;
}

// Where marker dot `dot` of light n sits, as an offset from the baked position.
// Three parts, and the split must match debug_mark_role in build_objs.py:
//   dot 0            the light's own origin
//   1..AxisDots      evenly along the aim, out to the reach
//   the rest         a ring on the CONE RIM at that distance, radius
//                    tan(half-spread) x reach
// The rim is what makes aim judgeable: a bare axis line was reported in-sim as
// not obviously following the light, which is what a short line looks like next
// to a pool of light a meter wide.
static void DbgMarkerOffset(int n, int dot, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    const DbgLight* lp = DbgBySlot(n);
    if (!lp) return;
    const DbgLight& l = *lp;
    out[0] = l.off[0]; out[1] = l.off[1]; out[2] = l.off[2];
    if (dot == 0) return;

    // While probing, the marker draws the PROBE's own direction slots — so the
    // arrow shows what 5/6/7 was set to and the light shows what X-Plane did
    // with it. That side by side IS the measurement.
    float probe[kDbgParamCount];
    const bool probing = DbgProbeParams(n, probe);
    const float d[3] = { probing ? probe[5] : l.v[5],
                         probing ? probe[6] : l.v[6],
                         probing ? probe[7] : l.v[7] };
    const double len = std::sqrt((double)d[0]*d[0] + (double)d[1]*d[1] + (double)d[2]*d[2]);
    if (len < 1e-9) return;                    // omnidirectional: stack on the dot
    const float axis[3] = { (float)(d[0]/len), (float)(d[1]/len), (float)(d[2]/len) };

    // Any perpendicular pair will do — the ring is symmetric about the axis.
    float up[3] = { 0.0f, 1.0f, 0.0f };
    if (std::fabs(axis[1]) > 0.9f) { up[0] = 1.0f; up[1] = 0.0f; }
    float u[3] = { axis[1]*up[2] - axis[2]*up[1],
                   axis[2]*up[0] - axis[0]*up[2],
                   axis[0]*up[1] - axis[1]*up[0] };
    const double ul = std::sqrt((double)u[0]*u[0] + (double)u[1]*u[1] + (double)u[2]*u[2]);
    if (ul < 1e-9) return;
    for (int i = 0; i < 3; ++i) u[i] = (float)(u[i] / ul);
    const float w[3] = { axis[1]*u[2] - axis[2]*u[1],
                         axis[2]*u[0] - axis[0]*u[2],
                         axis[0]*u[1] - axis[1]*u[0] };

    float add[3] = { 0.0f, 0.0f, 0.0f };
    if (dot <= kDbgMarkAxisDots) {
        const float t = gDbgMarkerReach * (float)dot / (float)kDbgMarkAxisDots;
        for (int i = 0; i < 3; ++i) add[i] = axis[i] * t;
    } else {
        // Half-spread from the cone cosine, clamped just under 90: at 90 the
        // tangent runs away and the ring would fly off to the horizon.
        float half = DbgConeToSpread(probing ? probe[8] : l.v[8]);
        if (half > 89.0f) half = 89.0f;
        const float radius = (float)std::tan(half * 3.14159265358979323846 / 180.0)
                           * gDbgMarkerReach;
        const double ang = 2.0 * 3.14159265358979323846
                         * (double)(dot - kDbgMarkAxisDots - 1) / (double)kDbgMarkRimDots;
        const float c = (float)std::cos(ang), s = (float)std::sin(ang);
        for (int i = 0; i < 3; ++i)
            add[i] = axis[i] * gDbgMarkerReach + (u[i] * c + w[i] * s) * radius;
    }
    for (int i = 0; i < 3; ++i) {
        float value = out[i] + add[i];
        if (value >  kDbgPosRange) value =  kDbgPosRange;    // the ANIM keyframe span
        if (value < -kDbgPosRange) value = -kDbgPosRange;
        out[i] = value;
    }
}

// How many dots the INSTALLED OBJ baked per light. It strides this array, so
// reading it from the manifest rather than assuming the constant means a stale
// OBJ drives its own dots instead of a neighbor's. Clamped to the constant: the
// array is sized from that and a larger stride would run off the end.
static int gDbgMarkStride = kDbgMarkDots;

static int ReadDbgMarker(void* /*refcon*/, float* out, int offset, int max) {
    const int total = kDbgMaxLights * kDbgMarkDots * 3;
    if (!out) return total;
    if (offset < 0) offset = 0;
    const int stride = gDbgMarkStride;
    int n = 0;
    for (int i = 0; i < max && offset + i < total; ++i) {
        const int slot = offset + i;
        const int light = slot / (stride * 3);
        const int dot   = (slot / 3) % stride;
        float xyz[3];
        DbgMarkerOffset(light, dot, xyz);
        out[i] = xyz[slot % 3];
        ++n;
    }
    return n;
}

// One color per part, because the whole point is to see WHICH END is the light
// and where the beam edge falls: green origin, amber axis, blue cone rim.
static int ReadDbgMarkParam(void* refcon, float* out, int offset, int max) {
    if (!out) return kDbgParamCount;
    if (offset < 0) offset = 0;
    static const float kRole[kDbgMarkRoles][4] = {
        { 0.2f, 1.0f, 0.3f, 1.0f },      // origin — the light itself
        { 1.0f, 0.6f, 0.1f, 1.0f },      // axis   — which way it points
        { 0.3f, 0.6f, 1.0f, 1.0f },      // rim    — the cone edge
    };
    const int role = (int)(intptr_t)refcon;
    // r g b a size, then the light-texture region the OBJ bakes.
    const float p[kDbgParamCount] = {
        kRole[role][0], kRole[role][1], kRole[role][2], kRole[role][3],
        gDbgMarkerSize, 0.0f, 0.0f, 0.5f, 0.5f
    };
    int n = 0;
    for (int i = 0; i < max && offset + i < kDbgParamCount; ++i) { out[i] = p[offset + i]; ++n; }
    return n;
}

static int   ReadDbgMarkShowInt(void*)   { return gDbgShowMarkers; }
static float ReadDbgMarkShowFloat(void*) { return (float)gDbgShowMarkers; }
static void  WriteDbgMarkShowInt(void*, int v)     { gDbgShowMarkers = v; }
static void  WriteDbgMarkShowFloat(void*, float v) { gDbgShowMarkers = (int)v; }
static int   ReadDbgCompareInt(void*)    { return gDbgCompare; }
static float ReadDbgCompareFloat(void*)  { return (float)gDbgCompare; }
static void  WriteDbgCompareInt(void*, int v)      { gDbgCompare = v ? 1 : 0; }
static void  WriteDbgCompareFloat(void*, float v)  { gDbgCompare = v >= 0.5f ? 1 : 0; }

// --- the manifest ------------------------------------------------------------
// One per debug-capable target. Slots come from per-target bases
// (DEBUG_SLOT_BASE in build_objs.py: interior from 0, screens from 48), so BOTH
// debug OBJs can be installed and driven at once — every manifest found is
// merged into one list (2026-08-09; before the bases, both numbered from 0 and
// the newest manifest won). A per-slot collision can now only come from a stale
// pre-base OBJ; the duplicate-slot log below still catches it.
static void DbgLoadManifest() {
    gDbgLights.clear();
    gDbgManifest.clear();
    gDbgTarget.clear();
    gDbgPosTunable = false;
    gDbgMarkerDots = 0;
    gDbgMarkStride = kDbgMarkDots;
    gDbgSel = -1;
    gDbgScanned = true;
    gDbgFloodBoost = gDbgFloodBoostSeed = 1.0f;
    for (int i = 0; i < kDbgMaxLights; ++i) gDbgSlotToIndex[i] = -1;
    // The aircraft may have changed, and ToLiss's datarefs go with its plugin.
    gDbgRheoTried = false;
    for (int i = 0; i < 4; ++i) gDbgRheoRefs[i] = nullptr;

    char fileName[512] = {0}, path[512] = {0};
    XPLMGetNthAircraftModel(0, fileName, path);
    if (!path[0]) { gDbgNote = "no aircraft loaded"; return; }
    const fs::path objs = fs::path(path).parent_path() / "objects";

    struct Candidate { const char* label; const char* file; };
    static const Candidate kCandidates[] = {
        { "cockpit", "lights_inn.debug.json" },
        { "screens", "lights_screens.debug.json" },
    };
    for (int c = 0; c < 2; ++c) {
        const fs::path p = objs / kCandidates[c].file;
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        std::ifstream f(p, std::ios::binary);
        if (!f) { Log("light editor: manifest unreadable: " + p.string()); continue; }
        std::stringstream ss; ss << f.rdbuf();
        json::Value root;
        json::Parser parser(ss.str());
        if (!parser.value(root) || root.type != json::Value::Obj) {
            Log("light editor: could not parse " + p.string());
            continue;
        }
        if (!gDbgManifest.empty()) gDbgManifest += " + ";
        gDbgManifest += p.filename().string();
        if (!gDbgTarget.empty()) gDbgTarget += "+";
        gDbgTarget += kCandidates[c].label;

        bool posTunable = false;
        if (const json::Value* v = root.find("pos_tunable")) posTunable = v->i != 0;
        gDbgPosTunable = gDbgPosTunable || posTunable;
        int markerDots = 0;
        if (const json::Value* v = root.find("marker_dots")) markerDots = (int)v->i;
        if (markerDots > 0) {
            if (gDbgMarkerDots == 0) gDbgMarkerDots = markerDots;
            else if (gDbgMarkerDots != markerDots)
                Log("light editor: the two manifests disagree on marker_dots ("
                    + std::to_string(gDbgMarkerDots) + " vs "
                    + std::to_string(markerDots) + ") - the marker array is strided "
                    "by the first, so the other OBJ's markers land on the wrong "
                    "lights. Rebuild both debug OBJs from one tree.");
        }

        const json::Value* lights = root.find("lights");
        if (!lights || lights->type != json::Value::Arr) continue;
        for (size_t i = 0; i < lights->arr.size(); ++i) {
            const json::Value& e = lights->arr[i];
            if (e.type != json::Value::Obj) continue;
            DbgLight l;
            l.n = (int)gDbgLights.size();
            l.posTunable = posTunable;
            if (const json::Value* v = e.find("n"))        l.n = (int)v->i;
            if (const json::Value* v = e.find("name"))     l.name = v->s;
            if (const json::Value* v = e.find("fixture"))  l.fixture = v->s;
            if (const json::Value* v = e.find("category")) l.category = v->s;
            if (const json::Value* v = e.find("class"))    l.cls = v->s;
            if (const json::Value* v = e.find("source"))   l.source = v->s;
            if (const json::Value* v = e.find("index"))    l.index = (int)v->i;
            if (const json::Value* v = e.find("boost"))    l.boost = (float)v->num();
            if (l.boost > 0.0f) gDbgFloodBoostSeed = l.boost;
            if (const json::Value* v = e.find("pos"))
                for (int k = 0; k < 3; ++k)
                    if (const json::Value* c = v->at((size_t)k)) l.pos[k] = (float)c->num();
            if (const json::Value* v = e.find("rgb"))
                for (int k = 0; k < 3; ++k)
                    if (const json::Value* c = v->at((size_t)k)) l.v[k] = (float)c->num();
            l.v[3] = 1.0f;                                // alpha: the rheostat scales it
            if (const json::Value* v = e.find("size")) l.v[4] = (float)v->num(1.0);
            if (const json::Value* v = e.find("dir"))
                for (int k = 0; k < 3; ++k)
                    if (const json::Value* c = v->at((size_t)k)) l.v[5 + k] = (float)c->num();
            l.v[8] = 1.0f;
            if (const json::Value* v = e.find("cone")) l.v[8] = (float)v->num(1.0);

            DbgDirToAim(&l.v[5], &l.pitch, &l.yaw);
            l.basePitch = l.pitch; l.baseYaw = l.yaw;
            for (int k = 0; k < kDbgParamCount; ++k) l.base[k] = l.v[k];
            gDbgLights.push_back(l);
        }
    }
    if (gDbgManifest.empty()) {
        gDbgNote = "no debug OBJ installed";
        return;
    }
    gDbgFloodBoost = gDbgFloodBoostSeed;
    if (gDbgMarkerDots > 0 && gDbgMarkerDots != kDbgMarkDots) {
        Log("light editor: the installed OBJ bakes " + std::to_string(gDbgMarkerDots)
            + " marker dots per light but this build assumes "
            + std::to_string(kDbgMarkDots)
            + " - the marker array is strided by the OBJ's number. If it is LARGER, "
              "the tail is dropped: rebuild the OBJ, or raise kDbgMarkAxisDots/"
              "kDbgMarkRimDots here to match DEBUG_MARK_* in build_objs.py.");
    }
    gDbgMarkStride = (gDbgMarkerDots > 0 && gDbgMarkerDots <= kDbgMarkDots)
                   ? gDbgMarkerDots : kDbgMarkDots;
    if (gDbgLights.size() > (size_t)kDbgMaxLights) {
        Log("light editor: manifest holds " + std::to_string(gDbgLights.size())
            + " lights but only " + std::to_string(kDbgMaxLights)
            + " dataref slots exist - the tail will not light. Raise kDbgMaxLights "
              "here AND DEBUG_MAX_LIGHTS in build/build_objs.py.");
        gDbgLights.resize((size_t)kDbgMaxLights);
    }
    for (size_t i = 0; i < gDbgLights.size(); ++i) {
        const int slot = gDbgLights[i].n;
        if (slot < 0 || slot >= kDbgMaxLights) continue;
        if (gDbgSlotToIndex[slot] >= 0)
            Log("light editor: two lights claim dataref slot " + std::to_string(slot)
                + " - the later one wins and the earlier is unreachable");
        gDbgSlotToIndex[slot] = (int)i;
    }
    if (!gDbgLights.empty()) gDbgSel = gDbgLights[0].n;
    gDbgNote = std::to_string(gDbgLights.size()) + " lights from " + gDbgManifest;
    Log("light editor: " + gDbgNote + (gDbgPosTunable ? " (position tunable)" : ""));
    // ⚠ LAST, and only here. A saved session is a set of EDITS, so every light
    // has to be seeded from the manifest first — those baked values are what the
    // records are checked against before being applied. It also overwrites
    // gDbgNote, deliberately: "restored 6 tuned lights" is the more surprising
    // half of what just happened.
    DbgLoadTuning();
}

// Registered at XPluginStart, before any OBJ loads: an OBJ binds dataref NAMES at
// load time, and a name that does not exist yet reads 0 forever — which leaves
// every debug light black with nothing in the log to say why.
static void RegisterDbgLightRefs() {
    if (XPLMFindDataRef((std::string(kDbgLightPrefix) + "0").c_str())) {
        gDbgOwnsRefs = false;
        Log("light editor: ToLissPhoton/debug/light/0 is ALREADY registered - "
            "PI_PhotonDevReload.py (XPPython3) owns the debug lights in this "
            "session, so this editor stands down. Remove that script to drive them "
            "from here.");
        return;
    }
    gDbgOwnsRefs = true;
    for (int n = 0; n < kDbgMaxLights; ++n) {
        gDbgLightNames[n] = std::string(kDbgLightPrefix) + std::to_string(n);
        gDbgLightRefs[n] = XPLMRegisterDataAccessor(
            gDbgLightNames[n].c_str(), xplmType_FloatArray, 0,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, ReadDbgLight, nullptr, nullptr, nullptr,
            (void*)(intptr_t)n, nullptr);
    }
    gDbgPosRef = XPLMRegisterDataAccessor(
        kDbgPosDataRef, xplmType_FloatArray, 0,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, ReadDbgPos, nullptr, nullptr, nullptr, nullptr, nullptr);
    gDbgMarkRef = XPLMRegisterDataAccessor(
        kDbgMarkDataRef, xplmType_FloatArray, 0,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, ReadDbgMarker, nullptr, nullptr, nullptr, nullptr, nullptr);
    gDbgMarkShowRef = XPLMRegisterDataAccessor(
        kDbgMarkShowRef, xplmType_Int | xplmType_Float, 1,
        ReadDbgMarkShowInt, WriteDbgMarkShowInt,
        ReadDbgMarkShowFloat, WriteDbgMarkShowFloat,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    gDbgCompareRef = XPLMRegisterDataAccessor(
        kDbgCompareRefName, xplmType_Int | xplmType_Float, 1,
        ReadDbgCompareInt, WriteDbgCompareInt,
        ReadDbgCompareFloat, WriteDbgCompareFloat,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    for (int k = 0; k < kDbgMarkRoles; ++k) {
        gDbgMarkParamNames[k] = std::string(kDbgMarkParamRef) + "/" + std::to_string(k);
        gDbgMarkParamRefs[k] = XPLMRegisterDataAccessor(
            gDbgMarkParamNames[k].c_str(), xplmType_FloatArray, 0,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, ReadDbgMarkParam, nullptr, nullptr, nullptr,
            (void*)(intptr_t)k, nullptr);
    }
    Log("light editor: registered " + std::to_string(kDbgMaxLights)
        + " debug light slots + pos/marker/compare");
}

// ⚠ The exact mirror of the above, and it MUST exist: the registration runs from
// StartPanelPass, i.e. from XPluginEnable, so it runs AGAIN every time the user
// ticks us off and back on in Plugin Admin. Without this, the second pass finds
// ToLissPhoton/debug/light/0 already registered — BY US — concludes that
// PI_PhotonDevReload.py owns the pool, sets gDbgOwnsRefs = false and stands the
// editor down for the rest of the session. The symptom is the whole
// ToLissPhoton/debug/* family going inert after one disable/enable cycle, with a
// log line blaming a script that may not even be installed.
//
// It also stops 71 accessors leaking per cycle, and stops them outliving the
// plugin: XPluginDisable is guaranteed to run before XPluginStop, so this is the
// unload path too.
//
// The cost is that the debug names do not exist while we are disabled, so a debug
// OBJ that loads in that window binds nothing. That is inherent to registering
// from Enable at all, and the pool cannot move to XPluginStart without flipping
// the ownership race: PI_PhotonDevReload.py registers from ITS XPluginStart, and
// being reliably later is what makes the standoff decidable.
static void UnregisterDbgLightRefs() {
    if (!gDbgOwnsRefs) return;              // we never owned them; nothing to undo
    for (int n = 0; n < kDbgMaxLights; ++n)
        if (gDbgLightRefs[n]) { XPLMUnregisterDataAccessor(gDbgLightRefs[n]); gDbgLightRefs[n] = nullptr; }
    for (int k = 0; k < kDbgMarkRoles; ++k)
        if (gDbgMarkParamRefs[k]) { XPLMUnregisterDataAccessor(gDbgMarkParamRefs[k]); gDbgMarkParamRefs[k] = nullptr; }
    if (gDbgPosRef)      { XPLMUnregisterDataAccessor(gDbgPosRef);      gDbgPosRef = nullptr; }
    if (gDbgMarkRef)     { XPLMUnregisterDataAccessor(gDbgMarkRef);     gDbgMarkRef = nullptr; }
    if (gDbgMarkShowRef) { XPLMUnregisterDataAccessor(gDbgMarkShowRef); gDbgMarkShowRef = nullptr; }
    if (gDbgCompareRef)  { XPLMUnregisterDataAccessor(gDbgCompareRef);  gDbgCompareRef = nullptr; }
    gDbgOwnsRefs = false;
    Log("light editor: released the debug light slots");
}
#endif  // PHOTON_DEV — the live light editor

// PhotonImgui has no access to Log(); hand it one so backend diagnostics and
// any ImGui assertion land in Log.txt with everything else.
static void ImguiLogBridge(const char* msg) { Log(std::string(msg)); }

// Bring the panel pass up: resolve the datarefs it reads, register the draw
// callback, and load the FX layer definition. In a dev build, additionally
// register the probe's knobs and the light editor's slots.
//
// ⚠ Called from XPluginEnable, and the light-editor registration inside it MUST
// stay there: a debug OBJ binds those dataref names when the aircraft loads, and
// a name that does not exist by then reads 0 for the rest of the session.
static void StartPanelPass() {
    gPanelRenderTypeRef = XPLMFindDataRef(kPanelRenderType);
    // ⚠ Only a FIRST attempt. This runs from XPluginEnable, which on a stock
    // install is before ToLiss's plugin exists, so a miss here is expected and
    // says nothing — UpdateMenuVisibility rebinds it per aircraft and AutoLoop
    // retries at 1 Hz. It used to be the one and only attempt; see the resolver.
    ResolveDisplayFallback();

    RegisterPanelPass();
    PhotonImgui::SetLogger(ImguiLogBridge);
    LoadPanelFx();

#if PHOTON_DEV
    gPanelExtentRefs[0] = XPLMFindDataRef("sim/graphics/view/panel_total_pnl_l");
    gPanelExtentRefs[1] = XPLMFindDataRef("sim/graphics/view/panel_total_pnl_b");
    gPanelExtentRefs[2] = XPLMFindDataRef("sim/graphics/view/panel_total_pnl_r");
    gPanelExtentRefs[3] = XPLMFindDataRef("sim/graphics/view/panel_total_pnl_t");
    for (int i = 0; i < 4; ++i)
        if (!gPanelExtentRefs[i]) { Log("panel probe: MISSING panel_total_pnl_* dataref"); return; }

    gPanelProbeModeRef = XPLMRegisterDataAccessor(
        kPanelProbeMode, xplmType_Int, 1,
        ReadPanelProbeMode, WritePanelProbeMode, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    gPanelProbeRectRef = XPLMRegisterDataAccessor(
        kPanelProbeRect, xplmType_Int, 1,
        ReadPanelProbeRect, WritePanelProbeRect, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    gPanelProbeDrawRef = XPLMRegisterDataAccessor(
        kPanelProbeDraw, xplmType_Int, 1,
        ReadPanelProbeDraw, WritePanelProbeDraw, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    gPanelProbeTypeRef = XPLMRegisterDataAccessor(
        kPanelProbeType, xplmType_Int, 1,
        ReadPanelProbeType, WritePanelProbeType, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    const char* tintNames[3] = { kPanelTintR, kPanelTintG, kPanelTintB };
    for (int i = 0; i < 3; ++i)
        gPanelTintRefs[i] = XPLMRegisterDataAccessor(
            tintNames[i], xplmType_Float, 1,
            nullptr, nullptr, ReadPanelTint, WritePanelTint,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            (void*)(intptr_t)i, (void*)(intptr_t)i);
    gPanelProbeCycleCmd = XPLMCreateCommand(kPanelProbeCycle, "Photon: cycle panel-FBO probe mode");
    if (gPanelProbeCycleCmd)
        XPLMRegisterCommandHandler(gPanelProbeCycleCmd, PanelProbeCycleHandler, 1, nullptr);

    RegisterDbgLightRefs();
    // After registration so a restored "mode" takes effect immediately, and after
    // the accessors exist so anything watching the datarefs sees the same values.
    BuildDbgRectOpts();
    LoadPanelDebug();
    // Probe builds only, and only until the ImGui backend is signed off in-sim:
    // verbose first-frame logging plus the two GL sanity markers.
    PhotonImgui::SetDiagnostics(true);
    LoadDevPrefs();
    Log("dev build: Plugins > ToLiss Photon > Dev > Dev window... holds every knob "
        "- profiles, the panel probe, the FX compositor with its history, the "
        "repo build commands and this log. Settings persist across reloads.");
    Log("panel probe ACTIVE (mode 1 = after Gauges; rect -2 = FLOOD; draw method 0 "
        "= Clear). Walk " + std::string(kPanelProbeDraw) + " 0->1->2 FIRST — the "
        "framebuffer is already known good, so the draw path is what is in doubt. "
        "Then " + std::string(kPanelProbeRect) + " -1 for all 6 DU rects, 0 for the "
        "capt PFD alone.");
    if (FcuRowTarget() < 0)
        Log("panel probe: no group named '" + std::string(kFcuRowName)
            + "' in the table — the FCU tint has no target on this build");
    Log("panel probe: FCU TINT — set " + std::string(kPanelProbeRect) + " = "
        + std::to_string(FcuRowTarget()) + " (FCU row: strip + both baro windows), then walk "
        + std::string(kPanelProbeType) + " 1 -> 2 -> -1 with draw method "
        + std::to_string(kDrawImmediate) + " to find the pass whose magenta REPLACES "
        "the digits. Then draw method " + std::to_string(kDrawMultiply)
        + " and tune " + std::string(kPanelTintR) + "/_g/_b. "
        "See docs/fcu_tint_plan.md §5.");
#endif  // PHOTON_DEV — the probe's knobs and the light editor's slots
}

static void StopPanelPass() {
    UnregisterPanelPass();
    DestroyDebugImguiWindows();
#if PHOTON_DEV
    // ⚠ Pairs with the RegisterDbgLightRefs in StartPanelPass. Everything this
    // function undoes is registered from XPluginEnable, so every line here has to
    // be symmetric or a Plugin Admin disable/enable leaves a duplicate behind —
    // see UnregisterDbgLightRefs for the one that latched.
    UnregisterDbgLightRefs();
    if (gPanelProbeCycleCmd) {
        XPLMUnregisterCommandHandler(gPanelProbeCycleCmd, PanelProbeCycleHandler, 1, nullptr);
        gPanelProbeCycleCmd = nullptr;
    }
    if (gPanelProbeModeRef) { XPLMUnregisterDataAccessor(gPanelProbeModeRef); gPanelProbeModeRef = nullptr; }
    if (gPanelProbeRectRef) { XPLMUnregisterDataAccessor(gPanelProbeRectRef); gPanelProbeRectRef = nullptr; }
    if (gPanelProbeDrawRef) { XPLMUnregisterDataAccessor(gPanelProbeDrawRef); gPanelProbeDrawRef = nullptr; }
    if (gPanelProbeTypeRef) { XPLMUnregisterDataAccessor(gPanelProbeTypeRef); gPanelProbeTypeRef = nullptr; }
    for (int i = 0; i < 3; ++i)
        if (gPanelTintRefs[i]) { XPLMUnregisterDataAccessor(gPanelTintRefs[i]); gPanelTintRefs[i] = nullptr; }
#endif
}

static void ReseatPanelPass() {   // push ourselves to the tail of the callback list
    if (!gPanelProbeRegistered) return;
    UnregisterPanelPass();
    RegisterPanelPass();
    Log("panel pass: re-registered after aircraft load (moves us later in the phase)");
}

// ======================== dev tooling: the Dev window ========================
// One window, tabbed, holding every knob this plugin can drive. It exists
// because the measurement loop is the one place this project cannot afford
// friction: every wrong conclusion in docs/fcu_tint_plan.md's history came from
// reasoning instead of looking, and looking used to mean typing six dataref
// paths into DataRefEditor after every reload.
//
// DEV-ONLY, structurally. All of it is inside #if PHOTON_DEV, and the menu code
// calls AppendDebugMenu unconditionally because the shipping build gets the
// no-op declared further up. A release therefore has no Dev row, no window, and
// never writes the dev prefs files.
//
// ⚠ The Panel FX PERSISTENCE block below is deliberately OUTSIDE this guard — a
// shipping build reads the layer definition, it just has no editor for it. The
// guard closes before it and reopens after.
#if PHOTON_DEV

struct DbgOpt { int value; const char* label; };

static const DbgOpt kDbgModeOpts[] = {
    { kProbeOff,          "Off" },
    { kProbeAfterGauges,  "After Gauges" },
    { kProbeAfterPanel,   "After Panel" },
    { kProbeBeforeGauges, "Before Gauges" },
};
static const DbgOpt kDbgPassOpts[] = {
    { -1, "Both" }, { 1, "Non-lit (1)" }, { 2, "Lit (2)" },
};
static const DbgOpt kDbgDrawOpts[] = {
    { kDrawClear,     "Clear" },     { kDrawImmediate, "Immediate" },
    { kDrawForced,    "Forced" },    { kDrawMultiply,  "Multiply" },
};

// Built from the live rect table rather than written out, so re-deriving
// kPanelRectsHi cannot leave the window offering a name that no longer
// exists — or, worse, quietly omitting a new one.
static DbgOpt gDbgRectOpts[2 + 32];
static int    gDbgRectOptCount = 0;
static void BuildDbgRectOpts() {
    const int cap = (int)(sizeof(gDbgRectOpts) / sizeof(gDbgRectOpts[0]));
    gDbgRectOptCount = 0;
    gDbgRectOpts[gDbgRectOptCount++] = { kProbeFloodRect, "Flood" };
    gDbgRectOpts[gDbgRectOptCount++] = { -1, "All" };
    // Groups first: they are the useful targets.
    for (int g = 0; g < kPanelGroupCount && gDbgRectOptCount < cap; ++g)
        gDbgRectOpts[gDbgRectOptCount++] = { kPanelRectCount + g, kPanelGroups[g].name };
    for (int i = 0; i < kPanelRectCount && gDbgRectOptCount < cap; ++i)
        gDbgRectOpts[gDbgRectOptCount++] = { i, kPanelRectsHi[i].name };
}

// ---- persistence -----------------------------------------------------------
// Its own file, NOT the livery prefs: this is throwaway dev state on a knob that
// does not ship, and it must never end up in a user's profile entry. Plain
// key/value text rather than JSON because nothing reads it but this block.
static fs::path PanelDebugFilePath() {
    char root[512] = {0};
    XPLMGetSystemPath(root);
    return fs::path(root) / "Output" / "preferences" / "ToLissPhoton_paneldebug.txt";
}

static void SavePanelDebug() {
    std::error_code ec;
    fs::path p = PanelDebugFilePath();
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "tint %.4f %.4f %.4f\n",
                  gPanelTint[0], gPanelTint[1], gPanelTint[2]);
    f << "mode " << gPanelProbeMode       << "\n"
      << "rect " << gPanelProbeRect       << "\n"
      << "pass " << gPanelProbeType       << "\n"
      << "draw " << gPanelProbeDrawMethod << "\n"
      << buf;
}

// Restored through the same Write* functions the datarefs use, so a hand-edited
// or stale file cannot introduce a value the dataref path would have rejected.
static void LoadPanelDebug() {
    std::ifstream f(PanelDebugFilePath(), std::ios::binary);
    if (!f) return;
    std::string key;
    while (f >> key) {
        int i = 0; float r = 0, g = 0, b = 0;
        if      (key == "mode" && (f >> i)) WritePanelProbeMode(nullptr, i);
        else if (key == "rect" && (f >> i)) WritePanelProbeRect(nullptr, i);
        else if (key == "pass" && (f >> i)) WritePanelProbeType(nullptr, i);
        else if (key == "draw" && (f >> i)) WritePanelProbeDraw(nullptr, i);
        else if (key == "tint" && (f >> r >> g >> b)) {
            WritePanelTint((void*)(intptr_t)0, r);
            WritePanelTint((void*)(intptr_t)1, g);
            WritePanelTint((void*)(intptr_t)2, b);
        } else {
            std::string rest;
            std::getline(f, rest);       // unknown key: skip its line, keep parsing
        }
    }
    Log("panel debug: restored settings from " + PanelDebugFilePath().string()
        + " — the probe may already be drawing");
}

// One click to the state the FCU tint is tuned in. Pass 2 is not a guess: pass 1
// draws nothing on this rect and pass 2 replaces the digits, measured in-sim
// 2026-07-31, so the lit pass is where compositing has to happen.
static void ApplyFcuPreset() {
    WritePanelProbeMode(nullptr, kProbeAfterGauges);
    WritePanelProbeType(nullptr, 2);
    WritePanelProbeDraw(nullptr, kDrawMultiply);
    const int target = FcuRowTarget();
    if (target >= 0) WritePanelProbeRect(nullptr, target);
    else Log("panel probe: FCU preset has no target to aim at — rect left as-is");
    Log("panel probe: FCU tint preset (after Gauges, LIT pass, Multiply, FCU row)");
}

// ---- the Probe tab ---------------------------------------------------------
// Plain-English restatement of the knobs, so the tab answers "what is on screen
// right now" without the reader recomposing it from four selected buttons.
// Reading this back is how a mis-set knob gets caught before it is mistaken for
// a finding.
static std::string DbgSummary() {
    if (gPanelProbeMode == kProbeOff) return "Probe OFF - nothing is drawn.";
    std::string s = "Drawing ";
    s += gPanelProbeMode == kProbeAfterGauges ? "after Gauges"
       : gPanelProbeMode == kProbeAfterPanel  ? "after Panel"
                                              : "before Gauges (ToLiss paints over this)";
    s += "  |  pass ";
    s += gPanelProbeType == 1 ? "1 non-lit" : gPanelProbeType == 2 ? "2 LIT" : "both";
    s += "  |  ";
    s += kDrawMethodName[gPanelProbeDrawMethod];
    s += "  |  ";
    s += PanelTargetName(gPanelProbeRect);
    return s;
}

// A row of radio buttons over an option table, writing through the dataref
// setter so a click cannot introduce a value the dataref path would reject.
//
// ⚠ PushID(label) around the row, for the same reason DisplayToggleRow has one:
// three of these are built back to back in BuildProbeTab, an ImGui item's ID is
// its LABEL TEXT hashed with the ID stack, and the option labels come from three
// separate tables that nothing keeps distinct from each other. They happen not to
// collide today; the day someone adds an "Off" to the Draw row, ImGui raises "2
// visible items with conflicting ID" and clicking one row's button lights up the
// other's. Scoping the row costs one line and removes the whole class.
typedef void (*DbgWriter)(void*, int);
static bool DbgOptionRow(const char* label, const DbgOpt* opts, int n,
                         int cur, DbgWriter write) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    for (int i = 0; i < n; ++i) {
        ImGui::SameLine();
        if (UiRadioButton(opts[i].label, cur == opts[i].value)) {
            write(nullptr, opts[i].value);
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

// ---- the circuit-breaker index finder ---------------------------------------
// kPanelBreakers is empty because NOTHING names the 363 elements of
// AirbusFBW/CBArray — not the OBJ, not ToLiss's binary, not the manuals. This
// turns "which index is the captain's PFD breaker" from a research problem into
// one pull of the breaker: arm the watch, pull it, read the index.
//
// Deliberately not a survey or a heuristic. The project's own rule from the
// custom-light slot rotation applies exactly: probe one slot at a time before
// hypothesizing. Three plausible theories cost a session there; a diff costs
// nothing and cannot be wrong.
static std::vector<float> gCbWatchBase;      // empty = not armed
static std::string        gCbWatchLog;       // most recent changes, newest first
static int                gCbWatchCount = 0;

// Both int and float arrays, because which one ToLiss registered is not known
// and reading with the wrong getter returns 0 for every element — which would
// look exactly like "every breaker is pulled".
static int ReadCbArray(std::vector<float>& out) {
    XPLMDataRef ref = XPLMFindDataRef(kFxCbArray);
    if (!ref) return 0;
    const XPLMDataTypeID t = XPLMGetDataRefTypes(ref);
    if (t & xplmType_FloatArray) {
        const int n = XPLMGetDatavf(ref, nullptr, 0, 0);
        if (n <= 0) return 0;
        out.assign((size_t)n, 0.0f);
        return XPLMGetDatavf(ref, out.data(), 0, n);
    }
    if (t & xplmType_IntArray) {
        const int n = XPLMGetDatavi(ref, nullptr, 0, 0);
        if (n <= 0) return 0;
        std::vector<int> tmp((size_t)n, 0);
        const int got = XPLMGetDatavi(ref, tmp.data(), 0, n);
        out.assign((size_t)n, 0.0f);
        for (size_t k = 0; k < tmp.size(); ++k) out[k] = (float)tmp[k];   // explicit: no C4244
        return got;
    }
    return 0;
}

static void BuildCbWatch() {
    ImGui::SeparatorText("CB watch");
    UiHint("Nothing names the elements of %s. Arm this, pull one breaker in the "
           "cockpit, and it tells you the index - which is what a kPanelBreakers "
           "row needs.", kFxCbArray);

    std::vector<float> now;
    const int got = ReadCbArray(now);
    if (got <= 0) {
        ImGui::TextDisabled("%s is not readable (no ToLiss aircraft loaded?)", kFxCbArray);
        return;
    }

    if (ImGui::Button(gCbWatchBase.empty() ? "Arm" : "Re-arm")) {
        gCbWatchBase = now;
        gCbWatchCount = got;
        gCbWatchLog.clear();
        Log("cb watch: armed on " + std::to_string(got) + " breakers");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) { gCbWatchBase.clear(); gCbWatchLog.clear(); }
    ImGui::SameLine();
    ImGui::Text("%d breakers", got);

    // Re-baseline on a length change rather than diffing across it: a different
    // aircraft has a different array and every index would "change".
    if (!gCbWatchBase.empty() && (int)gCbWatchBase.size() != got) {
        gCbWatchBase = now;
        gCbWatchCount = got;
        gCbWatchLog.clear();
    }
    if (gCbWatchBase.empty()) {
        ImGui::TextDisabled("not armed");
        return;
    }

    for (int i = 0; i < got && i < (int)gCbWatchBase.size(); ++i) {
        if (gCbWatchBase[i] == now[i]) continue;
        char line[96];
        std::snprintf(line, sizeof(line), "[%d]  %.0f -> %.0f  (%s)",
                      i, gCbWatchBase[i], now[i],
                      now[i] > 0.5f ? "pushed in" : "PULLED");
        gCbWatchLog.insert(0, std::string(line) + "\n");
        gCbWatchBase[i] = now[i];
        Log(std::string("cb watch: ") + line);
    }
    if (gCbWatchLog.empty()) ImGui::TextDisabled("armed - no breaker has moved yet");
    else                     ImGui::TextUnformatted(gCbWatchLog.c_str());
}

// The two electrical readings the gates run on, and the DCDU levels Photon is
// inferring. All three are unreadable from anywhere else at the moment they
// matter, which is what makes a pane worth the space: kDcduLevelMax and
// kDcduLevelDefault are ASSUMPTIONS, and this is where they get checked.
static void BuildElectricalWatch() {
    ImGui::SeparatorText("Display power");
    // ⚠ NAME WHICH FAULT. "not bound" covered three of them at once and cost a
    // round trip: the handle being null (ToLiss's plugin had not started when we
    // resolved, and the 1 Hz retry was missing), the handle being fine but the
    // TYPE query failing (so the count is 0), and the index being past the end.
    // They need different fixes, and this pane is the only place any of them is
    // visible. Same rule as the FX table's "x0.00 power" vs "x0.00 bright".
    if (!gDcBusRef) {
        ImGui::TextDisabled("%s NOT FOUND - the dataref does not exist yet.", kDcBusRef);
        ImGui::TextDisabled("Retried once a second from AutoLoop; if it never "
                            "appears, ToLiss does not publish it on this aircraft.");
    } else if (gDcBusCount <= 0) {
        ImGui::TextDisabled("%s is bound but reports NO ELEMENTS (type mask %d).",
                            kDcBusRef, (int)gDcBusTypes);
        ImGui::TextDisabled("Neither a float nor an int array - read with the "
                            "wrong getter it would silently return 0.");
    } else if (kDcBusIndex >= gDcBusCount) {
        ImGui::TextDisabled("%s holds %d buses; kDcBusIndex is %d - past the end.",
                            kDcBusRef, gDcBusCount, kDcBusIndex);
    } else {
        float v = 0.0f;
        if (gDcBusTypes & xplmType_FloatArray) {
            XPLMGetDatavf(gDcBusRef, &v, kDcBusIndex, 1);
        } else {
            int iv = 0;
            XPLMGetDatavi(gDcBusRef, &iv, kDcBusIndex, 1);
            v = (float)iv;
        }
        ImGui::Text("%s[%d] = %.1f V  ->  %s", kDcBusRef, kDcBusIndex, v,
                    v > kDcBusLiveVolts ? "LIVE" : "DEAD (screens + spill off)");
        // The whole array, because kDcBusIndex is a choice and this is the only
        // place the alternatives are visible. The DUs split across DC ESS / DC 2
        // / DC BAT on the real aircraft, so if a partial failure ought to kill
        // only some screens, the answer is one of the other columns here.
        std::string all;
        for (int i = 0; i < gDcBusCount && i < 16; ++i) {
            float e = 0.0f;
            if (gDcBusTypes & xplmType_FloatArray) XPLMGetDatavf(gDcBusRef, &e, i, 1);
            else { int ie = 0; XPLMGetDatavi(gDcBusRef, &ie, i, 1); e = (float)ie; }
            char b[32];
            std::snprintf(b, sizeof(b), "%s[%d] %.0f", i ? "  " : "", i, e);
            all += b;
        }
        ImGui::TextDisabled("%s", all.c_str());
    }
    ImGui::Text("DCDU level  capt %d   FO %d   (of %d, assumed; default %d)",
                gDcduLevel[0], gDcduLevel[1], kDcduLevelMax, kDcduLevelDefault);
    UiHint("Press the DCDU brightness buttons and watch these. If they stop "
           "moving before the screen does, kDcduLevelMax is wrong; if the tint "
           "starts out at the wrong brightness, kDcduLevelDefault is.");

    // ⚠ The count is shown SEPARATELY from the levels, because "not bound" and
    // "bound but never fires" produce the identical symptom — a level that sits
    // at 8 forever — and want opposite fixes. Without this row the first one
    // costs a session to tell from the second.
    if (gDcduBound == kDcduCommandCount) {
        ImGui::TextDisabled("Watching all %d CPDLC brightness commands. If the "
                            "levels above still do not move when you press the "
                            "buttons, the handler is bound and not firing.",
                            kDcduCommandCount);
    } else {
        ImGui::TextDisabled("Watching %d of %d CPDLC brightness commands - "
                            "retried once a second from AutoLoop.",
                            gDcduBound, kDcduCommandCount);
        for (int i = 0; i < kDcduCommandCount; ++i)
            if (!gDcduCmdRefs[i])
                ImGui::TextDisabled("   missing: %s", kDcduCommands[i].name);
    }
}

static void BuildProbeTab() {
    UiHint("%s", DbgSummary().c_str());
    ImGui::Spacing();

    bool dirty = false;
    dirty |= DbgOptionRow("Probe", kDbgModeOpts, 4, gPanelProbeMode, WritePanelProbeMode);
    dirty |= DbgOptionRow("Pass ", kDbgPassOpts, 3, gPanelProbeType, WritePanelProbeType);
    dirty |= DbgOptionRow("Draw ", kDbgDrawOpts, 4, gPanelProbeDrawMethod, WritePanelProbeDraw);

    // The target list is built from the live table rather than written out, so
    // re-deriving kPanelRectsHi cannot leave the window offering a name that no
    // longer exists — or, worse, quietly omitting a new one.
    ImGui::SetNextItemWidth(240.0f);
    int rectIdx = 0;
    for (int i = 0; i < gDbgRectOptCount; ++i)
        if (gDbgRectOpts[i].value == gPanelProbeRect) rectIdx = i;
    if (ImGui::BeginCombo("Target", gDbgRectOpts[rectIdx].label)) {
        for (int i = 0; i < gDbgRectOptCount; ++i) {
            // ⚠ Scoped by index. This list splices GROUP names and RECT names
            // into one array, and nothing prevents a group from being named
            // after a rect — the group table is hand-written and the rect table
            // is derived. Two same-named entries would share an ID, so picking
            // one would select the other.
            ImGui::PushID(i);
            if (ImGui::Selectable(gDbgRectOpts[i].label, i == rectIdx)) {
                WritePanelProbeRect(nullptr, gDbgRectOpts[i].value);
                dirty = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Multiply tint");
    float tint[3] = { gPanelTint[0], gPanelTint[1], gPanelTint[2] };
    if (ImGui::ColorEdit3("##tint", tint, ImGuiColorEditFlags_Float)) {
        for (int i = 0; i < 3; ++i) WritePanelTint((void*)(intptr_t)i, tint[i]);
        dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        for (int i = 0; i < 3; ++i) WritePanelTint((void*)(intptr_t)i, kPanelTintDefault[i]);
        dirty = true;
    }
    UiHint("A color is judged by walking it, not by jumping to it - drag a "
           "channel slowly rather than typing a number.");

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("FCU tint preset")) { ApplyFcuPreset(); dirty = true; }
    if (ImGui::IsItemHovered())
        UiTooltip("After Gauges, LIT pass, Multiply, FCU row - the state "
                  "the tint is tuned in.");
    ImGui::SameLine();
    if (ImGui::Button("Probe off")) { WritePanelProbeMode(nullptr, kProbeOff); dirty = true; }
    ImGui::SameLine();
    if (ImGui::Button("Re-log GL state")) {
        // Force the next frame of each pass to re-dump the GL state, the way a
        // mode/rect change does. Without this the only way to see a fresh dump
        // is to toggle a knob and toggle it back.
        gPanelStateLoggedMask = gPanelErrLoggedMask = 0;
        Log("panel probe: GL state + glGetError will be re-logged next frame");
    }

    ImGui::Spacing();
    BuildElectricalWatch();
    ImGui::Spacing();
    BuildCbWatch();

    if (dirty) SavePanelDebug();
}

#endif  // PHOTON_DEV — the Dev window's base and its Probe tab

// ================== panel FX: persistence, history, editor ===================
// ⚠ This block is OUTSIDE #if PHOTON_DEV as far as READING goes. The layer
// definition is part of the mod's look, so a shipping .xpl must load it; only
// the editor above it and the history below it are dev tooling.

// The authored layer definition, IN THE PLUGIN FOLDER — not in
// Output/preferences. It is a shipped artifact the installer replaces on upgrade,
// on the same footing as the overlay images the layers name, rather than a user
// setting. The user's own Display choices (on/off and strength) live in
// ToLissPhoton_profiles.json instead.
//
// ⚠ A SHIPPING BUILD NEVER WRITES THIS FILE. SavePanelFx is dev-only, and that is
// load-bearing twice over: an installer upgrade would otherwise clobber whatever
// a user's session had written into it, and on a dev machine the file is a
// SYMLINK into the repo, so anything a user-facing control wrote would land in
// source control.
static const char* kPanelFxFileName = "panelfx.txt";

static fs::path PanelFxFilePath() {
    return PhotonPluginDir() / kPanelFxFileName;
}

static int FxBlendFromName(const std::string& name) {
    for (int i = 0; i < kFxBlendCount; ++i)
        if (name == kFxBlendName[i]) return i;
    return kFxMultiply;
}

// Resolve a saved name back to a target index. Rects and groups share the space,
// so one lookup covers both.
static int PanelTargetByName(const std::string& name) {
    const int r = RectIndexByName(name.c_str());
    if (r >= 0) return r;
    return GroupTargetByName(name.c_str());
}

// ---- the wire format -------------------------------------------------------
// ONE serializer, used by the save file, the undo stack, the history journal and
// the clipboard. They were three ad-hoc writers; a look that round-trips through
// the file but not through undo is the kind of divergence nobody notices until a
// tuning pass is already lost.
//
// Targets are written by NAME, never by index: the rect table is append-only but
// groups are not, and a stack that silently retargets itself after an edit to the
// table is exactly the failure this project keeps writing tripwires about.
// ⚠ The image file name is written LAST, and read with getline rather than `>>`,
// because file names contain spaces and nothing else on the line may. "-" is the
// no-image marker; an empty tail would be indistinguishable from a truncated
// line. A layer written before images existed simply ends after the opacity, so
// the two extra reads fail and the defaults (no image, tile 1) stand.
static std::string SerializeFxStacks() {
    std::string out;
    char buf[512];
    for (size_t s = 0; s < gFxStacks.size(); ++s) {
        out += "target ";
        out += PanelTargetName(gFxStacks[s].target);
        out += "\n";
        // Driver overrides, written only when set — a stack that follows the
        // built-in table has nothing to say, and writing "bright -" for every
        // stack would make every old file diff against every new one.
        // ⚠ The dataref name goes FIRST here and is read with >>, unlike the
        // image name on a layer line: dataref paths cannot contain spaces, image
        // file names routinely do.
        // ⚠ The per-target switches take their OWN keys (tfollow/tcurve), not
        // the global ones. Parsing dispatches on the key alone, and a per-stack
        // line spelled "follow" would be read as the master switch by any file
        // whose lines are ever reordered.  -1 = inherit, and is not written.
        if (gFxStacks[s].follow != kFxInherit) {
            std::snprintf(buf, sizeof(buf), "tfollow %d\n", gFxStacks[s].follow);
            out += buf;
        }
        if (gFxStacks[s].curve != kFxInherit) {
            std::snprintf(buf, sizeof(buf), "tcurve %d\n", gFxStacks[s].curve);
            out += buf;
        }
        if (gFxStacks[s].bright.set()) {
            const FxDriver& d = gFxStacks[s].bright;
            std::snprintf(buf, sizeof(buf), "bright %s %d %.4f %.4f %.4f\n",
                          d.dref.c_str(), d.index, d.lo, d.hi, d.floorF);
            out += buf;
        }
        if (gFxStacks[s].power.set()) {
            const FxDriver& d = gFxStacks[s].power;
            std::snprintf(buf, sizeof(buf), "power %s %d %.4f\n",
                          d.dref.c_str(), d.index, d.floorF);
            out += buf;
        }
        for (size_t i = 0; i < gFxStacks[s].layers.size(); ++i) {
            const FxLayer& l = gFxStacks[s].layers[i];
            std::snprintf(buf, sizeof(buf),
                          "layer %s %d %.4f %.4f %.4f %.4f %.4f %s\n",
                          kFxBlendName[l.blend], l.enabled ? 1 : 0,
                          l.color[0], l.color[1], l.color[2], l.opacity,
                          l.tile, l.tex.empty() ? "-" : l.tex.c_str());
            out += buf;
            // ⚠ The layer's two extras are their OWN LINES, following the layer
            // they belong to, and they are not appended to it. The layer line
            // ends with an image NAME read by getline precisely because names
            // contain spaces — so nothing can ever come after it. This is the
            // same reason tfollow/tcurve are separate lines rather than columns.
            //
            // Both are written only when set, so a file whose layers are all
            // era-agnostic and all inherit is byte-identical to what the previous
            // version wrote.
            if (l.hasDay) {
                std::snprintf(buf, sizeof(buf), "day %.4f %.4f %.4f %.4f\n",
                              l.dayColor[0], l.dayColor[1], l.dayColor[2],
                              l.dayOpacity);
                out += buf;
            }
            // ⚠ THREE floats, not four. The ramp's start color has no opacity
            // of its own — opacity across the knob is what `lfollow` and the
            // curve already answer, and a second opacity here would be a third
            // control multiplying into the same number with no way to tell from
            // the cockpit which of the three produced a dim layer.
            if (l.hasRamp) {
                std::snprintf(buf, sizeof(buf), "ramp %.4f %.4f %.4f\n",
                              l.rampColor[0], l.rampColor[1], l.rampColor[2]);
                out += buf;
            }
            if (l.follow != kFxInherit) {
                std::snprintf(buf, sizeof(buf), "lfollow %d\n", l.follow);
                out += buf;
            }
            // ⚠ The COUNT comes first and the pairs follow on the same line. A
            // curve is one thing - a knot on a line of its own would be a shape
            // that can be truncated into a different, perfectly valid shape by
            // any edit that loses a line, and there is no marker that could tell
            // the two apart afterwards. The count also lets the reader refuse a
            // half-parsed table outright rather than keeping what it managed to
            // read. Bounded by kFxCurveMaxKnots at both ends of the wire.
            if (l.curve.on && l.curve.knots.size() >= 2) {
                std::snprintf(buf, sizeof(buf), "lcurve %d",
                              (int)l.curve.knots.size());
                out += buf;
                for (size_t k = 0; k < l.curve.knots.size(); ++k) {
                    std::snprintf(buf, sizeof(buf), " %.4f %.4f",
                                  l.curve.knots[k].x, l.curve.knots[k].y);
                    out += buf;
                }
                out += "\n";
            }
        }
    }
    return out;
}

// Replaces the whole stack list. `dropped` counts targets whose name no longer
// resolves — reported rather than silently skipped, because a dropped target is
// a look that will not come back and the user should know before tuning on top
// of it.
static void DeserializeFxStacks(const std::string& text, int* dropped) {
    gFxStacks.clear();
    if (dropped) *dropped = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream in(line);
        std::string key;
        in >> key;

        if (key == "enabled") {
            int v = 0; in >> v; gFxEnabled = (v != 0);
        } else if (key == "follow") {
            int v = 1; in >> v; gFxFollowBrightness = (v != 0);
        } else if (key == "curve") {
            int v = 1; in >> v; gFxCurve = (v != 0);
        } else if (key == "ambient") {
            // The global day/night calibration. `tau` is optional so a file that
            // only ever moved lo/hi round-trips without it.
            in >> gFxAmbientLo >> gFxAmbientHi;
            float tau = gFxAmbientTau;
            if (in >> tau) gFxAmbientTau = tau;
        } else if (key == "ambientref") {
            FxDriver d;
            in >> d.dref >> d.index;
            if (d.dref == "-") d.dref.clear();        // the explicit "no override"
            gFxAmbientSource = d;
        } else if (key == "tfollow" || key == "tcurve") {
            if (gFxStacks.empty()) continue;          // a switch before any target
            int v = kFxInherit; in >> v;
            const int tri = v < 0 ? kFxInherit : (v ? kFxOn : kFxOff);
            if (key == "tfollow") gFxStacks.back().follow = tri;
            else                  gFxStacks.back().curve  = tri;
        } else if (key == "day" || key == "ramp" || key == "lfollow"
                   || key == "lcurve") {
            // ⚠ These attach to the LAST LAYER, not the last target — they are
            // written directly under the layer line they belong to. A file where
            // one appears before any layer is malformed rather than ambiguous, so
            // it is dropped instead of being applied to something else.
            if (gFxStacks.empty() || gFxStacks.back().layers.empty()) continue;
            FxLayer& l = gFxStacks.back().layers.back();
            if (key == "day") {
                in >> l.dayColor[0] >> l.dayColor[1] >> l.dayColor[2] >> l.dayOpacity;
                l.hasDay = true;
            } else if (key == "ramp") {
                // ⚠ Into a local, then adopted — the same rule the tile field
                // learned. operator>> zeroes its target on failure, so a short
                // line read straight into the layer would leave a BLACK start
                // color and hasRamp true: a layer that fades to black at a dim
                // knob, which is a plausible enough look that nobody would read
                // it as a truncated line.
                float c[3] = { 0.0f, 0.0f, 0.0f };
                if (in >> c[0] >> c[1] >> c[2]) {
                    for (int k = 0; k < 3; ++k) l.rampColor[k] = c[k];
                    l.hasRamp = true;
                } else {
                    Log("panel fx: ramp line needs three floats - the layer keeps "
                        "one color across the knob");
                }
            } else if (key == "lcurve") {
                // ⚠ All or nothing. A table read into the layer as the stream
                // fails would be a DIFFERENT SHAPE that is still perfectly
                // valid - fewer knots, silently applied, with nothing in the
                // cockpit to distinguish it from the one that was authored. So
                // it is assembled in a local and only adopted whole.
                int n = 0;
                in >> n;
                if (n >= 2 && n <= kFxCurveMaxKnots) {
                    FxCurve c;
                    bool ok = true;
                    for (int k = 0; k < n && ok; ++k) {
                        CurveKnot knot = { 0.0f, 0.0f };
                        if (in >> knot.x >> knot.y) c.knots.push_back(knot);
                        else                        ok = false;
                    }
                    if (ok) {
                        c.on = true;
                        FxCurveNormalize(c);
                        l.curve = c;
                    } else {
                        Log("panel fx: lcurve line has fewer knots than it "
                            "claims - the layer keeps the target's response "
                            "curve");
                    }
                } else {
                    Log("panel fx: lcurve needs 2.."
                        + std::to_string(kFxCurveMaxKnots)
                        + " knots - ignored");
                }
            } else {
                int v = kFxInherit; in >> v;
                l.follow = v < 0 ? kFxInherit : (v ? kFxOn : kFxOff);
            }
        } else if (key == "bright" || key == "power") {
            if (gFxStacks.empty()) continue;          // a driver before any target
            FxDriver d;
            in >> d.dref >> d.index;
            if (key == "bright") {
                in >> d.lo >> d.hi >> d.floorF;
                gFxStacks.back().bright = d;
            } else {
                in >> d.floorF;
                gFxStacks.back().power = d;
            }
        } else if (key == "target") {
            std::string name;
            std::getline(in, name);
            if (!name.empty() && name[0] == ' ') name.erase(0, 1);
            const int t = PanelTargetByName(name);
            if (t < 0) {
                if (dropped) ++*dropped;
                Log("panel fx: unknown target '" + name + "' - dropped");
            } else {
                FxStack s;
                s.target = t;
                gFxStacks.push_back(s);
            }
        } else if (key == "layer") {
            if (gFxStacks.empty()) continue;      // a layer before any target
            std::string blend; int on = 1;
            FxLayer l;
            in >> blend >> on >> l.color[0] >> l.color[1] >> l.color[2] >> l.opacity;
            l.blend   = FxBlendFromName(blend);
            l.enabled = (on != 0);

            // ⚠ Read into a local and only keep it if the stream survived.
            // operator>> ZEROES its target on failure, so reading straight into
            // l.tile would turn every pre-image layer's tile into 0 — an image
            // sampled at a single texel, which is not a defaulting bug anyone
            // would recognize from the symptom.
            float tile = 1.0f;
            if (in >> tile) {
                l.tile = tile;
                std::string tex;
                if (std::getline(in, tex)) {
                    while (!tex.empty() && (tex.front() == ' ' || tex.front() == '\t'))
                        tex.erase(0, 1);
                    while (!tex.empty() && (tex.back() == ' ' || tex.back() == '\r'))
                        tex.pop_back();
                    if (tex != "-") l.tex = tex;
                }
            }
            gFxStacks.back().layers.push_back(l);
        }
    }
}

#if PHOTON_DEV
// ⚠ DEV ONLY, and not an oversight — see kPanelFxFileName. A plain std::ofstream
// rather than a write-temp-and-rename: on a dev machine the destination is a
// SYMLINK into the repo, and a rename would replace the link with a plain file,
// quietly ending the round trip after exactly one save.
static void SavePanelFx() {
    std::ofstream f(PanelFxFilePath(), std::ios::binary);
    if (!f) { Log("panel fx: could not write " + PanelFxFilePath().string()); return; }
    f << "# ToLiss Photon panel FX layers - generated, safe to delete.\n";
    f << "# target <name>\n";
    f << "# bright <dataref> <index> <lo> <hi> <floor>   (optional, per target)\n";
    f << "# power  <dataref> <index> <above>             (optional, per target)\n";
    f << "# tfollow 0|1  /  tcurve 0|1   (optional, per target; absent = inherit)\n";
    f << "# layer <blend> <enabled> <r> <g> <b> <opacity> <tile> <image|->\n";
    f << "#   day <r> <g> <b> <opacity>  (optional, per LAYER: the daylight half)\n";
    f << "#   ramp <r> <g> <b>           (optional, per LAYER: the color at a\n";
    f << "#                               dark knob; the layer's own color is\n";
    f << "#                               the color at a full one)\n";
    f << "#   lfollow 0|1                (optional, per LAYER; absent = inherit)\n";
    f << "#   lcurve <n> <x0> <y0> ...   (optional, per LAYER: its own response\n";
    f << "#                               curve, overriding tcurve for that layer)\n";
    f << "enabled " << (gFxEnabled ? 1 : 0) << "\n";
    f << "follow " << (gFxFollowBrightness ? 1 : 0) << "\n";
    f << "curve "  << (gFxCurve ? 1 : 0) << "\n";
    // The day/night calibration, always written: the two ends of the ambient
    // range are the numbers a tuning session moves, and a file that omitted them
    // would silently re-adopt whatever the constants happened to be.
    char amb[128];
    std::snprintf(amb, sizeof(amb), "ambient %.4f %.4f %.4f\n",
                  gFxAmbientLo, gFxAmbientHi, gFxAmbientTau);
    f << amb;
    if (gFxAmbientSource.set())
        f << "ambientref " << gFxAmbientSource.dref << " "
          << gFxAmbientSource.index << "\n";
    f << SerializeFxStacks();
}
#endif  // PHOTON_DEV — only a dev build writes the layer definition

static void LoadPanelFx() {
    std::ifstream f(PanelFxFilePath(), std::ios::binary);
    if (!f) {
        // On a shipping build this means the installer's copy is missing, which
        // is worth a line: every Displays effect will be inert and there is
        // nothing in the cockpit to see. In a dev build it is just first run.
        Log("panel fx: no layer definition at " + PanelFxFilePath().string()
            + " - the panel effects have nothing to draw");
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    int dropped = 0;
    DeserializeFxStacks(ss.str(), &dropped);
    Log("panel fx: restored " + std::to_string(gFxStacks.size()) + " stack(s)"
        + (dropped ? " (" + std::to_string(dropped) + " dropped)" : "")
        + " from " + PanelFxFilePath().string()
        + (gFxEnabled ? " - ENABLED" : " - disabled"));

    // ⚠ The overlay folder is scanned HERE, not on first draw. It walks a
    // directory, may create it and may write a README, and the lazy scan that
    // FxTextureByName still carries as a backstop would do all of that inside the
    // panel pass — one hitch, on the first frame a layer with an image is drawn,
    // in a release build. This runs from XPluginEnable instead, off the render
    // thread, which is also where the log line belongs.
    if (!gFxTexturesScanned) ScanFxTextures();
}

#if PHOTON_DEV
// ---- history: undo, redo, and an append-only journal ------------------------
//
// The journal stays in Output/preferences, unlike the layer definition beside the
// plugin: it is a record of one machine's tuning sessions, it grows without bound
// by design, and it is worthless to anyone but the person who made the edits. An
// installer upgrade replacing the plugin folder must not take it with it.
static fs::path PanelFxJournalPath() {
    char root[512] = {0};
    XPLMGetSystemPath(root);
    return fs::path(root) / "Output" / "preferences" / "ToLissPhoton_panelfx_history.txt";
}

// Two different recoveries, because two different things go wrong.
//
//   * UNDO is for the edit you just made. In-memory, instant, and it is what
//     makes a wide slider safe to drag: you can ruin a look and get it back.
//   * The JOURNAL is for the session you already closed. Every commit appends a
//     timestamped snapshot to its own file, so a look that was good an hour ago
//     is still on disk after the sim has been restarted twice. The save file
//     alone cannot do this - it holds exactly one state, the latest, and the
//     edit that destroyed a good look overwrites it immediately.
//
// The journal is append-only ON PURPOSE. Trimming it to "recent" entries would
// discard the old snapshot that is precisely what you want when you realize
// three days later that the look was better before.
struct FxHistoryEntry {
    std::string when;      // wall clock, for recognizing a session
    std::string what;      // the edit that produced this state
    std::string text;      // serialized stacks
};

static std::vector<FxHistoryEntry> gFxUndo, gFxRedo, gFxJournal;
static bool gFxHistoryLoaded = false;

// Bounded only to keep the window scrollable; the journal on disk is not.
static const size_t kFxUndoMax = 64;

static std::string FxNowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

// Journal lines are one record each so the file stays greppable and a partial
// write costs one entry rather than the file: the serialized stacks go on the
// line with newlines escaped as \n.
static std::string FxEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\')      out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c != '\r') out += c;
    }
    return out;
}
static std::string FxUnescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += (s[i] == 'n') ? '\n' : s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

static void AppendFxJournal(const FxHistoryEntry& e) {
    std::ofstream f(PanelFxJournalPath(), std::ios::binary | std::ios::app);
    if (!f) return;
    f << e.when << " | " << FxEscape(e.what) << " | " << FxEscape(e.text) << "\n";
}

static void LoadFxJournal() {
    gFxJournal.clear();
    std::ifstream f(PanelFxJournalPath(), std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t a = line.find(" | ");
        if (a == std::string::npos) continue;
        const size_t b = line.find(" | ", a + 3);
        if (b == std::string::npos) continue;
        FxHistoryEntry e;
        e.when = line.substr(0, a);
        e.what = FxUnescape(line.substr(a + 3, b - a - 3));
        e.text = FxUnescape(line.substr(b + 3));
        gFxJournal.push_back(e);
    }
    gFxHistoryLoaded = true;
}

// Call AFTER mutating gFxStacks, naming the edit. Saves, records undo, journals.
// ⚠ Every mutation goes through here. A direct SavePanelFx() call is a change
// that undo cannot reach and the journal never saw, which is worse than not
// having either - the user trusts a history that has a hole in it.
static void FxCommit(const std::string& what, const std::string& before) {
    FxHistoryEntry undo;
    undo.when = FxNowStamp();
    undo.what = what;
    undo.text = before;
    gFxUndo.push_back(undo);
    if (gFxUndo.size() > kFxUndoMax) gFxUndo.erase(gFxUndo.begin());
    gFxRedo.clear();                  // a new edit forks the timeline

    SavePanelFx();

    FxHistoryEntry now;
    now.when = undo.when;
    now.what = what;
    now.text = SerializeFxStacks();
    gFxJournal.push_back(now);
    AppendFxJournal(now);
}

// Sugar for the common shape: snapshot, mutate, commit.
struct FxEdit {
    std::string before;
    explicit FxEdit() : before(SerializeFxStacks()) {}
    void Commit(const std::string& what) { FxCommit(what, before); }
};

static void FxRestore(const std::string& text, const std::string& what) {
    const std::string before = SerializeFxStacks();
    DeserializeFxStacks(text, nullptr);
    FxCommit(what, before);
}

static void FxUndoOnce() {
    if (gFxUndo.empty()) return;
    FxHistoryEntry e = gFxUndo.back();
    gFxUndo.pop_back();
    FxHistoryEntry redo;
    redo.when = FxNowStamp();
    redo.what = e.what;
    redo.text = SerializeFxStacks();
    gFxRedo.push_back(redo);
    DeserializeFxStacks(e.text, nullptr);
    SavePanelFx();
    Log("panel fx: undo - " + e.what);
}

static void FxRedoOnce() {
    if (gFxRedo.empty()) return;
    FxHistoryEntry e = gFxRedo.back();
    gFxRedo.pop_back();
    FxHistoryEntry undo;
    undo.when = FxNowStamp();
    undo.what = e.what;
    undo.text = SerializeFxStacks();
    gFxUndo.push_back(undo);
    DeserializeFxStacks(e.text, nullptr);
    SavePanelFx();
    Log("panel fx: redo - " + e.what);
}

// ---- the editor ------------------------------------------------------------

// ⚠ The selection is a TARGET, not an index into gFxStacks. Deleting a target
// shuffles that vector, so a stored index silently re-points the layer pane at
// a different target's layers — the editing equivalent of the retargeting bug
// the save format's name lookup exists to prevent.
static int gFxSelectedTarget = -1;

// The layer clipboard. A layer is four numbers and a mode, and the same four
// numbers are usually wanted on three targets — retyping them is both slow and
// the way two targets end up almost, but not quite, matching.
static FxLayer gFxClipboard;
static bool    gFxClipboardFull = false;

// The whole-STACK clipboard, separate from the single-layer one above. Once a
// look is settled it is nearly always wanted verbatim on a sibling — the six DUs
// share a tint, the two MCDUs share one — and rebuilding a three-layer stack by
// hand is how five of six end up matching and the sixth quietly does not. Kept
// as a copy of the layers rather than a target reference so the source can be
// edited or deleted afterwards without changing what pastes.
static std::vector<FxLayer> gFxStackClipboard;
static std::string          gFxStackClipboardFrom;

static int FxStackIndex(int target) {
    if (target < 0) return -1;
    for (size_t s = 0; s < gFxStacks.size(); ++s)
        if (gFxStacks[s].target == target) return (int)s;
    return -1;
}

// The stack for a target, created empty if it has none. Every path that adds a
// layer goes through this, because selecting a target no longer creates one —
// the stack starts existing at the moment something is actually put in it.
// ⚠ Construct the FxEdit snapshot BEFORE calling this: it mutates gFxStacks.
static FxStack& FxEnsureStack(int target) {
    const int s = FxStackIndex(target);
    if (s >= 0) return gFxStacks[(size_t)s];
    FxStack st;
    st.target = target;
    gFxStacks.push_back(st);
    return gFxStacks.back();
}

static void FxDeleteTarget(int target) {
    const int s = FxStackIndex(target);
    if (s < 0) return;
    FxEdit edit;
    gFxStacks.erase(gFxStacks.begin() + s);
    if (gFxSelectedTarget == target) gFxSelectedTarget = -1;
    edit.Commit(std::string("delete target ") + PanelTargetName(target));
}

// Dump the whole stack to Log.txt in the save-file syntax. The tuning loop ends
// with numbers that have to reach source, and reconstructing them from memory
// after a session is how a good look gets lost.
static void LogPanelFx() {
    Log("panel fx: --- current stacks, in composite order ---");
    Log(std::string("panel fx: defaults - follow display brightness = ")
        + (gFxFollowBrightness ? "ON" : "off (nothing below is dimmed OR gated)")
        + ", response curve = " + (gFxCurve ? "ON" : "off (raw dataref)")
        + "; each target may override, resolved per target below");
    const std::vector<int> order = FxDrawOrder();
    for (size_t n = 0; n < order.size(); ++n) {
        const size_t s = (size_t)order[n];
        // The RESOLVED switches, not the raw tri-state: "inherit" plus a master
        // set three panes away is two lookups to answer "is this target being
        // dimmed at all", and this dump exists so that question needs none.
        const bool follows = FxStackFollows(&gFxStacks[s]);
        const bool curves  = FxStackCurves(&gFxStacks[s]);
        // The per-rect rows below describe the TARGET, so they are read through
        // the target's shape. A layer with knots of its own is reported on its
        // own line instead - the factor in a rect row is not that layer's.
        const FxCurve* shape = FxShapeForStack(&gFxStacks[s]);
        Log(std::string("panel fx: target ") + PanelTargetName(gFxStacks[s].target)
            + "  [follow " + (follows ? "on" : "OFF - full strength when powered")
            + (gFxStacks[s].follow == kFxInherit ? " (inherited)" : " (target)")
            + ", curve " + (curves ? "on" : "off - linear")
            + (gFxStacks[s].curve == kFxInherit ? " (inherited)" : " (target)") + "]");
        // The live factor per member rect, because a stack that looks wrong in
        // the cockpit is at least as likely to be a mis-guessed dataref as a
        // mis-set color, and the two are indistinguishable from a screenshot.
        if (!gRectDriversBuilt) BuildRectDrivers();
        const PanelMask mask = PanelTargetMask(gFxStacks[s].target);
        for (int i = 0; i < kPanelRectCount; ++i) {
            if (!(mask & ((PanelMask)1 << i))) continue;
            gFxDrawStack = &gFxStacks[s];
            const float f = FxRectFactor(i);
            gFxDrawStack = nullptr;
            FxDriver& br = gFxStacks[s].bright.set() ? gFxStacks[s].bright : gRectBright[i];
            FxDriver& pw = gFxStacks[s].power.set()  ? gFxStacks[s].power  : gRectPower[i];
            // The live VALUES, not just the names: a factor of 0 with the name
            // alone is indistinguishable between "the power gate is doing its
            // job" and "this brightness dataref is not in the units lo/hi
            // assume", and those need opposite fixes.
            char buf[768];
            std::snprintf(buf, sizeof(buf),
                          "panel fx:   %-14s x%.2f  bright=%s  power=%s  bus=%s  cb=%s",
                          kPanelRectsHi[i].name, f,
                          FxDriverState(br, false, shape).c_str(),
                          FxDriverState(pw, true, shape).c_str(),
                          // The bus and breaker are never a stack's to override,
                          // so these are always the rect's own.
                          FxDriverState(gRectBus[i], true, shape).c_str(),
                          FxDriverState(gRectCb[i], true, shape).c_str());
            Log(buf);
        }
        for (size_t i = 0; i < gFxStacks[s].layers.size(); ++i) {
            const FxLayer& l = gFxStacks[s].layers[i];
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "panel fx:   layer %s %d %.4f %.4f %.4f %.4f %.4f %s",
                          kFxBlendName[l.blend], l.enabled ? 1 : 0,
                          l.color[0], l.color[1], l.color[2], l.opacity,
                          l.tile, l.tex.empty() ? "-" : l.tex.c_str());
            Log(buf);
            // Same argument as the curve below: a ramp is invisible in the layer
            // line above, and "wrong at one end of the knob" is precisely the
            // report a ramp explains.
            if (l.hasRamp) {
                char rbuf[128];
                std::snprintf(rbuf, sizeof(rbuf),
                              "panel fx:     ramp from %.4f %.4f %.4f at a dark "
                              "knob to the color above at a full one",
                              l.rampColor[0], l.rampColor[1], l.rampColor[2]);
                Log(rbuf);
            }
            // ⚠ Only when it has one. A curve is the setting most likely to be
            // blamed for a look that is wrong at one end of the knob and right
            // at the other, and "which layer has one" is otherwise a question
            // only the editor can answer.
            if (l.curve.on && l.curve.knots.size() >= 2) {
                std::string knots;
                for (size_t k = 0; k < l.curve.knots.size(); ++k) {
                    char one[32];
                    std::snprintf(one, sizeof(one), "%s%.3f>%.3f", k ? "  " : "",
                                  l.curve.knots[k].x, l.curve.knots[k].y);
                    knots += one;
                }
                Log("panel fx:     curve (overrides the target's response): " + knots);
            }
        }
    }
}

// ---- a layer's image row ----------------------------------------------------
// Its own line under the layer's color controls. Folding the picker into that
// row made it wider than the window at every sane size, and an image is a
// different kind of choice from a color anyway.
//
// The combo lists what is in the overlays folder. A layer whose saved name is no
// longer there still shows that name, marked missing, rather than reverting to
// "(none)": silently clearing it would lose the only record of what the look was
// built from.
static void BuildFxLayerImageRow(FxLayer& layer, bool* dirty, std::string* what) {
    if (!gFxTexturesScanned) ScanFxTextures();

    const bool takesTex = FxBlendTakesTexture(layer.blend);
    FxTexture* cur      = FxTextureByName(layer.tex);
    const bool missing  = !layer.tex.empty() && cur == nullptr;

    ImGui::Indent(24.0f);

    ImGui::TextUnformatted("Image");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);

    char preview[288];
    if (layer.tex.empty())   std::snprintf(preview, sizeof(preview), "(none - solid color)");
    else if (missing)        std::snprintf(preview, sizeof(preview), "%s  [MISSING]",
                                           layer.tex.c_str());
    else                     std::snprintf(preview, sizeof(preview), "%s", layer.tex.c_str());

    if (ImGui::BeginCombo("##img", preview)) {
        if (ImGui::Selectable("(none - solid color)", layer.tex.empty())) {
            layer.tex.clear();
            *dirty = true; *what = "clear layer image";
        }
        for (size_t i = 0; i < gFxTextures.size(); ++i) {
            const bool sel = (gFxTextures[i].name == layer.tex);
            ImGui::PushID((int)i);
            if (ImGui::Selectable(gFxTextures[i].name.c_str(), sel)) {
                layer.tex = gFxTextures[i].name;
                *dirty = true; *what = "set layer image";
            }
            if (sel) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        if (gFxTextures.empty())
            ImGui::TextDisabled("Nothing in the overlays folder yet");
        ImGui::EndCombo();
    }

    // Thumbnail. Uploading here is safe and is what makes the preview honest —
    // an image that cannot be decoded shows nothing here for the same reason it
    // draws nothing in the cockpit.
    if (cur && UploadFxTexture(*cur) && cur->w > 0 && cur->h > 0) {
        const float side = 44.0f;
        const float w = cur->w >= cur->h ? side : side * (float)cur->w / (float)cur->h;
        const float h = cur->w >= cur->h ? side * (float)cur->h / (float)cur->w : side;
        ImGui::SameLine();
        ImGui::Image((ImTextureID)(intptr_t)cur->id, ImVec2(w, h));
        if (ImGui::IsItemHovered())
            UiTooltip("%s - %dx%d\n(the thumbnail is premultiplied, so a "
                      "semi-transparent image previews slightly dark)\n\n"
                      "Edit and save the file and both this and the cockpit "
                      "follow within a second.\nClick to re-read it now.",
                      cur->name.c_str(), cur->w, cur->h);
        // A click on the thumbnail is the manual reload. Not an edit: nothing in
        // the stack changes, so it is deliberately outside the undo journal.
        if (ImGui::IsItemClicked()) {
            cur->stale  = cur->id != 0;
            cur->failed = false;
            Log("panel fx: re-reading overlay '" + cur->name + "' on request");
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderFloat("tile", &layer.tile, 0.25f, 8.0f, "%.2fx")) {
            *dirty = true; *what = "change layer tiling";
        }
        if (ImGui::IsItemHovered())
            UiTooltip("How many times the image repeats across the target. "
                      "1 stretches it to fit.");
    }

    if (missing) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "Not in the overlays folder - this layer is skipped.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan")) ScanFxTextures();
    } else if (!layer.tex.empty() && !takesTex) {
        // Not a warning about a bad value — the layer is genuinely not drawn.
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "%s cannot carry an image - this layer is skipped.",
                           kFxBlendName[layer.blend]);
        if (ImGui::IsItemHovered())
            UiTooltip("Burn emits the complement of the color and Darken is "
                      "GL_MIN, and fixed-function GL can express neither one "
                      "per pixel. Multiply and Subtract are the closest modes "
                      "that can.");
    }

    ImGui::Unindent(24.0f);
}

// ---- identify: flood one target magenta in the cockpit ----------------------
// Most rect names below the six DUs are inferred from geometry rather than told
// to us by ToLiss, so "which face is this?" is a real question with a one-click
// answer. It drives the probe rather than the compositor: a layer would have to
// be added, tuned to something visible, and then removed again.
//
// Clicking it on the target it is already showing turns the probe OFF. Without
// that the button has no visible off state, and the way back is a different tab.
static bool FxIdentifyActive(int t) {
    return gPanelProbeMode       == kProbeAfterGauges
        && gPanelProbeDrawMethod == kDrawImmediate
        && gPanelProbeRect       == t;
}

static void FxIdentifyTarget(int t) {
    if (FxIdentifyActive(t)) {
        WritePanelProbeMode(nullptr, kProbeOff);
        SavePanelDebug();
        Log(std::string("dev: identify off - '") + PanelTargetName(t)
            + "' is no longer magenta");
        return;
    }
    WritePanelProbeMode(nullptr, kProbeAfterGauges);
    WritePanelProbeType(nullptr, kFxPass);
    WritePanelProbeDraw(nullptr, kDrawImmediate);
    WritePanelProbeRect(nullptr, t);
    SavePanelDebug();
    Log(std::string("dev: identifying '") + PanelTargetName(t)
        + "' - it is now magenta. Click ? again (or Probe tab > Probe off) to stop.");
}

// One row of the target tree, plus its children. Every target is always listed,
// whether or not it carries layers — a target with no layers is drawn dimmed,
// and selecting it opens an empty layer pane rather than creating anything.
//
// Selecting used to CREATE a stack holding one layer, which made adding and
// selecting the same gesture — and made every mis-click on the way down the tree
// leave a real, saved, invisible-until-tuned layer behind on a target nobody
// meant to touch. Adding is now "Add layer", and only that.
static void BuildFxTargetNode(int t, int* pendingDelete) {
    const int  s        = FxStackIndex(t);
    const bool isGroup  = (t >= kPanelRectCount);
    const bool hasStack = (s >= 0);

    char label[160];
    if (hasStack)
        std::snprintf(label, sizeof(label), "%s  [%d]", PanelTargetName(t),
                      (int)gFxStacks[s].layers.size());
    else
        std::snprintf(label, sizeof(label), "%s", PanelTargetName(t));

    // OpenOnArrow so that clicking the NAME selects rather than collapsing — a
    // group is a target in its own right here, not just a folder.
    //
    // ⚠ AllowOverlap is LOAD-BEARING, not decoration. SpanAvailWidth extends the
    // node's hit box to the right edge of the pane, i.e. straight over the row
    // buttons below — and ItemHoverable refuses hover to any later item once
    // g.HoveredId is taken ("if (g.HoveredId != 0 && g.HoveredId != id &&
    // !g.HoveredIdAllowOverlap) return false", imgui.cpp). Without this flag the
    // ? and x buttons still DRAW and are completely dead: no hover, no tooltip,
    // no click, no GL or ImGui complaint. ImGui's own comment on SpanAvailWidth
    // says exactly this. That shipped, and read in-sim as "the buttons do
    // nothing".
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_AllowOverlap
                             | ImGuiTreeNodeFlags_DefaultOpen;
    if (!isGroup)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    // Selection follows the CLICK, not the presence of a stack: an empty target
    // is a legitimate thing to have selected now that selecting creates nothing.
    if (gFxSelectedTarget == t) flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(t);
    if (!hasStack)
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", label);
    if (!hasStack) ImGui::PopStyleColor();

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) gFxSelectedTarget = t;

    // Captured now: BeginPopupContextItem below submits a window of its own, so
    // "the last item" is no longer this node once it has run.
    const bool hovered = ImGui::IsItemHovered();

    // ---- the right-click menu ----------------------------------------------
    // Must come before any other item on this row: BeginPopupContextItem binds
    // to the LAST submitted item, and one SameLine button later it would open
    // off the x instead of off the node.
    //
    // Copy/paste lives here rather than on the row because it is a whole-stack
    // gesture between two targets, and the row has no room for four more
    // buttons. The layer pane carries the same two as named buttons.
    if (ImGui::BeginPopupContextItem("ctx")) {
        const int  layers = hasStack ? (int)gFxStacks[s].layers.size() : 0;
        const int  onClip = (int)gFxStackClipboard.size();
        const bool full   = onClip > 0;
        char item[128];

        ImGui::TextDisabled("%s - %d rect(s), %d layer(s)",
                            PanelTargetName(t), PanelTargetBreadth(t), layers);
        ImGui::Separator();

        std::snprintf(item, sizeof(item), "Copy all %d layer(s)", layers);
        if (ImGui::MenuItem(item, nullptr, false, layers > 0)) {
            gFxStackClipboard     = gFxStacks[s].layers;
            gFxStackClipboardFrom = PanelTargetName(t);
            Log("panel fx: copied " + std::to_string(layers) + " layer(s) from "
                + gFxStackClipboardFrom);
        }

        // Two pastes, because both are wanted and they are not the same edit:
        // replace makes this target match the source, add-on-top refines what is
        // already here. Neither is safe to guess from context.
        std::snprintf(item, sizeof(item), "Paste %d layer(s) - replace", onClip);
        if (ImGui::MenuItem(item, nullptr, false, full)) {
            FxEdit edit;
            FxEnsureStack(t).layers = gFxStackClipboard;
            gFxSelectedTarget = t;
            edit.Commit(std::string("paste layers onto ") + PanelTargetName(t)
                        + " (replace)");
        }
        std::snprintf(item, sizeof(item), "Paste %d layer(s) - add on top", onClip);
        if (ImGui::MenuItem(item, nullptr, false, full)) {
            FxEdit edit;
            FxStack& st = FxEnsureStack(t);
            st.layers.insert(st.layers.end(),
                             gFxStackClipboard.begin(), gFxStackClipboard.end());
            gFxSelectedTarget = t;
            edit.Commit(std::string("paste layers onto ") + PanelTargetName(t)
                        + " (add on top)");
        }
        // TextDisabled rather than UiHint: UiHint wraps at the window width, and
        // in an auto-sizing popup that is a width that depends on the text.
        if (full) ImGui::TextDisabled("clipboard: %d layer(s) from \"%s\"",
                                      onClip, gFxStackClipboardFrom.c_str());
        else      ImGui::TextDisabled("clipboard empty");

        ImGui::Separator();
        if (ImGui::MenuItem(FxIdentifyActive(t) ? "Stop identifying"
                                                : "Identify - flood it magenta"))
            FxIdentifyTarget(t);
        // ⚠ Deferred like the x button, and for the same reason: erasing from
        // gFxStacks here would invalidate the stack the child recursion below is
        // about to read.
        if (ImGui::MenuItem("Delete target and all its layers", nullptr, false, hasStack))
            *pendingDelete = t;
        ImGui::EndPopup();
    }

    if (hovered && !ImGui::IsPopupOpen("ctx")) {
        if (hasStack) UiTooltip("%d rect(s) - right-click to copy or paste "
                                "the whole stack", PanelTargetBreadth(t));
        else          UiTooltip("No layers yet - select it and use \"Add "
                                "layer\", or right-click to paste a stack "
                                "(%d rect(s))", PanelTargetBreadth(t));
    }

    // Row buttons, in TWO FIXED COLUMNS off the right edge. Deliberately NOT
    // GetContentRegionMax(), which lives in ImGui's obsolete block — the whole
    // point of the CmdListsCount episode is that this project should be able to
    // define IMGUI_DISABLE_OBSOLETE_FUNCTIONS one day.
    //
    // ⚠ The ? sits in the same column whether or not this row has an x, so a
    // target with no stack leaves a HOLE on the right rather than sliding its ?
    // over into the x's place. Right-aligning the pair as a group put the ? in
    // one of two positions depending on the row below it, and a column of
    // buttons that is not a column is read as misalignment however deliberate
    // it is — the eye is looking for the edge, not for the group.
    const float kRowBtnW   = 18.0f;        // one SmallButton of a single glyph
    const float kRowBtnGap = 4.0f;
    ImGui::SameLine();
    const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(rightEdge - kRowBtnW * 2.0f - kRowBtnGap);
    const bool identifying = FxIdentifyActive(t);
    if (identifying)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 1.0f, 1.0f));
    if (ImGui::SmallButton("?")) FxIdentifyTarget(t);
    if (identifying) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        UiTooltip(identifying
                  ? "This target is the magenta one right now - click to "
                    "turn the probe off."
                  : "Paint this target magenta in the cockpit so you can "
                    "see which face it is. Click again to stop.");
    if (hasStack) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(rightEdge - kRowBtnW);
        if (ImGui::SmallButton("x")) *pendingDelete = t;
        if (ImGui::IsItemHovered())
            UiTooltip("Delete this target and all its layers");
    }

    if (isGroup && open) {
        for (int c = 0; c < kPanelTargetCount; ++c)
            if (PanelTargetParent(c) == t) BuildFxTargetNode(c, pendingDelete);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

static void BuildFxTargetPane() {
    ImGui::SeparatorText("Targets");

    int pendingDelete = -1;

    // Roots are the targets nothing else contains. Derived, not listed, so
    // deleting a group from the table re-roots its members instead of hiding
    // them.
    for (int t = 0; t < kPanelTargetCount; ++t)
        if (PanelTargetParent(t) < 0) BuildFxTargetNode(t, &pendingDelete);

    // Deferred: erasing mid-walk would invalidate the stack the recursion below
    // it is about to read.
    if (pendingDelete >= 0) FxDeleteTarget(pendingDelete);
}

// ---- the brightness / power sources for one target -------------------------
// Folded away by default: it is the part you set once and then stop looking at.
// What it shows when open is deliberately the LIVE value and the factor it
// produces, not just the dataref name — every one of the built-in names is read
// off ToLiss's binary rather than measured, so "is this the right dataref" is
// the question the pane exists to answer, and a name alone cannot answer it.
static void FxDriverRow(const char* what, FxDriver& d, bool isPower,
                        const FxCurve* shape) {
    if (!d.set()) { ImGui::TextDisabled("%s: none", what); return; }
    double v = 0.0;
    const bool ok = FxDriverRaw(d, v);
    char idx[24] = "";
    if (d.index >= 0) std::snprintf(idx, sizeof(idx), "[%d]", d.index);
    if (!ok) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s: %s%s - unreadable",
                           what, d.dref.c_str(), idx);
        if (ImGui::IsItemHovered())
            UiTooltip("The dataref does not exist, is not numeric, or the "
                      "index is past the end of the array. Treated as "
                      "\"no driver\" - the tint is NOT being dimmed.");
        return;
    }
    if (isPower)
        ImGui::Text("%s: %s%s = %.3f -> %s", what, d.dref.c_str(), idx, v,
                    v > (double)d.floorF ? "ON" : "OFF (target skipped)");
    else
        ImGui::Text("%s: %s%s = %.3f -> x%.2f", what, d.dref.c_str(), idx, v,
                    FxDriverFactor(d, shape));
}

// The editable half. Returns true when something changed.
static bool FxDriverEditor(const char* id, FxDriver& d, bool isPower) {
    bool changed = false;
    ImGui::PushID(id);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", d.dref.c_str());
    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::InputText("dataref", buf, sizeof(buf))) {
        d.dref = buf;
        d.tried = false; d.ref = nullptr;   // re-resolve on the next read
        changed = true;
    }
    if (ImGui::IsItemHovered())
        UiTooltip("Empty = follow whatever drives each rect of this "
                  "target (the built-in table). A name here overrides "
                  "that for the WHOLE target, which is what you want on "
                  "a group whose members share one knob.");

    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("index", &d.index, 0)) {
        if (d.index < -1) d.index = -1;
        d.tried = false; d.ref = nullptr;
        changed = true;
    }
    if (ImGui::IsItemHovered())
        UiTooltip("Array element. -1 for a scalar dataref.");

    ImGui::SameLine();
    if (isPower) {
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputFloat("above", &d.floorF, 0.0f, 0.0f, "%.2f")) changed = true;
        if (ImGui::IsItemHovered())
            UiTooltip("The target draws only while the value is ABOVE "
                      "this. 0.5 for an on/off flag.");
    } else {
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputFloat("off at", &d.lo, 0.0f, 0.0f, "%.2f")) changed = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputFloat("full at", &d.hi, 0.0f, 0.0f, "%.2f")) changed = true;
        if (ImGui::IsItemHovered())
            UiTooltip("The two dataref values that map to opacity x0 and "
                      "x1. \"off at\" ABOVE \"full at\" inverts the "
                      "source - that is how a fault flag or a dim knob "
                      "is expressed.");
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::SliderFloat("floor", &d.floorF, 0.0f, 1.0f, "x%.2f")) changed = true;
        if (ImGui::IsItemHovered())
            UiTooltip("Never dim below this. A readout that keeps a "
                      "little tint with the knob at its stop looks more "
                      "like glass than one that vanishes.");
    }

    ImGui::PopID();
    return changed;
}

// One of the two per-target tri-states as a combo. The Inherit entry names the
// value it is inheriting ("Inherit (follow)") rather than saying just "Inherit",
// because the master switch lives in a different pane and the whole question
// this pane answers is "what is this target actually doing right now".
static bool FxTriCombo(const char* label, int& tri, bool inherited,
                       const char* offText, const char* onText, const char* help) {
    char inheritLabel[96];
    std::snprintf(inheritLabel, sizeof(inheritLabel), "Inherit (%s)",
                  inherited ? onText : offText);
    const char* items[3] = { inheritLabel, offText, onText };
    int cur = tri < 0 ? 0 : (tri ? 2 : 1);
    ImGui::SetNextItemWidth(190.0f);
    const bool changed = ImGui::Combo(label, &cur, items, 3);
    if (ImGui::IsItemHovered()) UiTooltip("%s", help);
    if (changed) tri = cur == 0 ? kFxInherit : (cur == 2 ? kFxOn : kFxOff);
    return changed;
}

static void BuildFxSourcePane(FxStack& stack) {
    if (!gRectDriversBuilt) BuildRectDrivers();
    if (!ImGui::CollapsingHeader("Brightness / power sources")) return;

    ImGui::Indent(8.0f);
    if (!gFxFollowBrightness)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "\"Follow display brightness\" is OFF - nothing below "
                           "is being applied, on ANY target.");

    // The two switches, per target. They sit above the sources rather than beside
    // the master checkboxes because what they answer is "how does THIS face
    // behave", and the answer has to be readable next to the live values below.
    if (FxTriCombo("dimming", stack.follow, true,
                   "Full strength when powered", "Follow the knob",
                   "Follow the knob: this target's layers scale with its "
                   "brightness driver.\n"
                   "Full strength when powered: the driver is ignored, but the "
                   "POWER gate still applies - the tint is either fully there or "
                   "not there at all. Right for a face whose backlight does not "
                   "actually track the readout brightness.\n"
                   "Either way the master \"Follow display brightness\" switch, "
                   "when off, still wins."))
        SavePanelFx();
    ImGui::SameLine();
    if (FxTriCombo("response", stack.curve, gFxCurve, "Linear", "Curved",
                   "Curved: lo..hi is reshaped by kBrightnessCurve, so the "
                   "bottom fifth of the knob is dark and half a turn is still "
                   "dim - the same curve the screen-glow lights use.\n"
                   "Linear: straight from lo to hi. For tuning, or for a source "
                   "that is already a brightness rather than a knob position.\n"
                   "Has no effect when dimming is off."))
        SavePanelFx();

    const bool follows = FxStackFollows(&stack);
    const bool curves  = FxStackCurves(&stack);
    // The table below is per RECT and describes the target, so it reads every
    // driver through the TARGET's shape. A layer with knots of its own is shown
    // on the layer, where the shape it actually uses can be seen beside it.
    const FxCurve* shape = FxShapeForStack(&stack);
    if (gFxFollowBrightness && !follows)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "This target is NOT dimmed - full strength whenever "
                           "its power driver says on.");

    // The curve, drawn. A response curve is a shape, and a shape argued for in a
    // comment is a shape nobody checks; plotted next to the live factors it is
    // obvious at a glance whether a face is dim because the knob is down or
    // because the dataref is wrong. Sampled from BrightnessResponse itself, so
    // this cannot drift from what the compositor applies.
    {
        const int kPlot = 64;
        float samples[kPlot];
        for (int i = 0; i < kPlot; ++i) {
            const float x = (float)i / (float)(kPlot - 1);
            samples[i] = curves ? BrightnessResponse(x) : x;
        }
        char overlay[64];
        std::string knots;
        for (int i = 0; i < kBrightnessCurveCount; ++i) {
            char k[32];
            std::snprintf(k, sizeof(k), "%.2f>%.2f", kBrightnessCurve[i].x,
                          kBrightnessCurve[i].y);
            if (i) knots += "  ";
            knots += k;
        }
        std::snprintf(overlay, sizeof(overlay), "%s",
                      !follows ? "not dimmed"
                               : (curves ? "knob -> opacity" : "linear"));
        ImGui::PlotLines("##fxcurve", samples, kPlot, 0, overlay, 0.0f, 1.0f,
                         ImVec2(0.0f, 48.0f));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", knots.c_str());
        if (ImGui::IsItemHovered())
            UiTooltip("Knots of kBrightnessCurve in plugin.cpp, monotone "
                      "cubic between them.\nThe screen-glow spill lights "
                      "run through the same curve, unconditionally - the "
                      "response switch only affects this target's tint.");

        // ⚠ Said HERE, next to the plot it contradicts. A layer's own knots
        // outrank this switch, so a stack with one is a stack where the shape
        // drawn above is not the shape being applied - and the pane that shows
        // a curve is the last place that should be quietly wrong about it.
        int authored = 0;
        for (size_t i = 0; i < stack.layers.size(); ++i)
            if (stack.layers[i].curve.on && stack.layers[i].curve.knots.size() >= 2)
                ++authored;
        if (authored > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                               "%d layer(s) here draw their OWN curve instead of "
                               "this one.", authored);
            UiItemTooltip("A layer with a custom curve ignores the response "
                          "switch above, including \"Linear\". Its shape is "
                          "plotted on the layer itself, under \"custom curve\".");
        }
    }

    // What is actually in force, which is the question. A group with no override
    // is following one driver PER MEMBER, so the members are listed with their
    // own live factors rather than pretending there is a single answer.
    //
    // ⚠ There used to be TWO driver blocks on this pane — a live readout of the
    // stack's own sources here, and the editor below the table — both headed
    // "brightness" and "power", and the pair read as two different features
    // rather than as one thing shown twice (reported 2026-08-02). The readout is
    // gone: the table already resolves the override per rect and shows the live
    // value, so it was the same information with a different layout. What is left
    // is one table that says what IS, under a heading that says what the two
    // columns MEAN, and one editor under a heading that says what an override is.
    const PanelMask mask = PanelTargetMask(stack.target);
    const bool overridden = stack.bright.set() || stack.power.set();

    ImGui::Spacing();
    ImGui::SeparatorText("What drives each face of this target");
    ImGui::TextWrapped(
        "Every face reads two datarefs. BRIGHTNESS is how bright the real unit's "
        "own lighting is right now, and it scales this target's opacity, so the "
        "tint comes up and down with the knob. POWER asks whether the unit is on "
        "at all, and it is a hard on/off - not a dim. Both are looked up PER FACE "
        "in the built-in table, which is why a group can follow six knobs at once.");
    if (overridden)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                           "This target overrides the built-in %s below, so every "
                           "row shows the override instead.",
                           stack.bright.set() && stack.power.set() ? "brightness and power"
                             : stack.bright.set() ? "brightness" : "power");
    // ⚠ Bus and breaker get a COLUMN EACH (2026-08-02). They shared one headed
    // "bus / cb", stacking two lines in a cell, which reads as one gate with two
    // readings — and they are not remotely the same question: the bus is a
    // VOLTAGE on a shared supply and the breaker is a per-face switch. Worse, an
    // empty cell was ambiguous between "no breaker on this face" and "the breaker
    // table is empty" (it is, deliberately), so the one column that should be
    // conspicuously blank looked like a rendering detail of the one beside it.
    if (ImGui::BeginTable("fxsrc", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("rect");
        ImGui::TableSetupColumn("brightness");
        ImGui::TableSetupColumn("power");
        ImGui::TableSetupColumn("bus");
        ImGui::TableSetupColumn("breaker");
        ImGui::TableSetupColumn("x");
        ImGui::TableHeadersRow();
        const ImVec4 kBad  (1.00f, 0.45f, 0.35f, 1.0f);   // unreadable
        const ImVec4 kGated(1.00f, 0.75f, 0.30f, 1.0f);   // read fine, gating off
        for (int i = 0; i < kPanelRectCount; ++i) {
            if (!(mask & ((PanelMask)1 << i))) continue;
            FxDriver& br = stack.bright.set() ? stack.bright : gRectBright[i];
            FxDriver& pw = stack.power.set()  ? stack.power  : gRectPower[i];
            // ⚠ Never a stack override — the electrical gates are the rect's own,
            // by design. See FxRectFactor.
            FxDriver& bus = gRectBus[i];
            FxDriver& cb  = gRectCb[i];

            // Read each ONCE per row and reuse: the cells, the factor and the
            // coloring must all describe the same frame, and a knob being turned
            // while the table is open would otherwise show a value that does not
            // explain the factor beside it.
            double bv = 0.0, pv = 0.0;
            const bool bok = br.set() && FxDriverRaw(br, bv);
            const bool pok = pw.set() && FxDriverRaw(pw, pv);
            const bool gated   = pok && !(pv > (double)pw.floorF);
            const bool busDead = bus.set() && !FxDriverPowered(bus);
            const bool cbOut   = cb.set()  && !FxDriverPowered(cb);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kPanelRectsHi[i].name);

            ImGui::TableSetColumnIndex(1);
            if (!br.set())      ImGui::TextDisabled("-");
            else if (!follows)  ImGui::TextDisabled("%s  (not dimmed)", br.dref.c_str());
            else if (!bok)      ImGui::TextColored(kBad, "%s", FxDriverState(br, false, shape).c_str());
            else                ImGui::TextUnformatted(FxDriverState(br, false, shape).c_str());

            ImGui::TableSetColumnIndex(2);
            if (!pw.set())      ImGui::TextDisabled("-");
            else if (!pok)      ImGui::TextColored(kBad, "%s", FxDriverState(pw, true, shape).c_str());
            else if (gated)     ImGui::TextColored(kGated, "%s", FxDriverState(pw, true, shape).c_str());
            else                ImGui::TextUnformatted(FxDriverState(pw, true, shape).c_str());

            ImGui::TableSetColumnIndex(3);
            if (!bus.set())     ImGui::TextDisabled("-");
            else if (busDead)   ImGui::TextColored(kGated, "%s", FxDriverState(bus, true, shape).c_str());
            else                ImGui::TextUnformatted(FxDriverState(bus, true, shape).c_str());

            ImGui::TableSetColumnIndex(4);
            if (!cb.set())      ImGui::TextDisabled("-");
            else if (cbOut)     ImGui::TextColored(kGated, "%s", FxDriverState(cb, true, shape).c_str());
            else                ImGui::TextUnformatted(FxDriverState(cb, true, shape).c_str());

            ImGui::TableSetColumnIndex(5);
            gFxDrawStack = &stack;
            const float f = FxRectFactor(i);
            gFxDrawStack = nullptr;
            // ⚠ Name WHICH driver produced a zero. Several roads end at x0.00 and
            // they need different fixes - a gate that is doing its job versus a
            // brightness dataref whose units are not what lo/hi assume - and for a
            // long time this column showed only the number. The order matches
            // FxRectFactor's, so the cause named is the one that actually
            // returned first.
            //
            // ⚠ THE DISPLAYS-TAB SWITCH IS FIRST, and its absence here was a real
            // hole (2026-08-02). FxRectFactor evaluates FxDisplayUserFactor before
            // anything else, but this chain had no case for it, so a target
            // switched off on the Displays tab fell through to the final `else`
            // and was reported as "x0.00 bright" - i.e. the pane blamed the
            // aircraft's rheostat for a switch in our own UI two tabs away. That
            // is precisely the wrong-cause-named failure this column exists to
            // prevent, and it is worse than showing a bare number because a
            // confident wrong answer sends you to the dataref tables.
            //
            // It also names WHICH switch, because the screen/other split is per
            // RECT: a group target can span both, so "the switch is off" is not
            // answerable for a stack, only for a row.
            const float userF = FxDisplayUserFactor(i);
            if (userF <= 0.0f)
                ImGui::TextColored(kGated, "x0.00 Displays: %s off",
                                   PanelRectIsScreen(i) ? "screens" : "other readouts");
            else if (f >= 0.999f) ImGui::TextDisabled("x1.00");
            else if (gated)     ImGui::TextColored(kGated, "x0.00 power");
            else if (busDead)   ImGui::TextColored(kGated, "x0.00 bus");
            else if (cbOut)     ImGui::TextColored(kGated, "x0.00 CB");
            else if (f <= 0.0f) ImGui::TextColored(kGated, "x0.00 bright");
            else                ImGui::Text("x%.2f", f);
            // The user gain is a multiplier the driver columns cannot account
            // for, so a row reading x0.30 with every source healthy is otherwise
            // unexplainable from this table alone.
            if (userF > 0.0f && userF < 0.999f) {
                ImGui::SameLine();
                ImGui::TextDisabled("(x%.2f gain)", userF);
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Override those sources for this target");
    // ⚠ The two editors below are IDENTICAL in shape - a dataref name and an
    // index - and used to sit under one grey line reading "Override for this
    // target", with nothing between them but a separator. That is what made this
    // pane unreadable: the only thing distinguishing "the knob that dims it" from
    // "the switch that turns it off" was the order they happened to be drawn in.
    // Each now carries its own heading and says what leaving it empty does.
    ImGui::TextWrapped(
        "The table above comes from a table compiled into the plugin - a reading "
        "of ToLiss's own datarefs, and below the six displays most of it is an "
        "educated guess. Naming a dataref here replaces it for the WHOLE target, "
        "which is what you want when the built-in guess is wrong, or on a group "
        "whose faces really do share one source. ONLY what you type here is saved "
        "to panelfx.txt; the built-in table is not written out and cannot be "
        "edited from the sim.");
    bool changed = false;

    ImGui::Spacing();
    ImGui::TextUnformatted("Brightness  - scales opacity");
    UiItemTooltip("How bright the real unit is, 0..1 after \"off at\"/\"full at\". "
                  "Empty = each face keeps its own built-in brightness source.");
    ImGui::Indent(8.0f);
    changed |= FxDriverEditor("bright", stack.bright, false);
    ImGui::Unindent(8.0f);

    ImGui::Spacing();
    ImGui::TextUnformatted("Power  - draws or does not");
    UiItemTooltip("Whether the unit is switched on. A hard on/off, never a dim: "
                  "below the threshold this target paints nothing at all. Empty = "
                  "each face keeps its own built-in power source.\n\n"
                  "This is the only one of the four gates a look may override. The "
                  "bus and the breaker are facts about the aircraft's electrical "
                  "state and stay in force whatever is typed here.");
    ImGui::Indent(8.0f);
    changed |= FxDriverEditor("power", stack.power, true);
    ImGui::Unindent(8.0f);

    if (ImGui::Button("Clear overrides")) {
        stack.bright = FxDriver();
        stack.power  = FxDriver();
        changed = true;
    }
    if (ImGui::IsItemHovered())
        UiTooltip("Back to the built-in per-rect sources.");

    ImGui::Unindent(8.0f);

    // Saved, not journalled: a source is plumbing, and filling one journal entry
    // per keystroke in a text box would bury the color edits that the journal
    // is for.
    if (changed) SavePanelFx();
}

// ---- where the knob sits on the curve RIGHT NOW ------------------------------
// The curve editor draws a shape; this is what turns it into an answer to "why
// is this layer invisible". Taken from the FIRST member rect of the target
// (unless the stack overrides the driver, in which case there is only one), and
// the rect is named beside it — a group follows one knob per member, so a single
// marker on a six-member group would otherwise be a number pretending to speak
// for six.
static bool FxCurveLiveX(FxStack& stack, float* outX, const char** outRect) {
    if (!gRectDriversBuilt) BuildRectDrivers();
    FxDriver* d    = nullptr;
    const char* nm = "override";
    if (stack.bright.set()) {
        d = &stack.bright;
    } else {
        const PanelMask mask = PanelTargetMask(stack.target);
        for (int i = 0; i < kPanelRectCount; ++i)
            if ((mask >> i) & 1) {
                if (gRectBright[i].set()) { d = &gRectBright[i]; nm = kPanelRectsHi[i].name; }
                break;
            }
    }
    // ⚠ Through FxDriverNorm, not a normalization of its own. It is the same
    // number the compositor feeds the curve — including while the test slider
    // pins it, which is what makes dragging that slider walk this marker along
    // the shape being edited.
    float x = 0.0f;
    if (!d || !d->set() || !FxDriverNorm(*d, &x)) return false;
    *outX    = x;
    *outRect = nm;
    return true;
}

// ---- the curve editor --------------------------------------------------------
// A knot table typed as numbers is a shape nobody can see, and the whole reason
// a layer wants its own curve is that the default one is the wrong SHAPE for it.
// So it is drawn, and dragged.
//
// What is on the canvas, and why each thing is there:
//
//   * the LINEAR diagonal and the BUILT-IN curve, both faint. A custom curve is
//     only ever authored as a deviation from one of those two, and "how far off
//     the default am I" is unanswerable from a plot of one line.
//   * the curve itself, sampled through FxCurveEval — the same function the
//     compositor calls, so this cannot drift from what is drawn in the cockpit.
//   * the LIVE knob position, as a vertical line and a dot. This is the part
//     that turns the editor into a diagnostic: a layer that never appears with
//     the marker parked in the dead zone is a curve doing its job, and a layer
//     that never appears with the marker at the right-hand edge is not.
//
// ⚠ It is ONE InvisibleButton, not a widget per knot. Dragging has to keep
// working when the pointer leaves the canvas — a knot dragged past the top
// otherwise stops following the mouse at the edge and then jumps when it comes
// back — and holding ActiveId on the canvas is also what makes the layer pane's
// deferred-commit machinery treat a drag as one history entry instead of sixty.
static bool FxCurveEditor(const char* id, FxCurve& curve,
                          bool haveLive, float liveX) {
    bool changed = false;
    ImGui::PushID(id);

    // Nothing below this line handles a curve with fewer than two knots — the
    // insert clamps against neighbours and the drag pins the two ends — and the
    // one way to get one here is a file edited by hand past what the loader
    // rejects. Seed rather than refuse: an editor that draws nothing is a worse
    // answer than one that starts from the default.
    if (curve.knots.size() < 2) {
        curve.knots.assign(kBrightnessCurve, kBrightnessCurve + kBrightnessCurveCount);
        curve.built = false;
    }

    const ImVec2 size(300.0f, 150.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("canvas", size);
    const ImGuiID myId    = ImGui::GetItemID();
    const bool    hovered = ImGui::IsItemHovered();
    const ImVec2  p1(p0.x + size.x, p0.y + size.y);
    ImDrawList*   dl = ImGui::GetWindowDrawList();

    auto toScreen = [&](float x, float y) {
        return ImVec2(p0.x + x * size.x, p1.y - y * size.y);
    };

    // ---- the drag, before the draw so the curve drawn is the curve now ------
    // Static rather than per-editor state because exactly one drag can be in
    // flight at a time; the owning ID is stored with it so a second editor
    // opened mid-drag cannot inherit it.
    static ImGuiID dragOwner = 0;
    static int     dragKnot  = -1;

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int   nearest = -1;
    float nearestD = 11.0f;                       // pixels; a knot is drawn at 4
    for (size_t i = 0; i < curve.knots.size(); ++i) {
        const ImVec2 s = toScreen(curve.knots[i].x, curve.knots[i].y);
        const float  d = std::sqrt((s.x - mouse.x) * (s.x - mouse.x)
                                 + (s.y - mouse.y) * (s.y - mouse.y));
        if (d < nearestD) { nearestD = d; nearest = (int)i; }
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (nearest < 0 && curve.knots.size() < (size_t)kFxCurveMaxKnots) {
            // A click on empty canvas ADDS a knot and picks it up in the same
            // gesture. Requiring a double-click, or a separate "+" button
            // followed by a drag, is two actions for what is one intention.
            CurveKnot k;
            k.x = (mouse.x - p0.x) / size.x;
            k.y = (p1.y - mouse.y) / size.y;
            // Held off both ends: a new knot sharing an endpoint's x makes a
            // zero-width segment, which is not a crash but is a corner the
            // curve visibly jumps at for no reason the canvas can show.
            k.x = k.x < 0.01f ? 0.01f : (k.x > 0.99f ? 0.99f : k.x);
            k.y = k.y < 0.00f ? 0.00f : (k.y > 1.00f ? 1.00f : k.y);
            size_t at = 0;
            while (at < curve.knots.size() && curve.knots[at].x < k.x) ++at;
            // Never in front of the first or behind the last: those two are the
            // ends of the domain and FxCurveNormalize pins their x.
            if (at == 0) at = 1;
            if (at > curve.knots.size() - 1) at = curve.knots.size() - 1;
            curve.knots.insert(curve.knots.begin() + at, k);
            curve.built = false;
            nearest = (int)at;
            changed = true;
        }
        if (nearest >= 0) { dragOwner = myId; dragKnot = nearest; }
    }

    // Right-click removes, but never an endpoint and never below two knots — a
    // curve with one knot is a constant, and one with none is not a curve.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        && nearest > 0 && nearest + 1 < (int)curve.knots.size()) {
        curve.knots.erase(curve.knots.begin() + nearest);
        curve.built = false;
        changed = true;
        nearest = -1;
    }

    if (dragOwner == myId && dragKnot >= 0 && dragKnot < (int)curve.knots.size()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            CurveKnot& k = curve.knots[(size_t)dragKnot];
            float x = (mouse.x - p0.x) / size.x;
            float y = (p1.y - mouse.y) / size.y;
            if (y < 0.0f) y = 0.0f;
            if (y > 1.0f) y = 1.0f;
            // ⚠ The two ends hold their x. The curve has to span the whole 0..1
            // input range or it silently clamps its own first value across the
            // bottom of every knob — see FxCurveNormalize.
            if (dragKnot == 0)                             x = 0.0f;
            else if (dragKnot + 1 == (int)curve.knots.size()) x = 1.0f;
            else {
                const float lo = curve.knots[(size_t)dragKnot - 1].x + 0.01f;
                const float hi = curve.knots[(size_t)dragKnot + 1].x - 0.01f;
                if (x < lo) x = lo;
                if (x > hi) x = hi;
            }
            if (k.x != x || k.y != y) {
                k.x = x; k.y = y;
                curve.built = false;
                changed = true;
            }
        } else {
            dragOwner = 0;
            dragKnot  = -1;
        }
    }

    // ---- the draw -----------------------------------------------------------
    const ImU32 cBg    = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 cBrd   = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 cGrid  = ImGui::GetColorU32(ImGuiCol_Separator, 0.45f);
    const ImU32 cFaint = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.45f);
    const ImU32 cLine  = ImGui::GetColorU32(ImGuiCol_PlotLines);
    const ImU32 cKnot  = ImGui::GetColorU32(ImGuiCol_SliderGrab);
    const ImU32 cHot   = ImGui::GetColorU32(ImGuiCol_SliderGrabActive);
    const ImU32 cLive  = ImGui::GetColorU32(ImGuiCol_PlotHistogram);

    dl->AddRectFilled(p0, p1, cBg);
    for (int g = 1; g < 4; ++g) {
        const float f = (float)g / 4.0f;
        dl->AddLine(toScreen(f, 0.0f), toScreen(f, 1.0f), cGrid);
        dl->AddLine(toScreen(0.0f, f), toScreen(1.0f, f), cGrid);
    }
    dl->AddLine(toScreen(0.0f, 0.0f), toScreen(1.0f, 1.0f), cFaint);

    const int kSamples = 65;
    ImVec2 pts[kSamples];
    for (int i = 0; i < kSamples; ++i) {
        const float x = (float)i / (float)(kSamples - 1);
        pts[i] = toScreen(x, BrightnessResponse(x));
    }
    dl->AddPolyline(pts, kSamples, cFaint, 1.0f, ImDrawFlags_None);
    for (int i = 0; i < kSamples; ++i) {
        const float x = (float)i / (float)(kSamples - 1);
        pts[i] = toScreen(x, FxCurveEval(curve, x));
    }
    dl->AddPolyline(pts, kSamples, cLine, 2.0f, ImDrawFlags_None);

    for (size_t i = 0; i < curve.knots.size(); ++i) {
        const ImVec2 s = toScreen(curve.knots[i].x, curve.knots[i].y);
        const bool   hot = ((int)i == nearest) || ((int)i == dragKnot && dragOwner == myId);
        dl->AddCircleFilled(s, hot ? 6.0f : 4.0f, hot ? cHot : cKnot);
    }

    if (haveLive) {
        const float y = FxCurveEval(curve, liveX);
        dl->AddLine(toScreen(liveX, 0.0f), toScreen(liveX, 1.0f), cLive);
        dl->AddCircleFilled(toScreen(liveX, y), 3.5f, cLive);
    }
    dl->AddRect(p0, p1, cBrd);

    if (hovered && dragOwner != myId)
        UiTooltip("Drag a knot to reshape the curve. Click anywhere on the "
                  "canvas to add one, right-click a knot to remove it - the "
                  "two ends stay put, because the curve has to cover the whole "
                  "knob.\n\n"
                  "Faint diagonal: linear. Faint curve: the built-in response, "
                  "which is what this layer would use without a curve of its "
                  "own. The bright vertical line is where the knob is RIGHT "
                  "NOW.\n\n"
                  "x = the brightness driver, normalised through its off-at / "
                  "full-at range. y = the layer's opacity multiplier.");

    // ---- the presets and the readout ---------------------------------------
    if (ImGui::SmallButton("Built-in")) {
        curve.knots.assign(kBrightnessCurve, kBrightnessCurve + kBrightnessCurveCount);
        curve.built = false;
        changed = true;
    }
    UiItemTooltip("Start from kBrightnessCurve - the shape every layer without "
                  "a curve of its own uses, and the one the screen-glow spill "
                  "lights use unconditionally.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Linear")) {
        curve.knots.clear();
        curve.knots.push_back(CurveKnot{ 0.0f, 0.0f });
        curve.knots.push_back(CurveKnot{ 1.0f, 1.0f });
        curve.built = false;
        changed = true;
    }
    UiItemTooltip("Straight from off-at to full-at.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Late")) {
        // A shape worth one click because it is the one this feature was asked
        // for: nothing until the screen is already bright, then a fast rise. A
        // bleed or a wash that is visible at a quarter turn reads as fog.
        curve.knots.clear();
        curve.knots.push_back(CurveKnot{ 0.00f, 0.0f });
        curve.knots.push_back(CurveKnot{ 0.60f, 0.0f });
        curve.knots.push_back(CurveKnot{ 0.85f, 0.5f });
        curve.knots.push_back(CurveKnot{ 1.00f, 1.0f });
        curve.built = false;
        changed = true;
    }
    UiItemTooltip("Nothing until the knob is well up, then a fast rise - for a "
                  "bleed or a wash that should only appear on a bright screen.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Early")) {
        curve.knots.clear();
        curve.knots.push_back(CurveKnot{ 0.00f, 0.0f });
        curve.knots.push_back(CurveKnot{ 0.15f, 0.5f });
        curve.knots.push_back(CurveKnot{ 0.40f, 0.9f });
        curve.knots.push_back(CurveKnot{ 1.00f, 1.0f });
        curve.built = false;
        changed = true;
    }
    UiItemTooltip("Up almost at once and then nearly flat - for a color "
                  "correction that should be fully there as soon as the screen "
                  "is readable at all.");

    ImGui::SameLine();
    std::string knots;
    for (size_t i = 0; i < curve.knots.size(); ++i) {
        char one[32];
        std::snprintf(one, sizeof(one), "%s%.2f>%.2f", i ? " " : "",
                      curve.knots[i].x, curve.knots[i].y);
        knots += one;
    }
    ImGui::TextDisabled("%d knots", (int)curve.knots.size());
    UiItemTooltip("%s", knots.c_str());

    ImGui::PopID();
    if (changed) FxCurveNormalize(curve);
    return changed;
}

// ---- the per-layer row under the image row ----------------------------------
// Three things that belong to ONE LAYER rather than to its target: the daylight
// half of its look, whether it dims with the knob at all, and — when it does —
// the SHAPE it dims through. All folded into one row because all are usually
// unset, and a stack of five layers each carrying four extra widgets is
// unreadable.
static void BuildFxLayerExtrasRow(FxLayer& layer, FxStack& stack,
                                  bool* dirty, std::string* what) {
    ImGui::Indent(24.0f);

    // ---- does this layer follow the brightness knob? ------------------------
    if (FxTriCombo("dims", layer.follow, FxStackFollows(&stack),
                   "Steady when powered", "Follow the knob",
                   "Follow the knob: this LAYER scales with the screen's "
                   "brightness driver, like the target does.\n"
                   "Steady when powered: it does not dim at all - but it is still "
                   "switched off completely by the power, bus and breaker "
                   "gates. This is the setting for a color CORRECTION, which is a "
                   "property of the glass rather than of the backlight: fading it "
                   "out as the knob comes down lets the readout drift back to its "
                   "untinted color exactly where the correction is most visible.\n"
                   "A backlight wash or a bleed wants the opposite - those ARE the "
                   "screen's light.\n"
                   "Inherit takes the target's setting, which in turn takes the "
                   "master switch.")) {
        *dirty = true; *what = "change layer dimming";
    }

    // ---- the daylight half --------------------------------------------------
    ImGui::SameLine();
    bool has = layer.hasDay;
    if (ImGui::Checkbox("daylight variant", &has)) {
        layer.hasDay = has;
        // Seed the day half FROM the night half rather than from the struct's
        // defaults. Ticking the box must not change what is on screen right now —
        // otherwise the first thing it does is throw away the color being tuned,
        // and the box reads as a reset button.
        if (has) {
            for (int c = 0; c < 3; ++c) layer.dayColor[c] = layer.color[c];
            layer.dayOpacity = layer.opacity;
        }
        *dirty = true; *what = has ? "add daylight variant" : "drop daylight variant";
    }
    UiItemTooltip(
        "A second color and opacity for full daylight. The compositor blends "
        "between the two on how much light is in the cockpit, so this is a fade "
        "through dusk rather than a switch.\n\n"
        "Set the daylight opacity to 0 for a layer that should only exist after "
        "dark; set the NIGHT opacity to 0 and this one above it for a layer that "
        "only exists in daylight. That is how a target gets two looks with "
        "different blend modes - two layers, not two stacks.");

    if (layer.hasDay) {
        ImGui::SameLine();
        if (ImGui::ColorEdit3("##daycol", layer.dayColor,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            *dirty = true; *what = "change daylight color";
        }
        UiItemTooltip("The color at full daylight.");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderFloat("##dayop", &layer.dayOpacity, 0.0f, 1.0f, "day %.3f")) {
            *dirty = true; *what = "change daylight opacity";
        }
        UiItemTooltip("The opacity at full daylight. 0 makes this a night-only "
                      "layer.");

        // What the blend is doing to this layer RIGHT NOW. The same argument as
        // the driver rows above: two numbers and an ambient value that is not
        // moving are the difference between "the blend is wrong" and "it is
        // working and it is midnight".
        ImGui::SameLine();
        const FxLayerNow now = FxLayerAtAmbient(layer);
        ImGui::TextDisabled("-> %.3f @ %.2f", now.opacity, FxAmbientNow());
        UiItemTooltip("The opacity actually being drawn, and the ambient blend "
                      "producing it (0 = night, 1 = day). Pin the blend on the "
                      "Ambient pane to author either end.");
    }

    // ---- the color ramp ----------------------------------------------------
    // ⚠ Its own line, not appended to the daylight row: that row already carries
    // four widgets when the box is ticked, and these two questions are asked at
    // different times — the daylight half is about the hour, this is about the
    // knob.
    bool ramp = layer.hasRamp;
    if (ImGui::Checkbox("color ramp", &ramp)) {
        layer.hasRamp = ramp;
        // Seeded FROM the layer's own color, like the daylight box above and
        // for the identical reason: ticking it must not change what is on
        // screen. Start == end is a ramp that does nothing, which is the only
        // honest starting point for one.
        if (ramp) for (int c = 0; c < 3; ++c) layer.rampColor[c] = layer.color[c];
        *dirty = true; *what = ramp ? "add color ramp" : "drop color ramp";
    }
    UiItemTooltip(
        "A second color for a DIM screen. The compositor interpolates between "
        "it and the layer's own color on the brightness driver, so the swatch "
        "above becomes the color at a full knob and this one the color at a "
        "dark one.\n\n"
        "This is what a dim LCD actually does - it goes warm, its backlight "
        "reddens, its black stops being lifted - and none of that is the "
        "authored color more faintly, which is all dimming can produce.\n\n"
        "It does NOT need \"Follow the knob\": that switch is about opacity, "
        "this is about color, and a correction meant to stay at full strength "
        "while powered is the layer most likely to want a ramp. The master "
        "\"Follow display brightness\" switch still turns both off, and a dim "
        "knob still cannot un-gate a dead screen.");

    if (layer.hasRamp) {
        ImGui::SameLine();
        if (ImGui::ColorEdit3("##rampcol", layer.rampColor,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            *dirty = true; *what = "change ramp start color";
        }
        UiItemTooltip("The color at a dark knob. The swatch on the layer row is "
                      "the color at a full one.");

        // What is being MIXED right now, and from what. The same argument as the
        // daylight row's live readout: a ramp that looks like it does nothing and
        // a knob sitting at the top are the same picture without the number, and
        // the swatch is the half of it a color value cannot replace.
        //
        // ⚠ Through FxCurveLiveX — the SAME reader the curve editor's marker
        // uses, because the ramp now interpolates on the same raw knob position
        // that marker shows. Two live readouts in one pane disagreeing about
        // where the knob is would be worse than either one missing.
        ImGui::SameLine();
        float knob = 1.0f;
        const char* rectName = nullptr;
        const FxLayerNow now = FxLayerAtAmbient(layer);
        if (FxCurveLiveX(stack, &knob, &rectName)) {
            float mixed[3];
            FxLayerColorAtDrive(layer, now.color, knob, mixed);
            ImGui::ColorButton("##rampnow", ImVec4(mixed[0], mixed[1], mixed[2], 1.0f),
                               ImGuiColorEditFlags_NoTooltip, ImVec2(20.0f, 20.0f));
            UiItemTooltip("The color being painted right now: the mix at knob "
                          "%.3f, read from \"%s\".\n\n"
                          "This is the raw dataref position, NOT the response "
                          "curve's output - the ramp follows where the knob is, "
                          "not how strongly the layer is being painted there.\n\n"
                          "Hold the slider under \"Brightness knob override\" at "
                          "the top of this tab to sweep it without leaving the "
                          "editor.", knob, rectName ? rectName : "?");
            ImGui::SameLine();
            ImGui::TextDisabled("mix @ knob %.2f", knob);
        } else {
            ImGui::TextDisabled("(no member rect to read a knob from)");
        }
    }

    // ---- the response curve, for this layer alone ---------------------------
    // ⚠ Its own line rather than appended to the row above: with a canvas and
    // four preset buttons it is as wide as the pane on its own.
    bool hasCurve = layer.curve.on;
    if (ImGui::Checkbox("custom curve", &hasCurve)) {
        layer.curve.on = hasCurve;
        // Seeded from the shape IN FORCE, not from the struct's defaults, and
        // only when there is nothing to keep. Ticking the box must not change
        // what is on screen — the same argument as the daylight checkbox above,
        // and the reason unticking it leaves the knots alone is the other half:
        // the box is how you A/B a curve against the default, so it cannot be
        // destructive in either direction.
        if (hasCurve && layer.curve.knots.size() < 2) {
            if (const FxCurve* cur = FxShapeForStack(&stack)) {
                layer.curve.knots = cur->knots;
            } else {
                layer.curve.knots.clear();
                layer.curve.knots.push_back(CurveKnot{ 0.0f, 0.0f });
                layer.curve.knots.push_back(CurveKnot{ 1.0f, 1.0f });
            }
            layer.curve.built = false;
        }
        *dirty = true; *what = hasCurve ? "add layer curve" : "drop layer curve";
    }
    UiItemTooltip(
        "Give this layer its own shape for how the brightness knob becomes its "
        "opacity, instead of the target's response setting.\n\n"
        "This is what to reach for when two layers of one target need to come up "
        "at different points on the knob - a bleed that should only show on an "
        "already-bright screen, over a color correction that has to be fully "
        "there the moment the readout is readable. One curve on the target "
        "cannot express both.\n\n"
        "It overrides the target's response switch, including \"Linear\". It has "
        "no effect at all on a layer that does not dim, and it NEVER reaches the "
        "screen-glow spill lights - those always use the built-in curve.");

    if (layer.curve.on) {
        ImGui::Indent(8.0f);
        // What the curve is being fed. A shape with no input marked on it is
        // half the diagnostic: "the layer never appears" and "the knob never
        // leaves the dead zone" look identical without it.
        float liveX = 0.0f;
        const char* liveRect = nullptr;
        const bool haveLive = FxCurveLiveX(stack, &liveX, &liveRect);
        if (FxCurveEditor("curve", layer.curve, haveLive, liveX)) {
            *dirty = true; *what = "edit layer curve";
        }

        if (!FxLayerFollows(&layer, &stack))
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                               "This layer does not dim, so the curve is not "
                               "being applied.");
        else if (haveLive)
            ImGui::TextDisabled("knob %.3f -> x%.3f   (from %s)", liveX,
                                FxCurveEval(layer.curve, liveX), liveRect);
        else
            ImGui::TextDisabled("no readable brightness driver on this target - "
                                "nothing is being read through the curve");
        ImGui::Unindent(8.0f);
    }

    ImGui::Unindent(24.0f);
}

static void BuildFxLayerPane() {
    if (gFxSelectedTarget < 0) {
        ImGui::TextUnformatted("Pick a target on the left.");
        UiHint("Selecting one shows its layers; \"Add layer\" is what starts a "
               "stack. Right-click a target to copy or paste a whole stack.");
        return;
    }

    // ⚠ The selected target need NOT have a stack — selecting stopped creating
    // one. Everything below therefore works from `target`, and `si` is only
    // valid where it has been re-checked.
    const int target = gFxSelectedTarget;
    const int si     = FxStackIndex(target);

    ImGui::SeparatorText(PanelTargetName(target));

    // What the hierarchy does, said where it is acted on. A group's layers are
    // already on the pixels by the time a member's layers run, so a member is
    // refining a result rather than replacing one.
    const int parent = PanelTargetParent(target);
    if (parent >= 0 && FxStackIndex(parent) >= 0)
        UiHint("Composites on top of \"%s\", which is applied first.",
               PanelTargetName(parent));

    if (ImGui::Button("Add layer")) {
        // Added at the TOP of the list, which is the end of the vector: a new
        // layer goes on top of the look so far, the same as every paint program.
        FxEdit edit;
        FxEnsureStack(target).layers.push_back(FxLayer());
        edit.Commit("add layer");
        return;                            // gFxStacks may have been reallocated
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!gFxClipboardFull);
    const bool pasteLayer = ImGui::Button("Paste layer");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        UiTooltip(gFxClipboardFull
                  ? "Add a copy of the clipboard layer on top"
                  : "Nothing copied yet - use a layer's C button");
    if (pasteLayer) {
        FxEdit edit;
        FxEnsureStack(target).layers.push_back(gFxClipboard);
        edit.Commit(std::string("paste layer onto ") + PanelTargetName(target));
        return;
    }

    // The whole-stack clipboard, mirrored from the tree's right-click menu.
    // Discoverability: a gesture that exists only on right-click is a gesture
    // most of its users never find.
    // Disabled rather than inert: a button that accepts the click and does
    // nothing is the same failure this pass is fixing elsewhere in the tab.
    ImGui::SameLine();
    ImGui::BeginDisabled(si < 0 || gFxStacks[si].layers.empty());
    if (ImGui::Button("Copy stack")) {
        gFxStackClipboard     = gFxStacks[si].layers;
        gFxStackClipboardFrom = PanelTargetName(target);
        Log("panel fx: copied " + std::to_string(gFxStackClipboard.size())
            + " layer(s) from " + gFxStackClipboardFrom);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        UiTooltip("Copy every layer here, to paste onto another target");

    ImGui::SameLine();
    ImGui::BeginDisabled(gFxStackClipboard.empty());
    const bool pasteStack = ImGui::Button("Paste stack");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (gFxStackClipboard.empty())
            UiTooltip("Nothing copied yet - use Copy stack, or right-click "
                      "a target in the tree");
        else
            UiTooltip("Add all %d layer(s) from \"%s\" on top.\n"
                      "Right-click a target in the tree to REPLACE instead.",
                      (int)gFxStackClipboard.size(),
                      gFxStackClipboardFrom.c_str());
    }
    if (pasteStack) {
        // Add on top, matching "Paste layer" right beside it. Replace is on the
        // tree's right-click menu, where it cannot be hit by reflex.
        FxEdit edit;
        FxStack& st = FxEnsureStack(target);
        st.layers.insert(st.layers.end(),
                         gFxStackClipboard.begin(), gFxStackClipboard.end());
        edit.Commit(std::string("paste layers onto ") + PanelTargetName(target)
                    + " (add on top)");
        return;
    }

    if (si < 0) {
        UiHint("No layers on this target yet - \"Add layer\" starts a stack, or "
               "paste one copied from another target. Nothing is saved for this "
               "target until then.");
        return;
    }
    FxStack& stack = gFxStacks[si];

    ImGui::SameLine();
    if (ImGui::Button("Delete target")) {
        FxDeleteTarget(target);
        return;                            // stack is gone; do not touch it below
    }
    ImGui::SameLine();
    UiHint("(topmost draws last; the number is composite order)");

    BuildFxSourcePane(stack);

    int moveFrom = -1, moveTo = -1, remove = -1, copy = -1;
    bool dirty = false;
    std::string dirtyWhat;

    // ⚠ Listed TOP-DOWN: row 0 is the LAST layer to composite. The vector stays
    // bottom-first because that is the order the compositor walks it in, and so
    // does the save file; only the display is reversed. Everything below that
    // works in terms of `i` is therefore in composite order, and only the loop
    // bound and the two arrow buttons know about the flip.
    const int layerCount = (int)stack.layers.size();
    const std::string before = SerializeFxStacks();
    for (int row = 0; row < layerCount; ++row) {
        const int i = layerCount - 1 - row;
        FxLayer& layer = stack.layers[i];
        ImGui::PushID(i);
        ImGui::Separator();

        if (ImGui::Checkbox("##on", &layer.enabled)) { dirty = true; dirtyWhat = "toggle layer"; }
        if (ImGui::IsItemHovered()) UiTooltip("Draw this layer");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##blend", &layer.blend, kFxBlendName, kFxBlendCount)) {
            dirty = true; dirtyWhat = "change blend mode";
        }
        if (ImGui::IsItemHovered()) UiTooltip("%s", kFxBlendHelp[layer.blend]);

        ImGui::SameLine();
        if (ImGui::ColorEdit3("##col", layer.color,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            dirty = true; dirtyWhat = "change layer color";
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("##opacity", &layer.opacity, 0.0f, 1.0f, "%.3f")) {
            dirty = true; dirtyWhat = "change layer opacity";
        }
        if (ImGui::IsItemHovered()) UiTooltip("Opacity");

        // Reordering: arrows always work; drag-and-drop is the faster way once
        // there are more than two or three layers.
        //
        // ⚠ Up moves toward the top of the LIST, which is toward the END of the
        // vector — the flip lives here and nowhere else in this loop.
        ImGui::SameLine();
        if (ImGui::ArrowButton("up", ImGuiDir_Up)
            && i + 1 < layerCount)                            { moveFrom = i; moveTo = i + 1; }
        ImGui::SameLine();
        if (ImGui::ArrowButton("dn", ImGuiDir_Down) && i > 0) { moveFrom = i; moveTo = i - 1; }

        ImGui::SameLine();
        char grip[64];
        std::snprintf(grip, sizeof(grip), " %d ", i);
        // ⚠ Nothing between this and BeginDragDropSource — not even a tooltip.
        // The drag source binds to the LAST item, and SetTooltip opens a window
        // of its own in between.
        ImGui::Button(grip);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("PHOTON_FX_LAYER", &i, sizeof(int));
            ImGui::Text("layer %d - %s", i, kFxBlendName[layer.blend]);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PHOTON_FX_LAYER")) {
                moveFrom = *(const int*)p->Data;
                moveTo   = i;
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("C")) copy = i;
        if (ImGui::IsItemHovered()) UiTooltip("Copy this layer's style");

        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = i;
        if (ImGui::IsItemHovered()) UiTooltip("Delete this layer");

        BuildFxLayerImageRow(layer, &dirty, &dirtyWhat);
        BuildFxLayerExtrasRow(layer, stack, &dirty, &dirtyWhat);

        ImGui::PopID();
    }

    // Copying touches nothing, so it is not an edit and does not commit.
    if (copy >= 0 && copy < (int)stack.layers.size()) {
        gFxClipboard     = stack.layers[copy];
        gFxClipboardFull = true;
        Log(std::string("panel fx: copied layer ") + std::to_string(copy) + " ("
            + kFxBlendName[gFxClipboard.blend] + ")");
    }

    // Structural edits are applied after the loop — mutating the vector while
    // iterating it would invalidate the reference the next iteration reads.
    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo
        && moveFrom < (int)stack.layers.size() && moveTo < (int)stack.layers.size()) {
        FxLayer moved = stack.layers[moveFrom];
        stack.layers.erase(stack.layers.begin() + moveFrom);
        stack.layers.insert(stack.layers.begin() + moveTo, moved);
        dirty = true; dirtyWhat = "reorder layers";
    }
    if (remove >= 0 && remove < (int)stack.layers.size()) {
        stack.layers.erase(stack.layers.begin() + remove);
        dirty = true; dirtyWhat = "delete layer";
    }
    // ⚠ Commit on any change, not on a Save button. The point of this tool is
    // that a look survives the sim restart that in-sim tuning requires; a tuning
    // pass lost to a forgotten click is the exact friction it exists to remove.
    //
    // A slider drag commits every frame it moves, which would bury the journal in
    // near-identical records — so a continuous edit only commits when the widget
    // is RELEASED. The value is live either way; only the history entry waits.
    static bool pendingContinuous = false;
    static std::string pendingBefore, pendingWhat;
    if (dirty && ImGui::IsAnyItemActive()) {
        if (!pendingContinuous) {
            pendingContinuous = true;
            pendingBefore     = before;
            pendingWhat       = dirtyWhat;
        }
        SavePanelFx();                     // value is live and on disk; history waits
    } else if (dirty) {
        FxCommit(dirtyWhat, pendingContinuous ? pendingBefore : before);
        pendingContinuous = false;
    } else if (pendingContinuous && !ImGui::IsAnyItemActive()) {
        FxCommit(pendingWhat, pendingBefore);
        pendingContinuous = false;
    }
}

// ---- history pane ----------------------------------------------------------
static void BuildFxHistoryPane() {
    if (!gFxHistoryLoaded) LoadFxJournal();

    if (ImGui::Button("Undo")) FxUndoOnce();
    ImGui::SameLine();
    if (ImGui::Button("Redo")) FxRedoOnce();
    ImGui::SameLine();
    ImGui::TextDisabled("%d undo / %d redo", (int)gFxUndo.size(), (int)gFxRedo.size());
    ImGui::SameLine();
    if (ImGui::Button("Reload journal")) LoadFxJournal();

    UiHint("Every edit is journalled to %s. Restore brings a snapshot back as a "
           "new edit, so nothing is lost by trying one.",
           PanelFxJournalPath().string().c_str());

    ImGui::Separator();
    if (!ImGui::BeginTable("hist", 3,
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg
                         | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
        return;
    ImGui::TableSetupColumn("When",  ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("Edit");
    ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    // Newest first: the entry you want is nearly always the most recent one.
    int restore = -1;
    for (int i = (int)gFxJournal.size() - 1; i >= 0; --i) {
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(gFxJournal[i].when.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(gFxJournal[i].what.c_str());
        if (ImGui::IsItemHovered() && !gFxJournal[i].text.empty())
            UiTooltip("%s", gFxJournal[i].text.c_str());
        ImGui::TableSetColumnIndex(2);
        if (ImGui::SmallButton("Restore")) restore = i;
        ImGui::PopID();
    }
    ImGui::EndTable();

    if (restore >= 0)
        FxRestore(gFxJournal[restore].text,
                  "restore " + gFxJournal[restore].when);
}

// ---- the day/night blend, and the one control that makes it authorable ------
// Folded away like the source pane: it is calibrated once and then read, not
// touched. What it must show while open is the LIVE value, for the same reason
// every driver row shows one — a blend that is stuck reads exactly like a blend
// that is working at 3 a.m.
static void BuildFxAmbientPane() {
    if (!ImGui::CollapsingHeader("Ambient day / night blend")) return;
    ImGui::Indent(8.0f);

    const bool pinned = gFxAmbientManual >= 0.0f;
    const float shown = FxAmbientNow();

    ImGui::Text("ambient %.3f", shown);
    ImGui::SameLine();
    ImGui::ProgressBar(shown, ImVec2(160.0f, 0.0f), shown < 0.5f ? "night" : "day");
    ImGui::SameLine();
    if (pinned) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "PINNED");
    else        ImGui::TextDisabled("raw %.3f -> smoothing %.3f",
                                    gFxAmbientRaw, gFxAmbient);
    UiItemTooltip("raw is what the source dataref reads right now; the second "
                  "number is that mapped through night/day below and smoothed. "
                  "A layer with no daylight variant ignores all of this.");

    // ⚠ The pin is what makes authoring the daylight half possible at all. The
    // alternative is changing the sim's time of day and waiting out the
    // smoothing, twice, for every edit.
    bool pin = pinned;
    if (ImGui::Checkbox("Pin", &pin)) {
        gFxAmbientManual = pin ? shown : -1.0f;
    }
    UiItemTooltip("Hold the blend at a value instead of reading the cockpit, so "
                  "the daylight end of a look can be authored after dark. DEV "
                  "ONLY and never saved - a shipping build always reads the sim.");
    if (pin) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderFloat("##pinned", &gFxAmbientManual, 0.0f, 1.0f,
                           "%.2f  (0 night, 1 day)");
    }

    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::InputFloat("night at", &gFxAmbientLo, 0.0f, 0.0f, "%.3f")) SavePanelFx();
    UiItemTooltip("The source reading that means full night. Not 0: the cockpit "
                  "tint sits well above zero under any moon, so the useful part "
                  "of its travel is a narrow band and these two are what a tuning "
                  "session moves. Putting this ABOVE \"day at\" inverts the "
                  "blend, the same way lo above hi does on a brightness driver.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::InputFloat("day at", &gFxAmbientHi, 0.0f, 0.0f, "%.3f")) SavePanelFx();
    UiItemTooltip("The source reading that means full daylight.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::InputFloat("smoothing", &gFxAmbientTau, 0.0f, 0.0f, "%.2f s")) SavePanelFx();
    UiItemTooltip("Seconds to close about 63%% of a step. NOT cosmetic: the raw "
                  "value jumps the instant a cloud crosses the sun or the "
                  "aircraft banks, and every screen in the cockpit would shift "
                  "color with it. 0 disables the smoothing entirely.");

    // Which dataref is being read, and the escape hatch to change it. Same shape
    // as the driver editor, and for the same reason: the built-in choice is a
    // reading of ToLiss's binary plus the sim's own documentation, not a measurement.
    ImGui::Spacing();
    if (gFxAmbientSource.set()) {
        FxDriverRow("source", gFxAmbientSource, false, nullptr);
    } else {
        ImGui::TextDisabled("source: max(sim/graphics/misc/cockpit_light_level_{r,g,b})");
        UiItemTooltip("The sim's own cockpit night-tint level, which is what "
                      "ToLiss reads too - those three names are in the AirbusFBW "
                      "plugin binary. MAX of the three rather than an average, "
                      "because the night tint is strongly blue-shifted and an "
                      "average reads a moonlit cockpit as darker than it looks.");
    }
    // ⚠ Deliberately NOT FxDriverEditor. That one also offers lo/hi/floor, and
    // this source does not use them — the calibration is the pair above. Showing
    // three inputs that are silently ignored is how a tuning session gets spent
    // on the wrong two numbers.
    {
        ImGui::PushID("ambientsrc");
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", gFxAmbientSource.dref.c_str());
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::InputText("source dataref", buf, sizeof(buf))) {
            gFxAmbientSource.dref  = buf;
            gFxAmbientSource.tried = false;
            gFxAmbientSource.ref   = nullptr;
            SavePanelFx();
        }
        UiItemTooltip("Empty = the built-in cockpit light level. Anything else "
                      "replaces it wholesale: outside_light_level_r for a value "
                      "that ignores the cockpit's own lamps, or one of ToLiss's "
                      "if it turns out to expose a better one. \"night at\" and "
                      "\"day at\" above still do the calibration.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputInt("index", &gFxAmbientSource.index, 0)) {
            if (gFxAmbientSource.index < -1) gFxAmbientSource.index = -1;
            gFxAmbientSource.tried = false;
            gFxAmbientSource.ref   = nullptr;
            SavePanelFx();
        }
        UiItemTooltip("Array element. -1 for a scalar dataref.");
        ImGui::PopID();
    }

    ImGui::Unindent(8.0f);
}

// ---- driving the aircraft's own brightness datarefs -------------------------
// The read-side pin below makes every TINT sweep, and that is half of what the
// slider is for. The other half is the screen underneath: a tint judged over a
// display that is still at full brightness is a tint judged against the wrong
// background, which is the one thing this whole feature exists to avoid.
//
// So while the slider is held, the same value is also WRITTEN to the datarefs
// the drivers read, which dims ToLiss's own displays with it.
//
// ⚠ NOTHING IS RESTORED, deliberately (2026-08-07, user). This is a bench
// instrument for a parked aircraft, not something to reach for in flight: the
// knobs stay where the slider left them, and putting them back is the sim's own
// rheostats. A restore would have to survive whatever interrupts a drag, and
// would fight ToLiss for any dataref it rewrites per frame — two hard problems
// bought for a case that does not arise.
//
// ⚠ It writes in the DATAREF's units, not the factor's: the pinned 0..1 goes
// back through lo/hi, which is exactly the mapping those two numbers exist to
// describe. Writing the normalized value would put 0.4 into a knob that counts
// 0..100 and read as the write silently not working.
//
// ⚠ And it writes the RAW position, never the curved one. The curve is Photon's
// model of how a screen responds to its knob; the knob itself is where the pilot
// put it. Writing the curved value would bend the aircraft's own dimming through
// our shape and then read it back through it a second time.
static bool FxWriteDriverValue(FxDriver& d, float t) {
    if (!FxResolveDriver(d)) return false;
    if (!XPLMCanWriteDataRef(d.ref)) return false;
    const double v = (double)d.lo + (double)t * ((double)d.hi - (double)d.lo);

    if (d.index >= 0) {
        // ⚠ The same length probe the reader does. A write past the end of
        // ToLiss's array is undefined, and this one is a WRITE — the read at
        // least only returned nonsense.
        if (d.types & xplmType_FloatArray) {
            if (XPLMGetDatavf(d.ref, nullptr, 0, 0) <= d.index) return false;
            float f = (float)v;
            XPLMSetDatavf(d.ref, &f, d.index, 1);
            return true;
        }
        if (d.types & xplmType_IntArray) {
            if (XPLMGetDatavi(d.ref, nullptr, 0, 0) <= d.index) return false;
            int i = (int)std::lround(v);
            XPLMSetDatavi(d.ref, &i, d.index, 1);
            return true;
        }
        return false;                      // an index on a scalar is a mistake
    }
    // ⚠ WIDEST FIRST, matching FxReadScalar. A dataref carrying both an int and
    // a float writer, written through the int one, quantizes a 0..1 knob to 0 or
    // 1 — the mirror image of the truncation trap on the read side, and just as
    // quiet: the screen would snap between off and full and look like a dataref
    // that simply does not do what its name says.
    if (d.types & xplmType_Double) { XPLMSetDatad(d.ref, v);         return true; }
    if (d.types & xplmType_Float)  { XPLMSetDataf(d.ref, (float)v);  return true; }
    if (d.types & xplmType_Int)    { XPLMSetDatai(d.ref, (int)std::lround(v)); return true; }
    if (d.types & xplmType_FloatArray) {
        if (XPLMGetDatavf(d.ref, nullptr, 0, 0) < 1) return false;
        float f = (float)v;
        XPLMSetDatavf(d.ref, &f, 0, 1);
        return true;
    }
    if (d.types & xplmType_IntArray) {
        if (XPLMGetDatavi(d.ref, nullptr, 0, 0) < 1) return false;
        int i = (int)std::lround(v);
        XPLMSetDatavi(d.ref, &i, 0, 1);
        return true;
    }
    return false;
}

// What the last hold managed, for the pane. Counted rather than logged per
// frame: a drag is sixty frames a second and the log is where the ONE line that
// matters would be buried.
static int         gFxDriveWrote    = 0;
static int         gFxDriveReadOnly = 0;
static std::string gFxDriveRefused;        // names, for the tooltip

// Every distinct brightness dataref in the cockpit, written once.
//
// ⚠ DEDUPED ON THE RESOLVED (ref, index), the same key the value cache uses and
// for the same reason: six DUs on one DUBrightness array are six FxDriver
// objects, and the six rects of a group are six more. Writing per driver would
// write the same element a dozen times a frame.
//
// ⚠ EVERY driver, not the selected target's. That is what "and the child targets
// too" means for a group, and it also keeps the two halves of the pin honest
// with each other: the read side is global, so a write side that was not would
// dim one target's screen while every other target's tint swept without its
// screen following — which reads as the write being broken.
static void FxDriveTheAircraft(float t) {
    if (!gRectDriversBuilt) BuildRectDrivers();

    XPLMDataRef seenRef[128];
    int         seenIdx[128];
    int         seen = 0;
    gFxDriveWrote = gFxDriveReadOnly = 0;
    gFxDriveRefused.clear();

    auto drive = [&](FxDriver& d) {
        if (!d.set() || !FxResolveDriver(d)) return;
        for (int i = 0; i < seen; ++i)
            if (seenRef[i] == d.ref && seenIdx[i] == d.index) return;
        if (seen < 128) { seenRef[seen] = d.ref; seenIdx[seen] = d.index; ++seen; }

        if (FxWriteDriverValue(d, t)) {
            ++gFxDriveWrote;
        } else {
            ++gFxDriveReadOnly;
            // Named, because "3 refused" is not actionable and the answer is
            // usually "that one is ours" or "that one is a computed output".
            if (gFxDriveRefused.size() < 400) {
                if (!gFxDriveRefused.empty()) gFxDriveRefused += "\n";
                gFxDriveRefused += d.dref;
                if (d.index >= 0) gFxDriveRefused += "[" + std::to_string(d.index) + "]";
            }
        }
    };

    for (int i = 0; i < kPanelRectCount && i < 64; ++i) drive(gRectBright[i]);
    // A stack override replaces the built-in source for its whole target, so it
    // is the dataref that face actually reads and has to move with the rest.
    for (size_t s = 0; s < gFxStacks.size(); ++s) drive(gFxStacks[s].bright);
}

// ---- the held brightness pin ------------------------------------------------
// Authoring the dim end of a look means putting the knob there, and the knob is
// a rheostat somewhere in the cockpit: find it, hold it down, let go to reach
// the editor, and it has moved by the time you are looking at the slider you
// wanted to compare against. This is that knob, in the editor.
//
// TWO things happen while it is held, and both are needed:
//
//   * the READ is pinned — every driver reports the slider's value, so the tint
//     sweeps even on a face whose dataref is read-only or unbound;
//   * the DATAREF is written — so ToLiss's display dims too, and the tint is
//     being judged over the background it will actually sit on.
//
// The read pin alone was the first version of this and it is not enough: a tint
// evaluated over a display still at full brightness is a tint evaluated against
// the wrong picture, which is the specific mistake the whole driver mechanism
// exists to prevent.
//
// ⚠ The writes are NOT undone. See FxDriveTheAircraft — this is a bench
// instrument for a parked aircraft, and the knobs stay where the slider left
// them.
static void BuildFxDrivePane() {
    // ⚠ DefaultOpen, unlike the ambient pane beside it. A folded header is a
    // control most of its users never find, and this one has no other way in:
    // the ambient pin at least has a plotted blend and a PINNED badge elsewhere
    // to hint that it exists. (ImGui remembers the state per window once it has
    // been clicked, so this only decides the first run.)
    if (!ImGui::CollapsingHeader("Brightness knob override (test)",
                                 ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::Indent(8.0f);

    // Kept between frames so releasing and grabbing again resumes where it was.
    // Never saved: it is a gesture, not a setting.
    static float value = 0.50f;
    // On by default: driving the real displays is the point of the control, and
    // a checkbox that starts off makes the feature look like the read-only pin
    // it used to be. Here to be turned OFF, for the pass where the aircraft's
    // own state must not move.
    static bool  writeRefs = true;
    static bool  wasHeld   = false;

    ImGui::SetNextItemWidth(260.0f);
    ImGui::SliderFloat("##fxdrive", &value, 0.0f, 1.0f, "%.3f  (0 dark, 1 full)");
    // ⚠ HELD, exactly. IsItemActive is true only while the mouse is down inside
    // the slider, so there is no state to leave switched on by accident — and
    // the watchdog in FxHoldDriveOverride covers the case where the window stops
    // being drawn before this line can run.
    const bool held = ImGui::IsItemActive();
    if (held) {
        FxHoldDriveOverride(value);
        // Every frame of the drag: ToLiss owns these datarefs and may well write
        // its own value back between two of our frames, exactly as it does with
        // the beacon ratios. Writing once on mouse-down would work on the refs
        // it leaves alone and fail silently on the ones it does not.
        if (writeRefs) FxDriveTheAircraft(value);
    } else if (gFxDriveManual >= 0.0f) {
        FxReleaseDriveOverride();
    }
    // One line per drag, not per frame. What it records is which datarefs
    // REFUSED, which is the answer to "why did that screen not dim" and is worth
    // having in the log after the session.
    if (held && !wasHeld && writeRefs)
        Log("panel fx: brightness pin held - driving "
            + std::to_string(gFxDriveWrote) + " dataref(s), "
            + std::to_string(gFxDriveReadOnly) + " refused"
            + (gFxDriveRefused.empty() ? "" : " (" + gFxDriveRefused + ")"));
    wasHeld = held;

    UiItemTooltip(
        "Hold this to put every brightness driver at a given knob position, so a "
        "look can be judged across the whole range without hunting for the "
        "rheostat that feeds the face being edited.\n\n"
        "It does two things at once: the tint follows immediately, and - with "
        "the box beside it ticked - the aircraft's own brightness datarefs are "
        "written too, so the DISPLAY behind the tint dims with it. That second "
        "half is what makes the result worth looking at.\n\n"
        "It acts only while the mouse is down, and it applies to EVERY target at "
        "once - a group and its members move together, which is the point.\n\n"
        "The pin lands AFTER lo/hi and BEFORE the response curve, so the curve "
        "and the floor still shape the tint: you are watching the authored "
        "shape, not a bypass of it.\n\n"
        "DEV ONLY and never saved. The power, bus and breaker gates are "
        "untouched - a dead screen stays dead.");

    ImGui::SameLine();
    if (gFxDriveManual >= 0.0f)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f), "PINNED %.3f",
                           gFxDriveManual);
    else
        ImGui::TextDisabled("release to read the real knobs");

    ImGui::Checkbox("Drive the aircraft's own datarefs", &writeRefs);
    UiItemTooltip(
        "Write the slider's value back to the datarefs the drivers read, in "
        "THEIR units (through this target's \"off at\"/\"full at\"), so ToLiss "
        "dims the real display along with the tint.\n\n"
        "⚠ NOTHING IS PUT BACK when you let go. The knobs stay where this left "
        "them - use the cockpit's own rheostats, or reload the aircraft. This is "
        "a bench control for a parked aeroplane and it is deliberately not safe "
        "to reach for in flight.\n\n"
        "Untick it to sweep the tint alone and leave the aircraft's state "
        "exactly as it is.");

    if (writeRefs && (gFxDriveWrote || gFxDriveReadOnly)) {
        ImGui::SameLine();
        ImGui::TextDisabled("last hold: wrote %d, refused %d", gFxDriveWrote,
                            gFxDriveReadOnly);
        // ⚠ A refusal is the answer to "the tint swept but that screen stayed
        // lit", and it is not a failure of the pin - the read side still moved
        // the tint. Naming the refs is the difference between that and a bug
        // hunt through the rect table.
        if (gFxDriveReadOnly)
            UiItemTooltip("Read-only or unwritable, so the tint sweeps but the "
                          "screen behind it will not:\n\n%s\n\n"
                          "Some of these are Photon's own (the inferred DCDU "
                          "level), and some are values ToLiss computes rather "
                          "than stores.",
                          gFxDriveRefused.c_str());
        else
            UiItemTooltip("Every brightness dataref in the cockpit took the "
                          "write.");
    }

    if (!gFxFollowBrightness)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                           "\"Follow display brightness\" is OFF, so nothing "
                           "reads a knob at all - this does nothing until it "
                           "is back on.");

    // ⚠ Which faces this cannot reach, said before it is reached for. A rect with
    // no brightness source in kPanelSources resolves to drive 1 whatever is
    // pinned, and a slider that visibly does nothing on one face of a group is
    // otherwise indistinguishable from a pin that is not working at all.
    if (gFxSelectedTarget >= 0) {
        if (!gRectDriversBuilt) BuildRectDrivers();
        const int si = FxStackIndex(gFxSelectedTarget);
        FxDriver* over = (si >= 0 && gFxStacks[si].bright.set())
                       ? &gFxStacks[si].bright : nullptr;
        const PanelMask mask = PanelTargetMask(gFxSelectedTarget);
        int total = 0, driven = 0;
        for (int i = 0; i < kPanelRectCount; ++i) {
            if (!((mask >> i) & 1)) continue;
            ++total;
            if (over ? over->set() : gRectBright[i].set()) ++driven;
        }
        if (total > 0 && driven < total)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                               "%d of %d face(s) of \"%s\" have no brightness "
                               "source - those stay at full and this slider "
                               "cannot move them.",
                               total - driven, total,
                               PanelTargetName(gFxSelectedTarget));
        else if (total > 0)
            ImGui::TextDisabled("all %d face(s) of \"%s\" follow it", total,
                                PanelTargetName(gFxSelectedTarget));
    }

    ImGui::Unindent(8.0f);
}

static void BuildFxTab() {
    // Scanned here as well as from LoadPanelFx, because the thumbnails have to be
    // right with Panel FX disabled or every stack empty — which is exactly the
    // state you are in while building the first look. ⚠ Only when it has never
    // been scanned: this is a per-frame function, and re-scanning from it would
    // be the per-second folder walk this feature just stopped doing, at sixty
    // times the rate. Picking up an edit is the button below.
    if (!gFxTexturesScanned) ScanFxTextures();   // also creates the drop folder

    if (ImGui::Checkbox("Panel FX enabled", &gFxEnabled)) SavePanelFx();
    ImGui::SameLine();
    if (ImGui::Button("Log stack")) LogPanelFx();
    ImGui::SameLine();
    if (ImGui::Button("Reload file")) LoadPanelFx();
    ImGui::SameLine();
    if (ImGui::Button("Undo")) FxUndoOnce();
    ImGui::SameLine();
    if (ImGui::Button("Redo")) FxRedoOnce();
    ImGui::SameLine();
    if (ImGui::Button("Rescan images")) {
        if (ImGui::GetIO().KeyShift) FxReloadAllTextures();
        ScanFxTextures();
    }
    if (ImGui::IsItemHovered())
        UiTooltip("Re-read %s.\nDrop PNG/JPG/BMP/TGA files there and they "
                  "appear in every layer's Image box.\n\n"
                  "Click this after EDITING an image too - a file whose bytes "
                  "have changed is re-uploaded and the cockpit follows "
                  "immediately. Nothing is picked up on its own.\n"
                  "Shift-click to force a re-read of every image, changed or "
                  "not.",
                  FxOverlayDir().string().c_str());
    ImGui::SameLine();
    if (ImGui::Checkbox("Follow display brightness", &gFxFollowBrightness)) SavePanelFx();
    if (ImGui::IsItemHovered())
        UiTooltip("Scale every layer's opacity by the brightness knob of "
                  "the screen it is on, and skip a target whose power is "
                  "off.\nOff = every tint is drawn as authored and NOTHING "
                  "is power-gated, which is what you want while picking a "
                  "color. Off wins over any per-target setting.\nThis is "
                  "the default for targets left on Inherit; each target "
                  "can say otherwise under \"Brightness / power "
                  "sources\".");
    ImGui::SameLine();
    if (ImGui::Checkbox("Curve", &gFxCurve)) SavePanelFx();
    if (ImGui::IsItemHovered())
        UiTooltip("Reshape the knob through the display response curve "
                  "instead of using it raw.\nThe bottom fifth of the "
                  "knob is dead and half a turn is still dim - the same "
                  "curve the screen-glow lights use, so a tint and the "
                  "spill under it come up together.\nOff is linear from lo "
                  "to hi.\nThis is the default for targets left on "
                  "Inherit; each target can say otherwise, and the shape "
                  "in force is plotted under \"Brightness / power "
                  "sources\".");
    ImGui::SameLine();
    if (ImGui::Button("Rebind")) {
        FxForgetDrivers();
        Log("panel fx: dropped every cached driver handle - they re-resolve on "
            "the next frame");
    }
    if (ImGui::IsItemHovered())
        UiTooltip("Re-resolve every brightness/power dataref. Needed when "
                  "the aircraft's own plugin started after this one, "
                  "which is when they show as unreadable.");
    ImGui::SameLine();
    ImGui::TextDisabled("lit pass, after Gauges - A3xx");

    UiHint("Broader targets composite first, so a group is a base and its "
           "members refine it. A layer can be a flat color or an image from "
           "%s (%d found).",
           FxOverlayDir().string().c_str(), (int)gFxTextures.size());

    BuildFxAmbientPane();
    // Beside the ambient pin and not inside the per-target source pane: both are
    // "hold the world still while I author one end of a look", and the pair read
    // as one idea when they sit together. This one is global in effect anyway.
    BuildFxDrivePane();
    ImGui::Separator();

    // Wider than a flat list needed: the tree spends width on indentation, and
    // each row carries a layer count and two right-aligned buttons that the
    // longest names would otherwise run into at four levels of nesting.
    ImGui::BeginChild("fx_targets", ImVec2(310.0f, 0.0f), ImGuiChildFlags_Borders);
    BuildFxTargetPane();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("fx_layers", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    BuildFxLayerPane();
    ImGui::EndChild();
}

// ========================= the Dev window and its menu =======================
// ONE window. Every dev knob lives in a tab of it, and the Dev submenu has one
// item that opens it. The alternative — a menu item and a window per experiment
// — is what this replaced: four windows to arrange, each remembering its own
// position, and no way to see the probe and the effect it is probing at once.

// ---- Build tab: run the repo's Python tooling without leaving the sim -------
// The largest single piece of overhead in this project is that a lighting change
// means alt-tabbing out, remembering which of six build invocations is the right
// one, and coming back. These buttons are those invocations.
//
// Commands run DETACHED on a worker thread and their output is captured, because
// a synchronous std::system() inside a draw callback freezes the simulator for
// however long the generator takes.
static std::mutex              gCmdMutex;
static std::vector<std::string> gCmdOutput;      // guarded by gCmdMutex
static std::atomic<bool>       gCmdRunning{false};
static std::string             gCmdLast;

static void CmdAppend(const std::string& line) {
    std::lock_guard<std::mutex> lock(gCmdMutex);
    gCmdOutput.push_back(line);
    // Bounded: a full build prints hundreds of lines and this pane is a
    // progress readout, not a build log. Log.txt keeps the whole thing.
    if (gCmdOutput.size() > 400) gCmdOutput.erase(gCmdOutput.begin());
}

// Where the repo lives. Not derivable from the X-Plane install — the two are
// unrelated directories — so it is asked for once and persisted with the rest of
// the dev prefs. PHOTON_REPO in the environment seeds it, which is what a
// developer who already has one set expects.
static std::string gRepoPath;

static fs::path DevPrefsPath() {
    char root[512] = {0};
    XPLMGetSystemPath(root);
    return fs::path(root) / "Output" / "preferences" / "ToLissPhoton_dev.txt";
}

static void SaveDevPrefs() {
    std::error_code ec;
    fs::path p = DevPrefsPath();
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "repo " << gRepoPath << "\n";
    Log("dev: saved dev prefs to " + p.string());
}

static void LoadDevPrefs() {
    std::ifstream f(DevPrefsPath(), std::ios::binary);
    if (f) {
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("repo ", 0) == 0) gRepoPath = line.substr(5);
        }
    }
    if (gRepoPath.empty())
        if (const char* env = std::getenv("PHOTON_REPO")) gRepoPath = env;
}

static void RunRepoCommand(const std::string& args) {
    if (gCmdRunning.exchange(true)) {
        CmdAppend("(a command is already running)");
        return;
    }
    if (gRepoPath.empty()) {
        CmdAppend("Set the repo path first - nothing to run in.");
        gCmdRunning = false;
        return;
    }
    gCmdLast = args;
    {
        std::lock_guard<std::mutex> lock(gCmdMutex);
        gCmdOutput.clear();
    }
    CmdAppend("> python " + args);

    // Detached: the sim keeps drawing while the generator runs. The thread only
    // touches gCmdOutput (mutex-guarded) and the two atomics, never any X-Plane
    // API — XPLM is not thread-safe and a stray call from here would be a crash
    // with no useful stack.
    // `repo` is copied, not read through the global: the text field can be edited
    // on the UI thread while this runs.
    const std::string repo = gRepoPath;
    std::thread([args, repo] {
#if defined(_WIN32)
        std::string cmd = "cd /d \"" + repo + "\" && python " + args + " 2>&1";
#else
        std::string cmd = "cd \"" + repo + "\" && python " + args + " 2>&1";
#endif
        if (FILE* pipe = POPEN(cmd.c_str(), "r")) {
            char buf[512];
            while (std::fgets(buf, sizeof(buf), pipe)) {
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();
                CmdAppend(line);
            }
            const int rc = PCLOSE(pipe);
            CmdAppend(rc == 0 ? "-- done --"
                              : "-- FAILED, exit " + std::to_string(rc) + " --");
        } else {
            CmdAppend("-- could not start python --");
        }
        gCmdRunning = false;
    }).detach();
}

struct RepoCommand { const char* label; const char* args; const char* help; };
static const RepoCommand kRepoCommands[] = {
    { "Build all",      "build/build_objs.py build --write",
      "Exterior + interior OBJs into dist/ and the live aircraft." },
    { "Interior",       "build/build_objs.py build --target interior --write",
      "Cockpit lights only - the fast loop when tuning int_* fixtures." },
    { "Interior debug", "build/build_objs.py build --target interior --debug --write",
      "Cockpit lights with the live tuning wrappers (cone/aim/size become knobs)." },
    { "Screens",        "build/build_objs.py build --target screens --debug --write",
      "The experimental display-glow OBJ. Not in --target both." },
    { "Everything",     "build/build_objs.py build --target all --write",
      "Exterior + interior + screens. The dev tuning pass." },
    { "Check goldens",  "build/build_objs.py check",
      "Compare generated output against reference/photon/." },
    { "Tests",          "-m unittest discover -s tests",
      "The full suite." },
};
static const int kRepoCommandCount =
    (int)(sizeof(kRepoCommands) / sizeof(kRepoCommands[0]));

static void BuildBuildTab() {
    char pathBuf[512];
    std::snprintf(pathBuf, sizeof(pathBuf), "%s", gRepoPath.c_str());
    ImGui::SetNextItemWidth(-140.0f);
    if (ImGui::InputText("Repo path", pathBuf, sizeof(pathBuf))) gRepoPath = pathBuf;
    ImGui::SameLine();
    if (ImGui::Button("Save path")) SaveDevPrefs();
    if (gRepoPath.empty())
        UiHint("Point this at your checkout of toliss-photon-lighting. Set "
               "PHOTON_REPO in the environment to have it filled in for you.");

    ImGui::Separator();
    ImGui::BeginDisabled(gCmdRunning.load());
    for (int i = 0; i < kRepoCommandCount; ++i) {
        if (i && (i % 4)) ImGui::SameLine();
        // Scoped by index: the labels are short and come from a table, so two
        // build variants of the same command ("build", "build --debug") are one
        // edit away from sharing a label and therefore an ID.
        ImGui::PushID(i);
        if (ImGui::Button(kRepoCommands[i].label)) RunRepoCommand(kRepoCommands[i].args);
        if (ImGui::IsItemHovered())
            UiTooltip("%s\n\npython %s",
                      kRepoCommands[i].help, kRepoCommands[i].args);
        ImGui::PopID();
    }
    ImGui::EndDisabled();

    // ⚠ There is no "reload aircraft" button here on purpose. That command runs
    // SYNCHRONOUSLY and unloads the aircraft's plugins, so issuing it from a
    // draw callback tears down the aircraft in the middle of a frame. A --write
    // build already triggers the reload through watch.py's channel; if you need
    // one by hand, use the Dev menu item, which runs in menu-handler context.
    UiHint("A --write build installs into the live aircraft. Use Dev > Reload "
           "aircraft to pick it up - never from this window, see the source.");

    ImGui::Separator();
    if (gCmdRunning.load()) ImGui::TextUnformatted("Running...");
    else if (!gCmdLast.empty()) ImGui::TextDisabled("Last: python %s", gCmdLast.c_str());

    ImGui::BeginChild("cmdout", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lock(gCmdMutex);
        for (size_t i = 0; i < gCmdOutput.size(); ++i)
            ImGui::TextUnformatted(gCmdOutput[i].c_str());
    }
    // Follow the tail while a command runs, then leave the scroll alone so the
    // output can be read.
    if (gCmdRunning.load() && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// ---- Log tab ---------------------------------------------------------------
static void BuildLogTab() {
    if (ImGui::Button("Clear")) ClearLogRing();
    ImGui::SameLine();
    UiHint("Everything this plugin writes to Log.txt, most recent last.");
    ImGui::Separator();

    ImGui::BeginChild("logring", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const std::vector<std::string> lines = LogRingSnapshot();
    for (size_t i = 0; i < lines.size(); ++i) ImGui::TextUnformatted(lines[i].c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// ---- Lights tab: the live light editor -------------------------------------
// The hierarchy is the manifest's own — CATEGORY then FIXTURE then light — and
// that is not cosmetic. A category is the thing the Cockpit menu switches and a
// fixture is the thing that was authored as one unit in the DSL, so "which of
// these do I edit together" has the same answer here as in lights.layout.phdsl.
// A flat list of 30 names in `int_*#side` form does not.
static void DbgSelect(int n) {
    gDbgSel = n;
    // Following the selection is what makes the markers usable: you click a light
    // and the arrow is already on it. -1 (all) stays as-is if that is what is set.
    if (gDbgShowMarkers > 0) gDbgShowMarkers = n + 1;
}

static std::string DbgFmt(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", (double)v);
    return buf;
}

// Plain English for an aim angle. The sign convention is the one thing about an
// aim that cannot be checked by eye without already knowing the answer, so the
// window says it rather than making you remember it.
static const char* DbgPitchWord(float p) {
    if (p <= -60.0f) return "straight down";
    if (p <  -10.0f) return "down";
    if (p <=  10.0f) return "level";
    if (p <   60.0f) return "up";
    return "straight up";
}
static const char* DbgYawWord(float y) {
    const float a = y < 0.0f ? y + 360.0f : y;
    if (a <  22.5f || a >= 337.5f) return "forward";
    if (a <  67.5f)  return "forward-right";
    if (a < 112.5f)  return "right";
    if (a < 157.5f)  return "aft-right";
    if (a < 202.5f)  return "aft";
    if (a < 247.5f)  return "aft-left";
    if (a < 292.5f)  return "left";
    return "forward-left";
}

// The deliverable of a tuning session. Split into the two files it belongs to,
// because color and geometry live apart in this DSL: a palette entry carries the
// rgb, the light type or fixture carries aim/spread/size. Pasting rgb into a
// light type would hard-code a color that is supposed to change with the profile.
static void DbgLogDsl(const DbgLight& l) {
    Log("---- DSL for " + l.name + " ----");
    Log("  fixture " + l.fixture + ", category " + l.category + ", "
        + l.source + "[" + std::to_string(l.index) + "]");
    Log("  # lights.style.phdsl - the palette entry (color only):");
    Log("      rgb: " + DbgFmt(l.v[0]) + " " + DbgFmt(l.v[1]) + " " + DbgFmt(l.v[2]) + ";");
    Log("  # lights.layout.phdsl - the fixture (geometry):");
    Log("      at: " + DbgFmt(l.pos[0] + l.off[0]) + " "
                     + DbgFmt(l.pos[1] + l.off[1]) + " "
                     + DbgFmt(l.pos[2] + l.off[2]) + ";");
    const bool omni = l.v[5] == 0.0f && l.v[6] == 0.0f && l.v[7] == 0.0f;
    if (!omni) {
        Log("      aim: " + DbgFmt(l.pitch) + " " + DbgFmt(l.yaw) + ";");
        Log("      spread: " + DbgFmt(DbgConeToSpread(l.v[8])) + ";");
    } else {
        Log("      # omnidirectional (dir 0 0 0) - no aim or spread");
    }
    Log("      size: " + DbgFmt(l.v[4]) + ";");
    if (l.boost > 0.0f)
        Log("      optimize: boost " + DbgFmt(gDbgFloodBoost)
            + ";   # simplified copy - the size above is the authored value");
    Log("  # offset applied while tuning: "
        + DbgFmt(l.off[0]) + " " + DbgFmt(l.off[1]) + " " + DbgFmt(l.off[2])
        + " (already folded into `at:` above)");
}

// ---- the tuning file --------------------------------------------------------
// "Log DSL" prints one light; this is the WHOLE session — every light whose
// values differ from the OBJ's baked seeds, in .phdsl shape, plus the
// simplified-flood boost factor — in one file that outlives the sim. It lands in
// the repo's .scratch/ (gitignored) when the Build tab knows where the repo is,
// else beside the preferences; the path is shown in the tab and logged. The file
// is the deliverable of a tuning session: transcribe it into src/lights/*.phdsl,
// then rebuild.
//
// ⚠ IT IS ALSO READ BACK, AND SAVED WITHOUT BEING ASKED (2026-08-09). Until then
// a tuning pass lived only in this plugin's memory, so closing X-Plane, reloading
// the aircraft or deploying a new build discarded it in silence — which is
// exactly how an afternoon of screen-glow colour work was lost, with no file, no
// log line and nothing in git to recover from. Hence: auto-save on the way out
// and on every aircraft load, restore when the manifest loads, and a format that
// is parseable as well as readable.
//
// ⚠ THE IDENTITY IS FIXTURE + NAME, never the slot. Slots shift the moment the
// DSL gains or loses a light, and the cockpit names REPEAT — `int_panelflood#a`
// is two different lamps on two different fixtures. Fixture+name is unique, and
// a test pins that it stays so.
//
// ⚠ EVERY RECORD CARRIES THE SEED IT WAS TUNED AGAINST (`from:`), and a restore
// SKIPS any light whose OBJ no longer bakes that seed. Otherwise editing the
// .phdsl and rebuilding would appear to do nothing: the saved session would keep
// overwriting the new authored values, which is the same class of trap as a
// debug OBJ silently hiding the gate it is meant to show.
static fs::path DbgTuningFilePath() {
    if (!gRepoPath.empty())
        return fs::path(gRepoPath) / ".scratch" / "light_tuning.txt";
    char root[512] = {0};
    XPLMGetSystemPath(root);
    return fs::path(root) / "Output" / "preferences" / "ToLissPhoton_tuning.txt";
}

static bool DbgLightChanged(const DbgLight& l) {
    for (int i = 0; i < kDbgParamCount; ++i)
        if (l.v[i] != l.base[i]) return true;
    for (int i = 0; i < 3; ++i)
        if (l.off[i] != l.baseOff[i]) return true;
    return l.pitch != l.basePitch || l.yaw != l.baseYaw;
}

static int DbgTuningEditCount() {
    int n = 0;
    for (size_t i = 0; i < gDbgLights.size(); ++i)
        if (DbgLightChanged(gDbgLights[i])) ++n;
    return n;
}

// Is there anything worth writing? Auto-save asks first, so that a session in
// which nothing was touched cannot blank a file a previous one filled in.
static bool DbgTuningDirty() {
    return DbgTuningEditCount() > 0 || gDbgFloodBoost != gDbgFloodBoostSeed;
}

// The nine numbers a record is checked against on the way back in: the OBJ's
// baked colour, alpha, size and cone, plus its baked position. If the .phdsl has
// moved on, these no longer match and the record is skipped rather than applied.
static void DbgTuningSeed(const DbgLight& l, float out[9]) {
    out[0] = l.base[0]; out[1] = l.base[1]; out[2] = l.base[2];
    out[3] = l.base[3]; out[4] = l.base[4]; out[5] = l.base[8];
    out[6] = l.pos[0];  out[7] = l.pos[1];  out[8] = l.pos[2];
}

static void DbgSaveTuning(bool quiet) {
    std::error_code ec;
    const fs::path p = DbgTuningFilePath();
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        gDbgNote = "could not write " + p.string();
        Log("light editor: " + gDbgNote);
        return;
    }
    f << "# ToLiss Photon - in-sim light tuning (Dev window; " << gDbgManifest << ")\n"
      << "#\n"
      << "# WRITTEN BY THE PLUGIN, and read back by it: the values below are\n"
      << "# re-applied on the next aircraft load, so a session survives closing the\n"
      << "# sim. Edit or delete this file freely - it is not source.\n"
      << "#\n"
      << "# TO KEEP THESE VALUES: transcribe into src/lights/lights.style.phdsl\n"
      << "# (colors, alpha, size, aim, spread - the light type or palette entry) and\n"
      << "# lights.layout.phdsl (per-fixture geometry), rebuild, then delete this\n"
      << "# file. See docs/dsl.md. Only lights that differ from the OBJ's baked\n"
      << "# values are listed; `from:` is what they were baked at when tuned, and a\n"
      << "# record whose OBJ no longer matches it is SKIPPED rather than applied.\n\n";

    int boosted = 0;
    for (size_t i = 0; i < gDbgLights.size(); ++i)
        if (gDbgLights[i].boost > 0.0f) ++boosted;
    if (boosted) {
        f << "# The simplified main panel floods -> `optimize: boost <f>;` in\n"
          << "# lights.layout.phdsl, on every line that carries one today ("
          << boosted << " lights).\n"
          << "boost: " << DbgFmt(gDbgFloodBoost) << ";\n"
          << "boost_from: " << DbgFmt(gDbgFloodBoostSeed) << ";\n\n";
    }

    const int changed = DbgTuningEditCount();
    for (size_t i = 0; i < gDbgLights.size(); ++i) {
        const DbgLight& l = gDbgLights[i];
        if (!DbgLightChanged(l)) continue;
        f << "light " << l.name << "\n";
        f << "  fixture: " << l.fixture << ";\n";
        f << "  where: " << (l.category.empty() ? "-" : l.category.c_str())
          << " " << l.source << "[" << l.index << "] slot " << l.n << ";\n";
        f << "  rgb: " << DbgFmt(l.v[0]) << " " << DbgFmt(l.v[1]) << " "
          << DbgFmt(l.v[2]) << ";\n";
        f << "  alpha: " << DbgFmt(l.v[3]) << ";\n";
        f << "  size: " << DbgFmt(l.v[4]) << ";\n";
        const bool omni = l.v[5] == 0.0f && l.v[6] == 0.0f && l.v[7] == 0.0f;
        if (!omni) {
            f << "  aim: " << DbgFmt(l.pitch) << " " << DbgFmt(l.yaw) << ";\n";
            f << "  spread: " << DbgFmt(DbgConeToSpread(l.v[8])) << ";\n";
        } else {
            f << "  # omnidirectional (dir 0 0 0) - no aim or spread\n";
        }
        f << "  at: " << DbgFmt(l.pos[0] + l.off[0]) << " "
          << DbgFmt(l.pos[1] + l.off[1]) << " "
          << DbgFmt(l.pos[2] + l.off[2]) << ";\n";
        if (l.boost > 0.0f)
            f << "  # simplified copy - `size:` above is the AUTHORED size; it is\n"
              << "  # drawn at that x the boost factor above.\n";
        float seed[9];
        DbgTuningSeed(l, seed);
        f << "  from:";
        for (int k = 0; k < 9; ++k) f << " " << DbgFmt(seed[k]);
        f << ";\n\n";
    }
    if (!changed) f << "# (no per-light edits)\n";
    gDbgNote = "saved " + std::to_string(changed) + " edited light(s) -> " + p.string();
    if (!quiet) Log("light editor: " + gDbgNote);
    else        Log("light editor: auto-saved " + std::to_string(changed)
                    + " edited light(s) -> " + p.string());
}

// Called from the two paths that would otherwise DISCARD a session's work: the
// aircraft load that reseeds every light from the manifest, and plugin shutdown.
static void DbgAutoSaveTuning() {
    if (gDbgLights.empty() || !DbgTuningDirty()) return;
    DbgSaveTuning(true);
}

// ---- reading it back --------------------------------------------------------
// Line-oriented on purpose: the file is meant to be read and edited by a person,
// and a format that only a parser can produce would stop being the deliverable
// it is supposed to be.
static std::string DbgTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// "  size: 0.9;   # comment"  ->  key "size", value "0.9"
static bool DbgSplitKeyValue(const std::string& line, std::string& key, std::string& value) {
    const std::string t = DbgTrim(line);
    if (t.empty() || t[0] == '#') return false;
    const size_t colon = t.find(':');
    if (colon == std::string::npos) return false;
    key = DbgTrim(t.substr(0, colon));
    value = DbgTrim(t.substr(colon + 1));
    const size_t hash = value.find('#');
    if (hash != std::string::npos) value = DbgTrim(value.substr(0, hash));
    if (!value.empty() && value.back() == ';') value.pop_back();
    return !key.empty();
}

static int DbgParseFloats(const std::string& s, float* out, int max) {
    std::istringstream in(s);
    int n = 0;
    while (n < max && (in >> out[n])) ++n;
    return n;
}

// One parsed record, applied when the next `light` line (or EOF) closes it.
struct DbgTuningRecord {
    std::string name, fixture;
    bool  hasRgb = false, hasAlpha = false, hasSize = false;
    bool  hasAim = false, hasSpread = false, hasAt = false, hasSeed = false;
    float rgb[3] = {0,0,0}, alpha = 0.0f, size = 0.0f;
    float pitch = 0.0f, yaw = 0.0f, spread = 0.0f, at[3] = {0,0,0}, seed[9] = {0};
};

// applied / skipped-because-the-OBJ-moved / skipped-because-no-such-light
static void DbgApplyTuningRecord(const DbgTuningRecord& r, int& ok, int& stale, int& gone) {
    if (r.name.empty()) return;
    DbgLight* target = nullptr;
    for (size_t i = 0; i < gDbgLights.size(); ++i) {
        DbgLight& l = gDbgLights[i];
        if (l.name == r.name && l.fixture == r.fixture) { target = &l; break; }
    }
    if (!target) { ++gone; return; }
    if (r.hasSeed) {
        float seed[9];
        DbgTuningSeed(*target, seed);
        for (int k = 0; k < 9; ++k) {
            if (std::fabs(seed[k] - r.seed[k]) > 1e-3f) { ++stale; return; }
        }
    }
    DbgLight& l = *target;
    if (r.hasRgb) { l.v[0] = r.rgb[0]; l.v[1] = r.rgb[1]; l.v[2] = r.rgb[2]; }
    if (r.hasAlpha) l.v[3] = r.alpha;
    if (r.hasSize)  l.v[4] = r.size;
    if (r.hasAim) {
        l.pitch = r.pitch; l.yaw = r.yaw;
        DbgAimToDir(l.pitch, l.yaw, &l.v[5]);
    }
    if (r.hasSpread) l.v[8] = DbgSpreadToCone(r.spread);
    if (r.hasAt) {
        for (int k = 0; k < 3; ++k) {
            float off = r.at[k] - l.pos[k];
            if (off >  kDbgPosRange) off =  kDbgPosRange;   // the ANIM keyframe span
            if (off < -kDbgPosRange) off = -kDbgPosRange;
            l.off[k] = off;
        }
    }
    ++ok;
}

// Restore the saved session onto the freshly loaded manifest. Runs at the END of
// DbgLoadManifest, so the baked values are the seeds and these are edits on top.
static void DbgLoadTuning() {
    const fs::path p = DbgTuningFilePath();
    std::ifstream f(p, std::ios::binary);
    if (!f) return;                          // no saved session; nothing to say

    DbgTuningRecord rec;
    bool inRecord = false;
    int ok = 0, stale = 0, gone = 0;
    bool haveBoost = false, haveBoostFrom = false;
    float boost = 0.0f, boostFrom = 0.0f;
    std::string line;
    while (std::getline(f, line)) {
        const std::string t = DbgTrim(line);
        if (t.rfind("light ", 0) == 0) {
            if (inRecord) DbgApplyTuningRecord(rec, ok, stale, gone);
            rec = DbgTuningRecord();
            rec.name = DbgTrim(t.substr(6));
            inRecord = true;
            continue;
        }
        std::string key, value;
        if (!DbgSplitKeyValue(line, key, value)) continue;
        float nums[9];
        const int got = DbgParseFloats(value, nums, 9);
        if (!inRecord) {
            if (key == "boost" && got >= 1)      { boost = nums[0];     haveBoost = true; }
            else if (key == "boost_from" && got >= 1) { boostFrom = nums[0]; haveBoostFrom = true; }
            continue;
        }
        if      (key == "fixture") rec.fixture = value;
        else if (key == "rgb"    && got >= 3) { rec.hasRgb = true; for (int k=0;k<3;++k) rec.rgb[k]=nums[k]; }
        else if (key == "alpha"  && got >= 1) { rec.hasAlpha = true; rec.alpha = nums[0]; }
        else if (key == "size"   && got >= 1) { rec.hasSize = true;  rec.size  = nums[0]; }
        else if (key == "aim"    && got >= 2) { rec.hasAim = true; rec.pitch = nums[0]; rec.yaw = nums[1]; }
        else if (key == "spread" && got >= 1) { rec.hasSpread = true; rec.spread = nums[0]; }
        else if (key == "at"     && got >= 3) { rec.hasAt = true; for (int k=0;k<3;++k) rec.at[k]=nums[k]; }
        else if (key == "from"   && got >= 9) { rec.hasSeed = true; for (int k=0;k<9;++k) rec.seed[k]=nums[k]; }
    }
    if (inRecord) DbgApplyTuningRecord(rec, ok, stale, gone);

    // ⚠ Same staleness rule as a light's `from:`: a boost saved against an
    // authored 1.5 must not keep overriding a .phdsl that now says something
    // else. With no `boost_from:` recorded, the file predates the check and the
    // value is taken as-is.
    if (haveBoost && (!haveBoostFrom || std::fabs(boostFrom - gDbgFloodBoostSeed) < 1e-3f))
        gDbgFloodBoost = boost;
    else if (haveBoost)
        ++stale;

    if (!ok && !stale && !gone) return;
    std::string note = "restored " + std::to_string(ok) + " tuned light(s) from "
                     + p.filename().string();
    if (stale) note += ", skipped " + std::to_string(stale) + " the OBJ has moved past";
    if (gone)  note += ", " + std::to_string(gone) + " no longer in the manifest";
    gDbgNote = note;
    Log("light editor: " + note + " (" + p.string() + ")");
    if (stale)
        Log("light editor: a skipped record was tuned against different baked "
            "values than this OBJ has - the .phdsl moved on, so the authored "
            "values win. Delete the tuning file to stop it being offered.");
}

static void DbgForgetTuning() {
    std::error_code ec;
    const fs::path p = DbgTuningFilePath();
    if (fs::remove(p, ec)) gDbgNote = "deleted " + p.string();
    else                   gDbgNote = "no tuning file at " + p.string();
    Log("light editor: " + gDbgNote);
}

static void BuildLightRow(DbgLight& l) {
    ImGui::PushID(l.n);
    const bool selected = (gDbgSel == l.n);
    char label[192];
    std::snprintf(label, sizeof(label), "%s##%d", l.name.c_str(), l.n);
    if (ImGui::Selectable(label, selected)) DbgSelect(l.n);
    if (ImGui::IsItemHovered())
        UiTooltip("slot %d - %s[%d]\nbaked at %.3f %.3f %.3f",
                  l.n, l.source.c_str(), l.index,
                  (double)l.pos[0], (double)l.pos[1], (double)l.pos[2]);
    ImGui::PopID();
}

static void BuildLightTree() {
    // Grouped on the fly rather than into a prebuilt tree: the list is at most 64
    // entries and rebuilding it per frame cannot go stale after a reload.
    std::vector<std::string> categories;
    for (size_t i = 0; i < gDbgLights.size(); ++i) {
        const std::string& c = gDbgLights[i].category;
        if (std::find(categories.begin(), categories.end(), c) == categories.end())
            categories.push_back(c);
    }
    for (size_t ci = 0; ci < categories.size(); ++ci) {
        const std::string& cat = categories[ci];
        int count = 0;
        for (size_t i = 0; i < gDbgLights.size(); ++i)
            if (gDbgLights[i].category == cat) ++count;
        char header[128];
        std::snprintf(header, sizeof(header), "%s (%d)",
                      cat.empty() ? "(no category)" : cat.c_str(), count);
        if (!ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_DefaultOpen)) continue;

        std::vector<std::string> fixtures;
        for (size_t i = 0; i < gDbgLights.size(); ++i) {
            if (gDbgLights[i].category != cat) continue;
            const std::string& f = gDbgLights[i].fixture;
            if (std::find(fixtures.begin(), fixtures.end(), f) == fixtures.end())
                fixtures.push_back(f);
        }
        for (size_t fi = 0; fi < fixtures.size(); ++fi) {
            const std::string& fx = fixtures[fi];
            // A fixture with one light would be a node wrapping a single child —
            // the same "single-member group" pointlessness the FX target tree
            // avoids — so it is flattened into the category.
            int members = 0;
            for (size_t i = 0; i < gDbgLights.size(); ++i)
                if (gDbgLights[i].category == cat && gDbgLights[i].fixture == fx) ++members;
            const bool wrap = members > 1;
            if (wrap && !ImGui::TreeNodeEx((fx + "##fx").c_str(),
                                           ImGuiTreeNodeFlags_DefaultOpen)) continue;
            for (size_t i = 0; i < gDbgLights.size(); ++i)
                if (gDbgLights[i].category == cat && gDbgLights[i].fixture == fx)
                    BuildLightRow(gDbgLights[i]);
            if (wrap) ImGui::TreePop();
        }
        ImGui::TreePop();
    }
}

static void BuildLightEditor(DbgLight& l) {
    ImGui::SeparatorText(l.name.c_str());
    ImGui::TextDisabled("slot %d - fixture %s - category %s - %s[%d]",
                        l.n, l.fixture.c_str(), l.category.c_str(),
                        l.source.c_str(), l.index);
    if (l.boost > 0.0f) {
        ImGui::TextDisabled("simplified flood - drawn at size x%.2f "
                            "(the multiplier on the Cockpit tab)",
                            (double)gDbgFloodBoost);
        if (ImGui::IsItemHovered())
            UiTooltip("This is the one-light-per-lamp copy the 'Use simplified "
                      "lighting' checkbox switches to. The size below is the "
                      "AUTHORED size; the shared multiplier scales it live and "
                      "writes back as `optimize: boost <f>` in the DSL.");
    }
    if (l.source == "du") {
        ImGui::Checkbox("Edit all screens together", &gDbgScreensLink);
        if (ImGui::IsItemHovered())
            UiTooltip("The six screen-glow lights are one design in six places, "
                      "so their shared attributes - color, brightness, size, aim, "
                      "spread - are edited as one; edits here land on all six. "
                      "Position stays per-light. Untick to tune one screen alone.");
    }

    const bool sizeDriven = (l.source == "du");
    ImGui::ColorEdit3("color", l.v);
    ImGui::SliderFloat("brightness", &l.v[3], 0.0f, 1.0f, "x%.3f");
    if (ImGui::IsItemHovered()) {
        if (sizeDriven)
            UiTooltip("The light's alpha, and for a screen glow that is a "
                      "CONSTANT: the DU knob drives size instead (%s[%d] = "
                      "%.3f), so this is the intensity at every knob "
                      "position. Write it back as `alpha:` in the DSL - the "
                      "shipping plugin never overwrites it.",
                      l.source.c_str(), l.index, (double)DbgRheostat(l));
        else
            UiTooltip("The light's own alpha. What reaches the sim is this "
                      "TIMES the rheostat it is wired to (%s[%d] = %.3f), "
                      "which is why a light can be at 1.0 and still dark.",
                      l.source.c_str(), l.index, (double)DbgRheostat(l));
    }
    ImGui::SliderFloat("size", &l.v[4], 0.0f, 8.0f, "%.3f m");
    if (ImGui::IsItemHovered()) {
        if (sizeDriven)
            UiTooltip("How far the pool reaches AT A SCREEN TURNED FULLY UP - "
                      "this is the driven slot for a screen glow, scaled live "
                      "by %s[%d] (= %.3f now) and by the Displays tab's "
                      "strength. Turn the DU knob down and the pool pulls in "
                      "rather than fading.",
                      l.source.c_str(), l.index, (double)DbgRheostat(l));
        else
            UiTooltip("How far the pool reaches. Raise this FIRST when a "
                      "spread edit looks inert - a cone two centimeters from "
                      "the panel it lights has nowhere to widen into.");
    }

    const bool omni = l.v[5] == 0.0f && l.v[6] == 0.0f && l.v[7] == 0.0f;
    if (omni) {
        ImGui::TextDisabled("omnidirectional (dir 0 0 0) - no aim, no spread");
    } else {
        bool aimed = false;
        aimed |= ImGui::SliderFloat("pitch", &l.pitch, -90.0f, 90.0f, "%.1f deg");
        if (ImGui::IsItemHovered())
            UiTooltip("Degrees above horizontal. 0 level, +90 straight up, "
                      "-90 straight down.");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", DbgPitchWord(l.pitch));
        aimed |= ImGui::SliderFloat("yaw", &l.yaw, -180.0f, 180.0f, "%.1f deg");
        if (ImGui::IsItemHovered())
            UiTooltip("0 forward, 90 right, 180 aft, -90 left.");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", DbgYawWord(l.yaw));

        // ⚠ One direction of flow, always: the angles are what this window edits
        // and what "Log DSL" prints; slots 5..7 are derived. That is also what
        // keeps the tuner incapable of emitting a NON-UNIT vector, the bug class
        // that made the interior's cones inert (a cone is compared against a dot
        // product with dir, so |dir| > 1 makes the threshold unsatisfiable).
        if (aimed) DbgAimToDir(l.pitch, l.yaw, &l.v[5]);

        // One click puts a light on an unambiguous axis, which is the only clean
        // way to answer "which way does the sim think this points". Sweeping an
        // angle is not clean: an interpolated arc through a cockpit full of
        // surfaces can be read several ways, and has been.
        ImGui::TextUnformatted("aim");
        static const struct { const char* label; float pitch, yaw; } kPresets[] = {
            { "Fwd", 0.0f, 0.0f },   { "Aft", 0.0f, 180.0f },
            { "Left", 0.0f, -90.0f },{ "Right", 0.0f, 90.0f },
            { "Up", 90.0f, 0.0f },   { "Down", -90.0f, 0.0f },
        };
        for (int i = 0; i < 6; ++i) {
            ImGui::SameLine();
            if (ImGui::SmallButton(kPresets[i].label)) {
                l.pitch = kPresets[i].pitch;
                l.yaw   = kPresets[i].yaw;
                DbgAimToDir(l.pitch, l.yaw, &l.v[5]);
                gDbgNote = std::string("aim ") + kPresets[i].label;
            }
        }

        float spread = DbgConeToSpread(l.v[8]);
        if (ImGui::SliderFloat("spread", &spread, 0.5f, 89.5f, "%.1f deg"))
            l.v[8] = DbgSpreadToCone(spread);
        if (ImGui::IsItemHovered())
            UiTooltip("Half-angle of the cone. Stored as cos(spread), which "
                      "is why the DSL's `cone:` runs backwards - bigger is "
                      "narrower. Prefer `spread:` when writing it back.");
        ImGui::SameLine();
        ImGui::TextDisabled("(cone %.3f)", (double)l.v[8]);
    }

    if (l.posTunable) {
        ImGui::SeparatorText("position");
        ImGui::TextDisabled("baked %.3f %.3f %.3f  ->  live %.3f %.3f %.3f",
                            (double)l.pos[0], (double)l.pos[1], (double)l.pos[2],
                            (double)(l.pos[0] + l.off[0]),
                            (double)(l.pos[1] + l.off[1]),
                            (double)(l.pos[2] + l.off[2]));
        // Offsets, not absolutes: the OBJ bakes the position and this rides three
        // ANIM_trans blocks on top of it, so the span is the keyframe range.
        ImGui::SliderFloat("x (right+)", &l.off[0], -kDbgPosRange, kDbgPosRange, "%.3f m");
        ImGui::SliderFloat("y (up+)",    &l.off[1], -kDbgPosRange, kDbgPosRange, "%.3f m");
        ImGui::SliderFloat("z (aft+)",   &l.off[2], -kDbgPosRange, kDbgPosRange, "%.3f m");
    } else {
        ImGui::TextDisabled("position is not tunable in this OBJ - rebuild with "
                            "--debug to get the ANIM_trans wrappers");
    }

    ImGui::Separator();
    if (ImGui::Button("Revert this light")) {
        for (int i = 0; i < kDbgParamCount; ++i) l.v[i] = l.base[i];
        for (int i = 0; i < 3; ++i) l.off[i] = l.baseOff[i];
        l.pitch = l.basePitch; l.yaw = l.baseYaw;
        gDbgNote = "reverted " + l.name;
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert all")) {
        for (size_t i = 0; i < gDbgLights.size(); ++i) {
            DbgLight& o = gDbgLights[i];
            for (int k = 0; k < kDbgParamCount; ++k) o.v[k] = o.base[k];
            for (int k = 0; k < 3; ++k) o.off[k] = o.baseOff[k];
            o.pitch = o.basePitch; o.yaw = o.baseYaw;
        }
        gDbgFloodBoost = gDbgFloodBoostSeed;
        gDbgNote = "reverted every light to the OBJ's baked values";
    }
    if (ImGui::IsItemHovered())
        UiTooltip("Back to what the OBJ bakes, for every light - the "
                  "clean slate to start a tuning pass from.");
    ImGui::SameLine();
    if (ImGui::Button("Log DSL")) DbgLogDsl(l);
    if (ImGui::IsItemHovered())
        UiTooltip("Print this light in .phdsl shape to the Log tab - the "
                  "deliverable of a tuning session. Nothing here writes "
                  "the DSL for you; a tuned value that never gets "
                  "transcribed is lost on the next build.");

    // The screens group link, applied AFTER this frame's widgets so whatever
    // they did to the selected light lands on the other five the same frame.
    // Everything but position is shared: the baked pos and the live offset are
    // the one per-light attribute of a screen-glow light.
    if (gDbgScreensLink && l.source == "du") {
        for (size_t i = 0; i < gDbgLights.size(); ++i) {
            DbgLight& o = gDbgLights[i];
            if (o.n == l.n || o.source != "du") continue;
            for (int k = 0; k < kDbgParamCount; ++k) o.v[k] = l.v[k];
            o.pitch = l.pitch; o.yaw = l.yaw;
        }
    }
}

// ---- dev-only extras under the shared Cockpit pane --------------------------
// The Dev window's Cockpit tab is the SHIPPING pane plus this: a live intensity
// multiplier for the simplified main panel floods, i.e. the DSL's
// `optimize: boost <f>` as a knob. It sits on THIS tab, beside the "Use
// simplified lighting" checkbox that shows what it drives, and deliberately not
// inside BuildCockpitPerformance — that pane is built by the shipping window
// too, and nothing dev-only may reach a release (same rule as the probe).
static void BuildDevCockpitTuning() {
    if (!gDbgScanned) DbgLoadManifest();
    int boosted = 0;
    for (size_t i = 0; i < gDbgLights.size(); ++i)
        if (gDbgLights[i].boost > 0.0f) ++boosted;

    ImGui::PushID("cockpit-dev-tuning");
    ImGui::Spacing();
    ImGui::SeparatorText("Simplified flood tuning (dev)");
    ImGui::Spacing();
    if (!gDbgOwnsRefs) {
        UiHint("Another plugin owns ToLissPhoton/debug/light/* - see the Lights "
               "tab for who and why.");
    } else if (!boosted) {
        UiHint("Needs a DEBUG interior OBJ, which emits the simplified floods as "
               "tunable lights:\n"
               "    src\\native\\deploy.ps1 -Dev -DebugObjs   (or Build tab > "
               "Interior debug)\n"
               "then Reload manifest on the Lights tab.");
    } else {
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderFloat("intensity multiplier", &gDbgFloodBoost,
                           0.25f, 3.0f, "x%.2f");
        if (ImGui::IsItemHovered())
            UiTooltip("Scales all %d simplified main panel floods together - the "
                      "live form of the DSL's `optimize: boost <f>`. Their color, "
                      "aim and authored size are per-light knobs on the Lights "
                      "tab. Authored factor: x%.2f.",
                      boosted, (double)gDbgFloodBoostSeed);
        ImGui::SameLine();
        if (ImGui::SmallButton("authored")) gDbgFloodBoost = gDbgFloodBoostSeed;
        ImGui::SameLine();
        if (ImGui::SmallButton("Save tuning")) DbgSaveTuning(false);
        if (ImGui::IsItemHovered())
            UiTooltip("Write the multiplier and every edited light to the tuning "
                      "file - same button as the Lights tab's Save tuning.");
        // The gate this knob depends on, said HERE: with the full stacks drawing
        // the simplified set is hidden and the slider looks inert.
        if (!gCockpit.optimized)
            UiHint("'Use simplified lighting' is OFF above, so the full stacks "
                   "are drawing and the simplified floods are hidden - turn it "
                   "on to see what this knob moves.");
        if (!gDbgNote.empty()) ImGui::TextDisabled("%s", gDbgNote.c_str());
    }
    ImGui::PopID();
}

static void BuildLightsTab() {
    // Normally already loaded on aircraft load — the OBJ reads these datarefs
    // whether or not this window is open, so a manifest that waited for the tab
    // would mean every debug light black until someone clicked it. This is the
    // fallback for a dev build started with the aircraft already in place.
    if (!gDbgScanned) DbgLoadManifest();

    if (!gDbgOwnsRefs) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "Another plugin already owns ToLissPhoton/debug/light/*.");
        UiHint("PI_PhotonDevReload.py (XPPython3) registers the same names, and its "
               "accessors are read-only computed values - two owners cannot share "
               "them. Remove that script from XPPython3 and restart the sim to "
               "drive the lights from here, or keep using its own window.");
        return;
    }

    if (ImGui::Button("Reload manifest")) DbgLoadManifest();
    if (ImGui::IsItemHovered())
        UiTooltip("Re-read lights_inn.debug.json + lights_screens.debug.json "
                  "from the loaded aircraft (both are merged - they live on "
                  "separate dataref slots). Needed after every --debug "
                  "rebuild that changes the light LIST; editing values "
                  "needs nothing.");
    ImGui::SameLine();
    const int edits = DbgTuningEditCount();
    char saveLabel[64];
    std::snprintf(saveLabel, sizeof(saveLabel), "Save tuning (%d)###savetuning", edits);
    if (ImGui::Button(saveLabel)) DbgSaveTuning(false);
    if (ImGui::IsItemHovered())
        UiTooltip("Write every edited light - plus the simplified-flood "
                  "multiplier - in .phdsl shape to the file named below.\n"
                  "This also happens BY ITSELF when the aircraft reloads and "
                  "when the sim shuts down, and the file is read back on the "
                  "next load, so a session is not lost by closing X-Plane. "
                  "Transcribe it into src/lights/*.phdsl when the look is "
                  "settled - a rebuild is what makes it permanent.");
    ImGui::SameLine();
    ImGui::Checkbox("Isolate", &gDbgIsolate);
    if (ImGui::IsItemHovered())
        UiTooltip("Black out every light but the selected one. The fastest "
                  "answer to \"which lamp is that?\".");
    ImGui::SameLine();
    ImGui::Checkbox("Mark", &gDbgMark);
    if (ImGui::IsItemHovered())
        UiTooltip("Paint the selected light magenta - nothing in a cockpit "
                  "is that color.");
    ImGui::SameLine();
    ImGui::Checkbox("Blink", &gDbgBlink);
    ImGui::SameLine();
    bool compare = gDbgCompare != 0;
    if (ImGui::Checkbox("Tunable", &compare)) gDbgCompare = compare ? 1 : 0;
    if (ImGui::IsItemHovered())
        UiTooltip("On: the tunable copies. Off: the ORIGINAL light lines, "
                  "which this window cannot touch.\nThe only way to tell a "
                  "bad LIGHT_SPILL_CUSTOM conversion apart from bad "
                  "authored values - they look identical in the cockpit.");

    if (gDbgMarkerDots > 0) {
        ImGui::SameLine();
        bool markers = gDbgShowMarkers != 0;
        if (ImGui::Checkbox("Markers", &markers))
            gDbgShowMarkers = markers ? (gDbgSel >= 0 ? gDbgSel + 1 : -1) : 0;
        if (ImGui::IsItemHovered())
            UiTooltip("Draw the light's origin (green), its aim (amber) and "
                      "the cone rim (blue) as billboards.\nA spill light is "
                      "invisible in air and the bright part of its pool is "
                      "not its origin, so this is the only way to see where "
                      "one actually is.");
        if (markers) {
            ImGui::SameLine();
            if (ImGui::SmallButton(gDbgShowMarkers < 0 ? "all" : "selected"))
                gDbgShowMarkers = gDbgShowMarkers < 0 ? (gDbgSel + 1) : -1;
            // Size and reach live next to the switch that turned the markers on:
            // a marker too small to see and a marker that is not drawn look
            // exactly alike, so the fix has to be where that confusion happens.
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("size", &gDbgMarkerSize, 0.01f, 0.30f, "%.3f m");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("reach", &gDbgMarkerReach, 0.05f, 3.00f, "%.2f m");
            if (ImGui::IsItemHovered())
                UiTooltip("How far the arrow and the cone ring are drawn. "
                          "The right length depends entirely on the light - "
                          "a 20 cm arrow is unreadable next to a pool of "
                          "light a meter across.");
        }
    }

    // ---- the slot probe ----------------------------------------------------
    // Its own row because it is not a knob: while it runs, every value on this
    // page is bypassed and the cockpit is showing one slot of the baseline.
    ImGui::Separator();
    ImGui::TextUnformatted("Probe");
    ImGui::SameLine();
    if (ImGui::SmallButton("off")) gDbgProbe = -1;
    for (int k = 0; k < kDbgParamCount; ++k) {
        ImGui::SameLine();
        char id[8];
        std::snprintf(id, sizeof(id), "%d", k);
        const bool on = gDbgProbe == k;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.7f, 1.0f));
        if (ImGui::SmallButton(id)) gDbgProbe = on ? -1 : k;
        if (on) ImGui::PopStyleColor();
    }
    if (gDbgProbe >= 0)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 1.0f, 1.0f),
                           "slot %d = %g -> expect: %s   (every other light is "
                           "blacked out, and this page is bypassed)",
                           gDbgProbe, (double)kDbgProbeValue,
                           kDbgProbeExpect[gDbgProbe]);
    else
        UiHint("Sends a fixed baseline with ONE of the nine floats set, so whatever "
               "the sim does is caused by that slot and nothing else. Nine clicks "
               "map our array onto X-Plane's parameters with no inference - which is "
               "how the wire order was measured. Probe one slot at a time BEFORE "
               "hypothesizing: three earlier explanations were all wrong.");

    if (gDbgLights.empty()) {
        UiHint("No debug lights. Build the OBJs in their tunable shape first:\n"
               "    src\\native\\deploy.ps1 -Dev -DebugObjs\n"
               "(or per target: build/build_objs.py build --target interior "
               "--debug --write, same for screens)\nthen Reload manifest. %s",
               gDbgNote.empty() ? "" : ("(" + gDbgNote + ")").c_str());
        return;
    }

    ImGui::TextDisabled("%s - %d lights%s", gDbgManifest.c_str(),
                        (int)gDbgLights.size(),
                        gDbgPosTunable ? ", position tunable" : "");
    if (!gDbgNote.empty()) { ImGui::SameLine(); ImGui::TextDisabled("| %s", gDbgNote.c_str()); }

    // ⚠ The destination, spelled out. It moves depending on whether the Build
    // tab knows the repo, and a file written somewhere the user is not looking
    // is the same as no file at all — which is the failure this whole mechanism
    // was added to stop happening twice.
    const std::string tuningPath = DbgTuningFilePath().string();
    ImGui::TextDisabled("tuning file: %s", tuningPath.c_str());
    if (ImGui::IsItemHovered())
        UiTooltip("Saved here automatically on aircraft reload and sim shutdown, "
                  "and read back on the next load. Set the repo path on the "
                  "Build tab to have it land in .scratch/ instead, where it is "
                  "beside the .phdsl you will transcribe it into.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Forget saved")) DbgForgetTuning();
    if (ImGui::IsItemHovered())
        UiTooltip("Delete that file, so the next aircraft load shows the OBJ's "
                  "authored values with nothing layered on top. Do this once a "
                  "session has been transcribed into the .phdsl - otherwise the "
                  "saved copy keeps being restored over a rebuild.");
    ImGui::Separator();

    ImGui::BeginChild("dbg_tree", ImVec2(260.0f, 0.0f), ImGuiChildFlags_Borders);
    BuildLightTree();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("dbg_edit", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    if (DbgLight* sel = DbgBySlot(gDbgSel)) BuildLightEditor(*sel);
    else                                    ImGui::TextUnformatted("Pick a light on the left.");
    ImGui::EndChild();
}

// ---- the window ------------------------------------------------------------
// One tab, panel and all. Eleven of them, so the shipping window's spelled-out
// three-liner would bury this file's only interesting line per tab — and a tab
// added without the panel would be transparent over the cockpit, which is the
// failure UiBeginPanel exists to prevent. A captureless lambda converts to the
// function pointer, so the call sites still read as one line each.
static void DevTab(const char* label, void (*body)()) {
    if (!ImGui::BeginTabItem(label)) return;
    if (UiBeginPanel("##pane")) body();
    UiEndPanel();
    ImGui::EndTabItem();
}

static void BuildDevUi() {
    if (!UiBeginTabBar("photon_dev")) return;

    // The same two builders the shipping settings window uses, so a tuning pass
    // never has to wonder whether the Dev window's copy has drifted.
    DevTab("Exterior", [] { BuildConfigTab(kExtAxis); });
    // The shipping pane plus the dev-only simplified-flood knob — appended
    // OUTSIDE the shared builder so it cannot reach the release window.
    DevTab("Cockpit",  [] { BuildConfigTab(kIntAxis); BuildDevCockpitTuning(); });
    DevTab("Displays", [] { BuildDisplaysTab(); });
    // Same pane as the shipping Performance window, with `dev` true: the internal
    // levers and the full sweep are the only difference.
    DevTab("Perf",     [] { BuildPerfTab(true); });
    DevTab("Panel FX", [] { BuildFxTab(); });
    DevTab("Lights",   [] { BuildLightsTab(); });
    DevTab("History",  [] { BuildFxHistoryPane(); });
    DevTab("Probe",    [] { BuildProbeTab(); });
    DevTab("Build",    [] { BuildBuildTab(); });
    DevTab("Log",      [] { BuildLogTab(); });
    // The backend smoke test. If the stock demo draws, scrolls, responds to the
    // mouse and accepts typing, imgui_xplm.cpp is correct — a far better first
    // question than "why does my tool window look wrong", and the fastest widget
    // reference there is.
    DevTab("ImGui", [] {
        UiHint("Backend smoke test. Everything below is stock ImGui.");
        ImGui::Separator();
        ImGui::ShowDemoWindow();
    });
    ImGui::EndTabBar();
}

static UiWindow gDevWin = { "ToLiss Photon - Dev", 1000, 620, BuildDevUi, nullptr };
static void ShowDevWindow()    { ShowUiWindow(gDevWin); }
static void DestroyDevWindow() { DestroyUiWindow(gDevWin); }

// Called from the two teardown paths (menu rebuild and plugin stop).
static void DestroyDebugImguiWindows() { DestroyDevWindow(); }

// ---- the Dev submenu -------------------------------------------------------
// Level-2 nesting, same as Exterior / Cockpit: the no-submenu rule in CLAUDE.md
// is an XPPython3 defect and this is native C++. See CreatePhotonMenu's note.
static XPLMMenuID gDbgMenuID = nullptr;
enum { kDbgMenuWindow = 0, kDbgMenuFcu, kDbgMenuOff, kDbgMenuReload };

static void DebugMenuHandler(void*, void* itemRef) {
    switch ((intptr_t)itemRef) {
        case kDbgMenuWindow: ShowDevWindow(); break;
        case kDbgMenuFcu:    ApplyFcuPreset(); SavePanelDebug(); ShowDevWindow(); break;
        case kDbgMenuOff:    WritePanelProbeMode(nullptr, kProbeOff); SavePanelDebug(); break;
        case kDbgMenuReload:
            // ⚠ MENU-HANDLER CONTEXT ONLY. This command runs synchronously and
            // unloads the aircraft's plugins; from a flight loop or a draw
            // callback it tears the aircraft down mid-frame. That is why the
            // Build tab has no button for it.
            if (XPLMCommandRef c = XPLMFindCommand("sim/operation/reload_aircraft")) {
                Log("dev: reloading the aircraft");
                XPLMCommandOnce(c);
            }
            break;
        default: break;
    }
}

static void AppendDebugMenu(XPLMMenuID parent) {
    // The separator is OURS, not CreatePhotonMenu's — the shipping build's stub
    // does nothing at all, so a release ends cleanly on About... rather than on
    // a divider with nothing under it.
    XPLMAppendMenuSeparator(parent);
    int item = XPLMAppendMenuItem(parent, "Dev", nullptr, 0);
    gDbgMenuID = XPLMCreateMenu("Dev", parent, item, DebugMenuHandler, nullptr);
    if (!gDbgMenuID) return;
    XPLMAppendMenuItem(gDbgMenuID, "Dev window...", (void*)(intptr_t)kDbgMenuWindow, 0);
    XPLMAppendMenuSeparator(gDbgMenuID);
    // The states worth reaching without reading anything: set up the tuning
    // pass, stop painting over the cockpit, pick up a rebuilt OBJ.
    XPLMAppendMenuItem(gDbgMenuID, "FCU tint preset",  (void*)(intptr_t)kDbgMenuFcu, 0);
    XPLMAppendMenuItem(gDbgMenuID, "Probe off",        (void*)(intptr_t)kDbgMenuOff, 0);
    XPLMAppendMenuItem(gDbgMenuID, "Reload aircraft",  (void*)(intptr_t)kDbgMenuReload, 0);
}

static void DestroyDebugMenu() {
    if (gDbgMenuID) { XPLMDestroyMenu(gDbgMenuID); gDbgMenuID = nullptr; }
}

#endif  // PHOTON_DEV — the Dev window, the FX editor, the history and the Dev menu

// ================================ lifecycle ==================================
PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    std::strcpy(outName, "ToLiss Photon");
    std::strcpy(outSig,  "toliss.photon.lighting");
    std::strcpy(outDesc, "ToLiss Photon Lighting: per-category profiles, datarefs, and per-livery save.");

    // Native (forward-slash) paths so std::filesystem joins behave on all OSes.
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);

    for (int i = 0; i < NCAT; ++i) {
        gCategoryRefs[i] = XPLMRegisterDataAccessor(
            kCategories[i].dataref, xplmType_Int | xplmType_Float, 0,
            ReadCategoryInt, nullptr, ReadCategoryFloat, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            (void*)(intptr_t)i, nullptr);
    }
    // Interior categories — same int|float pairing and the same XPluginStart
    // timing requirement as the exterior nine: an OBJ binds dataref names at LOAD
    // time, so one that doesn't exist yet leaves every ANIM_hide reading 0 forever.
    for (int i = 0; i < NINT; ++i) {
        gIntCategoryRefs[i] = XPLMRegisterDataAccessor(
            kIntCategories[i].dataref, xplmType_Int | xplmType_Float, 0,
            ReadIntCategoryInt, nullptr, ReadIntCategoryFloat, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            (void*)(intptr_t)i, nullptr);
    }
    // The reduced-light-count switch and the screens' master gate. Both are
    // ANIM_hide gates in an OBJ, so both have the same XPluginStart timing
    // requirement as the categories above — a name that does not exist when the
    // OBJ loads reads 0 for the life of that aircraft. For these two 0 is the
    // fail-open state (full stack / screens drawn), which is what makes that
    // survivable rather than silent.
    gIntOptimizedRef = XPLMRegisterDataAccessor(
        kIntOptimizedRef, xplmType_Int | xplmType_Float, 0,
        ReadIntOptimizedInt, nullptr, ReadIntOptimizedFloat, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr);
    gScreensDisabledRef = XPLMRegisterDataAccessor(
        kScreensDisabledRef, xplmType_Int | xplmType_Float, 0,
        ReadScreensDisabledInt, nullptr, ReadScreensDisabledFloat, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr);
    // The map spot's 9-float custom-light dataref (read-only to the sim).
    gSpillMapRef = XPLMRegisterDataAccessor(
        kSpillMapDataRef, xplmType_FloatArray, 0,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, ReadSpillMap, nullptr, nullptr, nullptr, nullptr, nullptr);
    // The six screen-glow custom-light datarefs (read-only to the sim). Registered
    // here with everything else because an OBJ binds dataref NAMES at LOAD time: a
    // name that does not exist yet reads 0 for the life of that aircraft, which
    // would leave the glow permanently black. Registered unconditionally — whether
    // lights_screens.obj is attached is not knowable at XPluginStart, and an unused
    // accessor costs nothing.
    for (int i = 0; i < kScreenCount; ++i) {
        gScreenDataRefNames[i] = std::string(kSpillScreenPrefix) + std::to_string(i);
        gSpillScreenRefs[i] = XPLMRegisterDataAccessor(
            gScreenDataRefNames[i].c_str(), xplmType_FloatArray, 0,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, ReadSpillScreen, nullptr, nullptr, nullptr,
            (void*)(intptr_t)i, nullptr);
    }
    // The inferred DCDU levels. Registered here with everything else because the
    // FX layer definition names it as a brightness source and that is resolved on
    // the first frame the compositor draws — which can be before any DCDU button
    // is ever pressed.
    gDcduBrightnessRef = XPLMRegisterDataAccessor(
        kDcduBrightnessRef, xplmType_FloatArray, 0,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, ReadDcduBrightness, nullptr, nullptr, nullptr, nullptr, nullptr);
    gIsLedRef = XPLMRegisterDataAccessor(
        kIsLedDataref, xplmType_Int | xplmType_Float, 0,
        ReadIsLedInt, nullptr, ReadIsLedFloat, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    // Photon's own skin-glow array (read-only to the sim) — must exist before any
    // redirected mesh OBJ binds it at load. Float-array accessor: only the
    // read-float-array slot is set.
    gGlowArrayRef = XPLMRegisterDataAccessor(
        kGlowArrayDataRef, xplmType_FloatArray, 0,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, ReadGlowArray, nullptr, nullptr, nullptr, nullptr, nullptr);
    gBeaconDbgRef = XPLMRegisterDataAccessor(
        kBeaconAlwaysOn, xplmType_Int | xplmType_Float, 1,
        ReadBeaconDbgInt, WriteBeaconDbgInt, ReadBeaconDbgFloat, WriteBeaconDbgFloat,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    gStrobeDbgRef = XPLMRegisterDataAccessor(
        kStrobeAlwaysOn, xplmType_Int | xplmType_Float, 1,
        ReadStrobeDbgInt, WriteStrobeDbgInt, ReadStrobeDbgFloat, WriteStrobeDbgFloat,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    LoadProfilesFile();
    RemoveSupersededFiles();
    Log("started; registered " + std::to_string(NCAT) + " exterior + "
        + std::to_string(NINT) + " interior category datarefs");
    return 1;
}

PLUGIN_API int XPluginEnable(void) {
    gLiveryIndexRef = XPLMFindDataRef(kLiveryIndexDataRef);
    if (!gAutoLoopRegistered) {
        XPLMRegisterFlightLoopCallback(AutoLoop, 1.0f, nullptr);
        gAutoLoopRegistered = true;
    }
    StartEngine();
    StartPanelPass();          // FX compositor (+ the probe in a dev build)
    UpdateMenuVisibility();
    LoadProfileForCurrentLivery();
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
#if PHOTON_DEV
    // ⚠ FIRST, and before StopPanelPass tears the light pool down. This is the
    // ordinary way a tuning session ends — the user quits X-Plane — and until it
    // existed that was also the way a tuning session was silently thrown away.
    DbgAutoSaveTuning();
#endif
    if (gAutoLoopRegistered) {
        XPLMUnregisterFlightLoopCallback(AutoLoop, nullptr);
        gAutoLoopRegistered = false;
    }
    StopPanelPass();
    // ⚠ BEFORE StopEngine, and it is not merely tidy ordering: a benchmark that is
    // mid-run has the waveform engine as one of its levers, so PerfShutdown's
    // restore may START the engine — after which StopEngine below is what actually
    // leaves it stopped. The other order leaves a disabled plugin running a
    // per-frame flight loop.
    PerfShutdown();
    StopEngine();
    // A disabled plugin must not still be counting DCDU button presses. The
    // handlers are registered from UpdateMenuVisibility, which XPluginEnable
    // calls, so this is the matching half — and without it the four handlers are
    // the one thing that keeps running while the user has us switched off.
    UnbindDcduCommands();
}

PLUGIN_API void XPluginStop(void) {
    // ⚠ Again here, and it is not redundant with XPluginDisable: the sampler is a
    // real thread, and a thread still running when this module is unloaded is a
    // crash inside code that no longer exists. PerfShutdown is idempotent, so the
    // ordinary Disable -> Stop path costs nothing.
    PerfShutdown();
    StopEngine();
    DestroyPhotonMenu();
    // Before the accessors: these are registrations against ToLiss's command
    // refs and must not outlive us any more than they may outlive ToLiss.
    UnbindDcduCommands();
    if (gDcduBrightnessRef) { XPLMUnregisterDataAccessor(gDcduBrightnessRef); gDcduBrightnessRef = nullptr; }
    if (gIsLedRef)     { XPLMUnregisterDataAccessor(gIsLedRef);     gIsLedRef = nullptr; }
    if (gGlowArrayRef) { XPLMUnregisterDataAccessor(gGlowArrayRef); gGlowArrayRef = nullptr; }
    if (gSpillMapRef)  { XPLMUnregisterDataAccessor(gSpillMapRef);  gSpillMapRef = nullptr; }
    for (int i = 0; i < kScreenCount; ++i)
        if (gSpillScreenRefs[i]) { XPLMUnregisterDataAccessor(gSpillScreenRefs[i]); gSpillScreenRefs[i] = nullptr; }
    if (gBeaconDbgRef) { XPLMUnregisterDataAccessor(gBeaconDbgRef); gBeaconDbgRef = nullptr; }
    if (gStrobeDbgRef) { XPLMUnregisterDataAccessor(gStrobeDbgRef); gStrobeDbgRef = nullptr; }
    for (int i = 0; i < NCAT; ++i)
        if (gCategoryRefs[i]) { XPLMUnregisterDataAccessor(gCategoryRefs[i]); gCategoryRefs[i] = nullptr; }
    for (int i = 0; i < NINT; ++i)
        if (gIntCategoryRefs[i]) { XPLMUnregisterDataAccessor(gIntCategoryRefs[i]); gIntCategoryRefs[i] = nullptr; }
    if (gIntOptimizedRef)    { XPLMUnregisterDataAccessor(gIntOptimizedRef);    gIntOptimizedRef = nullptr; }
    if (gScreensDisabledRef) { XPLMUnregisterDataAccessor(gScreensDisabledRef); gScreensDisabledRef = nullptr; }
    Log("stopped");
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int inMessage, void* inParam) {
    if ((intptr_t)inParam != 0) return;   // user aircraft only
    if (inMessage == XPLM_MSG_PLANE_LOADED) {
        UpdateMenuVisibility();
        ReseatPanelPass();     // after the aircraft's own plugin started — see RegisterPanelPass
        LoadProfileForCurrentLivery();
    } else if (inMessage == XPLM_MSG_LIVERY_LOADED) {
        LoadProfileForCurrentLivery();
    }
}
