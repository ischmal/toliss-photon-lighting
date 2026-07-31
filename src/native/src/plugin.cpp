// plugin.cpp — ToLiss Photon Lighting, native .xpl (X-Plane SDK)
//
// Native C++ port of src/plugin/PI_ToLissPhoton.py — same behavior, no XPPython3
// / Python runtime dependency (CLAUDE.md TODO #4). The Python plugin remains the
// reference / in-sim tuning implementation; keep the two in sync. See that file's
// module docstring and CLAUDE.md for the full design, the dataref contract, and
// the "hard-won facts" this port preserves (write-race phase, int+float accessor
// typing, per-livery keying, the level-2 submenu crash → Custom is a window).
//
// Sections below mirror the Python structure:
//   constants → JSON → waveform engine → state → accessors → profile resolution
//   → persistence → menu → Custom window → engine loop → Auto loop → lifecycle.
//
// Translation notes vs. Python: per-key lambda closures become the accessor
// read-refcon (category index); the getDatavf "fills-in-place" quirk (gotcha #6)
// is just the native signature here; both int and float readers stay (gotcha #4).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlanes.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

// The panel-FBO probe is the only thing here that draws raw GL, and it is a
// compile-time opt-in (-DPHOTON_PANEL_PROBE=ON) so the shipping .xpl neither
// links opengl32 nor carries the code. See the probe section near the bottom.
#if PHOTON_PANEL_PROBE
  #if IBM
    #include <windows.h>
    #include <GL/gl.h>
  #elif APL
    #include <OpenGL/gl.h>
  #else
    #include <GL/gl.h>
  #endif
  // Dear ImGui, for the Panel FX layer editor. Compiled into every build (see
  // CMakeLists.txt), but only pulled in here where something uses it — the
  // shipping build's windows are still the XPLMDrawString ones.
  #include "imgui_xplm.h"
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
// The cockpit ("Gus Mod") lights. A SEPARATE axis from the exterior in every
// respect: its own categories, its own profile enum, its own persisted keys.
// Where the exterior is boolean 0/1 throughout, these are mostly TERNARY:
//     0 = old halogen, 1 = new halogen, 2 = LED
// which is why lights_inn.obj gates each branch with a PAIR of ANIM_hide lines
// (the open-ended range either side) where a two-valued one needs only one. See
// build/build_objs.py Emitter::branch_gate.
//
// `dome` is TWO-valued — 0 = fluorescent, 1 = LED. Its ramp (P4, `fluoro`) is the
// same white in both halogen eras on purpose: a dome light is a fluorescent
// fitting with no incandescent era to follow, so a third branch would duplicate
// the first. `values` carries that; nothing here may assume 3.
//
// ⚠ Nothing may ever write 2 to the dome dataref: the two-value encoding hides
// branch 0 at exactly 1 and branch 1 at exactly 0, so at 2 NEITHER hides and both
// draw. Every writer goes through IntValueForProfile or ClampIntValue.
//
// ONE CATEGORY PER BRIGHTNESS SOURCE, so everything in a category switches look
// together because it dims together:
//   panel[0]        dome      | panel[1],[2]   mainpnl
//   panel[3]        pedestal  | inst[22],[23]  console
//   panel[3] + ckpt/lights/map  map            (the documented exception —
//     grouped by fixture role, so the two overhead reading lamps sit with the
//     map spot; without them the row holds only a light the map knob can hide)
//
// panel[1],[2] is the FLOOD LT MAIN PNL knob, not the map lights — that pair
// shipped the wrong way round once. The `mainpnl` assignment rests on a single
// in-sim observation; see the OPEN note on the fixtures in lights.layout.phdsl.
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

static void Log(const std::string& msg) {
    XPLMDebugString(("ToLiss Photon: " + msg + "\n").c_str());
}

