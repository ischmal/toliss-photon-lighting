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

// Every window in this plugin is Dear ImGui, shipping ones included, so the
// backend comes in unconditionally.
#include "imgui_xplm.h"

// Raw GL is dev-only — the panel probe and the FX compositor. -DPHOTON_DEV=ON
// keeps it, and everything else under "dev tooling", out of the shipping .xpl.
#if PHOTON_DEV
  #if IBM
    #include <windows.h>
    #include <GL/gl.h>
  #elif APL
    #include <OpenGL/gl.h>
  #else
    #include <GL/gl.h>
  #endif

  // The FX compositor's overlay images. The SDK has no image loader — XPLM4
  // dropped XPLMLoadTexture and left only "here is a texture number, upload it
  // yourself" — so one is vendored. Dev-only, like everything it serves.
  // STBI_NO_STDIO: files are read through std::ifstream from a fs::path, so a
  // non-ASCII path behaves like every other path in this plugin.
  #define STB_IMAGE_IMPLEMENTATION
  #define STBI_NO_STDIO
  #define STBI_ONLY_PNG
  #define STBI_ONLY_JPEG
  #define STBI_ONLY_BMP
  #define STBI_ONLY_TGA
  #include "stb_image.h"
#endif

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
// enum, own persisted keys. Mostly TERNARY (0 old halogen / 1 new halogen /
// 2 LED), which is why lights_inn.obj gates each branch with a PAIR of ANIM_hide
// lines. `dome` is TWO-valued (0 fluorescent / 1 LED); `values` carries that and
// nothing here may assume 3.
//
// ⚠ Nothing may ever write 2 to the dome dataref: its two-value encoding hides
// branch 0 at exactly 1 and branch 1 at exactly 0, so at 2 NEITHER hides and both
// draw. Every writer goes through IntValueForProfile or ClampIntValue.
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
    {"dome",     "ToLissPhoton/interior/dome",     "Dome Lights",       2,
     {"Fluorescent", "LED", nullptr}},
    {"map",      "ToLissPhoton/interior/map",      "Map Lights",        3,
     {"Halogen (Old)", "Halogen (New)", "LED"}},
    {"mainpnl",  "ToLissPhoton/interior/mainpnl",  "Main Panel Flood",  3,
     {"Halogen (Old)", "Halogen (New)", "LED"}},
    {"pedestal", "ToLissPhoton/interior/pedestal", "Pedestal & Tables", 3,
     {"Halogen (Old)", "Halogen (New)", "LED"}},
    {"console",  "ToLissPhoton/interior/console",  "Console Lights",    3,
     {"Halogen (Old)", "Halogen (New)", "LED"}},
};
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
// SPACE changes, which a clamp cannot recover from: a v1 `dome: 1` meant new
// halogen, a v2 `dome: 1` means LED — same number, opposite look. Absent == 1.
static const int kIntPrefsSchema = 2;

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
static const int   kSpillAlphaSlot  = 3;   // the ONLY slot we drive

// ─── display (screen) glow — EXPERIMENTAL, docs/screens_plan.md ──────────────
// Six spill lights, one per main display unit, washing each screen's own light
// back into the cockpit. Same mechanism as the map spot above and for the same
// reason: brightness comes from a per-screen ToLiss dataref, which no rheostat
// INDEX can express, so each light is a LIGHT_SPILL_CUSTOM on a 9-float dataref
// we own and whose ALPHA SLOT ALONE we drive.
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

// How much of a screen's brightness becomes spill. A DU at full brightness is a
// dim source next to a floodlamp, and the OBJ's own `size` already sets the
// reach, so this only trims the top end.
//
// 1.0 since 2026-07-29, was 0.85. The dev tool's Alpha row is the same multiplier
// this is, so the look signed off in-sim at Alpha 1 is what gain 1 reproduces —
// 0.85 would have shipped the approved tuning 15% dim. Trim `size` or the palette
// if it wants toning down, not this.
static const float kScreenGain = 1.0f;

// Is the cockpit mod on this aircraft? It is an independent opt-in install, and
// without it every Cockpit menu item is inert (the stock lights_inn.obj names
// none of our datarefs) — which reads as a broken mod. So the submenu is built
// only when the modded OBJ is really there.
//
// The sentinel is the OBJ BINDING OUR DATAREFS, not the installer's version
// marker: `build --target interior --write` installs a generated OBJ with no
// marker, and that build is as switchable as a released one. Keep in step with
// installer/constants.py INTERIOR_OBJ and the gate names the DSL emits.
static const char*  kInteriorObjName     = "lights_inn.obj";
static const char*  kInteriorObjNeedle   = "ToLissPhoton/interior/";
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
    {"Auto",          INT_PROFILE_AUTO},
    {"Halogen (Old)", INT_PROFILE_OLD},
    {"Halogen (New)", INT_PROFILE_NEW},
    {"LED",           INT_PROFILE_LED},
};
static const int NINTPROFILE = (int)(sizeof(kIntProfileItems) / sizeof(kIntProfileItems[0]));

// Row button labels come from kIntCategories[row].valueLabel — the dome offers
// two looks where the rest offer three. The wording matches the submenu profile
// items deliberately: the same name selects the same look.

// Shown in the About window. Bump with installer/constants.py VERSION.
static const char* kPhotonVersion = "0.6";
static const char* kOrgUrl =
    "https://forums.x-plane.org/files/file/"
    "100717-toliss-photon-exterior-lighting-mod-for-toliss-a319a320a321";
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
    // debug-light manifest is full of non-integers (positions, colours, cones)
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
//     step in the middle of a fade - the one artefact this curve exists to remove.
//   * piecewise LINEAR has a 4x slope break at the middle knot, which reads as a
//     pop rather than a fade.
// Fritsch-Carlson cannot overshoot either bound by construction, and it flattens
// the take-off at the dead-zone edge for free: a zero secant pins both of its
// tangents to zero, so the curve leaves 0.2 horizontally instead of with a corner.
struct CurveKnot { float x, y; };

static const CurveKnot kBrightnessCurve[] = {
    { 0.0f, 0.0f },
    { 0.2f, 0.0f },
    { 0.5f, 0.2f },
    { 1.0f, 1.0f },
};
static const int kBrightnessCurveCount =
    (int)(sizeof(kBrightnessCurve) / sizeof(kBrightnessCurve[0]));

static float gCurveTangent[kBrightnessCurveCount] = { 0.0f };
static bool  gCurveBuilt = false;

// Tangents depend only on the constant table, so this runs once. Two passes, in
// this order: flat segments pin their tangents to zero first, then the limiter
// scales what is left. Interleaving them would let the limiter operate on a
// tangent the next segment is about to zero anyway - harmless here, but only by
// accident of these particular knots.
static void BuildBrightnessCurve() {
    const int n = kBrightnessCurveCount;
    float secant[kBrightnessCurveCount] = { 0.0f };   // secant[i] spans i -> i+1
    for (int i = 0; i < n - 1; ++i) {
        const float dx = kBrightnessCurve[i + 1].x - kBrightnessCurve[i].x;
        secant[i] = dx > 0.0f
            ? (kBrightnessCurve[i + 1].y - kBrightnessCurve[i].y) / dx
            : 0.0f;
    }
    gCurveTangent[0]     = secant[0];
    gCurveTangent[n - 1] = secant[n - 2];
    for (int i = 1; i < n - 1; ++i)
        gCurveTangent[i] = 0.5f * (secant[i - 1] + secant[i]);

    for (int i = 0; i < n - 1; ++i)
        if (secant[i] == 0.0f) { gCurveTangent[i] = 0.0f; gCurveTangent[i + 1] = 0.0f; }

    for (int i = 0; i < n - 1; ++i) {
        if (secant[i] == 0.0f) continue;
        const float a = gCurveTangent[i]     / secant[i];
        const float b = gCurveTangent[i + 1] / secant[i];
        const float s = a * a + b * b;
        if (s > 9.0f) {                       // outside the monotonicity region
            const float k = 3.0f / std::sqrt(s);
            gCurveTangent[i]     = k * a * secant[i];
            gCurveTangent[i + 1] = k * b * secant[i];
        }
    }
    gCurveBuilt = true;
}

