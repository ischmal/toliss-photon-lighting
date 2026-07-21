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

// profiles
enum { PROFILE_AUTO = 0, PROFILE_CLASSIC = 1, PROFILE_HYBRID = 2,
       PROFILE_FULL_LED = 3, PROFILE_CUSTOM = 4 };
struct ProfileItem { const char* label; int value; };
static const ProfileItem kProfileItems[] = {   // menu order; separator before Custom
    {"Auto",       PROFILE_AUTO},
    {"Classic",    PROFILE_CLASSIC},
    {"Hybrid LED", PROFILE_HYBRID},
    {"Full LED",   PROFILE_FULL_LED},
    {"Custom...",  PROFILE_CUSTOM},
};
static const int NPROFILE = (int)(sizeof(kProfileItems) / sizeof(kProfileItems[0]));

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
    if (led) { p.duration = 0.10; p.shape = Shape::Square; }
    else     { p.duration = 0.06; p.hold = 0.40; p.shape = Shape::Decay; p.tau = 0.30; }
    return {p};
}

static std::vector<Pulse> StrobePulses(bool led, int index) {
    auto sq = [](double at, double dur) { Pulse p; p.at = at; p.duration = dur; p.shape = Shape::Square; return p; };
    if (led) {
        switch (index) {
            case 0: case 1: return {sq(0.00, 0.05), sq(0.15, 0.05)};   // double wingtips
            case 2:         return {sq(0.00, 0.10)};                    // single tail
            case 3:         return {sq(0.00, 0.08)};                    // single tail
        }
    } else {
        switch (index) {
            case 0: case 1: return {sq(0.00, 0.04), sq(0.12, 0.04)};
            case 2: { Pulse p; p.duration = 0.06; p.hold = 0.40; p.shape = Shape::Decay; p.tau = 0.30; return {p}; }
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

static XPLMDataRef gCategoryRefs[NCAT] = {nullptr};
static XPLMDataRef gIsLedRef = nullptr, gBeaconDbgRef = nullptr, gStrobeDbgRef = nullptr;
static XPLMDataRef gLiveryIndexRef = nullptr;

// persistence
struct Entry { int profile = PROFILE_AUTO; bool hasCats = false; int cats[NCAT] = {0}; };
static std::map<std::string, Entry> gProfiles;   // liveryKey -> entry

// menu
static bool       gMenuCreated = false;
static XPLMMenuID gMenuID = nullptr;
static int        gPluginsMenuItem = -1;
static int        gProfileItemIndex[NPROFILE];

// Custom window (modern XPLMCreateWindowEx UI)
static XPLMWindowID gWindowId = nullptr;

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

// forward decls
static void UpdateChecks();
static void ApplyProfileToValues();
static void LoadProfileForCurrentLivery();
static void UpdateMenuVisibility();

// ============================= dataref accessors =============================
static int   ReadCategoryInt(void* refcon)   { return gValues[(int)(intptr_t)refcon]; }
static float ReadCategoryFloat(void* refcon) { return (float)gValues[(int)(intptr_t)refcon]; }

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

// Read any numeric dataref as a double, regardless of how it is typed. Per
// XPLMDataAccess.h: "if there is a type mismatch, the functions that read data
// will return 0" — so XPLMGetDataf on an INT-typed dataref (e.g. ToLiss's
// AirbusFBW/LandingLightLocation) silently yields 0.0 and never trips a gate.
// This is gotcha #4 in reverse: there the OBJ read our int-backed dataref as
// float; here we read someone else's dataref without knowing its type. So detect
// the real type and use the matching getter. Returns false when the path is
// empty/unset, the dataref is missing, or it is non-numeric (byte/string data).
static bool ReadDataRefDouble(const char* path, double& out) {
    if (!path || !*path) return false;
    XPLMDataRef ref = XPLMFindDataRef(path);
    if (!ref) return false;
    XPLMDataTypeID types = XPLMGetDataRefTypes(ref);
    if (types & xplmType_Int)        { out = (double)XPLMGetDatai(ref); return true; }
    if (types & xplmType_Float)      { out = (double)XPLMGetDataf(ref); return true; }
    if (types & xplmType_Double)     { out = XPLMGetDatad(ref); return true; }
    // arrays: read element [0] (a scalar flag is the expected shape here)
    if (types & xplmType_IntArray)   { int v = 0;   if (XPLMGetDatavi(ref, &v, 0, 1) >= 1) { out = (double)v; return true; } }
    if (types & xplmType_FloatArray) { float v = 0; if (XPLMGetDatavf(ref, &v, 0, 1) >= 1) { out = (double)v; return true; } }
    return false;
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
        ApplyProfileToValues();
    } else {
        gProfile = it->second.profile;
        if (gProfile == PROFILE_CUSTOM && it->second.hasCats) {
            for (int i = 0; i < NCAT; ++i) gValues[i] = it->second.cats[i] == 1 ? 1 : 0;
        } else {
            ApplyProfileToValues();
        }
    }
    UpdateChecks();
}

static void SaveNamed(int profile) {
    Entry e; e.profile = profile; e.hasCats = false;
    gProfiles[CurrentLiveryKey()] = e;
    SaveProfilesFile();
}

static void SaveCustom() {
    Entry e; e.profile = PROFILE_CUSTOM; e.hasCats = true;
    for (int i = 0; i < NCAT; ++i) e.cats[i] = gValues[i];
    gProfiles[CurrentLiveryKey()] = e;
    SaveProfilesFile();
}

static void ClearSaved() {
    auto it = gProfiles.find(CurrentLiveryKey());
    if (it != gProfiles.end()) { gProfiles.erase(it); SaveProfilesFile(); }
}

// ===================== Custom config window (modern UI) ======================
// XPLMCreateWindowEx floating window (XPLM300+): HiDPI-correct, standard X-Plane
// chrome + close box, pop-out / VR capable. Replaces the legacy XPWidgets version.
// Two buttons per row: the classic look for that light (Halogen or Xenon) and
// LED. Each button is an XPLMDrawTranslucentDarkBox (renders on every backend,
// Vulkan/Metal included — unlike raw OpenGL, which silently no-ops there) with the
// state shown by the TEXT: active = blue + "•", hover brightens it, press stacks a
// second (darker) box. State is read live from gValues — nothing to sync on show.
// Still a plain window (not a menu), so CreatePhotonMenu's level-2 crash note holds.

static const char* kWinHeader = "Select the look for each exterior light:";
static const int kPadX = 4, kGap = 8, kTopInset = 20, kBtnPadX = 10, kLabelPad = 4;
static const int kHeaderTopGap = 4, kHeaderBotGap = 16;   // header spacing above / below
static const char* kBullet = "\xE2\x80\xA2 ";   // U+2022 "• " prefix on the active button

// computed once at window creation, then stable for the window's life
static int gFontH = 0, gBtnH = 0, gRowStride = 0, gHeaderH = 0;
static int gLabelColW = 0, gClassicBtnW = 0, gLedBtnW = 0;

// the button the mouse is currently holding down (-1 = none); col 0 = classic, 1 = LED
static int gPressRow = -1, gPressCol = -1;

// Per-row geometry from the live window top-left — shared by draw + hit-test so a
// click always lands on the button it looks like.
static void RowRects(int winL, int winT, int i, int& labelX, int& baseline,
                     int& cL, int& cR, int& lL, int& lR, int& top, int& bottom) {
    labelX = winL + kPadX;
    top    = (winT - kTopInset - gHeaderH) - i * gRowStride;
    bottom = top - gBtnH;
    cL = labelX + gLabelColW + kGap;
    cR = cL + gClassicBtnW;
    lL = cR + kGap;
    lR = lL + gLedBtnW;
    baseline = bottom + (gBtnH - gFontH) / 2 + 1;
}

// which button covers (x,y): row + col (0 classic, 1 LED); row = -1 if none
static void ButtonAt(int winL, int winT, int x, int y, int& outRow, int& outCol) {
    outRow = -1; outCol = -1;
    for (int i = 0; i < NCAT; ++i) {
        int labelX, baseline, cL, cR, lL, lR, top, bottom;
        RowRects(winL, winT, i, labelX, baseline, cL, cR, lL, lR, top, bottom);
        if (y < bottom || y > top) continue;
        if (x >= cL && x <= cR) { outRow = i; outCol = 0; return; }
        if (x >= lL && x <= lR) { outRow = i; outCol = 1; return; }
    }
}

static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static void DrawButton(int l, int b, int r, int t, bool selected, bool hover, bool pressed,
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
    int ty = b + (gBtnH - gFontH) / 2 + 1;
    XPLMDrawString(col, tx, ty, text.c_str(), nullptr, xplmFont_Proportional);
}

// Subtle divider: a dim rule drawn with the box-drawing glyph, so its weight is set
// by color (XPLMDrawTranslucentDarkBox has fixed, heavier opacity we can't dial down).
static void DrawDivider(int l, int r, int yCenter) {
    static const char* dash = "\xE2\x94\x80";   // U+2500  ─
    float dim[3] = {0.32f, 0.32f, 0.34f};
    int gw = (int)XPLMMeasureString(xplmFont_Proportional, dash, (int)std::strlen(dash));
    if (gw < 1) gw = 4;
    int n = (r - l) / gw;
    if (n < 1) n = 1;
    std::string line;
    line.reserve((size_t)n * 3);
    for (int i = 0; i < n; ++i) line += dash;
    XPLMDrawString(dim, l, yCenter - gFontH / 3, line.c_str(), nullptr, xplmFont_Proportional);
}

static void DrawCustomWindow(XPLMWindowID win, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);
    int mx = 0, my = 0;
    XPLMGetMouseLocationGlobal(&mx, &my);   // live cursor pos for hover feedback
    float header[3] = {0.86f, 0.86f, 0.86f};
    float white[3]  = {1.0f, 1.0f, 1.0f};
    // header vertically centered in its band just above the first row
    int firstRowTop = t - kTopInset - gHeaderH;
    XPLMDrawString(header, l + kPadX, firstRowTop + kHeaderBotGap, kWinHeader,
                   nullptr, xplmFont_Proportional);
    for (int i = 0; i < NCAT; ++i) {
        int labelX, baseline, cL, cR, lL, lR, top, bottom;
        RowRects(l, t, i, labelX, baseline, cL, cR, lL, lR, top, bottom);
        XPLMDrawString(white, labelX, baseline, kCategories[i].label, nullptr, xplmFont_Proportional);
        bool classicSel = gValues[i] == 0;
        bool ledSel     = gValues[i] == 1;
        std::string cText = (classicSel ? kBullet : "") + std::string(kCategories[i].classic);
        std::string lText = (ledSel ? kBullet : "") + std::string("LED");

        bool cOver = mx >= cL && mx <= cR && my >= bottom && my <= top;
        bool lOver = mx >= lL && mx <= lR && my >= bottom && my <= top;
        bool cPress = cOver && gPressRow == i && gPressCol == 0;   // held down on it
        bool lPress = lOver && gPressRow == i && gPressCol == 1;
        DrawButton(cL, bottom, cR, top, classicSel, cOver && !cPress, cPress, cText);
        DrawButton(lL, bottom, lR, top, ledSel,     lOver && !lPress, lPress, lText);

        // 1px divider centered in the gap below each row (except after the last)
        if (i < NCAT - 1) {
            int dy = bottom - (gRowStride - gBtnH) / 2;
            DrawDivider(l + kPadX, r - kPadX, dy);
        }
    }
}

// Button behavior: press on mouse-down, act on mouse-up if released over the same
// button — so the press state shows while held and a drag-off cancels the click.
static int HandleCustomClick(XPLMWindowID win, int x, int y, XPLMMouseStatus status, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);
    int row, col;
    ButtonAt(l, t, x, y, row, col);
    if (status == xplm_MouseDown) {
        gPressRow = row; gPressCol = col;
    } else if (status == xplm_MouseUp) {
        if (row >= 0 && row == gPressRow && col == gPressCol && col != gValues[row]) {
            gValues[row] = col;                 // col 0 = classic (Halogen/Xenon), 1 = LED
            gProfile = PROFILE_CUSTOM;
            SaveCustom();
            UpdateChecks();
            Log(std::string("custom: ") + kCategories[row].key + "=" + std::to_string(col));
        }
        gPressRow = -1; gPressCol = -1;
    }
    return 1;   // our window owns clicks within it
}

