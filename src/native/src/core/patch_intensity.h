// The cockpit-light intensity multiplier — scales the brightness of every
// interior ("Gus Mod") light by rewriting `lights_inn.obj` IN PLACE.
//
// Why a file rewrite and not a dataref: the interior's 87 LIGHT_PARAM lamps
// (`airplane_panel_sp` / `airplane_inst_sp`) have no per-light dataref hook, and
// converting them to LIGHT_SPILL_CUSTOM so one could be added was tried and
// rejected — it changes the rendered look, and it buys a per-frame callback per
// light for a value that changes once a month. So the light TYPES stay exactly
// as Gus authored them and the multiplier is baked into the one slot every line
// already has: slot 8, the `size` — the same slot `optimize: boost` scales in
// the DSL and the screen glow drives at runtime, i.e. the codebase's one
// established brightness lever. A change therefore lands on the next aircraft
// load, and both writers say so.
//
// ⚠ TWO WRITERS, ONE PATCH. The plugin applies the factor when the user moves
// the Settings-tab slider and again on aircraft load when the file disagrees
// with the saved setting; the installer applies it right after staging a fresh
// `lights_inn.obj`, reading the saved factor out of the plugin's prefs file —
// without that, every reinstall would silently reset the cockpit to 1x. Both
// go through PatchText, so the file cannot come to carry two dialects.
//
// ⚠ THE PATCH IS RECOMPUTED FROM PRESERVED ORIGINALS, NEVER COMPOUNDED. Every
// scaled light line is preceded by a `# ToLissPhoton orig <line>` comment
// holding the authored line verbatim (OBJ8 treats `#` lines as comments — the
// generated file is already full of them). Re-patching parses the original out
// of the comment and scales THAT, so 2x then 3x equals 3x exactly, float
// round-trips never accumulate, and 1x restores the file byte-for-byte —
// which also means a 1x file carries no trace of this mechanism at all.
//
// ⚠ `kInteriorObjNeedle` must survive the patch untouched: it is the cockpit
// submenu sentinel, and the category `ANIM_hide` gates that carry it are not
// light lines. Same for the installer's `# ToLissPhoton version:` marker.
#pragma once

#include <filesystem>
#include <string>

namespace photon {
namespace intensity {

// The user-facing range. 1.0 is Gus's authored brightness; the ceiling is high
// enough to out-shine a dimming environment mod and low enough that a spill
// light's pool cannot swallow the whole flight deck.
constexpr double kMin = 1.0;
constexpr double kMax = 4.0;
constexpr double kDefault = 1.0;

// The two comment prefixes this patcher owns. Distinct from the installer's
// version marker (`# ToLissPhoton version: `) by their next word, so
// marker::Parse and FactorIn can never read each other's lines.
constexpr char kFactorPrefix[] = "# ToLissPhoton intensity: ";
constexpr char kOrigPrefix[] = "# ToLissPhoton orig ";

// ⚠ A `--debug` interior OBJ reads every light's parameters from the dev
// tuning datarefs, so patching its baked numbers would change nothing and
// confuse the tuning session's save/restore. Its presence needle.
constexpr char kDebugNeedle[] = "ToLissPhoton/debug/";

// The prefs-file key the factor is saved under, inside the "$cockpit" global
// block (constants.h has the block key and the filename). The plugin WRITES
// the JSON through these constants and SavedFactor READS through them, which
// is what keeps the two halves agreeing by construction.
constexpr char kIntensityJsonKey[] = "intensity";

double Clamp(double factor);
bool AtDefault(double factor);
// Factors compare through one epsilon everywhere, sized to the "%.2f" both
// writers use, so "already applied" can never disagree between the two halves.
bool SameFactor(double a, double b);

// "2x" / "2.5x" — the one spelling for logs and install-step messages.
std::string Label(double factor);

// The factor recorded in `text` by a previous PatchText; kDefault when none.
// ⚠ Deliberately NOT clamped: a hand-edited out-of-range marker must compare
// unequal to every legal setting so the next apply repairs the file.
double FactorIn(const std::string& text);

// Is this a Photon interior OBJ this patcher may touch? True only for text
// binding our interior datarefs and NOT a --debug build. Callers skip, never
// error, on false — the stock ToLiss lights_inn.obj must never be rewritten.
bool CanPatch(const std::string& text);

// Rewrite every interior light line (LIGHT_PARAM airplane_panel_sp /
// airplane_inst_sp, LIGHT_SPILL_CUSTOM) to `factor`, working from the
// preserved originals. `scaled` = lines whose emitted form was scaled (or, at
// 1x, restored). Newline flavor and every non-light byte are preserved.
std::string PatchText(const std::string& text, double factor, int& scaled);

// The saved multiplier from `<xplaneRoot>/Output/preferences/` +
// kProfilesJsonName, "$cockpit"."intensity". kDefault when the file, the block
// or the key is absent or unreadable — a user who never touched the slider has
// no entry, and that must read as 1x, never as an error.
double SavedFactor(const std::string& xplaneRootUtf8);

// FactorIn of a file on disk; kDefault when unreadable.
double FactorOfFile(const std::filesystem::path& obj);

enum class Applied {
    kPatched,   // the file was rewritten and now carries `factor`
    kAlready,   // it already did — nothing written
    kSkipped,   // not a patchable Photon interior OBJ (stock, debug, missing)
    kFailed,    // read or write error; `detail` says which
};

// Read-check-patch-write one file. `detail` always carries a loggable line.
Applied ApplyToFile(const std::filesystem::path& obj, double factor,
                    std::string& detail);

}  // namespace intensity
}  // namespace photon