// Normalised 0..1 brightness in, 0..1 opacity out, monotone in between.
static float BrightnessResponse(float x) {
    if (!gCurveBuilt) BuildBrightnessCurve();
    const int n = kBrightnessCurveCount;
    if (x <= kBrightnessCurve[0].x)     return kBrightnessCurve[0].y;
    if (x >= kBrightnessCurve[n - 1].x) return kBrightnessCurve[n - 1].y;

    int i = 0;
    while (i < n - 2 && x > kBrightnessCurve[i + 1].x) ++i;
    const float h = kBrightnessCurve[i + 1].x - kBrightnessCurve[i].x;
    if (h <= 0.0f) return kBrightnessCurve[i + 1].y;

    const float t  = (x - kBrightnessCurve[i].x) / h;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float y  =  ( 2.0f * t3 - 3.0f * t2 + 1.0f) * kBrightnessCurve[i].y
                   +  (        t3 - 2.0f * t2 + t   ) * h * gCurveTangent[i]
                   +  (-2.0f * t3 + 3.0f * t2       ) * kBrightnessCurve[i + 1].y
                   +  (        t3 -        t2       ) * h * gCurveTangent[i + 1];
    // Monotone interpolation cannot leave [y_i, y_i+1], so this only matters if
    // someone edits the knots into a non-monotone table. Cheaper than the bug.
    return y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
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
// Live alpha for the map spill light, refreshed each frame from ckpt/lights/map.
static float gMapSpillAlpha = 0.0f;
// Live alpha per screen-glow light, refreshed each frame from AirbusFBW/DUBrightness.
static float gScreenAlpha[kScreenCount] = {0.0f};

static XPLMDataRef gCategoryRefs[NCAT] = {nullptr};
static XPLMDataRef gIntCategoryRefs[NINT] = {nullptr};
static XPLMDataRef gSpillMapRef = nullptr;
static XPLMDataRef gSpillScreenRefs[kScreenCount] = {nullptr};
// The names must outlive registration — XPLMRegisterDataAccessor keeps the char*.
static std::string gScreenDataRefNames[kScreenCount];
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

// The map spot's 9-float custom-light dataref.
//
// X-Plane runs the OBJ's BAKED parameters THROUGH this dataref: it pre-fills the
// buffer with them and the accessor modifies in place (the same
// "getDatavf fills in place" behaviour noted at the top of this file). So we
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
// Identical contract to ReadSpillMap above: X-Plane pre-fills the buffer with the
// OBJ's baked parameters and we modify it IN PLACE, so only alpha is touched and
// color/aim/cone/size keep coming from the DSL. With this plugin absent the
// dataref is simply not found and the baked parameters are drawn unmodified —
// which is why lights.style.phdsl bakes alpha low: the degraded state is a faint
// glow that ignores the DU knobs, never a glare in a dark cockpit.
static int ReadSpillScreen(void* refcon, float* out, int offset, int max) {
    if (!out) return kSpillParamCount;          // size query
    if (offset < 0) offset = 0;
    const int slot = (int)(intptr_t)refcon;
    int n = 0;
    for (int i = 0; i < max && offset + i < kSpillParamCount; ++i) {
        if (offset + i == kSpillAlphaSlot) out[i] = gScreenAlpha[slot];
        ++n;                                     // every other slot passes through
    }
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
static bool GlowMapForIcao(const std::string& icao, GlowMap& out) {
    out = GlowMap{ {kGlowNone,kGlowNone,kGlowNone,kGlowNone},
                   {kGlowNone,kGlowNone,kGlowNone,kGlowNone} };
    if (icao == "A339") {
        out.strobe[0] = 12;   // engine strobe ch0 = left wingtip
        out.strobe[1] = 13;   // ch1 = right wingtip
        out.strobe[2] = 14;   // ch2 = tail
        out.beacon[0] = 15;   // top + bottom beacon glow (both on the beacon
        out.beacon[1] = 16;   // waveform; live values ramp 0..1 with the fade)
        return true;
    }
    return false;
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
// value spaces differ: the ternary categories take the profile at face value while
// the two-valued dome folds BOTH halogen profiles onto 0 and LED onto 1. Routing
// every non-Custom write through here is what keeps 2 off the dome dataref.
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
            // MIGRATION schema 1 -> 2, the dome going two-valued: on the old
            // ternary scale both halogen values meant fluorescent and only 2 meant
            // LED. A stored 1 read on the new scale would come back as LED, which
            // is the one case a clamp cannot fix — hence the marker.
            if (schema < 2)
                out.intCats[kIntDomeIndex] = out.intCats[kIntDomeIndex] == 2 ? 1 : 0;
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

static void LoadProfilesFile() {
    gProfiles.clear();
    std::ifstream f(ProfilesFilePath(), std::ios::binary);
    if (!f) return;
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json::Value root;
    json::Parser parser(src);
    if (!parser.value(root) || root.type != json::Value::Obj) return;
    for (const auto& kv : root.obj) {
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

// ======================= Custom config windows ===============================
// TWO windows, one per axis — Exterior (9 rows x 2 looks) and Cockpit (5 rows,
// 3 looks each except the dome's 2), each opened from its own submenu. They were
// one stacked window; splitting them matches how the axes actually behave:
// independent profiles, independent persistence, independently installable.
// Everything is parameterised on a ConfigWindow so the two share one
// implementation and cannot drift apart.
//
// Buttons sit in a TABLE — one column per look, measured by ImGui — and a row
// may carry fewer buttons than there are columns. The dome is the only such row
// today: its "Fluorescent" covers both halogen eras, so it occupies the first
// column and leaves the second empty, keeping the LED column aligned down the
// window. See RowButtons.
static const int kMaxCols = 3;
static const int kMaxBtns = kMaxCols;   // a row can never have more buttons than columns

struct ConfigWindow {
    bool        interior;    // which axis this window edits
    const char* header;      // one header line above the rows
    int         rows;        // NCAT or NINT
    int         cols;        // grid width = the MOST looks any one row offers
    UiWindow    ui;
};

// One clickable button: the dataref value it selects, its label, and the grid
// columns it covers (colFirst == colLast for the ordinary case).
struct RowButton { int value; const char* label; int colFirst, colLast; };

// The buttons of `row`, left to right. Returns how many.
static int RowButtons(const ConfigWindow& w, int row, RowButton out[kMaxBtns]) {
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

static const char* RowLabel(const ConfigWindow& w, int row) {
    return w.interior ? kIntCategories[row].label : kCategories[row].label;
}

static int RowCurrentValue(const ConfigWindow& w, int row) {
    return w.interior ? gIntValues[row] : gValues[row];
}

// Toggling ANY category switches that axis to Custom and persists all of its
// categories — but only that axis. Changing a cockpit light must not knock the
// exterior off Auto.
static void SetCategoryValue(const ConfigWindow& w, int row, int want) {
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

// The profile row. Selecting one here is the same action as the menu item, so it
// goes through the same SelectProfile / SelectIntProfile — the window and the
// menu can therefore never disagree about what is selected.
static void BuildProfileRow(const ConfigWindow& w) {
    const int  cur = w.interior ? gIntProfile : gProfile;
    const int  n   = w.interior ? NINTPROFILE : NPROFILE;
    for (int i = 0; i < n; ++i) {
        const int value = w.interior ? kIntProfileItems[i].value : kProfileItems[i].value;
        const char* label = w.interior ? kIntProfileItems[i].label : kProfileItems[i].label;
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(label, cur == value) && cur != value) {
            if (w.interior) SelectIntProfile(value);
            else            SelectProfile(value);
        }
    }
    // Custom is a real profile but is never *selected* from here — it is what
    // toggling a row below puts the axis into. Showing it as a radio the user
    // could click would raise "custom of what?"; showing it lit when active is
    // the useful half.
    ImGui::SameLine();
    const bool custom = cur == (w.interior ? INT_PROFILE_CUSTOM : PROFILE_CUSTOM);
    if (custom) ImGui::TextColored(ImVec4(0.0f, 0.73f, 1.0f, 1.0f), "* Custom");
    else        ImGui::TextDisabled("(Custom: change a row below)");
}

static void BuildConfigUi(ConfigWindow& w) {
    ImGui::TextUnformatted(w.header);
    ImGui::Spacing();
    BuildProfileRow(w);
    ImGui::Separator();

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
            if (ImGui::RadioButton(btn[i].label, cur == btn[i].value))
                SetCategoryValue(w, row, btn[i].value);
            if (btn[i].colFirst != btn[i].colLast && ImGui::IsItemHovered())
                ImGui::SetTooltip("One look for both halogen eras — this fitting has "
                                  "no halogen era of its own.");
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static ConfigWindow gExtWin = {
    false, "Custom exterior lights", NCAT, 2,
    { "ToLiss Photon - Exterior", 430, 380, nullptr, nullptr },
};
static ConfigWindow gIntWin = {
    true, "Custom cockpit lighting (mod by Gus Rodrigues)", NINT, 3,
    { "ToLiss Photon - Cockpit", 500, 280, nullptr, nullptr },
};

static void ShowConfigWindow(ConfigWindow& w) {
    if (!w.ui.build) w.ui.build = [&w] { BuildConfigUi(w); };
    ShowUiWindow(w.ui);
}
static void DestroyConfigWindow(ConfigWindow& w) { DestroyUiWindow(w.ui); }

// ================================ About window ===============================
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

static void BuildAboutUi() {
    ImGui::TextUnformatted("ToLiss Photon Lighting");
    ImGui::SameLine();
    ImGui::TextDisabled("v%s", kPhotonVersion);
    ImGui::TextUnformatted("Exterior and cockpit lighting for ToLiss Airbus aircraft.");

    ImGui::Spacing();
    ImGui::SeparatorText("Credits");
    ImGui::TextUnformatted("Photon by schmal.");
    ImGui::TextUnformatted("Cockpit lighting by Gus Rodrigues, used with permission.");

    ImGui::Spacing();
    for (int i = 0; i < NABOUTLINK; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(kAboutLinks[i].label)) OpenUrl(kAboutLinks[i].url);
    }

    ImGui::Spacing();
    UiHint("Not affiliated with or endorsed by ToLiss.");
}

static UiWindow gAboutWin = { "About ToLiss Photon", 460, 250, BuildAboutUi, nullptr };
static void ShowAboutWindow()    { ShowUiWindow(gAboutWin); }
static void DestroyAboutWindow() { DestroyUiWindow(gAboutWin); }


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
    if (i == kMenuCustom) { ShowConfigWindow(gExtWin); return; }
    if (i < 0 || i >= NPROFILE) return;
    SelectProfile(kProfileItems[i].value);
}

static void InteriorMenuHandler(void*, void* itemRef) {
    intptr_t i = (intptr_t)itemRef;
    if (i == kMenuCustom) { ShowConfigWindow(gIntWin); return; }
    if (i < 0 || i >= NINTPROFILE) return;
    SelectIntProfile(kIntProfileItems[i].value);
}

// The top-level menu now holds only "About..." as a clickable item.
static void MenuHandler(void*, void*) { ShowAboutWindow(); }

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
    //                         |            Halogen (Old) / Halogen (New) / LED
    //                         |            ─────────
    //                         |            Custom...
    //                         ─────────
    //                         > About...
    //
    // The separator under Auto sets it apart as the only ADAPTIVE choice: it
    // picks an era from the airframe rather than naming one, so it isn't a peer
    // of the three fixed looks below it.
    //
    // Nesting is deliberate: CLAUDE.md's "no menu level 2+" rule is an XPPython3
    // 4.7a1 defect and binds PI_PhotonDevReload.py, not this native plugin.
    // Custom stays a WINDOW because a grid of toggles is not a menu shape.
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
    // greyed out. Everything downstream keys off gIntMenuID staying null:
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
    }

    // Probe builds only — a no-op in the shipping .xpl, so there is no Debug row
    // to hide or grey out. Same rule that keeps the magenta out of a release.
    AppendDebugMenu(gMenuID);

    XPLMAppendMenuSeparator(gMenuID);
    XPLMAppendMenuItem(gMenuID, "About...", (void*)(intptr_t)0, 0);

    gMenuCreated = true;
    UpdateChecks();
    Log(gMenuHasInterior ? "menu built (Exterior / Cockpit submenus)"
                         : "menu built (Exterior only - cockpit mod not installed)");
}

static void DestroyPhotonMenu() {   // not DestroyMenu: collides with the Win32 API
    DestroyConfigWindow(gExtWin);
    DestroyConfigWindow(gIntWin);
    DestroyAboutWindow();
    DestroyDebugImguiWindows();
    // Destroy the children before the parent: XPLMDestroyMenu on the parent
    // invalidates the submenu IDs it owns.
    if (gExtMenuID) { XPLMDestroyMenu(gExtMenuID); gExtMenuID = nullptr; }
    if (gIntMenuID) { XPLMDestroyMenu(gIntMenuID); gIntMenuID = nullptr; }
    DestroyDebugMenu();
    if (gMenuID) { XPLMDestroyMenu(gMenuID); gMenuID = nullptr; }
    gExtCustomItemIndex = gIntCustomItemIndex = -1;   // stale indices must not be re-checked
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

#if PHOTON_DEV
// Both defined far below, with the panel-FX drivers and the light editor; both
// called from the aircraft-load path, which is far above. See the calls for why
// they must happen there.
static void FxForgetDrivers();
static void DbgLoadManifest();
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
#if PHOTON_DEV
        // ⚠ Every panel-FX driver handle points into the PREVIOUS aircraft's
        // plugin. ToLiss's datarefs are unregistered with it, so a handle kept
        // across the swap is the one way this feature could take the sim down
        // rather than merely look wrong.
        FxForgetDrivers();
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
    // With the OBJ absent nobody reads gScreenAlpha and this is a wasted array
    // fetch; with it present and the array missing, every screen reads 0 (dark),
    // which is the honest answer rather than a guess.
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
        for (int i = 0; i < kScreenCount; ++i) {
            const int idx = kScreenDU[i];
            gScreenAlpha[i] = (idx >= 0 && idx < got)
                ? BrightnessResponse((float)ClampBrightness(du[idx])) * kScreenGain
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
    // ToLiss's own datarefs appear when ITS plugin starts, which can be after
    // our aircraft-loaded hook ran. Retry here (1 Hz) rather than from the
    // per-frame engine loop.
    if (!gMapRheostat) ResolveMapRheostat();
    if (!gDUBrightness) ResolveDUBrightness();
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

// ==================== panel-FBO probe — EXPERIMENTAL, dev-only ===============
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
#if PHOTON_DEV

static const char* kPanelProbeMode  = "ToLissPhoton/debug/panel_probe";
static const char* kPanelProbeRect  = "ToLissPhoton/debug/panel_probe_rect";
static const char* kPanelProbeDraw  = "ToLissPhoton/debug/panel_probe_draw";
static const char* kPanelProbeType  = "ToLissPhoton/debug/panel_probe_pass";
static const char* kPanelProbeCycle = "ToLissPhoton/debug/panel_probe_cycle";
static const char* kPanelRenderType = "sim/graphics/view/panel_render_type";
// The multiply method's colour, one dataref per channel so it can be dialled in
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
//               anything is black, so it masks itself and recolours only the lit
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

// Which callback slot draws. Each is registered separately; only the one matching
// gPanelProbeMode paints, so a single dataref walks all three without a rebuild.
enum {
    kProbeOff          = 0,
    kProbeAfterGauges  = 1,   // magenta — the candidate the real feature would use
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
    // safe; DCDU and the two centre readouts are the project's best reading and
    // nothing more. Confirm one the same way anything here gets confirmed —
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
    // A pair of 2.8x0.8cm windows directly below the ISIS. Named by POSITION
    // because the geometry does not say what they are and a wrong name here is
    // worse than no name.
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
    // invisible; tinting 7 px of a 75 px bezel is a coloured sliver of frame.
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
    // 1.7 px off a 257 px screen edge is invisible; bleeding MCDU colour onto
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
    // that knob without a command — so this one is placed by NEIGHBOURS: it sits
    // on the centreline between the speedbrake lever (11.1 cm) and the flap
    // lever (16.9 cm), just aft of the ENG mode switch (11.6 cm), which is
    // exactly where the A320's RUD TRIM panel is.
    { "rudder trim", 814.8f,  882.4f,  988.7f, 1016.4f, -1 },  // 3.2x1.3cm x-0.020
    // --- five aft-overhead readouts, derived 2026-08-01 ----------------------
    // ⚠ UNIDENTIFIED, and named by POSITION on purpose. They are in the readout
    // batch, they are readout-sized, and they are up on the overhead (y +0.93 to
    // +1.08, z -3.51 to -3.88, i.e. above and behind the RMP3/ACP3 cluster) —
    // but ToLiss puts NO manipulator within 40 cm of four of them, so the method
    // that named everything above has nothing to say here. Do not guess in the
    // name field. One click of the row's ? button floods one magenta in-sim;
    // rename it here then.
    { "ovhd fwd L",  123.1f,  197.9f,  941.1f,  980.1f, -1 },  // 3.6x1.9cm x-0.057
    { "ovhd fwd R",  211.3f,  286.1f,  941.1f,  980.1f, -1 },  // 3.6x1.9cm x+0.032
    { "ovhd aft L1", 381.4f,  457.0f,  890.6f,  929.6f, -1 },  // 3.6x1.9cm x-0.294
    { "ovhd aft L2", 474.9f,  550.6f,  890.6f,  929.6f, -1 },  // 3.6x1.9cm x-0.216
    { "ovhd aft R",  565.9f,  702.7f,  890.6f,  929.6f, -1 },  // 6.5x1.9cm x+0.259
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
    { "ovhd fwd L",  123.1f,  197.9f,  941.1f,  980.1f, -1 },
    { "ovhd fwd R",  211.3f,  286.1f,  941.1f,  980.1f, -1 },
    { "ovhd aft L1", 381.4f,  457.0f,  890.6f,  929.6f, -1 },
    { "ovhd aft L2", 474.9f,  550.6f,  890.6f,  929.6f, -1 },
    { "ovhd aft R",  565.9f,  702.7f,  890.6f,  929.6f, -1 },
};
static const int kPanelRectCount = (int)(sizeof(kPanelRectsHi) / sizeof(kPanelRectsHi[0]));
static_assert(sizeof(kPanelRectsLo) == sizeof(kPanelRectsHi),
              "the two resolution variants must hold the same faces in the same order - "
              "a target index selects a row in whichever table is live");

// --- groups ------------------------------------------------------------------
// A named set of rects addressed as one target, because the thing a person wants
// to recolour is often not one UV island.
//
// ⚠ A group is NOT a shortcut for one wide rect. The FCU row's three faces have
// 22 and 26 px of atlas between them that other cockpit geometry samples, so a
// single 896..2017 quad would recolour something unrelated. Three quads cost
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
//       ├── Centre readouts
//       ├── Radios              the three RMPs
//       │   ├── RMP1            active + standby
//       │   ├── RMP2
//       │   └── RMP3            (on the OVERHEAD, not the pedestal)
//       ├── Overhead readouts   the five unidentified aft-overhead windows
//       ├── XPDR code           (one rect)
//       ├── rudder trim         (one rect)
//       └── ped strips          (one rect)
//
// Glass vs. segment is the split that matters for tinting: a backlit LCD and a
// seven-segment window start from different colours and take a multiply
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
    { "Centre readouts", { "ctr readout L", "ctr readout R", nullptr } },
    { "RMP1",            { "RMP1 active", "RMP1 stby", nullptr } },
    { "RMP2",            { "RMP2 active", "RMP2 stby", nullptr } },
    { "RMP3",            { "RMP3 active", "RMP3 stby", nullptr } },
    { "Radios",          { "RMP1 active", "RMP1 stby", "RMP2 active", "RMP2 stby",
                           "RMP3 active", "RMP3 stby", nullptr } },
    { "Overhead readouts", { "ovhd fwd L", "ovhd fwd R",
                             "ovhd aft L1", "ovhd aft L2", "ovhd aft R", nullptr } },
    { "Segment readouts",{ "capt baro", "FCU strip", "FO baro",
                           "clock CHR", "clock UTC", "clock ET",
                           "ctr readout L", "ctr readout R", "ped strips",
                           "RMP1 active", "RMP1 stby", "RMP2 active", "RMP2 stby",
                           "RMP3 active", "RMP3 stby", "XPDR code", "rudder trim",
                           "ovhd fwd L", "ovhd fwd R",
                           "ovhd aft L1", "ovhd aft L2", "ovhd aft R", nullptr } },
    { "Whole panel",     { "capt PFD", "capt ND", "E/WD", "SD", "FO ND", "FO PFD",
                           "FCU strip", "capt baro", "FO baro",
                           "ISIS", "DCDU capt", "DCDU FO",
                           "clock CHR", "clock UTC", "clock ET",
                           "ctr readout L", "ctr readout R", "ped strips",
                           "EFB capt", "EFB FO", "EFB AviTab",
                           "MCDU capt", "MCDU FO",
                           "RMP1 active", "RMP1 stby", "RMP2 active", "RMP2 stby",
                           "RMP3 active", "RMP3 stby", "XPDR code", "rudder trim",
                           "ovhd fwd L", "ovhd fwd R",
                           "ovhd aft L1", "ovhd aft L2", "ovhd aft R", nullptr } },
};
static const int kPanelGroupCount = (int)(sizeof(kPanelGroups) / sizeof(kPanelGroups[0]));

// Targets share one index space so a single int selects either: [0, rectCount)
// is a rect, [rectCount, rectCount+groupCount) is a group.
static const int kPanelTargetCount = kPanelRectCount + kPanelGroupCount;

static XPLMDataRef gDisplayFallbackRef = nullptr;

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
// 36, and `1u << 35` is undefined behaviour that in practice wraps — every rect
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

static XPLMDataRef  gPanelProbeModeRef = nullptr;
static XPLMDataRef  gPanelProbeRectRef = nullptr;
static XPLMDataRef  gPanelProbeDrawRef = nullptr;
static XPLMDataRef  gPanelProbeTypeRef = nullptr;
static XPLMDataRef  gPanelTintRefs[3] = { nullptr, nullptr, nullptr };
static XPLMDataRef  gPanelRenderTypeRef = nullptr;
static XPLMDataRef  gPanelExtentRefs[4] = { nullptr, nullptr, nullptr, nullptr }; // l b r t
static XPLMCommandRef gPanelProbeCycleCmd = nullptr;

static int  gPanelProbeMode = kProbeAfterGauges;  // on by default: this build exists to look
static int  gPanelProbeRect = kProbeFloodRect;    // -2 = flood, -1 = all rects, else index
static int  gPanelProbeDrawMethod = kDrawClear;   // the one that bypasses the most
static bool gPanelProbeRegistered = false;
static int  gPanelTypesSeen = 0;                  // bitmask of panel_render_type values
// Latches are per panel_render_type, not global. X-Plane runs the 3-d panel
// through TWO passes — 1 = non-lit, 2 = lit — and they need not share a
// framebuffer. A single bool logged whichever pass happened to run first and
// silently hid the other, which is exactly the question "is this the LIT fbo?".
static int  gPanelStateLoggedMask = 0;            // bit N = type N dumped; cleared on any change
static int  gPanelErrLoggedMask = 0;              // same, for the post-draw glGetError
static int  gPanelProbeType = -1;                 // -1 = both passes, else only that type

// The multiply colour. Green-suppressing, to pull ToLiss's yellow-green FCU
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
static void EmitRectQuad(const PanelRect& r, float cr, float cg, float cb, float ca,
                         float tile) {
    GLint front = GL_CCW;
    glGetIntegerv(GL_FRONT_FACE, &front);

    const float t = tile;
    glColor4f(cr, cg, cb, ca);
    glBegin(GL_QUADS);
    if (front == GL_CCW) {
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
    glEnd();
}

// The probe's own fills are opaque and untextured; the FX compositor needs
// per-layer alpha and tiling, and PanelPaintFn's signature has no room for
// either. Both are set immediately before each PaintPanelTarget call.
static float gFxAlpha = 1.0f;
static float gFxTile  = 1.0f;
static void DrawProbeRect(const PanelRect& r, int, float cr, float cg, float cb) {
    EmitRectQuad(r, cr, cg, cb, 1.0f, 1.0f);
}
// Forward declaration: the FX rect painter needs the brightness drivers, which
// are declared with the layer stacks further down. Defined there.
static void DrawFxRect(const PanelRect& r, int rectIndex, float cr, float cg, float cb);

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

    const PanelGroup& g = kPanelGroups[target - kPanelRectCount];
    const int maxMembers = (int)(sizeof(g.members) / sizeof(g.members[0]));
    for (int m = 0; m < maxMembers && g.members[m]; ++m) {
        const int i = RectIndexByName(g.members[m]);
        if (i >= 0) paint(rects[i], i, cr, cg, cb);
    }
}

// ---- "which render target are we actually on?" -----------------------------
// The whole point of the rebuild: the first probe never asked. A quad that goes
// nowhere and a quad that goes somewhere invisible look identical from the
// cockpit, and only the framebuffer id distinguishes them.
//
// Expected if we are on the panel target: a NON-ZERO draw framebuffer whose
// viewport is the panel texture size (4096x4096 here), with a colour attachment
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
        Log("panel probe: ^ a shader program is BOUND — immediate-mode colour will "
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
// Forced had been used at least once. That is not a probe artefact to shrug at:
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
// whole-cockpit artefact rather than a wrong-looking rect. It is saved here
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

static void PushMultiplyBlend() {
    PushBlendFunc();
    glBlendFunc(GL_DST_COLOR, GL_ZERO);
}
static void PopMultiplyBlend() { PopBlendFunc(); }

// ===================== panel FX: the overlay texture pool ====================
//
// A layer can be an IMAGE instead of a flat colour: a gradient, a vignette, a
// scanline pattern, a hand-painted glow. The image modulates the layer colour
// (fixed-function GL_MODULATE, i.e. src = texel x colour), so every control that
// worked on a solid layer still works — the colour tints the image and the
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
// EDITING a file is live: the pool stat()s its files once a second and re-uploads
// any whose bytes have moved, so a save in a paint program shows up in the
// cockpit with nothing clicked. Only ADDING or renaming needs a rescan, because
// only those change the LIST — which is also why the two are separate operations
// rather than one "reload" button.
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
// (white x colour = colour), which is what lets the compositor set the graphics
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
static const fs::path& FxOverlayDir() {
    static fs::path dir;
    if (!dir.empty()) return dir;

    char name[512] = {0}, path[512] = {0}, sig[512] = {0}, desc[512] = {0};
    XPLMGetPluginInfo(XPLMGetMyID(), name, path, sig, desc);
    const fs::path xpl(path);
    // .../ToLissPhoton/<arch>/ToLissPhoton.xpl -> .../ToLissPhoton/
    if (xpl.has_parent_path() && xpl.parent_path().has_parent_path()) {
        dir = xpl.parent_path().parent_path() / "overlays";
        return dir;
    }

    char root[512] = {0};
    XPLMGetSystemPath(root);
    dir = fs::path(root) / "Resources" / "plugins" / "ToLissPhoton" / "overlays";
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
            "The image MODULATES the layer colour: the layer colour tints it and the\r\n"
            "opacity fades it out, exactly as for a solid layer. Alpha is respected --\r\n"
            "a transparent texel leaves the pixel under it alone in every blend mode.\r\n"
            "\r\n"
            "Layers are saved by FILE NAME, so renaming a file here orphans the layers\r\n"
            "that used it (it is reported in Log.txt, never silently ignored).\r\n"
            "\r\n"
            "EDITING an image is live: save it in your paint program and the cockpit\r\n"
            "follows within a second, no reload and no restart. Only ADDING or renaming\r\n"
            "a file needs the tab's \"Rescan images\" button.\r\n"
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

// Notice edits on their own, throttled — the point is that saving in a paint
// program is enough, with no button to remember.
//
// Cheap enough to sit in the panel pass: a handful of stat() calls a second.
// It runs from the compositor rather than only from the Panel FX tab because
// the thing you are watching while you iterate is the COCKPIT, not the editor,
// and the tab is often not even the visible one.
static void PollFxTextureChanges() {
    if (!gFxTexturesScanned) { ScanFxTextures(); return; }
    static float last = 0.0f;
    const float now = XPLMGetElapsedTime();
    if (now - last < 1.0f && now >= last) return;     // >= guards a clock reset
    last = now;

    const fs::path dir = FxOverlayDir();
    for (size_t i = 0; i < gFxTextures.size(); ++i) {
        FxTexture& t = gFxTextures[i];
        if (t.id == 0 && !t.failed) continue;          // not on the card yet anyway
        const FxStamp s = FxStampOf(dir / t.name);
        if (s.size == 0 || s == t.stamp) continue;     // gone, or unchanged
        if (t.id != 0) t.stale = true;
        t.failed = false;
        Log("panel fx: overlay '" + t.name + "' changed on disk - reloading");
    }
}

static FxTexture* FxTextureByName(const std::string& name) {
    if (name.empty()) return nullptr;
    if (!gFxTexturesScanned) ScanFxTextures();
    for (size_t i = 0; i < gFxTextures.size(); ++i)
        if (gFxTextures[i].name == name) return &gFxTextures[i];
    return nullptr;
}

// ⚠ Uploads PREMULTIPLIED alpha. Every blend mode below carries the layer's
// contribution in the colour channels and its coverage in alpha, so a texel with
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
// colour, a blend mode and an opacity; the compositor walks the stacks every
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
// ⚠ Not possible here, and not worth faking: Overlay, Soft/Hard Light, Colour
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
    "recolours the lit pixels and leaves the background alone.",
    "dst + src. Brightens; cannot suppress a channel. The partner to multiply.",
    "dst + src x (1-dst). Brightens, but rolls off instead of clipping.",
    "Straight alpha blend. Flat and opaque-looking — it ignores what is under it.",
    "dst - src. Removes a colour rather than scaling it: subtract a little blue "
    "to warm a readout without touching how bright it is.",
    "dst + src - 1 (linear burn). Deepens harder than Multiply and reaches black "
    "sooner. Use it when Multiply leaves the background washed out.",
    "min(src, dst) per channel. A CEILING — it caps each channel at the colour "
    "and leaves anything already darker alone. Clamps a blown-out white.",
    "max(src, dst) per channel. A FLOOR — sets a minimum glow without touching "
    "anything already brighter. The counterpart to Darken.",
};

// ⚠ Two modes CANNOT carry an image, and it is a property of fixed-function GL
// rather than an unfinished corner:
//
//   Burn    emits the COMPLEMENT of the layer colour (that is how dst + src - 1
//           is folded into one reverse subtract), and 1 - texel is not something
//           GL_MODULATE can produce.
//   Darken  is GL_MIN, whose equation ignores the blend factors entirely, so its
//           opacity has to be carried by fading the emitted colour toward white
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

struct FxLayer {
    bool  enabled  = true;
    int   blend    = kFxMultiply;
    float color[3] = { 1.00f, 0.82f, 0.90f };
    float opacity  = 1.00f;
    // Overlay image, by file name in the overlays folder. Empty = a solid fill,
    // which is the same code path with a 1x1 white texture bound.
    std::string tex;
    float tile     = 1.00f;            // UV repeats across the target
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

// The raw dataref value, honouring the array index. Returns false when there is
// nothing to read — the caller then behaves as if no driver were set.
static bool FxDriverRaw(FxDriver& d, double& out) {
    if (!FxResolveDriver(d)) return false;
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
// Three stages, in this order and for a reason each: lo/hi normalises whatever
// the source counts in onto 0..1; BrightnessResponse reshapes that into the fade
// a screen really has (kBrightnessCurve — the same curve the screen-glow lights
// use, so a tint and the spill under it come up together); the floor then lifts
// the result, so "never fully off" still means never fully off after the curve
// has flattened the bottom of the knob.
//
// ⚠ `useCurve` is a PARAMETER rather than a read of gFxCurve because the switch
// is per target now: the same driver feeds two stacks that want different
// shapes, so the curve cannot be a property of the driver or of the global.
static float FxDriverFactor(FxDriver& d, bool useCurve) {
    double v = 0.0;
    if (!FxDriverRaw(d, v)) return 1.0f;
    const float span = d.hi - d.lo;
    if (span == 0.0f) return v >= d.hi ? 1.0f : 0.0f;
    float f = (float)((v - d.lo) / span);
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    if (useCurve) f = BrightnessResponse(f);
    return d.floorF + f * (1.0f - d.floorF);
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
static std::string FxDriverState(FxDriver& d, bool isPower, bool useCurve) {
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
                      FxDriverFactor(d, useCurve));
    return s + buf;
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
// Empty means "nothing found for this face": the clock, the centre readouts, the
// transponder code and the rudder trim have no brightness dataref at all — on
// the real aircraft they are lit by the panel's integral lighting, so the
// panel-flood rheostat is the closest thing and is what they get.
struct PanelSource {
    const char* rect;
    const char* bright;  int brightIdx;  float brightLo, brightHi;
    const char* power;   int powerIdx;   float powerAbove;
};
static const char* kFxPanelIntegral = "AirbusFBW/PanelBrightnessLevel";
static const char* kFxOhpIntegral   = "AirbusFBW/OHPBrightnessLevel";
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
    // read, so they follow whether the aircraft has CPDLC at all.
    { "DCDU capt",     "", -1, 0.0f, 1.0f,  "AirbusFBW/HasCPDLC", -1, 0.5f },
    { "DCDU FO",       "", -1, 0.0f, 1.0f,  "AirbusFBW/HasCPDLC", -1, 0.5f },
    // The FCU row is integral-lit as one unit, baro windows included — one knob
    // for all three faces.
    //
    // ⚠ NOT in 0..1. This is the RAW KNOB ANIMATION value and it runs 1..270,
    // which is why lo/hi exist at all: every other row here happens to be 0..1
    // and it would be easy to assume the mechanism only ever normalises a
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
    // The radios: each RMP has its own lighting level and its own availability.
    { "RMP1 active",   "AirbusFBW/RMP1Lights", -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP1Available", -1, 0.5f },
    { "RMP1 stby",     "AirbusFBW/RMP1Lights", -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP1Available", -1, 0.5f },
    { "RMP2 active",   "AirbusFBW/RMP2Lights", -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP2Available", -1, 0.5f },
    { "RMP2 stby",     "AirbusFBW/RMP2Lights", -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP2Available", -1, 0.5f },
    { "RMP3 active",   "AirbusFBW/RMP3Lights", -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP3Available", -1, 0.5f },
    { "RMP3 stby",     "AirbusFBW/RMP3Lights", -1, 0.0f, 1.0f,
                       "AirbusFBW/RMP3Available", -1, 0.5f },
    // The transponder window has no brightness knob but does have a power
    // switch, which is exactly the "broader on/off" case.
    { "XPDR code",     "", -1, 0.0f, 1.0f,  "AirbusFBW/XPDRPower", -1, 0.5f },
    // Everything integral-lit by the main panel rheostat.
    { "clock CHR",     kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "clock UTC",     kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "clock ET",      kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ctr readout L", kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ctr readout R", kFxPanelIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ped strips",    "AirbusFBW/PedestalFloodBrightnessLevel", -1, 0.0f, 1.0f, "", -1, 0.0f },
    { "rudder trim",   "AirbusFBW/PedestalFloodBrightnessLevel", -1, 0.0f, 1.0f, "", -1, 0.0f },
    { "ovhd fwd L",    kFxOhpIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ovhd fwd R",    kFxOhpIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ovhd aft L1",   kFxOhpIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ovhd aft L2",   kFxOhpIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    { "ovhd aft R",    kFxOhpIntegral, -1, 0.0f, 1.0f,  "", -1, 0.0f },
    // The EFB tablets are on their own screen brightness, up/down buttons only.
};
static const int kPanelSourceCount = (int)(sizeof(kPanelSources) / sizeof(kPanelSources[0]));

// The per-rect drivers, built once from the table above. Mutable because a
// driver caches its resolved handle; the CONFIG is the constant part.
static FxDriver gRectBright[64];
static FxDriver gRectPower[64];
static bool     gRectDriversBuilt = false;

static void BuildRectDrivers() {
    for (int i = 0; i < kPanelRectCount && i < 64; ++i) {
        gRectBright[i] = FxDriver();
        gRectPower[i]  = FxDriver();
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
// while picking a colour and flip back, and if setting one target's switch
// silently froze every other target's, the hatch would stop working.
//
// The states are deliberately NOT symmetric with the master:
//
//   master OFF     nothing follows and nothing is power-gated. It is the "show
//                  me the raw colours" switch and it outranks every target,
//                  including one set to On — otherwise a target you set months
//                  ago is the reason the hatch appears broken today.
//   follow = Off   this target is NOT dimmed by its knob, but it IS still gated
//                  by its power driver. "Always on whenever the thing is
//                  powered", which is a real look on a face whose integral
//                  lighting does not actually track the readout brightness.
//   curve  = Off   linear from lo..hi instead of kBrightnessCurve. Only affects
//                  this target's tint; the screen-glow spill lights are never
//                  switchable (docs/screens_plan.md §1).
enum { kFxInherit = -1, kFxOff = 0, kFxOn = 1 };

static bool FxTriState(int tri, bool inherited) {
    return tri < 0 ? inherited : (tri != 0);
}

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
// a colour without having to hold a knob up.
static bool gFxFollowBrightness = true;

static void FxForgetDrivers() {
    for (int i = 0; i < kPanelRectCount && i < 64; ++i) {
        gRectBright[i].tried = false; gRectBright[i].ref = nullptr;
        gRectPower[i].tried  = false; gRectPower[i].ref  = nullptr;
    }
    for (size_t s = 0; s < gFxStacks.size(); ++s) {
        gFxStacks[s].bright.tried = false; gFxStacks[s].bright.ref = nullptr;
        gFxStacks[s].power.tried  = false; gFxStacks[s].power.ref  = nullptr;
    }
}

// The factor for ONE rect of the stack being drawn: the stack's own driver if it
// has one, else the rect's built-in one. Set up per layer by DrawFxLayer.
static FxStack* gFxDrawStack = nullptr;

// The effective switches for the stack being drawn. Split out so the UI, the log
// dump and the compositor cannot disagree about what is in force — a pane that
// says "curve on" over a target drawing linearly is worse than no pane.
static bool FxStackFollows(const FxStack* s) {
    return gFxFollowBrightness && FxTriState(s ? s->follow : kFxInherit, true);
}
static bool FxStackCurves(const FxStack* s) {
    return FxTriState(s ? s->curve : kFxInherit, gFxCurve);
}

static float FxRectFactor(int rectIndex) {
    if (!gFxFollowBrightness) return 1.0f;
    if (!gRectDriversBuilt) BuildRectDrivers();

    FxDriver* bright = nullptr;
    FxDriver* power  = nullptr;
    if (gFxDrawStack && gFxDrawStack->bright.set()) bright = &gFxDrawStack->bright;
    else if (rectIndex >= 0 && rectIndex < kPanelRectCount) bright = &gRectBright[rectIndex];
    if (gFxDrawStack && gFxDrawStack->power.set())  power  = &gFxDrawStack->power;
    else if (rectIndex >= 0 && rectIndex < kPanelRectCount) power  = &gRectPower[rectIndex];

    // ⚠ The power gate comes FIRST and is not what `follow` switches off. A
    // target with follow = Off means "full strength whenever this is powered",
    // so skipping the gate with it would leave the tint painted over a dead
    // screen — the exact artefact the whole driver mechanism exists to remove.
    if (power && power->set() && !FxDriverPowered(*power)) return 0.0f;
    if (!FxStackFollows(gFxDrawStack)) return 1.0f;
    return bright && bright->set()
        ? FxDriverFactor(*bright, FxStackCurves(gFxDrawStack))
        : 1.0f;
}

// The lit pass. Not a knob: pass 1 puts down nothing visible on these faces and
// pass 2 replaces the digits, measured in-sim 2026-07-31.
static const int kFxPass = 2;

// Set the func and equation for one layer, holding the ALPHA channel at "keep
// destination". Some of these modes would otherwise write alpha — additive would
// saturate it, Darken would floor it — and nothing here has any business changing
// the panel's alpha channel. Without the Separate entry points the alpha rides
// the RGB half, which is the pre-existing behaviour and not worse than refusing
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
// Six of the eight carry it the same way: the quad emits colour x opacity, and
// the identity is black, so opacity scales plainly. That includes MULTIPLY,
// which pays for it in the blend func instead of in the colour —
//
//     src=DST_COLOR, dst=ONE_MINUS_SRC_ALPHA, quad = (C x o, o)
//     => dst' = dst x C x o + dst x (1 - o) = dst x (1 - o + o x C)
//
// which is exactly the old "fade the colour toward white" arithmetic, expressed
// per PIXEL. That is what lets multiply take an image: with a premultiplied
// texture the same expression becomes dst x (1 - a x o + a x o x texel x C), so a
// transparent texel is a no-op and a solid one behaves as before. The two modes
// that cannot be written this way are Burn and Darken — see FxBlendTakesTexture.
//
// ⚠ Split into "the colour for one opacity" and "draw the layer" because the
// opacity is no longer one number per layer: a brightness driver scales it PER
// RECT, so a group whose members are on different knobs needs this arithmetic
// re-run per quad. The blend func and equation do NOT vary with opacity — every
// case below picks them from layer.blend alone — which is what keeps the state
// change outside the loop where it belongs.
struct FxQuadColor { float r, g, b, a; GLenum src, dst, eq; };

static FxQuadColor FxQuadColorFor(const FxLayer& layer, float o) {
    float r = layer.color[0], g = layer.color[1], b = layer.color[2];
    GLenum src = GL_ONE, dst = GL_ZERO, eq = GL_FUNC_ADD;
    float a = 1.0f;
    switch (layer.blend) {
        case kFxMultiply:
            // Opacity rides the ALPHA channel, not the colour — see above.
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
            // COMPLEMENT of the layer colour, which is why opacity scales that
            // complement — at 0 it emits black and subtracts nothing. It is also
            // why Burn cannot take an image: 1 - texel is not a modulate.
            r = (1.0f - r) * o; g = (1.0f - g) * o; b = (1.0f - b) * o;
            a = 0.0f;
            src = GL_ONE; dst = GL_ONE; eq = GL_FUNC_REVERSE_SUBTRACT;
            break;
        case kFxDarken:
            // min/max ignore the blend factors entirely — the equation is the
            // whole operation — so src/dst below are set only to leave the func
            // in a defined state, and opacity has nowhere to go but the colour.
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
            // Premultiplied: the texture already carries colour x alpha, so the
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
// DOES drive it, the layer's colour is recomputed at the scaled opacity rather
// than the quad being faded toward black: at opacity 0 each mode has to reach
// its OWN identity (white for Multiply and Darken, black for the rest), which is
// the same reason the opacity slider works the way it does.
static const FxLayer* gFxDrawLayer = nullptr;

static void DrawFxRect(const PanelRect& r, int rectIndex, float cr, float cg, float cb) {
    const float f = gFxDrawLayer ? FxRectFactor(rectIndex) : 1.0f;
    if (f >= 0.999f) { EmitRectQuad(r, cr, cg, cb, gFxAlpha, gFxTile); return; }
    if (f <= 0.0f) return;                 // dark screen: draw nothing at all
    const FxQuadColor q = FxQuadColorFor(*gFxDrawLayer, gFxDrawLayer->opacity * f);
    EmitRectQuad(r, q.r, q.g, q.b, q.a, gFxTile);
}

static void DrawFxLayer(const FxLayer& layer, int target) {
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

    const FxQuadColor q = FxQuadColorFor(layer, layer.opacity);
    if (!SetFxBlend(q.src, q.dst, q.eq)) return;
    gFxAlpha = q.a;
    gFxDrawLayer = &layer;                 // read back by DrawFxRect per quad
    PaintPanelTarget(target, DrawFxRect, q.r, q.g, q.b);
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
static std::vector<int> FxDrawOrder() {
    std::vector<int> order(gFxStacks.size());
    for (size_t s = 0; s < gFxStacks.size(); ++s) order[s] = (int)s;
    std::stable_sort(order.begin(), order.end(), [](int a, int b) {
        return PanelTargetBreadth(gFxStacks[a].target)
             > PanelTargetBreadth(gFxStacks[b].target);
    });
    return order;
}

static void DrawPanelFx() {
    if (!gFxEnabled || gFxStacks.empty()) return;

    // Edited an overlay in a paint program? It is picked up here, within a
    // second, with the Dev window shut. GL is current in this callback, so the
    // re-upload the poll asks for happens in the same pass, below.
    PollFxTextureChanges();

    // ONE texture unit for the whole pass, whether or not any layer has an
    // image: a solid layer binds the 1x1 white texture instead, which modulates
    // to the same colour. Toggling the unit per layer would mean calling
    // XPLMSetGraphicsState inside the loop, i.e. re-entering X-Plane's blend
    // tracker while our own func is set — the ordering trap PushBlendFunc exists
    // to avoid.
    EnsureFxWhiteTexture();
    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
    PushBlendFunc();                       // AFTER XPLMSetGraphicsState — see above

    // The texture env is not part of XPLMSetGraphicsState and is shared state
    // like the blend func, so it gets the same save/restore. MODULATE is the GL
    // default and is what makes "the image tints with the layer colour" true.
    GLint savedEnv = GL_MODULATE;
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &savedEnv);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    const std::vector<int> order = FxDrawOrder();
    for (size_t n = 0; n < order.size(); ++n) {
        FxStack& stack = gFxStacks[order[n]];
        // Which stack is being drawn, for the per-rect brightness lookup. Not a
        // parameter because PanelPaintFn is a plain function pointer and the
        // probe shares it — the same reason gFxAlpha and gFxTile are globals.
        gFxDrawStack = &stack;
        for (size_t i = 0; i < stack.layers.size(); ++i) {
            const FxLayer& layer = stack.layers[i];
            if (layer.enabled && layer.opacity > 0.0f) DrawFxLayer(layer, stack.target);
        }
        gFxDrawStack = nullptr;
    }

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, savedEnv);
    PopBlendFunc();
    gFxAlpha = 1.0f;
    gFxTile  = 1.0f;
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

static int PanelProbeDraw(XPLMDrawingPhase, int, void* refcon) {
    const int slot = (int)(intptr_t)refcon;

    // Record which passes actually run. 0 = 2-d panel, 1 = 3-d non-lit,
    // 2 = 3-d lit — the last is the one the texture browser names "lit panel fbo".
    const int type = gPanelRenderTypeRef ? XPLMGetDatai(gPanelRenderTypeRef) : -1;
    if (type >= 0 && type < 8 && !(gPanelTypesSeen & (1 << type))) {
        gPanelTypesSeen |= (1 << type);
        Log("panel probe: saw panel_render_type " + std::to_string(type));
    }
    if (type == 0) return 1;              // the 2-d panel / popups are not our target

    // The FX compositor runs from the after-Gauges slot in the lit pass, and is
    // deliberately NOT gated on the probe's mode/rect/draw knobs. Turning the
    // probe off to look at the effect must not turn the effect off with it —
    // that is the whole difference between a diagnostic and a feature.
    if (slot == kProbeAfterGauges && type == kFxPass) DrawPanelFx();

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
    // The slot colours identify which callback painted; a multiply wants the
    // tint being tuned, and would read as "black" under a magenta probe colour.
    if (multiply) { cr = gPanelTint[0]; cg = gPanelTint[1]; cb = gPanelTint[2]; }

    if (method == kDrawForced) PushForcedState();
    if (multiply)              PushMultiplyBlend();
    while (glGetError() != GL_NO_ERROR) {}   // drain, so what we log next is ours

    PaintPanelTarget(gPanelProbeRect,
                     (method == kDrawClear) ? ClearProbeRect : DrawProbeRect,
                     cr, cg, cb);

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

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);    // leave the shared GL colour as we found it
    return 1;
}

// Registration order within a phase is the lever we have on draw order: a plugin
// registering later is called later. So this is deliberately re-run on every
// aircraft load, AFTER the aircraft's own plugin has had its XPluginStart — which
// is the whole trick if the first result comes back "ToLiss draws over us".
static void RegisterPanelProbe() {
    const int a = XPLMRegisterDrawCallback(PanelProbeDraw, xplm_Phase_Gauges, 0,
                                           (void*)(intptr_t)kProbeAfterGauges);
    const int b = XPLMRegisterDrawCallback(PanelProbeDraw, xplm_Phase_Panel, 0,
                                           (void*)(intptr_t)kProbeAfterPanel);
    const int c = XPLMRegisterDrawCallback(PanelProbeDraw, xplm_Phase_Gauges, 1,
                                           (void*)(intptr_t)kProbeBeforeGauges);
    gPanelProbeRegistered = true;
    Log("panel probe: registered draw callbacks (after-gauges=" + std::to_string(a)
        + " after-panel=" + std::to_string(b) + " before-gauges=" + std::to_string(c)
        + "; 0 would mean the phase is unsupported)");
}

static void UnregisterPanelProbe() {
    if (!gPanelProbeRegistered) return;
    XPLMUnregisterDrawCallback(PanelProbeDraw, xplm_Phase_Gauges, 0, (void*)(intptr_t)kProbeAfterGauges);
    XPLMUnregisterDrawCallback(PanelProbeDraw, xplm_Phase_Panel,  0, (void*)(intptr_t)kProbeAfterPanel);
    XPLMUnregisterDrawCallback(PanelProbeDraw, xplm_Phase_Gauges, 1, (void*)(intptr_t)kProbeBeforeGauges);
    gPanelProbeRegistered = false;
}

static int PanelProbeCycleHandler(XPLMCommandRef, XPLMCommandPhase phase, void*) {
    if (phase == xplm_CommandBegin)
        WritePanelProbeMode(nullptr, (gPanelProbeMode + 1) % kProbeModeCount);
    return 0;
}

// Defined with the dev window below, which needs the accessors above.
static void BuildDbgRectOpts();
static void LoadPanelDebug();
static void LoadPanelFx();
static void LoadDevPrefs();

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
// colour, aim, spread, size, brightness and POSITION are live knobs with no
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
    float       pos[3]  = { 0.0f, 0.0f, 0.0f };     // baked, for display only
    float       v[kDbgParamCount]    = { 0.0f };
    float       base[kDbgParamCount] = { 0.0f };
    float       off[3]  = { 0.0f, 0.0f, 0.0f };     // live position offset, metres
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
// sim restarts of hypothesising found nothing.
//
// Baseline choices, each load-bearing:
//   dir 0 0 0   omnidirectional, so a probe of 5/6/7 introduces the ONLY
//               direction present and there is nothing to disentangle it from.
//   cone 0      cos(90 deg). If slot 8 is a direction component after all, a
//               baseline of 0 contributes nothing to it, keeping 5..7 clean.
//   size 4 m    big enough that the pool lands on real geometry rather than on
//               the panel two centimetres away — which is what made cone edits
//               look inert.
//   white, a=1  no rheostat, no palette, nothing to misread as a colour bug.
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

    const bool selected = (n == gDbgSel);
    if (gDbgMark && selected) { out[0] = 1.0f; out[1] = 0.0f; out[2] = 1.0f; }

    float alpha = out[3] * DbgRheostat(l);
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
// to a pool of light a metre wide.
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
// OBJ drives its own dots instead of a neighbour's. Clamped to the constant: the
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

// One colour per part, because the whole point is to see WHICH END is the light
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
// One per debug-capable target, and BOTH number their slots from 0 — so driving
// two at once would have every screen light fighting a cockpit light for the same
// dataref. The most recently WRITTEN manifest wins (that is the build just run)
// and its name goes in the window, so "why is the list full of the wrong lights"
// is never a puzzle.
static void DbgLoadManifest() {
    gDbgLights.clear();
    gDbgManifest.clear();
    gDbgTarget.clear();
    gDbgPosTunable = false;
    gDbgMarkerDots = 0;
    gDbgMarkStride = kDbgMarkDots;
    gDbgSel = -1;
    gDbgScanned = true;
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
    fs::path best;
    fs::file_time_type bestTime{};
    int found = 0;
    for (int i = 0; i < 2; ++i) {
        const fs::path p = objs / kCandidates[i].file;
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        ++found;
        const fs::file_time_type t = fs::last_write_time(p, ec);
        if (ec) continue;
        if (best.empty() || t > bestTime) { best = p; bestTime = t; gDbgTarget = kCandidates[i].label; }
    }
    if (best.empty()) {
        gDbgNote = "no debug OBJ installed";
        return;
    }
    if (found > 1)
        Log("light editor: two debug OBJs are installed and they share these dataref "
            "slots - driving the newest (" + gDbgTarget + "). Rebuild the other "
            "WITHOUT --debug.");

    std::ifstream f(best, std::ios::binary);
    if (!f) { gDbgNote = "manifest unreadable"; return; }
    std::stringstream ss; ss << f.rdbuf();
    json::Value root;
    json::Parser parser(ss.str());
    if (!parser.value(root) || root.type != json::Value::Obj) {
        gDbgNote = "manifest parse failed";
        Log("light editor: could not parse " + best.string());
        return;
    }
    gDbgManifest = best.filename().string();
    if (const json::Value* v = root.find("pos_tunable")) gDbgPosTunable = v->i != 0;
    if (const json::Value* v = root.find("marker_dots"))  gDbgMarkerDots = (int)v->i;
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

    const json::Value* lights = root.find("lights");
    if (!lights || lights->type != json::Value::Arr) { gDbgNote = "manifest has no lights"; return; }
    for (size_t i = 0; i < lights->arr.size(); ++i) {
        const json::Value& e = lights->arr[i];
        if (e.type != json::Value::Obj) continue;
        DbgLight l;
        l.n = (int)i;
        if (const json::Value* v = e.find("n"))        l.n = (int)v->i;
        if (const json::Value* v = e.find("name"))     l.name = v->s;
        if (const json::Value* v = e.find("fixture"))  l.fixture = v->s;
        if (const json::Value* v = e.find("category")) l.category = v->s;
        if (const json::Value* v = e.find("class"))    l.cls = v->s;
        if (const json::Value* v = e.find("source"))   l.source = v->s;
        if (const json::Value* v = e.find("index"))    l.index = (int)v->i;
        if (const json::Value* v = e.find("pos"))
            for (int k = 0; k < 3; ++k)
                if (const json::Value* c = v->at((size_t)k)) l.pos[k] = (float)c->num();
        if (const json::Value* v = e.find("rgb"))
            for (int k = 0; k < 3; ++k)
                if (const json::Value* c = v->at((size_t)k)) l.v[k] = (float)c->num();
        l.v[3] = 1.0f;                                    // alpha: the rheostat scales it
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

// PhotonImgui has no access to Log(); hand it one so backend diagnostics and
// any ImGui assertion land in Log.txt with everything else.
static void ImguiLogBridge(const char* msg) { Log(std::string(msg)); }

static void StartPanelProbe() {
    gPanelRenderTypeRef = XPLMFindDataRef(kPanelRenderType);
    gDisplayFallbackRef = XPLMFindDataRef("AirbusFBW/DisplayResolutionFallback");
    if (!gDisplayFallbackRef)
        Log("panel probe: AirbusFBW/DisplayResolutionFallback not found — assuming "
            "the full-resolution rect set (correct unless ToLiss has fallen back)");
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

    RegisterPanelProbe();
    // ⚠ Must happen at XPluginStart, i.e. here: a debug OBJ binds these names when
    // the aircraft loads, and a name that does not exist by then reads 0 for the
    // rest of the session.
    RegisterDbgLightRefs();
    // After registration so a restored "mode" takes effect immediately, and after
    // the accessors exist so anything watching the datarefs sees the same values.
    BuildDbgRectOpts();
    LoadPanelDebug();
    PhotonImgui::SetLogger(ImguiLogBridge);
    // Probe builds only, and only until the ImGui backend is signed off in-sim:
    // verbose first-frame logging plus the two GL sanity markers.
    PhotonImgui::SetDiagnostics(true);
    LoadPanelFx();
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
}

static void StopPanelProbe() {
    UnregisterPanelProbe();
    DestroyDebugImguiWindows();
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
}

static void ReseatPanelProbe() {   // push ourselves to the tail of the callback list
    if (!gPanelProbeRegistered) return;
    UnregisterPanelProbe();
    RegisterPanelProbe();
    Log("panel probe: re-registered after aircraft load (moves us later in the phase)");
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
typedef void (*DbgWriter)(void*, int);
static bool DbgOptionRow(const char* label, const DbgOpt* opts, int n,
                         int cur, DbgWriter write) {
    bool changed = false;
    ImGui::TextUnformatted(label);
    for (int i = 0; i < n; ++i) {
        ImGui::SameLine();
        if (ImGui::RadioButton(opts[i].label, cur == opts[i].value)) {
            write(nullptr, opts[i].value);
            changed = true;
        }
    }
    return changed;
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
            if (ImGui::Selectable(gDbgRectOpts[i].label, i == rectIdx)) {
                WritePanelProbeRect(nullptr, gDbgRectOpts[i].value);
                dirty = true;
            }
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
    UiHint("A colour is judged by walking it, not by jumping to it - drag a "
           "channel slowly rather than typing a number.");

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("FCU tint preset")) { ApplyFcuPreset(); dirty = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("After Gauges, LIT pass, Multiply, FCU row - the state "
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

    if (dirty) SavePanelDebug();
}

// ================== panel FX: persistence, history, editor ===================

// Its own file, like the probe's. This is tuning state for a feature that does
// not ship yet, so it has no business in a user's per-livery profile.
static fs::path PanelFxFilePath() {
    char root[512] = {0};
    XPLMGetSystemPath(root);
    return fs::path(root) / "Output" / "preferences" / "ToLissPhoton_panelfx.txt";
}
// Append-only journal beside it. See the history section below.
static fs::path PanelFxJournalPath() {
    char root[512] = {0};
    XPLMGetSystemPath(root);
    return fs::path(root) / "Output" / "preferences" / "ToLissPhoton_panelfx_history.txt";
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
        } else if (key == "tfollow" || key == "tcurve") {
            if (gFxStacks.empty()) continue;          // a switch before any target
            int v = kFxInherit; in >> v;
            const int tri = v < 0 ? kFxInherit : (v ? kFxOn : kFxOff);
            if (key == "tfollow") gFxStacks.back().follow = tri;
            else                  gFxStacks.back().curve  = tri;
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
            // would recognise from the symptom.
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

static void SavePanelFx() {
    std::ofstream f(PanelFxFilePath(), std::ios::binary);
    if (!f) { Log("panel fx: could not write " + PanelFxFilePath().string()); return; }
    f << "# ToLiss Photon panel FX layers - generated, safe to delete.\n";
    f << "# target <name>\n";
    f << "# bright <dataref> <index> <lo> <hi> <floor>   (optional, per target)\n";
    f << "# power  <dataref> <index> <above>             (optional, per target)\n";
    f << "# tfollow 0|1  /  tcurve 0|1   (optional, per target; absent = inherit)\n";
    f << "# layer <blend> <enabled> <r> <g> <b> <opacity> <tile> <image|->\n";
    f << "enabled " << (gFxEnabled ? 1 : 0) << "\n";
    f << "follow " << (gFxFollowBrightness ? 1 : 0) << "\n";
    f << "curve "  << (gFxCurve ? 1 : 0) << "\n";
    f << SerializeFxStacks();
}

static void LoadPanelFx() {
    std::ifstream f(PanelFxFilePath(), std::ios::binary);
    if (!f) return;                       // no file yet is the normal first run
    std::stringstream ss;
    ss << f.rdbuf();
    int dropped = 0;
    DeserializeFxStacks(ss.str(), &dropped);
    Log("panel fx: restored " + std::to_string(gFxStacks.size()) + " stack(s)"
        + (dropped ? " (" + std::to_string(dropped) + " dropped)" : "")
        + " from " + PanelFxFilePath().string()
        + (gFxEnabled ? " - ENABLED, the cockpit may already be tinted" : " - disabled"));
}

// ---- history: undo, redo, and an append-only journal ------------------------
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
// discard the old snapshot that is precisely what you want when you realise
// three days later that the look was better before.
struct FxHistoryEntry {
    std::string when;      // wall clock, for recognising a session
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
        Log(std::string("panel fx: target ") + PanelTargetName(gFxStacks[s].target)
            + "  [follow " + (follows ? "on" : "OFF - full strength when powered")
            + (gFxStacks[s].follow == kFxInherit ? " (inherited)" : " (target)")
            + ", curve " + (curves ? "on" : "off - linear")
            + (gFxStacks[s].curve == kFxInherit ? " (inherited)" : " (target)") + "]");
        // The live factor per member rect, because a stack that looks wrong in
        // the cockpit is at least as likely to be a mis-guessed dataref as a
        // mis-set colour, and the two are indistinguishable from a screenshot.
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
            char buf[512];
            std::snprintf(buf, sizeof(buf), "panel fx:   %-14s x%.2f  bright=%s  power=%s",
                          kPanelRectsHi[i].name, f,
                          FxDriverState(br, false, curves).c_str(),
                          FxDriverState(pw, true, curves).c_str());
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
        }
    }
}

// ---- a layer's image row ----------------------------------------------------
// Its own line under the layer's colour controls. Folding the picker into that
// row made it wider than the window at every sane size, and an image is a
// different kind of choice from a colour anyway.
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
    if (layer.tex.empty())   std::snprintf(preview, sizeof(preview), "(none - solid colour)");
    else if (missing)        std::snprintf(preview, sizeof(preview), "%s  [MISSING]",
                                           layer.tex.c_str());
    else                     std::snprintf(preview, sizeof(preview), "%s", layer.tex.c_str());

    if (ImGui::BeginCombo("##img", preview)) {
        if (ImGui::Selectable("(none - solid colour)", layer.tex.empty())) {
            layer.tex.clear();
            *dirty = true; *what = "clear layer image";
        }
        for (size_t i = 0; i < gFxTextures.size(); ++i) {
            const bool sel = (gFxTextures[i].name == layer.tex);
            if (ImGui::Selectable(gFxTextures[i].name.c_str(), sel)) {
                layer.tex = gFxTextures[i].name;
                *dirty = true; *what = "set layer image";
            }
            if (sel) ImGui::SetItemDefaultFocus();
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
            ImGui::SetTooltip("%s - %dx%d\n(the thumbnail is premultiplied, so a "
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
            ImGui::SetTooltip("How many times the image repeats across the target. "
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
            ImGui::SetTooltip("Burn emits the complement of the colour and Darken is "
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
        if (hasStack) ImGui::SetTooltip("%d rect(s) - right-click to copy or paste "
                                        "the whole stack", PanelTargetBreadth(t));
        else          ImGui::SetTooltip("No layers yet - select it and use \"Add "
                                        "layer\", or right-click to paste a stack "
                                        "(%d rect(s))", PanelTargetBreadth(t));
    }

    // Right-aligned row buttons. Deliberately NOT GetContentRegionMax(), which
    // lives in ImGui's obsolete block — the whole point of the CmdListsCount
    // episode is that this project should be able to define
    // IMGUI_DISABLE_OBSOLETE_FUNCTIONS one day.
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                         + ImGui::GetContentRegionAvail().x - (hasStack ? 38.0f : 20.0f));
    const bool identifying = FxIdentifyActive(t);
    if (identifying)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 1.0f, 1.0f));
    if (ImGui::SmallButton("?")) FxIdentifyTarget(t);
    if (identifying) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(identifying
                          ? "This target is the magenta one right now - click to "
                            "turn the probe off."
                          : "Paint this target magenta in the cockpit so you can "
                            "see which face it is. Click again to stop.");
    if (hasStack) {
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) *pendingDelete = t;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Delete this target and all its layers");
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
static void FxDriverRow(const char* what, FxDriver& d, bool isPower, bool useCurve) {
    if (!d.set()) { ImGui::TextDisabled("%s: none", what); return; }
    double v = 0.0;
    const bool ok = FxDriverRaw(d, v);
    char idx[24] = "";
    if (d.index >= 0) std::snprintf(idx, sizeof(idx), "[%d]", d.index);
    if (!ok) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s: %s%s - unreadable",
                           what, d.dref.c_str(), idx);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The dataref does not exist, is not numeric, or the "
                              "index is past the end of the array. Treated as "
                              "\"no driver\" - the tint is NOT being dimmed.");
        return;
    }
    if (isPower)
        ImGui::Text("%s: %s%s = %.3f -> %s", what, d.dref.c_str(), idx, v,
                    v > (double)d.floorF ? "ON" : "OFF (target skipped)");
    else
        ImGui::Text("%s: %s%s = %.3f -> x%.2f", what, d.dref.c_str(), idx, v,
                    FxDriverFactor(d, useCurve));
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
        ImGui::SetTooltip("Empty = follow whatever drives each rect of this "
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
        ImGui::SetTooltip("Array element. -1 for a scalar dataref.");

    ImGui::SameLine();
    if (isPower) {
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputFloat("above", &d.floorF, 0.0f, 0.0f, "%.2f")) changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The target draws only while the value is ABOVE "
                              "this. 0.5 for an on/off flag.");
    } else {
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputFloat("off at", &d.lo, 0.0f, 0.0f, "%.2f")) changed = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputFloat("full at", &d.hi, 0.0f, 0.0f, "%.2f")) changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The two dataref values that map to opacity x0 and "
                              "x1. \"off at\" ABOVE \"full at\" inverts the "
                              "source - that is how a fault flag or a dim knob "
                              "is expressed.");
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::SliderFloat("floor", &d.floorF, 0.0f, 1.0f, "x%.2f")) changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Never dim below this. A readout that keeps a "
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
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
            ImGui::SetTooltip("Knots of kBrightnessCurve in plugin.cpp, monotone "
                              "cubic between them.\nThe screen-glow spill lights "
                              "run through the same curve, unconditionally - the "
                              "response switch only affects this target's tint.");
    }

    // What is actually in force, which is the question. A group with no override
    // is following one driver PER MEMBER, so the members are listed with their
    // own live factors rather than pretending there is a single answer.
    const PanelMask mask = PanelTargetMask(stack.target);
    const bool overridden = stack.bright.set() || stack.power.set();
    if (overridden) {
        FxDriverRow("brightness", stack.bright, false, curves);
        FxDriverRow("power", stack.power, true, curves);
        ImGui::TextDisabled("(this target's own sources - the per-rect table below "
                            "is not used for the ones set here)");
    }
    if (ImGui::BeginTable("fxsrc", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("rect");
        ImGui::TableSetupColumn("brightness");
        ImGui::TableSetupColumn("power");
        ImGui::TableSetupColumn("x");
        ImGui::TableHeadersRow();
        const ImVec4 kBad  (1.00f, 0.45f, 0.35f, 1.0f);   // unreadable
        const ImVec4 kGated(1.00f, 0.75f, 0.30f, 1.0f);   // read fine, gating off
        for (int i = 0; i < kPanelRectCount; ++i) {
            if (!(mask & ((PanelMask)1 << i))) continue;
            FxDriver& br = stack.bright.set() ? stack.bright : gRectBright[i];
            FxDriver& pw = stack.power.set()  ? stack.power  : gRectPower[i];

            // Read each ONCE per row and reuse: the cells, the factor and the
            // colouring must all describe the same frame, and a knob being turned
            // while the table is open would otherwise show a value that does not
            // explain the factor beside it.
            double bv = 0.0, pv = 0.0;
            const bool bok = br.set() && FxDriverRaw(br, bv);
            const bool pok = pw.set() && FxDriverRaw(pw, pv);
            const bool gated = pok && !(pv > (double)pw.floorF);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kPanelRectsHi[i].name);

            ImGui::TableSetColumnIndex(1);
            if (!br.set())      ImGui::TextDisabled("-");
            else if (!follows)  ImGui::TextDisabled("%s  (not dimmed)", br.dref.c_str());
            else if (!bok)      ImGui::TextColored(kBad, "%s", FxDriverState(br, false, curves).c_str());
            else                ImGui::TextUnformatted(FxDriverState(br, false, curves).c_str());

            ImGui::TableSetColumnIndex(2);
            if (!pw.set())      ImGui::TextDisabled("-");
            else if (!pok)      ImGui::TextColored(kBad, "%s", FxDriverState(pw, true, curves).c_str());
            else if (gated)     ImGui::TextColored(kGated, "%s", FxDriverState(pw, true, curves).c_str());
            else                ImGui::TextUnformatted(FxDriverState(pw, true, curves).c_str());

            ImGui::TableSetColumnIndex(3);
            gFxDrawStack = &stack;
            const float f = FxRectFactor(i);
            gFxDrawStack = nullptr;
            // ⚠ Name WHICH driver produced a zero. Both roads end at x0.00 and
            // they need opposite fixes - a power gate that is doing its job
            // versus a brightness dataref whose units are not what the lo/hi
            // assume - and for a long time this column showed only the number.
            if (f >= 0.999f)    ImGui::TextDisabled("x1.00");
            else if (gated)     ImGui::TextColored(kGated, "x0.00 power");
            else if (f <= 0.0f) ImGui::TextColored(kGated, "x0.00 bright");
            else                ImGui::Text("x%.2f", f);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Override for this target (leave empty to use the table above)");
    bool changed = false;
    changed |= FxDriverEditor("bright", stack.bright, false);
    ImGui::Separator();
    changed |= FxDriverEditor("power", stack.power, true);
    if (ImGui::Button("Clear overrides")) {
        stack.bright = FxDriver();
        stack.power  = FxDriver();
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back to the built-in per-rect sources.");
    ImGui::Unindent(8.0f);

    // Saved, not journalled: a source is plumbing, and filling one journal entry
    // per keystroke in a text box would bury the colour edits that the journal
    // is for.
    if (changed) SavePanelFx();
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
        ImGui::SetTooltip(gFxClipboardFull
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
        ImGui::SetTooltip("Copy every layer here, to paste onto another target");

    ImGui::SameLine();
    ImGui::BeginDisabled(gFxStackClipboard.empty());
    const bool pasteStack = ImGui::Button("Paste stack");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (gFxStackClipboard.empty())
            ImGui::SetTooltip("Nothing copied yet - use Copy stack, or right-click "
                              "a target in the tree");
        else
            ImGui::SetTooltip("Add all %d layer(s) from \"%s\" on top.\n"
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draw this layer");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##blend", &layer.blend, kFxBlendName, kFxBlendCount)) {
            dirty = true; dirtyWhat = "change blend mode";
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kFxBlendHelp[layer.blend]);

        ImGui::SameLine();
        if (ImGui::ColorEdit3("##col", layer.color,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            dirty = true; dirtyWhat = "change layer colour";
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("##opacity", &layer.opacity, 0.0f, 1.0f, "%.3f")) {
            dirty = true; dirtyWhat = "change layer opacity";
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opacity");

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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy this layer's style");

        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete this layer");

        BuildFxLayerImageRow(layer, &dirty, &dirtyWhat);

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
            ImGui::SetTooltip("%s", gFxJournal[i].text.c_str());
        ImGui::TableSetColumnIndex(2);
        if (ImGui::SmallButton("Restore")) restore = i;
        ImGui::PopID();
    }
    ImGui::EndTable();

    if (restore >= 0)
        FxRestore(gFxJournal[restore].text,
                  "restore " + gFxJournal[restore].when);
}

static void BuildFxTab() {
    if (!gFxTexturesScanned) ScanFxTextures();   // also creates the drop folder
    // Also polled here, not only in the compositor: the thumbnails have to be
    // right even with Panel FX disabled or every stack empty, which is exactly
    // the state you are in while building the first look.
    PollFxTextureChanges();

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
        ImGui::SetTooltip("Re-read %s.\nDrop PNG/JPG/BMP/TGA files there and they "
                          "appear in every layer's Image box.\n\n"
                          "You do not need this to EDIT an image: a saved file is "
                          "picked up on its own within a second.\nShift-click to "
                          "force a re-read of every image anyway.",
                          FxOverlayDir().string().c_str());
    ImGui::SameLine();
    if (ImGui::Checkbox("Follow display brightness", &gFxFollowBrightness)) SavePanelFx();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scale every layer's opacity by the brightness knob of "
                          "the screen it is on, and skip a target whose power is "
                          "off.\nOff = every tint is drawn as authored and NOTHING "
                          "is power-gated, which is what you want while picking a "
                          "colour. Off wins over any per-target setting.\nThis is "
                          "the default for targets left on Inherit; each target "
                          "can say otherwise under \"Brightness / power "
                          "sources\".");
    ImGui::SameLine();
    if (ImGui::Checkbox("Curve", &gFxCurve)) SavePanelFx();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reshape the knob through the display response curve "
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
        ImGui::SetTooltip("Re-resolve every brightness/power dataref. Needed when "
                          "the aircraft's own plugin started after this one, "
                          "which is when they show as unreadable.");
    ImGui::SameLine();
    ImGui::TextDisabled("lit pass, after Gauges - A3xx");

    UiHint("Broader targets composite first, so a group is a base and its "
           "members refine it. A layer can be a flat colour or an image from "
           "%s (%d found).",
           FxOverlayDir().string().c_str(), (int)gFxTextures.size());
    ImGui::Separator();

    // Wider than a flat list needed: the tree spends width on indentation, and
    // each row carries a layer count and a delete button.
    ImGui::BeginChild("fx_targets", ImVec2(250.0f, 0.0f), ImGuiChildFlags_Borders);
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
        if (ImGui::Button(kRepoCommands[i].label)) RunRepoCommand(kRepoCommands[i].args);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n\npython %s",
                              kRepoCommands[i].help, kRepoCommands[i].args);
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
// because colour and geometry live apart in this DSL: a palette entry carries the
// rgb, the light type or fixture carries aim/spread/size. Pasting rgb into a
// light type would hard-code a colour that is supposed to change with the profile.
static void DbgLogDsl(const DbgLight& l) {
    Log("---- DSL for " + l.name + " ----");
    Log("  fixture " + l.fixture + ", category " + l.category + ", "
        + l.source + "[" + std::to_string(l.index) + "]");
    Log("  # lights.style.phdsl - the palette entry (colour only):");
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
    Log("  # offset applied while tuning: "
        + DbgFmt(l.off[0]) + " " + DbgFmt(l.off[1]) + " " + DbgFmt(l.off[2])
        + " (already folded into `at:` above)");
}

static void BuildLightRow(DbgLight& l) {
    ImGui::PushID(l.n);
    const bool selected = (gDbgSel == l.n);
    char label[192];
    std::snprintf(label, sizeof(label), "%s##%d", l.name.c_str(), l.n);
    if (ImGui::Selectable(label, selected)) DbgSelect(l.n);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("slot %d - %s[%d]\nbaked at %.3f %.3f %.3f",
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

    ImGui::ColorEdit3("colour", l.v);
    ImGui::SliderFloat("brightness", &l.v[3], 0.0f, 1.0f, "x%.3f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The light's own alpha. What reaches the sim is this "
                          "TIMES the rheostat it is wired to (%s[%d] = %.3f), "
                          "which is why a light can be at 1.0 and still dark.",
                          l.source.c_str(), l.index, (double)DbgRheostat(l));
    ImGui::SliderFloat("size", &l.v[4], 0.0f, 8.0f, "%.3f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far the pool reaches. Raise this FIRST when a "
                          "spread edit looks inert - a cone two centimetres from "
                          "the panel it lights has nowhere to widen into.");

    const bool omni = l.v[5] == 0.0f && l.v[6] == 0.0f && l.v[7] == 0.0f;
    if (omni) {
        ImGui::TextDisabled("omnidirectional (dir 0 0 0) - no aim, no spread");
    } else {
        bool aimed = false;
        aimed |= ImGui::SliderFloat("pitch", &l.pitch, -90.0f, 90.0f, "%.1f deg");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Degrees above horizontal. 0 level, +90 straight up, "
                              "-90 straight down.");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", DbgPitchWord(l.pitch));
        aimed |= ImGui::SliderFloat("yaw", &l.yaw, -180.0f, 180.0f, "%.1f deg");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 forward, 90 right, 180 aft, -90 left.");
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
            ImGui::SetTooltip("Half-angle of the cone. Stored as cos(spread), which "
                              "is why the DSL's `cone:` runs backwards - bigger is "
                              "narrower. Prefer `spread:` when writing it back.");
        ImGui::SameLine();
        ImGui::TextDisabled("(cone %.3f)", (double)l.v[8]);
    }

    if (gDbgPosTunable) {
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
        gDbgNote = "reverted every light to the OBJ's baked values";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back to what the OBJ bakes, for every light - the "
                          "clean slate to start a tuning pass from.");
    ImGui::SameLine();
    if (ImGui::Button("Log DSL")) DbgLogDsl(l);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Print this light in .phdsl shape to the Log tab - the "
                          "deliverable of a tuning session. Nothing here writes "
                          "the DSL for you; a tuned value that never gets "
                          "transcribed is lost on the next build.");
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
        ImGui::SetTooltip("Re-read lights_inn.debug.json / lights_screens.debug.json "
                          "from the loaded aircraft. Needed after every --debug "
                          "rebuild that changes the light LIST; editing values "
                          "needs nothing.");
    ImGui::SameLine();
    ImGui::Checkbox("Isolate", &gDbgIsolate);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Black out every light but the selected one. The fastest "
                          "answer to \"which lamp is that?\".");
    ImGui::SameLine();
    ImGui::Checkbox("Mark", &gDbgMark);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Paint the selected light magenta - nothing in a cockpit "
                          "is that colour.");
    ImGui::SameLine();
    ImGui::Checkbox("Blink", &gDbgBlink);
    ImGui::SameLine();
    bool compare = gDbgCompare != 0;
    if (ImGui::Checkbox("Tunable", &compare)) gDbgCompare = compare ? 1 : 0;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: the tunable copies. Off: the ORIGINAL light lines, "
                          "which this window cannot touch.\nThe only way to tell a "
                          "bad LIGHT_SPILL_CUSTOM conversion apart from bad "
                          "authored values - they look identical in the cockpit.");

    if (gDbgMarkerDots > 0) {
        ImGui::SameLine();
        bool markers = gDbgShowMarkers != 0;
        if (ImGui::Checkbox("Markers", &markers))
            gDbgShowMarkers = markers ? (gDbgSel >= 0 ? gDbgSel + 1 : -1) : 0;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw the light's origin (green), its aim (amber) and "
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
                ImGui::SetTooltip("How far the arrow and the cone ring are drawn. "
                                  "The right length depends entirely on the light - "
                                  "a 20 cm arrow is unreadable next to a pool of "
                                  "light a metre across.");
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
               "hypothesising: three earlier explanations were all wrong.");

    if (gDbgLights.empty()) {
        UiHint("No debug lights. Build the OBJ in its tunable shape first:\n"
               "    python build/build_objs.py build --target interior --debug --write\n"
               "then Reload manifest. %s",
               gDbgNote.empty() ? "" : ("(" + gDbgNote + ")").c_str());
        return;
    }

    ImGui::TextDisabled("%s - %d lights%s", gDbgManifest.c_str(),
                        (int)gDbgLights.size(),
                        gDbgPosTunable ? ", position tunable" : "");
    if (!gDbgNote.empty()) { ImGui::SameLine(); ImGui::TextDisabled("| %s", gDbgNote.c_str()); }
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
static void BuildDevUi() {
    if (!ImGui::BeginTabBar("photon_dev")) return;

    if (ImGui::BeginTabItem("Exterior")) { BuildConfigUi(gExtWin); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Cockpit"))  { BuildConfigUi(gIntWin); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Panel FX")) { BuildFxTab();           ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Lights"))   { BuildLightsTab();       ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("History"))  { BuildFxHistoryPane();   ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Probe"))    { BuildProbeTab();        ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Build"))    { BuildBuildTab();        ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Log"))      { BuildLogTab();          ImGui::EndTabItem(); }
    // The backend smoke test. If the stock demo draws, scrolls, responds to the
    // mouse and accepts typing, imgui_xplm.cpp is correct — a far better first
    // question than "why does my tool window look wrong", and the fastest widget
    // reference there is.
    if (ImGui::BeginTabItem("ImGui")) {
        UiHint("Backend smoke test. Everything below is stock ImGui.");
        ImGui::Separator();
        ImGui::ShowDemoWindow();
        ImGui::EndTabItem();
    }
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

#else   // probe compiled out — the shipping build
static void StartPanelProbe()   {}
static void StopPanelProbe()    {}
static void ReseatPanelProbe()  {}
#endif  // PHOTON_DEV

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
    StartPanelProbe();          // no-op unless built with -DPHOTON_DEV=ON
    UpdateMenuVisibility();
    LoadProfileForCurrentLivery();
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    if (gAutoLoopRegistered) {
        XPLMUnregisterFlightLoopCallback(AutoLoop, nullptr);
        gAutoLoopRegistered = false;
    }
    StopPanelProbe();
    StopEngine();
}

PLUGIN_API void XPluginStop(void) {
    StopEngine();
    DestroyPhotonMenu();
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
    Log("stopped");
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int inMessage, void* inParam) {
    if ((intptr_t)inParam != 0) return;   // user aircraft only
    if (inMessage == XPLM_MSG_PLANE_LOADED) {
        UpdateMenuVisibility();
        ReseatPanelProbe();     // after the aircraft's own plugin started — see the probe section
        LoadProfileForCurrentLivery();
    } else if (inMessage == XPLM_MSG_LIVERY_LOADED) {
        LoadProfileForCurrentLivery();
    }
}