// XPLMCreateWindowEx requires non-null callbacks for every slot.
static void            HandleCustomKey(XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {}
static XPLMCursorStatus HandleCustomCursor(XPLMWindowID, int, int, void*) { return xplm_CursorDefault; }
static int             HandleCustomWheel(XPLMWindowID, int, int, int, int, void*) { return 0; }
static int             HandleCustomRightClick(XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 1; }

static void CreateCustomWindow() {
    if (gWindowId) return;
    int fw = 0, fd = 0;
    XPLMGetFontDimensions(xplmFont_Proportional, &fw, &gFontH, &fd);
    gBtnH = gFontH + 12;        // row height +4
    gRowStride = gBtnH + 14;    // extra inter-row padding so the divider isn't compressed
    gHeaderH = gFontH + kHeaderTopGap + kHeaderBotGap;   // header text + gaps above/below

    gLabelColW = 0;
    for (int i = 0; i < NCAT; ++i)
        gLabelColW = std::max(gLabelColW, (int)XPLMMeasureString(
            xplmFont_Proportional, kCategories[i].label, (int)std::strlen(kCategories[i].label)));
    gLabelColW += kLabelPad;   // breathing room between the longest label and the button
    int classicTextW = 0;   // widest active-state classic label ("• Halogen" / "• Xenon")
    for (int i = 0; i < NCAT; ++i) {
        std::string s = std::string(kBullet) + kCategories[i].classic;
        classicTextW = std::max(classicTextW,
            (int)XPLMMeasureString(xplmFont_Proportional, s.c_str(), (int)s.size()));
    }
    gClassicBtnW = classicTextW + 2 * kBtnPadX;
    std::string ledActive = std::string(kBullet) + "LED";
    gLedBtnW = (int)XPLMMeasureString(xplmFont_Proportional, ledActive.c_str(), (int)ledActive.size())
               + 2 * kBtnPadX;

    int width  = kPadX + gLabelColW + kGap + gClassicBtnW + kGap + gLedBtnW + kPadX;
    int height = kTopInset + gHeaderH + gRowStride * (NCAT - 1) + gBtnH + kPadX;   // bottom == sides

    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    int left = (sl + sr) / 2 - width / 2;
    int top  = (st + sb) / 2 + height / 2;

    XPLMCreateWindow_t p;
    std::memset(&p, 0, sizeof(p));
    p.structSize = sizeof(p);
    p.left = left;  p.top = top;  p.right = left + width;  p.bottom = top - height;
    p.visible = 1;
    p.drawWindowFunc       = DrawCustomWindow;
    p.handleMouseClickFunc = HandleCustomClick;
    p.handleKeyFunc        = HandleCustomKey;
    p.handleCursorFunc     = HandleCustomCursor;
    p.handleMouseWheelFunc = HandleCustomWheel;
    p.refcon               = nullptr;
    p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
    p.layer                = xplm_WindowLayerFloatingWindows;
    p.handleRightClickFunc = HandleCustomRightClick;
    gWindowId = XPLMCreateWindowEx(&p);
    XPLMSetWindowTitle(gWindowId, "ToLiss Photon - Custom");
    XPLMSetWindowPositioningMode(gWindowId, xplm_WindowPositionFree, -1);
    // fixed size: min == max == the computed size, so the user can't resize it
    XPLMSetWindowResizingLimits(gWindowId, width, height, width, height);
    Log("custom window built (modern)");
}

static void ShowCustomWindow() {
    if (!gWindowId) CreateCustomWindow();
    if (gWindowId) { XPLMSetWindowIsVisible(gWindowId, 1); XPLMBringWindowToFront(gWindowId); }
}

static void DestroyCustomWindow() {
    if (gWindowId) { XPLMDestroyWindow(gWindowId); gWindowId = nullptr; }
}

// ================================== menu =====================================
static void SelectProfile(int profile) {
    gProfile = profile;
    ApplyProfileToValues();
    if (profile == PROFILE_AUTO) ClearSaved();   // Auto is never persisted
    else SaveNamed(profile);
    UpdateChecks();
}

static void MenuHandler(void*, void* itemRef) {
    int i = (int)(intptr_t)itemRef;
    if (i < 0 || i >= NPROFILE) return;
    if (kProfileItems[i].value == PROFILE_CUSTOM) ShowCustomWindow();
    else SelectProfile(kProfileItems[i].value);
}

static void UpdateChecks() {
    if (!gMenuCreated || !gMenuID) return;
    for (int i = 0; i < NPROFILE; ++i) {
        XPLMMenuCheck state = kProfileItems[i].value == gProfile ? xplm_Menu_Checked : xplm_Menu_Unchecked;
        XPLMCheckMenuItem(gMenuID, gProfileItemIndex[i], state);
    }
    // the modern Custom window reads gValues live each frame — nothing to sync
}

static void CreatePhotonMenu() {   // not CreateMenu: collides with the Win32 API
    // Every item lives at menu level 1 (Auto/Classic/Hybrid/Full LED/Custom); none
    // owns a submenu. Custom opens a floating window, NOT a level-2 submenu (which
    // crashes native menu click-dispatch in XPPython3 4.7a1 — kept here for safety
    // and because a window is the verified idiom).
    gPluginsMenuItem = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "ToLiss Photon", nullptr, 0);
    gMenuID = XPLMCreateMenu("ToLiss Photon", XPLMFindPluginsMenu(), gPluginsMenuItem, MenuHandler, nullptr);
    for (int i = 0; i < NPROFILE; ++i) {
        if (kProfileItems[i].value == PROFILE_CUSTOM) XPLMAppendMenuSeparator(gMenuID);
        gProfileItemIndex[i] = XPLMAppendMenuItem(gMenuID, kProfileItems[i].label, (void*)(intptr_t)i, 0);
    }
    gMenuCreated = true;
    UpdateChecks();
    Log("menu built");
}