// ================================== JSON =====================================
// Minimal self-contained reader/writer (keeps the plugin dependency-free). Only
// what ToLissPhoton_profiles.json needs: objects, strings, ints, true/false/null;
// arrays are parsed and ignored. Same on-disk format the Python plugin uses.
namespace json {

struct Value {
    enum Type { Null, Int, Str, Obj } type = Null;
    long i = 0;
    std::string s;
    std::map<std::string, Value> obj;
    const Value* find(const std::string& k) const {
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
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

    bool array(Value& v) {   // consumed but not stored (schema has no arrays)
        v.type = Value::Null;
        ++p; ws();
        if (p < end && *p == ']') { ++p; return true; }
        while (p < end) {
            Value child;
            if (!value(child)) return false;
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
        v.type = Value::Int;
        v.i = std::strtol(std::string(start, p).c_str(), nullptr, 10);
        return true;
    }

    bool boolean(Value& v) {
        if (end - p >= 4 && !std::strncmp(p, "true", 4))  { p += 4; v.type = Value::Int; v.i = 1; return true; }
        if (end - p >= 5 && !std::strncmp(p, "false", 5)) { p += 5; v.type = Value::Int; v.i = 0; return true; }
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

// ======================= Custom config windows (modern UI) ===================
// XPLMCreateWindowEx floating windows (XPLM300+): HiDPI-correct, standard X-Plane
// chrome + close box, pop-out / VR capable. Replaces the legacy XPWidgets version.
// One button per available look on each row. Each button is an
// XPLMDrawTranslucentDarkBox (renders on every backend, Vulkan/Metal included —
// unlike raw OpenGL, which silently no-ops there) with the state shown by the
// TEXT: active = blue + "•", hover brightens it, press stacks a second (darker)
// box. State is read live from gValues/gIntValues — nothing to sync on show.
// Still plain windows (not menus), so CreatePhotonMenu's level-2 crash note holds.
//
// TWO SEPARATE WINDOWS, one per axis — Exterior (9 rows x 2 looks) and Cockpit
// (5 rows, 3 looks each except the dome's 2). They were one stacked window;
// splitting them matches how the axes actually behave (independent profiles,
// independent persistence, independently installable) and lets each open from
// its own submenu. Everything below is parameterised on a ConfigWindow so the
// two share one implementation and cannot drift apart.
//
// Buttons sit on a fixed COLUMN GRID — `cols` columns measured once over every
// row — but a row may carry FEWER buttons than columns, in which case one button
// spans. That is how the two-valued dome fits the three-wide cockpit grid without
// knocking the other rows out of alignment. See RowButtons.
//
// kTopInset is the gap above the first line of content, shared by all three
// windows. It is both where drawing starts and part of the computed height, so
// changing it moves content and shrinks the window by the same amount.
static const int kPadX = 4, kGap = 8, kTopInset = 0, kBtnPadX = 10, kLabelPad = 4;
static const int kHeaderTopGap = 4, kHeaderBotGap = 16;   // header spacing above / below
static const char* kBullet = "\xE2\x80\xA2 ";   // U+2022 "• " prefix on the active button
static const int kMaxCols = 3;
static const int kMaxBtns = kMaxCols;   // a row can never have more buttons than columns

struct ConfigWindow {
    bool         interior;    // which axis this window edits
    const char*  title;       // window chrome title
    const char*  header;      // one header line above the rows
    int          rows;        // NCAT or NINT
    int          cols;        // grid width = the MOST looks any one row offers
    XPLMWindowID id;
    // layout, computed once at creation then stable for the window's life
    int fontH, btnH, rowStride, headerH, labelColW;
    int colW[kMaxCols];
    // the button the mouse is currently holding down (-1 = none). `pressBtn` is
    // an index into the row's buttons, NOT a column — the two differ on a row
    // that spans.
    int pressRow, pressBtn;
};

static ConfigWindow gExtWin = {
    false, "ToLiss Photon - Exterior", "Custom exterior lights - configure:",
    NCAT, 2, nullptr, 0, 0, 0, 0, 0, {0, 0, 0}, -1, -1,
};
static ConfigWindow gIntWin = {
    true, "ToLiss Photon - Cockpit", "Custom cockpit lighting (mod by Gus Rodrigues) - configure:",
    NINT, 3, nullptr, 0, 0, 0, 0, 0, {0, 0, 0}, -1, -1,
};

// One clickable button: the dataref value it selects, its label, and the grid
// columns it covers (colFirst == colLast for the ordinary case).
struct RowButton { int value; const char* label; int colFirst, colLast; };

// The buttons of `row`, left to right. Returns how many.
//
// When a row has fewer looks than the grid has columns — today only the dome —
// its FIRST button spans the leading columns and the rest keep the trailing ones.
// So "Fluorescent" covers both slots the other rows use for their halogen looks,
// and the LED column still lines up down the window.
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

// Top edge of a row, below the single header band.
static int RowTop(const ConfigWindow& w, int winT, int row) {
    return winT - kTopInset - w.headerH - row * w.rowStride;
}

// Left/right edge of each grid COLUMN, given where the button area starts.
static void ColumnEdges(const ConfigWindow& w, int x0, int xl[kMaxCols], int xr[kMaxCols]) {
    int x = x0;
    for (int c = 0; c < w.cols; ++c) {
        xl[c] = x;
        xr[c] = x + w.colW[c];
        x = xr[c] + kGap;
    }
}

// Per-row geometry from the live window top-left — shared by draw + hit-test so a
// click always lands on the button it looks like. Fills `btn[]` with the row's
// buttons and `bl[]`/`br[]` with the left/right edge of each, spans resolved.
static int RowRects(const ConfigWindow& w, int winL, int winT, int row,
                    int& labelX, int& baseline, RowButton btn[kMaxBtns],
                    int bl[kMaxBtns], int br[kMaxBtns], int& top, int& bottom) {
    labelX = winL + kPadX;
    top    = RowTop(w, winT, row);
    bottom = top - w.btnH;
    int xl[kMaxCols], xr[kMaxCols];
    ColumnEdges(w, labelX + w.labelColW + kGap, xl, xr);
    int n = RowButtons(w, row, btn);
    for (int i = 0; i < n; ++i) {
        bl[i] = xl[btn[i].colFirst];
        br[i] = xr[btn[i].colLast];
    }
    baseline = bottom + (w.btnH - w.fontH) / 2 + 1;
    return n;
}

// which button covers (x,y); row = -1 if none. `outBtn` indexes the ROW's
// buttons, so it is what RowButtons returns — not a grid column.
static void ButtonAt(const ConfigWindow& w, int winL, int winT, int x, int y,
                     int& outRow, int& outBtn) {
    outRow = -1; outBtn = -1;
    for (int i = 0; i < w.rows; ++i) {
        int labelX, baseline, bl[kMaxBtns], br[kMaxBtns], top, bottom;
        RowButton btn[kMaxBtns];
        int n = RowRects(w, winL, winT, i, labelX, baseline, btn, bl, br, top, bottom);
        if (y < bottom || y > top) continue;
        for (int c = 0; c < n; ++c)
            if (x >= bl[c] && x <= br[c]) { outRow = i; outBtn = c; return; }
    }
}

static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static void DrawButton(int l, int b, int r, int t, int baseline,
                       bool selected, bool hover, bool pressed,
                       const std::string& text) {
    // Background: a translucent dark box (an XPLM primitive, so it renders on every
    // backend — Vulkan/Metal included). Stacking a second box on press darkens it
    // for a "pressed" look; that is the only box shade we can vary, since there is
    // no light or colored box primitive.
    XPLMDrawTranslucentDarkBox(l, t, r, b);          // (left, top, right, bottom)
    if (pressed) XPLMDrawTranslucentDarkBox(l, t, r, b);

    // State is carried by the TEXT: active = blue + "•"; hover brightens it.
    float col[3];
    if (selected) { col[0] = 0.0f; col[1] = 0.729f; col[2] = 1.0f; }    // #00baff (active)
    else          { col[0] = 0.82f; col[1] = 0.82f; col[2] = 0.84f; }   // light grey
    if (hover && !pressed) { col[0] += 0.14f; col[1] += 0.14f; col[2] += 0.14f; }
    for (int k = 0; k < 3; ++k) col[k] = Clamp01(col[k]);

    int tw = (int)XPLMMeasureString(xplmFont_Proportional, text.c_str(), (int)text.size());
    int tx = l + ((r - l) - tw) / 2;
    XPLMDrawString(col, tx, baseline, text.c_str(), nullptr, xplmFont_Proportional);
}

// Subtle divider: a dim rule drawn with the box-drawing glyph, so its weight is set
// by color (XPLMDrawTranslucentDarkBox has fixed, heavier opacity we can't dial down).
static void DrawDivider(int l, int r, int yCenter, int fontH) {
    static const char* dash = "\xE2\x94\x80";   // U+2500  ─
    float dim[3] = {0.32f, 0.32f, 0.34f};
    int gw = (int)XPLMMeasureString(xplmFont_Proportional, dash, (int)std::strlen(dash));
    if (gw < 1) gw = 4;
    int n = (r - l) / gw;
    if (n < 1) n = 1;
    std::string line;
    line.reserve((size_t)n * 3);
    for (int i = 0; i < n; ++i) line += dash;
    XPLMDrawString(dim, l, yCenter - fontH / 3, line.c_str(), nullptr, xplmFont_Proportional);
}

// The refcon carries which ConfigWindow this is, so one draw/click pair serves both.
static void DrawConfigWindow(XPLMWindowID win, void* refcon) {
    ConfigWindow& w = *(ConfigWindow*)refcon;
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);
    int mx = 0, my = 0;
    XPLMGetMouseLocationGlobal(&mx, &my);   // live cursor pos for hover feedback
    float header[3] = {0.86f, 0.86f, 0.86f};
    float white[3]  = {1.0f, 1.0f, 1.0f};
    // The header sits vertically centered in the band above the first row, so it
    // moves with the rows rather than being positioned independently.
    XPLMDrawString(header, l + kPadX, RowTop(w, t, 0) + kHeaderBotGap, w.header,
                   nullptr, xplmFont_Proportional);

    for (int i = 0; i < w.rows; ++i) {
        int labelX, baseline, bl[kMaxBtns], br[kMaxBtns], top, bottom;
        RowButton btn[kMaxBtns];
        int n = RowRects(w, l, t, i, labelX, baseline, btn, bl, br, top, bottom);
        XPLMDrawString(white, labelX, baseline, RowLabel(w, i), nullptr, xplmFont_Proportional);
        int cur = RowCurrentValue(w, i);
        for (int c = 0; c < n; ++c) {
            bool sel = cur == btn[c].value;
            std::string text = (sel ? kBullet : "") + std::string(btn[c].label);
            bool over  = mx >= bl[c] && mx <= br[c] && my >= bottom && my <= top;
            bool press = over && w.pressRow == i && w.pressBtn == c;   // held down on it
            DrawButton(bl[c], bottom, br[c], top, baseline, sel, over && !press, press, text);
        }
        // 1px divider centered in the gap below each row, except after the last.
        if (i != w.rows - 1) {
            int dy = bottom - (w.rowStride - w.btnH) / 2;
            DrawDivider(l + kPadX, r - kPadX, dy, w.fontH);
        }
    }
}

// Button behavior: press on mouse-down, act on mouse-up if released over the same
// button — so the press state shows while held and a drag-off cancels the click.
static int HandleConfigClick(XPLMWindowID win, int x, int y, XPLMMouseStatus status,
                             void* refcon) {
    ConfigWindow& w = *(ConfigWindow*)refcon;
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);
    int row, btnIdx;
    ButtonAt(w, l, t, x, y, row, btnIdx);
    if (status == xplm_MouseDown) {
        w.pressRow = row; w.pressBtn = btnIdx;
    } else if (status == xplm_MouseUp) {
        if (row >= 0 && row == w.pressRow && btnIdx == w.pressBtn) {
            RowButton btn[kMaxBtns];
            RowButtons(w, row, btn);
            int want = btn[btnIdx].value;
            if (want != RowCurrentValue(w, row)) {
                // Toggling ANY category switches that axis to Custom and persists
                // all of its categories — but only that axis. Changing a cockpit
                // light must not knock the exterior off Auto.
                if (w.interior) {
                    gIntValues[row] = ClampIntValue(row, want);
                    gIntProfile = INT_PROFILE_CUSTOM;
                    SaveIntCustom();
                    Log(std::string("custom cockpit: ") + kIntCategories[row].key
                        + "=" + std::to_string(want));
                } else {
                    gValues[row] = want;        // 0 = classic (Halogen/Xenon), 1 = LED
                    gProfile = PROFILE_CUSTOM;
                    SaveCustom();
                    Log(std::string("custom: ") + kCategories[row].key
                        + "=" + std::to_string(want));
                }
                UpdateChecks();
            }
        }
        w.pressRow = -1; w.pressBtn = -1;
    }
    return 1;   // our window owns clicks within it
}

// XPLMCreateWindowEx requires non-null callbacks for every slot.
static void            HandleCustomKey(XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {}
static XPLMCursorStatus HandleCustomCursor(XPLMWindowID, int, int, void*) { return xplm_CursorDefault; }
static int             HandleCustomWheel(XPLMWindowID, int, int, int, int, void*) { return 0; }
static int             HandleCustomRightClick(XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 1; }

static int MeasureText(const std::string& s) {
    return (int)XPLMMeasureString(xplmFont_Proportional, s.c_str(), (int)s.size());
}

static void CreateConfigWindow(ConfigWindow& w) {
    if (w.id) return;
    int fw = 0, fd = 0;
    XPLMGetFontDimensions(xplmFont_Proportional, &fw, &w.fontH, &fd);
    w.btnH = w.fontH + 12;        // row height +4
    w.rowStride = w.btnH + 14;    // inter-row padding so the divider isn't compressed
    w.headerH = w.fontH + kHeaderTopGap + kHeaderBotGap;   // text + gaps above/below

    w.labelColW = 0;
    for (int i = 0; i < w.rows; ++i)
        w.labelColW = std::max(w.labelColW, MeasureText(RowLabel(w, i)));
    w.labelColW += kLabelPad;   // breathing room between the longest label and the button

    // Column widths are per-COLUMN, measured over every row, so the buttons line
    // up in a grid. The exterior's first column holds "Halogen" or "Xenon"
    // depending on the row, so it is the widest of those.
    //
    // Two passes, because a SPANNING button belongs to no single column: pass one
    // sizes each column from the buttons sitting in exactly one, pass two widens
    // the span's last column if the label still would not fit. Sizing a column
    // from a spanning label directly would inflate it for every other row.
    for (int c = 0; c < w.cols; ++c) w.colW[c] = 0;
    for (int i = 0; i < w.rows; ++i) {
        RowButton btn[kMaxBtns];
        int n = RowButtons(w, i, btn);
        for (int k = 0; k < n; ++k) {
            if (btn[k].colFirst != btn[k].colLast) continue;
            int need = MeasureText(std::string(kBullet) + btn[k].label) + 2 * kBtnPadX;
            w.colW[btn[k].colFirst] = std::max(w.colW[btn[k].colFirst], need);
        }
    }
    for (int i = 0; i < w.rows; ++i) {
        RowButton btn[kMaxBtns];
        int n = RowButtons(w, i, btn);
        for (int k = 0; k < n; ++k) {
            if (btn[k].colFirst == btn[k].colLast) continue;
            int have = 0;
            for (int c = btn[k].colFirst; c <= btn[k].colLast; ++c)
                have += w.colW[c] + (c > btn[k].colFirst ? kGap : 0);
            int need = MeasureText(std::string(kBullet) + btn[k].label) + 2 * kBtnPadX;
            if (need > have) w.colW[btn[k].colLast] += need - have;
        }
    }
    int rowW = 0;
    for (int c = 0; c < w.cols; ++c) rowW += w.colW[c] + (c ? kGap : 0);

    int bodyW = std::max(rowW, MeasureText(w.header) - w.labelColW - kGap);
    int width  = kPadX + w.labelColW + kGap + bodyW + kPadX;
    int height = kTopInset + w.headerH + w.rowStride * (w.rows - 1) + w.btnH + kPadX;

    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    int left = (sl + sr) / 2 - width / 2;
    int top  = (st + sb) / 2 + height / 2;

    XPLMCreateWindow_t p;
    std::memset(&p, 0, sizeof(p));
    p.structSize = sizeof(p);
    p.left = left;  p.top = top;  p.right = left + width;  p.bottom = top - height;
    p.visible = 1;
    p.drawWindowFunc       = DrawConfigWindow;
    p.handleMouseClickFunc = HandleConfigClick;
    p.handleKeyFunc        = HandleCustomKey;
    p.handleCursorFunc     = HandleCustomCursor;
    p.handleMouseWheelFunc = HandleCustomWheel;
    p.refcon               = &w;
    p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
    p.layer                = xplm_WindowLayerFloatingWindows;
    p.handleRightClickFunc = HandleCustomRightClick;
    w.id = XPLMCreateWindowEx(&p);
    XPLMSetWindowTitle(w.id, w.title);
    XPLMSetWindowPositioningMode(w.id, xplm_WindowPositionFree, -1);
    // fixed size: min == max == the computed size, so the user can't resize it
    XPLMSetWindowResizingLimits(w.id, width, height, width, height);
    Log(std::string("custom window built: ") + w.title);
}

static void ShowConfigWindow(ConfigWindow& w) {
    if (!w.id) CreateConfigWindow(w);
    if (w.id) { XPLMSetWindowIsVisible(w.id, 1); XPLMBringWindowToFront(w.id); }
}

static void DestroyConfigWindow(ConfigWindow& w) {
    if (w.id) { XPLMDestroyWindow(w.id); w.id = nullptr; }
}

// ================================ About window ===============================
// Static text plus two link buttons. The links are the same two the installer
// offers on its final screen (installer/constants.py) — keep the three in step.
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

// tone: 0 = bright (headings), 1 = normal, 2 = dim. An empty string is a spacer.
struct AboutLine { const char* text; int tone; };
static const AboutLine kAboutLines[] = {
    {"ToLiss Photon Lighting",                                       0},
    {"Exterior and cockpit lighting for ToLiss Airbus aircraft.",    1},
    {"",                                                             1},
    {"Credits",                                                      0},
    {"Photon by schmal.",                                            1},
    {"Cockpit lighting by Gus Rodrigues, used with permission.",     1},
    {"",                                                             1},
    {"Not affiliated with or endorsed by ToLiss.",                   2},
};
static const int NABOUT = (int)(sizeof(kAboutLines) / sizeof(kAboutLines[0]));

struct AboutLink { const char* label; const char* url; };
static const AboutLink kAboutLinks[] = {
    {"X-Plane.org Page",             kOrgUrl},
    {"GitHub Project",               kGitHubUrl},
    {"Gus Rodrigues on X-Plane.org", kGusUrl},
};
static const int NABOUTLINK = (int)(sizeof(kAboutLinks) / sizeof(kAboutLinks[0]));

static XPLMWindowID gAboutId = nullptr;
static int gAboutFontH = 0, gAboutLineH = 0, gAboutBtnH = 0, gAboutBtnW[NABOUTLINK] = {0};
static int gAboutPress = -1;

// Geometry of link button `i`, from the live window top-left.
static void AboutLinkRect(int winL, int winT, int i, int& l, int& r, int& top, int& bottom) {
    int x = winL + kPadX;
    for (int k = 0; k < i; ++k) x += gAboutBtnW[k] + kGap;
    l = x;
    r = x + gAboutBtnW[i];
    top = winT - kTopInset - NABOUT * gAboutLineH - kHeaderBotGap;
    bottom = top - gAboutBtnH;
}

static void DrawAboutWindow(XPLMWindowID win, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);
    int mx = 0, my = 0;
    XPLMGetMouseLocationGlobal(&mx, &my);

    float bright[3] = {1.0f, 1.0f, 1.0f};
    float normal[3] = {0.86f, 0.86f, 0.86f};
    // "dim" only steps below `normal`; it must still read against the translucent
    // window backing, which the old 0.55 grey did not.
    float dim[3]    = {0.80f, 0.80f, 0.82f};
    for (int i = 0; i < NABOUT; ++i) {
        if (!kAboutLines[i].text[0]) continue;
        float* col = kAboutLines[i].tone == 0 ? bright
                   : kAboutLines[i].tone == 1 ? normal : dim;
        int y = t - kTopInset - (i + 1) * gAboutLineH;
        XPLMDrawString(col, l + kPadX, y, kAboutLines[i].text, nullptr, xplmFont_Proportional);
    }
    // version, right-aligned on the title line
    std::string ver = std::string("v") + kPhotonVersion;
    XPLMDrawString(dim, r - kPadX - MeasureText(ver), t - kTopInset - gAboutLineH,
                   ver.c_str(), nullptr, xplmFont_Proportional);

    for (int i = 0; i < NABOUTLINK; ++i) {
        int bl, br, top, bottom;
        AboutLinkRect(l, t, i, bl, br, top, bottom);
        bool over  = mx >= bl && mx <= br && my >= bottom && my <= top;
        bool press = over && gAboutPress == i;
        int baseline = bottom + (gAboutBtnH - gAboutFontH) / 2 + 1;
        DrawButton(bl, bottom, br, top, baseline, false, over && !press, press,
                   kAboutLinks[i].label);
    }
}

static int HandleAboutClick(XPLMWindowID win, int x, int y, XPLMMouseStatus status, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);
    int hit = -1;
    for (int i = 0; i < NABOUTLINK; ++i) {
        int bl, br, top, bottom;
        AboutLinkRect(l, t, i, bl, br, top, bottom);
        if (x >= bl && x <= br && y >= bottom && y <= top) { hit = i; break; }
    }
    if (status == xplm_MouseDown) {
        gAboutPress = hit;
    } else if (status == xplm_MouseUp) {
        if (hit >= 0 && hit == gAboutPress) OpenUrl(kAboutLinks[hit].url);
        gAboutPress = -1;
    }
    return 1;
}

static void CreateAboutWindow() {
    if (gAboutId) return;
    int fw = 0, fd = 0;
    XPLMGetFontDimensions(xplmFont_Proportional, &fw, &gAboutFontH, &fd);
    gAboutLineH = gAboutFontH + 8;
    gAboutBtnH  = gAboutFontH + 12;

    int textW = 0;
    for (int i = 0; i < NABOUT; ++i) textW = std::max(textW, MeasureText(kAboutLines[i].text));
    int btnRowW = 0;
    for (int i = 0; i < NABOUTLINK; ++i) {
        gAboutBtnW[i] = MeasureText(kAboutLinks[i].label) + 2 * kBtnPadX;
        btnRowW += gAboutBtnW[i] + (i ? kGap : 0);
    }
    int width  = kPadX + std::max(textW + 4 * kPadX, btnRowW) + kPadX;
    int height = kTopInset + NABOUT * gAboutLineH + kHeaderBotGap + gAboutBtnH + kPadX * 2;

    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    int left = (sl + sr) / 2 - width / 2;
    int top  = (st + sb) / 2 + height / 2;

    XPLMCreateWindow_t p;
    std::memset(&p, 0, sizeof(p));
    p.structSize = sizeof(p);
    p.left = left;  p.top = top;  p.right = left + width;  p.bottom = top - height;
    p.visible = 1;
    p.drawWindowFunc       = DrawAboutWindow;
    p.handleMouseClickFunc = HandleAboutClick;
    p.handleKeyFunc        = HandleCustomKey;
    p.handleCursorFunc     = HandleCustomCursor;
    p.handleMouseWheelFunc = HandleCustomWheel;
    p.refcon               = nullptr;
    p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
    p.layer                = xplm_WindowLayerFloatingWindows;
    p.handleRightClickFunc = HandleCustomRightClick;
    gAboutId = XPLMCreateWindowEx(&p);
    XPLMSetWindowTitle(gAboutId, "About ToLiss Photon");
    XPLMSetWindowPositioningMode(gAboutId, xplm_WindowPositionFree, -1);
    XPLMSetWindowResizingLimits(gAboutId, width, height, width, height);
    Log("about window built");
}

static void ShowAboutWindow() {
    if (!gAboutId) CreateAboutWindow();
    if (gAboutId) { XPLMSetWindowIsVisible(gAboutId, 1); XPLMBringWindowToFront(gAboutId); }
}

static void DestroyAboutWindow() {
    if (gAboutId) { XPLMDestroyWindow(gAboutId); gAboutId = nullptr; }
}

// The Debug submenu is built by the panel-probe block further down, which needs
// the button/measure helpers above — hence forward declarations rather than
// moving either. In a shipping build these are the no-op versions, so the menu
// code below needs no #if of its own and there is no Debug row at all.
#if PHOTON_PANEL_PROBE
static void AppendDebugMenu(XPLMMenuID parent);
static void DestroyDebugMenu();
static void DestroyPanelDebugWindow();
static void DestroyDebugImguiWindows();
#else
static void AppendDebugMenu(XPLMMenuID) {}
static void DestroyDebugMenu() {}
static void DestroyPanelDebugWindow() {}
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
    // NOTE ON THE LEVEL-2 SUBMENU WARNING: nesting is used here deliberately.
    // The "no menu level 2+" rule recorded in CLAUDE.md is an **XPPython3 4.7a1**
    // defect in native menu click-dispatch, and this plugin is native C++ — the
    // rule still binds src/plugin/PI_PhotonDevReload.py, which remains XPPython3.
    // Custom stays a floating WINDOW rather than a third submenu because a grid
    // of toggles is not a menu shape, not because it can't be nested.
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
    DestroyPanelDebugWindow();
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
        for (int i = 0; i < kScreenCount; ++i) {
            const int idx = kScreenDU[i];
            gScreenAlpha[i] = (idx >= 0 && idx < got)
                ? (float)ClampBrightness(du[idx]) * kScreenGain
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
// Compile-time opt-in: cmake -DPHOTON_PANEL_PROBE=ON (deploy.ps1 -Probe).
//
// This exists to answer ONE question before any real work is built on it:
//   when Photon draws in a 2-d cockpit phase, do its pixels land ON TOP of
//   ToLiss's inside the aircraft's "<acf>/lit panel fbo:0" render target?
//
// Why the question is open: order of draw callbacks BETWEEN plugins within one
// phase is not specified by the SDK, and the aircraft's plugin loads after the
// global ones — so ToLiss may well be called after us and paint over whatever we
// drew. No amount of reading settles it; a magenta rectangle does.
//
// Why the target is reachable at all: ToLiss's cockpit_INN.obj declares
// GLOBAL_cockpit_lit and carries NO TEXTURE line, i.e. the whole 46k-vertex
// cockpit mesh is UV-mapped onto the aircraft's panel texture (Panel_Airliner.png,
// 4096x4096). The displays are ToLiss plugin-drawn into that same FBO. So a quad
// drawn in panel coordinates lands on real cockpit geometry — no shader injection
// and no texture replacement needed.
//
// HOW TO READ THE RESULT — START WITH THE FLOOD, NOT THE DU RECTS.
// Set panel_probe_rect to -2 (flood: one quad over the whole 4096 panel):
//   The whole cockpit goes magenta   -> we are in the panel FBO and our pixels
//                                       survive. Now switch to the DU rects.
//   Nothing anywhere                 -> we are NOT drawing into the panel target
//                                       at all (wrong framebuffer, or GL through
//                                       the Vulkan bridge is a no-op here). Read
//                                       the "GL target" line in Log.txt — rect
//                                       tuning cannot help until that is fixed.
//   Part of the cockpit only         -> right target, coordinates are off.
// Then, with the rects (mode 1, the default):
//   Captain's PFD is a flat magenta rectangle  -> we draw after ToLiss. Proceed.
//   Captain's PFD looks completely normal      -> ToLiss draws after us. Try the
//                                                 re-registration below, then mode 2.
//
// ⚠ Mode 3 ("before gauges") is NOT a sanity check that we draw at all — it is
// registered ahead of every gauge, so ToLiss is guaranteed to paint over it. It
// proves nothing on its own. The flood is the sanity check.
//
// Log.txt carries the registration results, the panel extents, the bound
// framebuffer + viewport + attached colour texture, and every distinct
// panel_render_type seen — check it either way. Those are re-logged on every
// mode/rect change, so a cycle through the modes leaves one block per mode.
//
// ⚠ COORDINATES ARE RAW PANEL PIXELS, NOT REMAPPED THROUGH panel_total_pnl_*.
// The first run of this probe divided by kPanelAtlas and multiplied by those
// extents, which report **1024** on the A320 — the aircraft's 2-D panel is empty
// (PANEL_2D_BEGIN immediately followed by PANEL_2D_END in a320.acf), so that
// dataref describes a default 2-D panel and not the 3-D panel texture. Every quad
// came out 4x too small in the bottom-left corner, on atlas area no cockpit
// geometry samples, and nothing was visible in any mode. The 3-D panel really is
// the 4096 space: a320.acf places GROUP NDCapt at POS 1139,2953 — 1139 is the
// exact x-centre of the "capt ND" rect below. The extents are still logged, as
// information; they are no longer applied.
#if PHOTON_PANEL_PROBE

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

// HOW the pixels are put down. Added after the 2026-07-31 run, which proved the
// framebuffer is right and the flood still showed nothing:
//     fbo=2 viewport=0,0 4096x4096 color0_type=0x1702 (GL_TEXTURE) color0_tex=254
// Non-zero FBO, viewport exactly the panel texture size, a real texture attached.
// So "wrong render target" and "wrong coordinates" are BOTH eliminated — a quad
// spanning -4096..8192 cannot miss a 4096 viewport. What is left is that the
// draw itself produces no fragments, and these three methods separate the causes.
//
//   Clear     — glClear through a scissor box. Touches NO vertex or fragment
//               stage: no shader, no matrices, no culling, no immediate mode.
//               If even this shows nothing, our GL calls are not reaching that
//               FBO at all and the approach needs rethinking, not tuning.
//   Immediate — the original glBegin/glEnd. Depends on X-Plane's fixed-function
//               matrices and on no shader program being bound.
//   Forced    — immediate, but with glUseProgram(0), culling and depth off, and
//               our own glOrtho over the 4096 panel. This is the fix if the
//               state dump shows a bound program or an unexpected projection.
//   Multiply  — NOT a diagnostic. This is the FCU-tint effect itself, living in
//               the probe until the look is signed off (docs/fcu_tint_plan.md
//               §5 step 3). Same quad as Immediate, but blended
//               glBlendFunc(GL_DST_COLOR, GL_ZERO) — a straight multiply of the
//               framebuffer by ToLissPhoton/debug/panel_tint_{r,g,b}.
//
// Why multiply is the right primitive: the FCU background is BLACK, and
// black x anything is black. So a multiply over the whole 818x66 strip leaves
// the background untouched and recolours only the lit digits — the mask is the
// content ToLiss already drew. It is also self-scaling: a dim readout gets a
// proportionally small absolute shift, so the tint tracks the FCU brightness
// knob with no dataref read. Multiply can only DARKEN, which is fine because
// pulling yellow-green toward orange means removing green.
//
// ⚠ Multiply and Forced are separate methods and cannot be combined: Forced
// disables GL_BLEND, which is precisely what multiply needs on. If the tint is
// invisible and the state dump shows a bound program, that is a real conflict
// to solve, not a matter of picking the other method.
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

// The panel texture is a 4096x4096 atlas. These rects were derived by walking
// cockpit_INN.obj: it carries no TEXTURE line and declares GLOBAL_cockpit_lit,
// so every ATTR_cockpit triangle samples the panel texture, UV 0..1 maps to the
// whole 4096 atlas, and a display face's UV bounding box IS its pixel rect in
// the FBO. Pixels are kept verbatim from that derivation rather than rounded,
// so they diff cleanly against a re-run of it.
//
// The derivation groups triangles into islands by shared VERTEX INDEX (an OBJ
// splits vertices wherever UVs differ, so a UV island is exactly one face — and
// a single TRIS batch covers many faces, which is what hid the SD last time).
// The display units then fall out geometrically: aft-facing (normal ~0,0.31,0.95),
// 16.8 x 16.1 cm, z ~ -4.6. The capt PFD and capt ND rects below were confirmed
// in-sim on 2026-07-31 to cover their screens exactly; the other four come from
// the identical derivation and share the normal, size and z to 3 decimals.
//
// ⚠ THE RECT SET IS RUNTIME-GATED. Every DU face exists TWICE in the OBJ, under
// `ANIM_hide … AirbusFBW/DisplayResolutionFallback`: the ~748 px variant is
// visible when that dataref is 0, and a smaller ~499 px variant anchored at the
// same origin is visible when it is 1. Draw the wrong set and the overlay is
// two-thirds size on a correctly-aimed screen. The dataref is read per frame.
//
// ⚠ A320 ONLY. Every airframe has its own atlas layout; re-derive per airframe
// before trusting these anywhere but the A320.
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
};

// DisplayResolutionFallback == 1. Same faces, same origins, ~499 px instead of
// ~748. The FCU row (strip + the two baro windows) is not resolution-switched —
// each has one variant only, so those three rows are identical in both tables.
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
};
static const int kPanelRectCount = (int)(sizeof(kPanelRectsHi) / sizeof(kPanelRectsHi[0]));

// --- groups ------------------------------------------------------------------
// A named set of rects addressed as one target, because the thing a person wants
// to recolour is often not one UV island. The FCU readout row is three: the
// centre strip plus the captain's and F/O's barometric-reference windows on the
// EFIS control panels, which flank it at x ±0.27 and share its exact v-row
// (954.0..1020.2) — derived 2026-07-31, and un-gated by any ANIM block just
// like the strip itself.
//
// ⚠ These are deliberately NOT merged into one wide rect. The atlas gaps between
// them (1023.7..1046.1 and 1863.9..1890.4) are sampled by other cockpit geometry,
// so a single 896..2017 quad would recolour whatever else lives in those 22 and
// 26 pixel columns. Three quads cost nothing and touch only our faces.
//
// Members are named, not indexed, for the same reason every other lookup here is
// by name: re-deriving the table must not silently retarget a group.
//
// Groups NEST by containment, not by declaration — see PanelTargetMask below.
// "All displays" contains "Screens" contains "capt PFD"; nothing declares that,
// it falls out of the rect sets. So adding a group is enough to place it in the
// hierarchy, and no group can end up claiming a rect it does not actually paint.
struct PanelGroup { const char* name; const char* members[10]; };
static const PanelGroup kPanelGroups[] = {
    { "FCU row", { "capt baro", "FCU strip", "FO baro", nullptr } },
    { "Screens", { "capt PFD", "capt ND", "E/WD", "SD", "FO ND", "FO PFD", nullptr } },
    { "Whole panel", { "capt PFD", "capt ND", "E/WD", "SD", "FO ND", "FO PFD",
                       "FCU strip", "capt baro", "FO baro", nullptr } },
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
static unsigned PanelTargetMask(int t) {
    if (t >= 0 && t < kPanelRectCount) return 1u << t;
    if (t >= kPanelRectCount && t < kPanelTargetCount) {
        const PanelGroup& g = kPanelGroups[t - kPanelRectCount];
        const int maxMembers = (int)(sizeof(g.members) / sizeof(g.members[0]));
        unsigned mask = 0;
        for (int m = 0; m < maxMembers && g.members[m]; ++m) {
            const int i = RectIndexByName(g.members[m]);
            if (i >= 0) mask |= 1u << i;
        }
        return mask;
    }
    return 0;
}

static int PanelTargetBreadth(int t) {
    unsigned m = PanelTargetMask(t);
    int n = 0;
    for (; m; m &= m - 1) ++n;
    return n;
}

// Strict containment: a contains b, and they are not the same set.
static bool PanelTargetContains(int a, int b) {
    const unsigned ma = PanelTargetMask(a), mb = PanelTargetMask(b);
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
static void EmitRectQuad(const PanelRect& r, float cr, float cg, float cb, float ca) {
    GLint front = GL_CCW;
    glGetIntegerv(GL_FRONT_FACE, &front);

    glColor4f(cr, cg, cb, ca);
    glBegin(GL_QUADS);
    if (front == GL_CCW) {
        glVertex2f(r.u0, r.v0);
        glVertex2f(r.u1, r.v0);
        glVertex2f(r.u1, r.v1);
        glVertex2f(r.u0, r.v1);
    } else {
        glVertex2f(r.u0, r.v0);
        glVertex2f(r.u0, r.v1);
        glVertex2f(r.u1, r.v1);
        glVertex2f(r.u1, r.v0);
    }
    glEnd();
}

// The probe's own fills are opaque; the FX compositor needs per-layer alpha, and
// PanelPaintFn's signature has no room for it. gFxAlpha is set immediately
// before each PaintPanelTarget call and read back here.
static float gFxAlpha = 1.0f;
static void DrawProbeRect(const PanelRect& r, float cr, float cg, float cb) {
    EmitRectQuad(r, cr, cg, cb, 1.0f);
}
static void DrawFxRect(const PanelRect& r, float cr, float cg, float cb) {
    EmitRectQuad(r, cr, cg, cb, gFxAlpha);
}

// glClear through a scissor box. The value of this path is everything it does
// NOT touch: no vertices, no shader, no matrix stack, no culling, no blending.
// A negative result here is the strongest possible one.
static void ClearProbeRect(const PanelRect& r, float cr, float cg, float cb) {
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
typedef void (*PanelPaintFn)(const PanelRect&, float, float, float);
static void PaintPanelTarget(int target, PanelPaintFn paint,
                             float cr, float cg, float cb) {
    const PanelRect* rects = PanelRects();
    if (target == kProbeFloodRect) { paint(kFloodRect, cr, cg, cb); return; }
    if (target < 0) {
        for (int i = 0; i < kPanelRectCount; ++i) paint(rects[i], cr, cg, cb);
        return;
    }
    if (target < kPanelRectCount) { paint(rects[target], cr, cg, cb); return; }
    if (target >= kPanelTargetCount) return;

    const PanelGroup& g = kPanelGroups[target - kPanelRectCount];
    const int maxMembers = (int)(sizeof(g.members) / sizeof(g.members[0]));
    for (int m = 0; m < maxMembers && g.members[m]; ++m) {
        const int i = RectIndexByName(g.members[m]);
        if (i >= 0) paint(rects[i], cr, cg, cb);
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

// Multiply blending: dst <- src * dst, componentwise. Blending is ALREADY
// enabled in this pass (measured: blend=1), so only the func changes — but the
// func is shared state on exactly the same footing as the enables above, and
// leaking it corrupts every blended thing X-Plane draws for the rest of the
// pass. Same lesson as the GL_CULL_FACE leak; save and restore.
//
// Alpha needs no colour mask: the src factor applies to all four channels, so
// dst.a becomes dst.a * src.a, and src.a is held at 1.0 by DrawProbeRect. The
// framebuffer's alpha therefore comes through untouched by construction.
//
// ⚠ Push AFTER XPLMSetGraphicsState, never before. That call is X-Plane's own
// state tracker and sets a blend func of its own when it enables blending; what
// we want to restore is the func the TRACKER believes is current, not the one
// the panel pass had before it ran. Saving first would put the tracker and the
// real GL state out of step — the same class of bug as the culling leak, minus
// the visible symptom.
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

// ======================== panel FX: the layer stacks ==========================
//
// The generalisation of the single multiply tint. A TARGET (a rect or a group)
// carries an ordered stack of LAYERS, each a colour, a blend mode and an
// opacity; the compositor walks the stacks bottom-to-top every lit-pass frame.
// The FCU work is what forced this: multiply alone lands the hue but cannot
// brighten, so the accepted look needs a multiply and an additive pass together
// (docs/fcu_tint_plan.md §3), and hard-coding a second pass beside the first
// would have to be undone at the third.
//
// Layer order is bottom-first — layers[0] composites first, later layers see its
// result. That is the same direction a paint program's layer list reads from the
// bottom up, and the editor lists it accordingly.
// --- blend modes -------------------------------------------------------------
// Everything expressible with a fixed-function blend func and equation, which is
// the whole budget: this composites by drawing a quad over the panel FBO, so
// only what GL can do between an incoming constant colour and the framebuffer is
// available. That covers the separable modes below.
//
// Not possible here, and not worth trying to fake: Overlay, Soft/Hard Light,
// Colour Burn/Dodge, Hue/Saturation/Colour/Luminosity, and anything gamma- or
// contrast-shaped. Every one needs the destination pixel as a shader INPUT — a
// branch or a divide on dst — and the blender only ever computes
// `src*sf OP dst*df` with the factors chosen per draw, not per pixel. They would
// need the panel FBO bound as a texture and a fragment shader, which is a
// different feature (and one X-Plane does not hand us a hook for).
//
// ⚠ Order here is free — the save file stores blend modes by NAME, so inserting
// a mode cannot silently change what a saved layer does. Names must stay single
// words: the loader reads them with `>>`.
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

struct FxLayer {
    bool  enabled  = true;
    int   blend    = kFxMultiply;
    float color[3] = { 1.00f, 0.82f, 0.90f };
    float opacity  = 1.00f;
};
struct FxStack {
    int                  target = -1;   // index in the shared rect/group space
    std::vector<FxLayer> layers;
};

static std::vector<FxStack> gFxStacks;
static bool gFxEnabled = false;

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
// mode's own identity colour and not toward black: white for the ones that
// scale the destination down (Multiply, Darken), black for the ones that add to
// it (Add, Screen, Subtract, Lighten), white again for Burn because it emits
// 1-colour. Getting this wrong does not look like a wrong number — it looks like
// the opacity slider doing the opposite of what it says.
static void DrawFxLayer(const FxLayer& layer, int target) {
    float r = layer.color[0], g = layer.color[1], b = layer.color[2];
    const float o = layer.opacity;
    GLenum src = GL_ONE, dst = GL_ZERO, eq = GL_FUNC_ADD;

    switch (layer.blend) {
        case kFxMultiply:
            // ⚠ Opacity interpolates toward WHITE, not toward black. White is
            // multiply's identity, so a 0-opacity layer has to be a no-op;
            // scaling the colour by opacity would instead fade the panel to
            // black, which is the opposite of "this layer is not there".
            r = 1.0f + o * (r - 1.0f);
            g = 1.0f + o * (g - 1.0f);
            b = 1.0f + o * (b - 1.0f);
            gFxAlpha = 1.0f;
            src = GL_DST_COLOR; dst = GL_ZERO;
            break;
        case kFxAdd:
            r *= o; g *= o; b *= o;
            gFxAlpha = 0.0f;               // additive: contribute no alpha
            src = GL_ONE; dst = GL_ONE;
            break;
        case kFxScreen:
            r *= o; g *= o; b *= o;
            gFxAlpha = 0.0f;
            src = GL_ONE_MINUS_DST_COLOR; dst = GL_ONE;
            break;
        case kFxSubtract:
            // dst - src, so the identity is black and opacity scales plainly.
            r *= o; g *= o; b *= o;
            gFxAlpha = 0.0f;
            src = GL_ONE; dst = GL_ONE; eq = GL_FUNC_REVERSE_SUBTRACT;
            break;
        case kFxBurn:
            // Linear burn, dst + src - 1, rewritten as dst - (1 - src) so the
            // same reverse subtract does it. The quad therefore carries the
            // COMPLEMENT of the layer colour, which is why opacity scales that
            // complement — at 0 it emits black and subtracts nothing.
            r = (1.0f - r) * o; g = (1.0f - g) * o; b = (1.0f - b) * o;
            gFxAlpha = 0.0f;
            src = GL_ONE; dst = GL_ONE; eq = GL_FUNC_REVERSE_SUBTRACT;
            break;
        case kFxDarken:
            // min/max ignore the blend factors entirely — the equation is the
            // whole operation — so src/dst below are set only to leave the func
            // in a defined state. Identity is white; source alpha 1 keeps
            // min(1, dst.a) = dst.a if the alpha half falls back to the RGB one.
            r = 1.0f + o * (r - 1.0f);
            g = 1.0f + o * (g - 1.0f);
            b = 1.0f + o * (b - 1.0f);
            gFxAlpha = 1.0f;
            src = GL_ONE; dst = GL_ZERO; eq = GL_MIN;
            break;
        case kFxLighten:
            // Identity is black, and source alpha 0 keeps max(0, dst.a).
            r *= o; g *= o; b *= o;
            gFxAlpha = 0.0f;
            src = GL_ONE; dst = GL_ZERO; eq = GL_MAX;
            break;
        default:                           // kFxNormal
            gFxAlpha = o;
            src = GL_SRC_ALPHA; dst = GL_ONE_MINUS_SRC_ALPHA;
            break;
    }

    if (!SetFxBlend(src, dst, eq)) return;
    PaintPanelTarget(target, DrawFxRect, r, g, b);
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

    // Blending on, nothing else. Same shape as the probe's multiply mode.
    XPLMSetGraphicsState(0, 0, 0, 0, 1, 0, 0);
    PushBlendFunc();                       // AFTER XPLMSetGraphicsState — see above

    const std::vector<int> order = FxDrawOrder();
    for (size_t n = 0; n < order.size(); ++n) {
        const FxStack& stack = gFxStacks[order[n]];
        for (size_t i = 0; i < stack.layers.size(); ++i) {
            const FxLayer& layer = stack.layers[i];
            if (layer.enabled && layer.opacity > 0.0f) DrawFxLayer(layer, stack.target);
        }
    }

    PopBlendFunc();
    gFxAlpha = 1.0f;
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

// Defined with the debug window below, which needs the accessors above.
static void BuildDbgRectOpts();
static void LoadPanelDebug();
static void LoadPanelFx();

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
    // After registration so a restored "mode" takes effect immediately, and after
    // the accessors exist so anything watching the datarefs sees the same values.
    BuildDbgRectOpts();
    LoadPanelDebug();
    PhotonImgui::SetLogger(ImguiLogBridge);
    // Probe builds only, and only until the ImGui backend is signed off in-sim:
    // verbose first-frame logging plus the two GL sanity markers.
    PhotonImgui::SetDiagnostics(true);
    LoadPanelFx();
    Log("panel probe: Plugins > ToLiss Photon > Debug > Panel Probe... drives all "
        "of this from a window, and the settings persist across reloads. "
        "Panel FX (layers)... is the layered compositor.");
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
    DestroyPanelDebugWindow();
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

// ==================== Debug submenu + probe window ===========================
// The probe has six live knobs and they were driven by typing dataref paths into
// DataRefEditor — then retyping all six after every sim reload, before any
// actual looking could start. That is friction in the measurement loop, which is
// the one place this project cannot afford it: every wrong conclusion in
// docs/fcu_tint_plan.md's history came from reasoning instead of looking.
//
// So: a Debug submenu, a window of buttons, and a small prefs file so a session
// resumes where the last one stopped.
//
// PROBE-ONLY, structurally. All of it is inside #if PHOTON_PANEL_PROBE, and the
// menu code calls AppendDebugMenu unconditionally because the shipping build
// gets the no-op declared up there. A release therefore has no Debug row, no
// window, and never writes the prefs file.
//
// Layout differs from the Custom windows on purpose: those use a fixed column
// GRID because their rows are parallel choices of the same shape, whereas these
// rows have 2 to 9 buttons of wildly different widths and a grid would leave
// most of the window empty. Buttons here sit at natural width, left to right.
// The button drawing itself (DrawButton, the press/hover states, the • prefix)
// is shared, so the two windows cannot drift apart visually.

static const int kDbgMaxBtns = 12;   // the Rect row is the widest: 2 + 7 rects

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

// Tint nudge sizes. 0.005 is finer than the eye resolves on a single step, which
// is the point: a colour is judged by walking it, not by jumping to it.
static const float kDbgSteps[] = { 0.005f, 0.01f, 0.05f };
static const DbgOpt kDbgStepOpts[] = { {0, "0.005"}, {1, "0.01"}, {2, "0.05"} };
static int gDbgStepIdx = 1;

// The Rect row is built from the live rect table rather than written out, so
// re-deriving kPanelRectsHi cannot leave the window offering a name that no
// longer exists — or, worse, quietly omitting a new one.
static DbgOpt gDbgRectOpts[2 + 32];
static int    gDbgRectOptCount = 0;
static void BuildDbgRectOpts() {
    const int cap = (int)(sizeof(gDbgRectOpts) / sizeof(gDbgRectOpts[0]));
    gDbgRectOptCount = 0;
    gDbgRectOpts[gDbgRectOptCount++] = { kProbeFloodRect, "Flood" };
    gDbgRectOpts[gDbgRectOptCount++] = { -1, "All" };
    // Groups first: they are the useful targets, and a row of nine buttons is
    // already long enough that the interesting one should not be last.
    for (int g = 0; g < kPanelGroupCount && gDbgRectOptCount < cap; ++g)
        gDbgRectOpts[gDbgRectOptCount++] = { kPanelRectCount + g, kPanelGroups[g].name };
    for (int i = 0; i < kPanelRectCount && gDbgRectOptCount < cap; ++i)
        gDbgRectOpts[gDbgRectOptCount++] = { i, kPanelRectsHi[i].name };
}

enum {
    kDbgRowMode = 0, kDbgRowPass, kDbgRowDraw, kDbgRowRect,
    kDbgRowTintR, kDbgRowTintG, kDbgRowTintB,
    kDbgRowStep, kDbgRowActions,
    kDbgRowCount
};
static const char* kDbgRowLabel[kDbgRowCount] = {
    "Probe", "Pass", "Draw", "Rect", "Tint R", "Tint G", "Tint B", "Step", "",
};

// Action-row button ids.
enum { kDbgActFcu = 0, kDbgActResetTint, kDbgActRelog };

struct DbgBtn { std::string label; int value; bool selected; };

static XPLMWindowID gDbgWinId = nullptr;
static int gDbgPressRow = -1, gDbgPressBtn = -1;
static int gDbgFontH = 0, gDbgBtnH = 0, gDbgRowStride = 0, gDbgLabelW = 0;
static int gDbgHeaderH = 0, gDbgValueW = 0;

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
      << "step " << gDbgStepIdx           << "\n"
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
        else if (key == "step" && (f >> i))
            gDbgStepIdx = (i < 0 || i >= (int)(sizeof(kDbgSteps) / sizeof(kDbgSteps[0]))) ? 1 : i;
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

// ---- rows ------------------------------------------------------------------
static int DbgRowButtons(int row, DbgBtn out[kDbgMaxBtns]) {
    const DbgOpt* opts = nullptr;
    int n = 0, cur = 0;
    switch (row) {
        case kDbgRowMode: opts = kDbgModeOpts; n = 4; cur = gPanelProbeMode; break;
        case kDbgRowPass: opts = kDbgPassOpts; n = 3; cur = gPanelProbeType; break;
        case kDbgRowDraw: opts = kDbgDrawOpts; n = 4; cur = gPanelProbeDrawMethod; break;
        case kDbgRowRect: opts = gDbgRectOpts; n = gDbgRectOptCount; cur = gPanelProbeRect; break;
        case kDbgRowStep: opts = kDbgStepOpts; n = 3; cur = gDbgStepIdx; break;
        // Steppers carry the DIRECTION as their value; the magnitude comes from
        // the Step row, so changing granularity does not touch these.
        case kDbgRowTintR: case kDbgRowTintG: case kDbgRowTintB:
            out[0] = { "-", -1, false };
            out[1] = { "+",  1, false };
            return 2;
        case kDbgRowActions:
            out[0] = { "FCU preset",   kDbgActFcu,       false };
            out[1] = { "Reset tint",   kDbgActResetTint, false };
            out[2] = { "Re-log state", kDbgActRelog,     false };
            return 3;
        default: return 0;
    }
    if (n > kDbgMaxBtns) n = kDbgMaxBtns;
    for (int i = 0; i < n; ++i)
        out[i] = { opts[i].label, opts[i].value, opts[i].value == cur };
    return n;
}

static std::string DbgRowValue(int row) {
    if (row < kDbgRowTintR || row > kDbgRowTintB) return "";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", gPanelTint[row - kDbgRowTintR]);
    return buf;
}

// Width ALWAYS includes room for the "• " prefix, selected or not. Measuring the
// live label instead would make every button resize as it is clicked, and the
// row would shuffle under the cursor.
static int DbgBtnWidth(const std::string& label) {
    return MeasureText(std::string(kBullet) + label) + 2 * kBtnPadX;
}

static int DbgRowRects(int winL, int winT, int row, int& labelX, int& baseline,
                       DbgBtn btn[kDbgMaxBtns], int bl[kDbgMaxBtns], int br[kDbgMaxBtns],
                       int& top, int& bottom) {
    labelX  = winL + kPadX;
    top     = winT - kTopInset - gDbgHeaderH - row * gDbgRowStride;
    bottom  = top - gDbgBtnH;
    int n   = DbgRowButtons(row, btn);
    int x   = labelX + gDbgLabelW + kGap;
    for (int i = 0; i < n; ++i) {
        bl[i] = x;
        br[i] = x + DbgBtnWidth(btn[i].label);
        x = br[i] + kGap;
    }
    baseline = bottom + (gDbgBtnH - gDbgFontH) / 2 + 1;
    return n;
}

static void DbgButtonAt(int winL, int winT, int x, int y, int& outRow, int& outBtn) {
    outRow = -1; outBtn = -1;
    for (int i = 0; i < kDbgRowCount; ++i) {
        int labelX, baseline, bl[kDbgMaxBtns], br[kDbgMaxBtns], top, bottom;
        DbgBtn btn[kDbgMaxBtns];
        int n = DbgRowRects(winL, winT, i, labelX, baseline, btn, bl, br, top, bottom);
        if (y < bottom || y > top) continue;
        for (int c = 0; c < n; ++c)
            if (x >= bl[c] && x <= br[c]) { outRow = i; outBtn = c; return; }
    }
}

// Plain-English restatement of the six knobs, so the window answers "what is on
// screen right now" without the reader recomposing it from four selected
// buttons. Reading this back is how a mis-set knob gets caught before it is
// mistaken for a finding.
static std::string DbgSummary() {
    if (gPanelProbeMode == kProbeOff) return "Probe OFF — nothing is drawn.";
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

static void DrawDebugWindow(XPLMWindowID win, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);
    int mx = 0, my = 0;
    XPLMGetMouseLocationGlobal(&mx, &my);

    float header[3] = {0.86f, 0.86f, 0.86f};
    float white[3]  = {1.0f, 1.0f, 1.0f};

    const int line1 = t - kTopInset - kHeaderTopGap - gDbgFontH;
    const int line2 = line1 - gDbgFontH - 4;
    const std::string summary = DbgSummary();
    XPLMDrawString(header, l + kPadX, line1, summary.c_str(),
                   nullptr, xplmFont_Proportional);

    // The tint triple drawn IN the tint colour — a swatch that cannot fail to
    // render, since it is the numbers themselves. The block glyphs are a bonus
    // if the font carries U+2588 and cost nothing if it does not.
    char swatch[80];
    std::snprintf(swatch, sizeof(swatch),
                  "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88  multiply %.3f %.3f %.3f",
                  gPanelTint[0], gPanelTint[1], gPanelTint[2]);
    float tint[3] = { gPanelTint[0], gPanelTint[1], gPanelTint[2] };
    XPLMDrawString(tint, l + kPadX, line2, swatch, nullptr, xplmFont_Proportional);

    for (int i = 0; i < kDbgRowCount; ++i) {
        int labelX, baseline, bl[kDbgMaxBtns], br[kDbgMaxBtns], top, bottom;
        DbgBtn btn[kDbgMaxBtns];
        int n = DbgRowRects(l, t, i, labelX, baseline, btn, bl, br, top, bottom);
        if (kDbgRowLabel[i][0])
            XPLMDrawString(white, labelX, baseline, kDbgRowLabel[i],
                           nullptr, xplmFont_Proportional);
        for (int c = 0; c < n; ++c) {
            std::string text = (btn[c].selected ? kBullet : "") + btn[c].label;
            bool over  = mx >= bl[c] && mx <= br[c] && my >= bottom && my <= top;
            bool press = over && gDbgPressRow == i && gDbgPressBtn == c;
            DrawButton(bl[c], bottom, br[c], top, baseline,
                       btn[c].selected, over && !press, press, text);
        }
        std::string val = DbgRowValue(i);
        if (!val.empty() && n > 0)
            XPLMDrawString(white, br[n - 1] + kGap, baseline, val.c_str(),
                           nullptr, xplmFont_Proportional);
        if (i != kDbgRowCount - 1) {
            int dy = bottom - (gDbgRowStride - gDbgBtnH) / 2;
            DrawDivider(l + kPadX, r - kPadX, dy, gDbgFontH);
        }
    }
}

static void DbgActivate(int row, int idx) {
    DbgBtn btn[kDbgMaxBtns];
    const int n = DbgRowButtons(row, btn);
    if (idx < 0 || idx >= n) return;
    const int v = btn[idx].value;
    switch (row) {
        case kDbgRowMode: WritePanelProbeMode(nullptr, v); break;
        case kDbgRowPass: WritePanelProbeType(nullptr, v); break;
        case kDbgRowDraw: WritePanelProbeDraw(nullptr, v); break;
        case kDbgRowRect: WritePanelProbeRect(nullptr, v); break;
        case kDbgRowStep: gDbgStepIdx = v; break;
        case kDbgRowTintR: case kDbgRowTintG: case kDbgRowTintB: {
            const int ch = row - kDbgRowTintR;
            WritePanelTint((void*)(intptr_t)ch,
                           gPanelTint[ch] + (float)v * kDbgSteps[gDbgStepIdx]);
            break;
        }
        case kDbgRowActions:
            if (v == kDbgActFcu) ApplyFcuPreset();
            else if (v == kDbgActResetTint)
                for (int i = 0; i < 3; ++i)
                    WritePanelTint((void*)(intptr_t)i, kPanelTintDefault[i]);
            else {
                // Force the next frame of each pass to re-dump the GL state, the
                // way a mode/rect change does. Without this the only way to see a
                // fresh dump is to toggle a knob and toggle it back.
                gPanelStateLoggedMask = gPanelErrLoggedMask = 0;
                Log("panel probe: GL state + glGetError will be re-logged next frame");
            }
            break;
        default: return;
    }
    SavePanelDebug();
}

static int HandleDebugClick(XPLMWindowID win, int x, int y, XPLMMouseStatus status, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);
    int row = -1, idx = -1;
    DbgButtonAt(l, t, x, y, row, idx);
    if (status == xplm_MouseDown) {
        gDbgPressRow = row; gDbgPressBtn = idx;
    } else if (status == xplm_MouseUp) {
        if (row >= 0 && row == gDbgPressRow && idx == gDbgPressBtn) DbgActivate(row, idx);
        gDbgPressRow = -1; gDbgPressBtn = -1;
    }
    return 1;
}

static void CreatePanelDebugWindow() {
    if (gDbgWinId) return;
    int fw = 0, fd = 0;
    XPLMGetFontDimensions(xplmFont_Proportional, &fw, &gDbgFontH, &fd);
    gDbgBtnH      = gDbgFontH + 12;
    gDbgRowStride = gDbgBtnH + 14;
    gDbgHeaderH   = kHeaderTopGap + 2 * gDbgFontH + 4 + kHeaderBotGap;   // two header lines

    gDbgLabelW = 0;
    for (int i = 0; i < kDbgRowCount; ++i)
        gDbgLabelW = std::max(gDbgLabelW, MeasureText(kDbgRowLabel[i]));
    gDbgLabelW += kLabelPad;

    gDbgValueW = MeasureText("0.000") + kGap;

    // Width is set by the widest ROW, not by a shared grid, plus enough room for
    // the header lines — the summary line is comfortably the longest string here.
    int bodyW = 0;
    for (int i = 0; i < kDbgRowCount; ++i) {
        DbgBtn btn[kDbgMaxBtns];
        int n = DbgRowButtons(i, btn), w = 0;
        for (int c = 0; c < n; ++c) w += DbgBtnWidth(btn[c].label) + (c ? kGap : 0);
        if (!DbgRowValue(i).empty()) w += kGap + gDbgValueW;
        bodyW = std::max(bodyW, w);
    }
    // A worst-case summary rather than the current one: the window is fixed-size,
    // and sizing it to whatever happened to be selected at creation would clip
    // the text on the next click.
    int headerW = MeasureText("Drawing before Gauges (ToLiss paints over this)  |  "
                              "pass 2 LIT  |  Immediate  |  FLOOD (whole panel)");
    int width  = kPadX + gDbgLabelW + kGap + std::max(bodyW, headerW - gDbgLabelW - kGap) + kPadX;
    int height = kTopInset + gDbgHeaderH + gDbgRowStride * (kDbgRowCount - 1)
               + gDbgBtnH + kPadX;

    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    int left = (sl + sr) / 2 - width / 2;
    int top  = (st + sb) / 2 + height / 2;

    XPLMCreateWindow_t p;
    std::memset(&p, 0, sizeof(p));
    p.structSize = sizeof(p);
    p.left = left;  p.top = top;  p.right = left + width;  p.bottom = top - height;
    p.visible = 1;
    p.drawWindowFunc       = DrawDebugWindow;
    p.handleMouseClickFunc = HandleDebugClick;
    p.handleKeyFunc        = HandleCustomKey;
    p.handleCursorFunc     = HandleCustomCursor;
    p.handleMouseWheelFunc = HandleCustomWheel;
    p.refcon               = nullptr;
    p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
    p.layer                = xplm_WindowLayerFloatingWindows;
    p.handleRightClickFunc = HandleCustomRightClick;
    gDbgWinId = XPLMCreateWindowEx(&p);
    XPLMSetWindowTitle(gDbgWinId, "ToLiss Photon - Panel Probe (DEBUG)");
    XPLMSetWindowPositioningMode(gDbgWinId, xplm_WindowPositionFree, -1);
    XPLMSetWindowResizingLimits(gDbgWinId, width, height, width, height);
    Log("panel probe: debug window built");
}

static void ShowPanelDebugWindow() {
    if (!gDbgWinId) CreatePanelDebugWindow();
    if (gDbgWinId) { XPLMSetWindowIsVisible(gDbgWinId, 1); XPLMBringWindowToFront(gDbgWinId); }
}

static void DestroyPanelDebugWindow() {
    if (gDbgWinId) { XPLMDestroyWindow(gDbgWinId); gDbgWinId = nullptr; }
}

// ====================== panel FX: persistence and editor ======================

// Its own file, like the probe's. This is tuning state for a feature that does
// not ship yet, so it has no business in a user's per-livery profile.
static fs::path PanelFxFilePath() {
    char root[512] = {0};
    XPLMGetSystemPath(root);
    return fs::path(root) / "Output" / "preferences" / "ToLissPhoton_panelfx.txt";
}

static int FxBlendFromName(const std::string& name) {
    for (int i = 0; i < kFxBlendCount; ++i)
        if (name == kFxBlendName[i]) return i;
    return kFxMultiply;
}

// Targets are saved by NAME, never by index: the rect table is append-only but
// groups are not, and a stack that silently retargets itself after an edit to
// the table is exactly the failure this project keeps writing tripwires about.
static void SavePanelFx() {
    std::ofstream f(PanelFxFilePath(), std::ios::binary);
    if (!f) { Log("panel fx: could not write " + PanelFxFilePath().string()); return; }

    f << "# ToLiss Photon panel FX layers — generated, safe to delete.\n";
    f << "# target <name> / layer <blend> <enabled> <r> <g> <b> <opacity>\n";
    f << "enabled " << (gFxEnabled ? 1 : 0) << "\n";
    for (size_t s = 0; s < gFxStacks.size(); ++s) {
        const FxStack& stack = gFxStacks[s];
        f << "target " << PanelTargetName(stack.target) << "\n";
        for (size_t i = 0; i < stack.layers.size(); ++i) {
            const FxLayer& l = stack.layers[i];
            char buf[160];
            std::snprintf(buf, sizeof(buf), "layer %s %d %.4f %.4f %.4f %.4f\n",
                          kFxBlendName[l.blend], l.enabled ? 1 : 0,
                          l.color[0], l.color[1], l.color[2], l.opacity);
            f << buf;
        }
    }
    Log("panel fx: saved " + std::to_string(gFxStacks.size()) + " stack(s) to "
        + PanelFxFilePath().string());
}

// Resolve a saved name back to a target index. Rects and groups share the space,
// so one lookup covers both.
static int PanelTargetByName(const std::string& name) {
    const int r = RectIndexByName(name.c_str());
    if (r >= 0) return r;
    return GroupTargetByName(name.c_str());
}

static void LoadPanelFx() {
    std::ifstream f(PanelFxFilePath(), std::ios::binary);
    if (!f) return;                       // no file yet is the normal first run

    gFxStacks.clear();
    std::string line;
    int dropped = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream in(line);
        std::string key;
        in >> key;

        if (key == "enabled") {
            int v = 0; in >> v; gFxEnabled = (v != 0);
        } else if (key == "target") {
            std::string name;
            std::getline(in, name);
            if (!name.empty() && name[0] == ' ') name.erase(0, 1);
            const int t = PanelTargetByName(name);
            if (t < 0) { ++dropped; Log("panel fx: unknown target '" + name + "' — dropped"); }
            else { FxStack s; s.target = t; gFxStacks.push_back(s); }
        } else if (key == "layer") {
            if (gFxStacks.empty()) continue;      // a layer before any target
            std::string blend; int on = 1;
            FxLayer l;
            in >> blend >> on >> l.color[0] >> l.color[1] >> l.color[2] >> l.opacity;
            l.blend   = FxBlendFromName(blend);
            l.enabled = (on != 0);
            gFxStacks.back().layers.push_back(l);
        }
    }
    Log("panel fx: restored " + std::to_string(gFxStacks.size()) + " stack(s)"
        + (dropped ? " (" + std::to_string(dropped) + " dropped)" : "")
        + " from " + PanelFxFilePath().string()
        + (gFxEnabled ? " — ENABLED, the cockpit may already be tinted" : " — disabled"));
}

// ---- the editor ------------------------------------------------------------

static ImguiWindow* gFxWindow = nullptr;

// ⚠ The selection is a TARGET, not an index into gFxStacks. Deleting a target
// shuffles that vector, so a stored index silently re-points the layer pane at
// a different target's layers — the editing equivalent of the retargeting bug
// the save format's name lookup exists to prevent.
static int gFxSelectedTarget = -1;

static int FxStackIndex(int target) {
    if (target < 0) return -1;
    for (size_t s = 0; s < gFxStacks.size(); ++s)
        if (gFxStacks[s].target == target) return (int)s;
    return -1;
}

static void FxDeleteTarget(int target) {
    const int s = FxStackIndex(target);
    if (s < 0) return;
    gFxStacks.erase(gFxStacks.begin() + s);
    if (gFxSelectedTarget == target) gFxSelectedTarget = -1;
    SavePanelFx();
}

// Dump the whole stack to Log.txt in the save-file syntax. The tuning loop ends
// with numbers that have to reach source, and reconstructing them from memory
// after a session is how a good look gets lost — same reason every tint write
// logs the whole triple.
static void LogPanelFx() {
    Log("panel fx: --- current stacks, in composite order ---");
    const std::vector<int> order = FxDrawOrder();
    for (size_t n = 0; n < order.size(); ++n) {
        const size_t s = (size_t)order[n];
        Log(std::string("panel fx: target ") + PanelTargetName(gFxStacks[s].target));
        for (size_t i = 0; i < gFxStacks[s].layers.size(); ++i) {
            const FxLayer& l = gFxStacks[s].layers[i];
            char buf[160];
            std::snprintf(buf, sizeof(buf), "panel fx:   layer %s %d %.4f %.4f %.4f %.4f",
                          kFxBlendName[l.blend], l.enabled ? 1 : 0,
                          l.color[0], l.color[1], l.color[2], l.opacity);
            Log(buf);
        }
    }
}

// One row of the target tree, plus its children. Every target is always listed,
// whether or not it carries layers — the old pane split them into a "current"
// list and an "Add" list, which put a group and its own members in different
// places and lost the one thing the tree is for. A target with no layers is
// drawn dimmed and clicking it starts a stack, so adding and selecting are the
// same gesture.
static void BuildFxTargetNode(int t, int* pendingDelete) {
    const int  s       = FxStackIndex(t);
    const bool isGroup = (t >= kPanelRectCount);
    const bool hasStack = (s >= 0);

    char label[160];
    if (hasStack)
        std::snprintf(label, sizeof(label), "%s  [%d]", PanelTargetName(t),
                      (int)gFxStacks[s].layers.size());
    else
        std::snprintf(label, sizeof(label), "%s", PanelTargetName(t));

    // OpenOnArrow so that clicking the NAME selects rather than collapsing — a
    // group is a target in its own right here, not just a folder.
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_DefaultOpen;
    if (!isGroup)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (hasStack && gFxSelectedTarget == t) flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(t);
    if (!hasStack)
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", label);
    if (!hasStack) ImGui::PopStyleColor();

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        gFxSelectedTarget = t;
        if (!hasStack) {
            FxStack stack;
            stack.target = t;
            stack.layers.push_back(FxLayer());
            gFxStacks.push_back(stack);
            SavePanelFx();
        }
    }
    if (ImGui::IsItemHovered()) {
        if (hasStack) ImGui::SetTooltip("%d rect(s)", PanelTargetBreadth(t));
        else          ImGui::SetTooltip("No layers yet — click to start a stack "
                                        "(%d rect(s))", PanelTargetBreadth(t));
    }

    if (hasStack) {
        // Right-align the delete button. Deliberately NOT GetContentRegionMax(),
        // which lives in ImGui's obsolete block — the whole point of the
        // CmdListsCount episode is that this project should be able to define
        // IMGUI_DISABLE_OBSOLETE_FUNCTIONS one day, so nothing we write may
        // depend on that block.
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                             + ImGui::GetContentRegionAvail().x - 16.0f);
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

static void BuildFxLayerPane() {
    const int si = FxStackIndex(gFxSelectedTarget);
    if (si < 0) {
        ImGui::TextUnformatted("Pick a target on the left.");
        ImGui::TextDisabled("Clicking one that has no layers starts a stack on it.");
        return;
    }
    FxStack& stack = gFxStacks[si];

    ImGui::SeparatorText(PanelTargetName(stack.target));

    // What the hierarchy does, said where it is acted on. A group's layers are
    // already on the pixels by the time a member's layers run, so a member is
    // refining a result rather than replacing one.
    const int parent = PanelTargetParent(stack.target);
    if (parent >= 0 && FxStackIndex(parent) >= 0)
        ImGui::TextDisabled("Composites on top of \"%s\", which is applied first.",
                            PanelTargetName(parent));

    if (ImGui::Button("Add layer")) {
        // Added at the TOP of the list, which is the end of the vector: a new
        // layer goes on top of the look so far, the same as every paint program.
        stack.layers.push_back(FxLayer());
        SavePanelFx();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete target")) {
        FxDeleteTarget(stack.target);
        return;                            // stack is gone; do not touch it below
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(topmost draws last; the number is composite order)");

    int moveFrom = -1, moveTo = -1, remove = -1;
    bool dirty = false;

    // ⚠ Listed TOP-DOWN: row 0 is the LAST layer to composite. The vector stays
    // bottom-first because that is the order the compositor walks it in, and so
    // does the save file; only the display is reversed. Everything below that
    // works in terms of `i` is therefore in composite order, and only the loop
    // bound and the two arrow buttons know about the flip.
    const int layerCount = (int)stack.layers.size();
    for (int row = 0; row < layerCount; ++row) {
        const int i = layerCount - 1 - row;
        FxLayer& layer = stack.layers[i];
        ImGui::PushID(i);
        ImGui::Separator();

        dirty |= ImGui::Checkbox("##on", &layer.enabled);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draw this layer");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        dirty |= ImGui::Combo("##blend", &layer.blend, kFxBlendName, kFxBlendCount);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kFxBlendHelp[layer.blend]);

        ImGui::SameLine();
        dirty |= ImGui::ColorEdit3("##col", layer.color,
                                   ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        dirty |= ImGui::SliderFloat("##opacity", &layer.opacity, 0.0f, 1.0f, "%.3f");
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
        // of its own in between. The number is the composite index (0 draws
        // first), which the header line explains.
        ImGui::Button(grip);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("PHOTON_FX_LAYER", &i, sizeof(int));
            ImGui::Text("layer %d — %s", i, kFxBlendName[layer.blend]);
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
        if (ImGui::SmallButton("x")) remove = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete this layer");

        ImGui::PopID();
    }

    // Structural edits are applied after the loop — mutating the vector while
    // iterating it would invalidate the reference the next iteration reads.
    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo
        && moveFrom < (int)stack.layers.size() && moveTo < (int)stack.layers.size()) {
        FxLayer moved = stack.layers[moveFrom];
        stack.layers.erase(stack.layers.begin() + moveFrom);
        stack.layers.insert(stack.layers.begin() + moveTo, moved);
        dirty = true;
    }
    if (remove >= 0 && remove < (int)stack.layers.size()) {
        stack.layers.erase(stack.layers.begin() + remove);
        dirty = true;
    }
    // ⚠ Save on any change, not on a Save button. The point of this tool is that
    // a look survives the sim restart that in-sim tuning requires; a tuning pass
    // lost to a forgotten click is the exact friction it exists to remove.
    if (dirty) SavePanelFx();
}

static void BuildFxUi() {
    ImGui::Checkbox("Panel FX enabled", &gFxEnabled);
    if (ImGui::IsItemDeactivatedAfterEdit()) SavePanelFx();
    ImGui::SameLine();
    if (ImGui::Button("Log stack")) LogPanelFx();
    ImGui::SameLine();
    if (ImGui::Button("Reload file")) LoadPanelFx();
    ImGui::SameLine();
    ImGui::TextDisabled("lit pass, after Gauges — A320 only");

    ImGui::TextDisabled("Broader targets composite first, so a group is a base and "
                        "its members refine it.");

    ImGui::Separator();

    // Wider than the flat list needed: the tree spends width on indentation, and
    // each row carries a layer count and a delete button.
    ImGui::BeginChild("fx_targets", ImVec2(250.0f, 0.0f), ImGuiChildFlags_Borders);
    BuildFxTargetPane();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("fx_layers", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    BuildFxLayerPane();
    ImGui::EndChild();
}

static void ShowPanelFxWindow() {
    if (!gFxWindow) {
        gFxWindow = new ImguiWindow(920, 560, "ToLiss Photon - Panel FX (DEBUG)");
        if (!gFxWindow->Ok()) {
            Log("panel fx: could not create the editor window");
            delete gFxWindow;
            gFxWindow = nullptr;
            return;
        }
        gFxWindow->BuildUi = BuildFxUi;
    }
    gFxWindow->BringToFront();
}

static void DestroyPanelFxWindow() {
    delete gFxWindow;
    gFxWindow = nullptr;
}

// The ImGui demo, as a smoke test. If it draws, responds to the mouse, scrolls
// and accepts typing, the backend in imgui_xplm.cpp is correct — which is a far
// better first question than "why does my tool window look wrong". It also
// happens to be the fastest widget reference there is.
static ImguiWindow* gImguiDemoWindow = nullptr;

static void ShowImguiDemoWindow() {
    if (!gImguiDemoWindow) {
        gImguiDemoWindow = new ImguiWindow(900, 640, "Dear ImGui Demo (Photon backend test)");
        if (!gImguiDemoWindow->Ok()) {
            Log("imgui: could not create the demo window");
            delete gImguiDemoWindow;
            gImguiDemoWindow = nullptr;
            return;
        }
        // The demo draws its own window; ours is the container, so let it fill.
        gImguiDemoWindow->BuildUi = [] {
            ImGui::TextUnformatted("Backend smoke test. Everything below is stock ImGui.");
            ImGui::Separator();
            ImGui::ShowDemoWindow();
        };
    }
    gImguiDemoWindow->BringToFront();
}

static void DestroyImguiDemoWindow() {
    delete gImguiDemoWindow;
    gImguiDemoWindow = nullptr;
}

// Both ImGui windows, for the two teardown paths (menu rebuild and plugin stop).
static void DestroyDebugImguiWindows() {
    DestroyPanelFxWindow();
    DestroyImguiDemoWindow();
}

// ---- the Debug submenu -----------------------------------------------------
// Level-2 nesting, same as Exterior / Cockpit: the no-submenu rule in CLAUDE.md
// is an XPPython3 defect and this is native C++. See CreatePhotonMenu's note.
static XPLMMenuID gDbgMenuID = nullptr;
enum { kDbgMenuWindow = 0, kDbgMenuFx, kDbgMenuDemo, kDbgMenuFcu, kDbgMenuOff };

static void DebugMenuHandler(void*, void* itemRef) {
    switch ((intptr_t)itemRef) {
        case kDbgMenuWindow: ShowPanelDebugWindow(); break;
        case kDbgMenuFx:     ShowPanelFxWindow(); break;
        case kDbgMenuDemo:   ShowImguiDemoWindow(); break;
        case kDbgMenuFcu:    ApplyFcuPreset(); ShowPanelDebugWindow(); SavePanelDebug(); break;
        case kDbgMenuOff:    WritePanelProbeMode(nullptr, kProbeOff); SavePanelDebug(); break;
        default: break;
    }
}

static void AppendDebugMenu(XPLMMenuID parent) {
    int item = XPLMAppendMenuItem(parent, "Debug", nullptr, 0);
    gDbgMenuID = XPLMCreateMenu("Debug", parent, item, DebugMenuHandler, nullptr);
    if (!gDbgMenuID) return;
    XPLMAppendMenuItem(gDbgMenuID, "Panel FX (layers)...", (void*)(intptr_t)kDbgMenuFx, 0);
    XPLMAppendMenuItem(gDbgMenuID, "Panel Probe...",       (void*)(intptr_t)kDbgMenuWindow, 0);
    XPLMAppendMenuSeparator(gDbgMenuID);
    // The two states worth reaching without reading anything: set up the tuning
    // pass, or stop painting over the cockpit.
    XPLMAppendMenuItem(gDbgMenuID, "FCU tint preset", (void*)(intptr_t)kDbgMenuFcu, 0);
    XPLMAppendMenuItem(gDbgMenuID, "Probe off",       (void*)(intptr_t)kDbgMenuOff, 0);
    XPLMAppendMenuSeparator(gDbgMenuID);
    XPLMAppendMenuItem(gDbgMenuID, "Dear ImGui demo (backend test)",
                       (void*)(intptr_t)kDbgMenuDemo, 0);
}

static void DestroyDebugMenu() {
    if (gDbgMenuID) { XPLMDestroyMenu(gDbgMenuID); gDbgMenuID = nullptr; }
}

#else   // probe compiled out — the shipping build
static void StartPanelProbe()   {}
static void StopPanelProbe()    {}
static void ReseatPanelProbe()  {}
#endif  // PHOTON_PANEL_PROBE

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
    StartPanelProbe();          // no-op unless built with -DPHOTON_PANEL_PROBE=ON
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
