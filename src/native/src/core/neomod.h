// Is something in this X-Plane install raising the SPILL-LIGHT CUTOFF — i.e.
// culling exactly the lights Photon's cockpit is made of?
//
// The mod that prompted this is BSS NEO ("NEO V2.x", Blue Sky Star Simulations),
// a payware global lighting package. Full analysis: docs/neo_compat_plan.md.
// What it does, and the whole of what Photon cares about, is one line in its
// FlyWithLua script:
//
//     set("sim/private/controls/lights/spill_cutoff_level", 0.09)
//
// `spill_cutoff_level` is the threshold below which X-Plane stops spending a
// spill light. Raising it culls the DIM, WIDE-CONE lights first — and every
// cockpit lamp Photon ships is `airplane_panel_sp` / `airplane_inst_sp`, i.e. a
// spill light. `int_dome` (size 1.414, omnidirectional) is the dimmest wide
// light in the flight deck and the one that visibly dies. That is the reported
// "dome light is too dim", and it is a global sim setting Photon never touched.
//
// ⚠ THIS ASKS A PHYSICAL QUESTION, NOT A BRANDING ONE. The verdict is "something
// active writes the spill cutoff", with BSS NEO as the *recognized* case rather
// than the only one. That is deliberate: it generalizes to PWL, to forks, to
// repackaged copies and to whatever the next version calls itself, and it needs
// no knowledge of X-Plane's stock default — which is version-dependent and which
// this module therefore never encodes.
//
// ─── the traps, each of which produced a wrong answer while this was written ──
//
// ⚠ NEVER MATCH THE TOKEN "NEO". A name grep over a real install returns ten
// hits and none of them is the mod: ToLiss's own `license321NEO.lic`, and
// `objects/neo.obj`, `objects/NEO.png`, `flaps_new_NEO.obj` INSIDE the A320,
// plus an `F-WNEO` livery and FlyWithLua's `EnterLineOfCode.lua`. This is the
// same shape as the `"sasl"` plugin-name match that hard-crashed the sim.
// Everything here matches file CONTENT and is scoped by PATH, exactly as
// `IsToLiss()` and the installer's aircraft detection already do.
//
// ⚠ ONLY `Scripts/` RUNS. FlyWithLua also has `Scripts (disabled)/` and
// `Scripts (Quarantine)/`, both of which exist on a real install. A copy parked
// in either is INERT, and counting it would warn — permanently — a user who has
// already fixed the problem. Only the one directory is read, and never
// recursively: FlyWithLua loads top-level `.lua` from it and nothing deeper.
//
// ⚠ THE FILENAME IS EVIDENCE, NEVER THE TEST. `NEO.lua` is what it ships as, but
// a troubleshooting user renames things and a repackaged copy arrives called
// anything at all. The name is carried in the result so a log line can say which
// file, and is not consulted to reach the verdict.
//
// ⚠ READ-ONLY AND NON-THROWING, ALWAYS — same rule as core/wingmod.h. This runs
// inside an install; a detector that could abort one would be a worse bug than
// the dim lamp it reports. Anything unreadable becomes "not detected" plus a
// note saying so, never an error and never a guess. (⚠ The PLUGIN never calls
// this — its 1 Hz probe reads the live dataref instead, `ProbeSpillCutoff` in
// plugin.cpp, because in the sim the question is answerable directly and a file
// scan would only say what the file WOULD do.)
//
// ⚠ ONLY THE SCRIPT IS CHECKED, AND THE OTHER TWO PAYLOAD HALVES ARE DELIBERATE
// OMISSIONS. The mod also ships `bitmaps/world/lites/` (a `lights.txt` +
// sprite sheet) and terrain textures. Neither can hurt us: every light class
// Photon emits — the `airplane_*_{bb,pm}` pairs and both `_sp` cockpit classes —
// is a `LIGHT_PARAM_DEF` passthrough whose every value comes from the OBJ line
// we write, and those definitions are byte-identical in the mod's copy. Only
// the `_static` variants differ, and Photon emits none. Checking for them would
// report a scary-looking finding with no consequence attached.
#pragma once