static void DestroyPhotonMenu() {   // not DestroyMenu: collides with the Win32 API
    DestroyCustomWindow();
    if (gMenuID) { XPLMDestroyMenu(gMenuID); gMenuID = nullptr; }
    if (gPluginsMenuItem >= 0) {
        XPLMRemoveMenuItem(XPLMFindPluginsMenu(), gPluginsMenuItem);
        gPluginsMenuItem = -1;
    }
    gMenuCreated = false;
}

// =========================== ToLiss detection ================================
static bool IsToLiss() {
    char fileName[512] = {0}, path[512] = {0};
    XPLMGetNthAircraftModel(0, fileName, path);
    std::string blob = std::string(path) + "|" + fileName;
    for (char& c : blob) c = (char)std::tolower((unsigned char)c);
    return blob.find("toliss") != std::string::npos;
}

static void UpdateMenuVisibility() {
    if (IsToLiss()) { if (!gMenuCreated) CreatePhotonMenu(); }
    else            { if (gMenuCreated) DestroyPhotonMenu(); }
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
        for (int i = 0; i < n && i < 4; ++i) {
            if (beaconAlwaysOn) out[i] = 1.0f;
            else if (BeaconActive(i, out[i], simTime)) out[i] = (float)ClampBrightness(brightness);
        }
        XPLMSetDatavf(gBeaconRatioRef, out, 0, 4);
    }

    // strobe: per-index waveforms, gated on the strobe switch. Tick every channel
    // every frame (even when gated off) so phase stays correct when it turns on.
    if (gStrobeBuilt && gStrobeRatioRef) {
        float out[4];
        ReadRatios(gStrobeRatioRef, out);
        bool gateOn = StrobeGateOn();
        for (int index = 0; index < 4; ++index) {
            double value = gStrobeChannel[index].Tick(deltaTime);
            if (strobeAlwaysOn) out[index] = 1.0f;
            else if (gateOn)    out[index] = (float)ClampBrightness(value);
        }
        XPLMSetDatavf(gStrobeRatioRef, out, 0, 4);
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
    if (gMenuCreated && gProfile == PROFILE_AUTO) {
        int before[NCAT];
        std::memcpy(before, gValues, sizeof(before));
        ApplyProfileToValues();
        if (std::memcmp(before, gValues, sizeof(before)) != 0) {
            UpdateChecks();
            Log("Auto re-resolved");
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
    gIsLedRef = XPLMRegisterDataAccessor(
        kIsLedDataref, xplmType_Int | xplmType_Float, 0,
        ReadIsLedInt, nullptr, ReadIsLedFloat, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
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
    Log("started; registered " + std::to_string(NCAT) + " category datarefs");
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
    if (gBeaconDbgRef) { XPLMUnregisterDataAccessor(gBeaconDbgRef); gBeaconDbgRef = nullptr; }
    if (gStrobeDbgRef) { XPLMUnregisterDataAccessor(gStrobeDbgRef); gStrobeDbgRef = nullptr; }
    for (int i = 0; i < NCAT; ++i)
        if (gCategoryRefs[i]) { XPLMUnregisterDataAccessor(gCategoryRefs[i]); gCategoryRefs[i] = nullptr; }
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
