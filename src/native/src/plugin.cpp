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

static XPLMDataRef gCategoryRefs[NCAT] = {nullptr};
static XPLMDataRef gIntCategoryRefs[NINT] = {nullptr};
static XPLMDataRef gSpillMapRef = nullptr;
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
    // Destroy the children before the parent: XPLMDestroyMenu on the parent
    // invalidates the submenu IDs it owns.
    if (gExtMenuID) { XPLMDestroyMenu(gExtMenuID); gExtMenuID = nullptr; }
    if (gIntMenuID) { XPLMDestroyMenu(gIntMenuID); gIntMenuID = nullptr; }
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
    } else {
        if (gMenuCreated) DestroyPhotonMenu();
        gInteriorInstalled = false;
        gGlowMapActive = false;
        gMapRheostat = nullptr;
        gMapRheostatType = 0;
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
    UpdateMenuVisibility();
    LoadProfileForCurrentLivery();
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    if (gAutoLoopRegistered) {
        XPLMUnregisterFlightLoopCallback(AutoLoop, nullptr);
        gAutoLoopRegistered = false;
    }
    StopEngine();
}

PLUGIN_API void XPluginStop(void) {
    StopEngine();
    DestroyPhotonMenu();
    if (gIsLedRef)     { XPLMUnregisterDataAccessor(gIsLedRef);     gIsLedRef = nullptr; }
    if (gGlowArrayRef) { XPLMUnregisterDataAccessor(gGlowArrayRef); gGlowArrayRef = nullptr; }
    if (gSpillMapRef)  { XPLMUnregisterDataAccessor(gSpillMapRef);  gSpillMapRef = nullptr; }
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
        LoadProfileForCurrentLivery();
    } else if (inMessage == XPLM_MSG_LIVERY_LOADED) {
        LoadProfileForCurrentLivery();
    }
}