#include <string>
#include <vector>

namespace photon {
namespace neomod {

// ─── the needles ─────────────────────────────────────────────────────────────
//
// ⚠ THE FIRST ONE IS THE VERDICT; THE OTHER TWO ONLY NAME THE MOD. A script that
// writes the cutoff hurts our cockpit whatever else it is, so recognition must
// never be a precondition for the compensation — that is the difference between
// this generalizing and this being a hard-coded allow-list of one product.
constexpr char kSpillCutoffRef[] = "sim/private/controls/lights/spill_cutoff_level";

// Corroborators. Both are rare enough that a script carrying all three is BSS
// NEO or something derived from it; neither appears in an ordinary FlyWithLua
// script, and neither is a name.
constexpr char kPhotoBbEvRef[] = "sim/private/controls/lights/photobb/hack_ev_lo";
constexpr char kTurbidityRef[] =
    "sim/private/controls/scattering/override_turbidity_t";

// The ONE directory whose scripts actually run, relative to the X-Plane root.
constexpr char kScriptsDir[] = "Resources/plugins/FlyWithLua/Scripts";

// How sure we are about WHAT was found — never about WHETHER something was.
enum class Confidence {
    kNone,        // nothing writes the cutoff
    kUnknownMod,  // something does, but carries none of the corroborators
    kRecognized,  // all three needles: BSS NEO or a derivative
};

enum class Severity {
    Note,     // worth saying; nothing is wrong
    Problem,  // this install will render wrong, and the user can act on it
};

struct Finding {
    Severity severity = Severity::Note;
    std::string text;
};

struct Result {
    Confidence confidence = Confidence::kNone;

    // The script that writes the cutoff — filename only, for the log line.
    // Evidence, not the test (see the header note).
    std::string scriptName;

    // The literal it sets the cutoff to. ⚠ NEGATIVE MEANS "NOT PARSED", which is
    // a different thing from zero: a script may compute the value, and zero is a
    // legal cutoff meaning "cull nothing". Nothing may treat <0 as a number.
    double spillCutoff = -1.0;

    // The one-line verdict, always set — including when nothing was found.
    std::string summary;

    // ⚠ Severity is carried rather than baked into the text, for the same reason
    // as wingmod::Finding: the caller picks the log level, and the GUI's run log
    // marks a WARN with `!` instead of a check.
    std::vector<Finding> findings;

    // The whole question this module exists to answer.
    bool Detected() const { return confidence != Confidence::kNone; }

    std::vector<std::string> Texts(Severity s) const;

    // The token used in `status --json` and in the manifest. Stable strings —
    // a UI may switch on them.
    std::string Token() const;
};

// Scan one X-Plane root. ⚠ Takes the ROOT, not an aircraft folder: this is a
// property of the INSTALL, not of an aeroplane, which is why it is not part of
// wingmod's per-aircraft answer and why the GUI asks it once.
Result Detect(const std::string& xplaneRootUtf8);

// ─── the pieces, exposed because each is a rule worth pinning on its own ─────

// Does this Lua text WRITE the spill cutoff? True only for a write — a script
// that merely reads the dataref changes nothing and must not count.
bool WritesSpillCutoff(const std::string& luaText);

// The literal from `set("<kSpillCutoffRef>", <number>)`. False when the script
// writes it some other way (a computed value, a `dataref()` binding assigned
// later); the caller then keeps Result::spillCutoff negative and still reports
// the mod as detected. ⚠ Parsing failure is NOT detection failure.
bool ParseSpillCutoff(const std::string& luaText, double& out);

// All three needles present.
bool LooksRecognized(const std::string& luaText);

}  // namespace neomod
}  // namespace photon
