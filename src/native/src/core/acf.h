// The `.acf` property engine — parsing, style detection, and in-place rewriting.
//
// ⚠ THE FORMAT TRAP — READ BEFORE EDITING
// ---------------------------------------
// XP12 and XP11 `.acf` files render the SAME values differently:
//
//     property            XP12              XP11
//     _spot_the/0         -70.000000000     -70.0
//     _spot_angle/0       0.000000000       0.0
//     _spot1_3d_xyz/1     1.000000000       1.0
//
// A literal string patch for `_spot_the/0 -60.000000000` works on the two XP12
// files and SILENTLY NO-OPS on the two XP11 ones — and every ToLiss airframe ships
// all four. So this engine always parses NUMERICALLY and re-renders in each file's
// own DETECTED style, and comparison is numeric with a tolerance — never by string
// and never by hash.
//
// ⚠ DETECTION IS NEVER A FILE HASH. The `.acf` legitimately carries other mods'
// edits (Durantula rewrites 312 lines of the `_obja/*` table in the very same
// file), so "did we patch this?" has to be a question about OUR properties.
//
// ⚠ BYTES IN, BYTES OUT. These files are CRLF on disk and one stock A321 `.acf`
// carries a stray NUL. `SetProps` preserves each line's own terminator and every
// byte it does not deliberately change, so a patch is a minimal diff rather than a
// whole-file rewrite. Read them with `fsutil::ReadFileBytes` and write them with
// `fsutil::AtomicWriteBytes`; nothing here re-encodes.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace photon {
namespace acf {

// The `.acf` could not be parsed, or lacks the block a patcher needs.
class AcfError : public std::runtime_error {
public:
    explicit AcfError(const std::string& what) : std::runtime_error(what) {}
};

enum class Style {
    Xp12,   // fixed 9-decimal floats
    Xp11,   // shortest round-tripping representation, always with a decimal point
};

// A parsed `P <prop> <value>` line. `head` is everything up to and including the
// whitespace before the value, so rewriting is "head + new value + own terminator"
// and the line keeps its exact original spacing.
struct PropLine {
    bool matched = false;
    std::string head;
    std::string prop;
    std::string value;   // trimmed
};

PropLine ParseLine(const std::string& lineWithoutEol);

// Every `P <prop> <value>` line as {prop: trimmed value}.
std::map<std::string, std::string> ReadProps(const std::string& text);

// ⚠ Decided by MAJORITY VOTE over every numeric `acf/` property in the file, not
// from one probe line: a handful of values (28.010000229) happen to need 9 decimals
// in BOTH styles and would mis-vote on their own.
Style DetectStyle(const std::string& text);

// Render a float the way `style`'s files write it.
std::string Render(double value, Style style);

// Rewrite the value of each named property in place. Returns the new text and sets
// `changed` to the number of lines that actually differ.
//
// ⚠ Throws AcfError if any named property is ABSENT. A silent skip is exactly the
// failure mode the format trap produces, so the engine refuses to report success
// over a file it did not fully patch.
std::string SetProps(const std::string& text,
                     const std::map<std::string, std::string>& values,
                     int& changed);

// Numeric comparison with the engine's tolerance. `raw` is a value string as it
// appears in the file.
constexpr double kTolerance = 1e-4;
bool NumericEquals(const std::string& raw, double want);

// ─── which sim a file is for ─────────────────────────────────────────────────
//
// Every `.acf` X-Plane writes stamps its own format version on the SECOND LINE:
//
//     I
//     1200 Version          <- X-Plane 12.  `1100 Version` on an XP11 file
//     ACF
//
// ⚠ THIS IS THE CONTENT TEST, AND `_XP11` IN THE FILENAME IS NOT IT. A ToLiss
// folder ships four variants — `a320{,_StdDef,_XP11,_XP11_StdDef}.acf` — but the
// naming is ToLiss's convention, not X-Plane's, and detection-by-content is
// already the principle governing every other question this installer asks about
// an aircraft folder. Verified against all four airframes here (2026-08-15): the
// XP12 pair stamps 1200 and the XP11 pair 1100, every time.
//
// ⚠ NOT `DetectStyle`, WHICH ANSWERS A DIFFERENT QUESTION. That one votes on how
// floats are RENDERED so a patch can be written back in the file's own style, and
// it has no "cannot tell" — it must return one of two styles for any text. This
// one is a stamp that is either there or not.
constexpr int kXp12FormatVersion = 1200;

// The stamp, or 0 when there is none to read.
int FormatVersion(const std::string& text);

// ⚠ AN UNSTAMPED FILE IS TREATED AS XP12, and that is deliberate: the failure it
// avoids is a SILENT SKIP. Everything gated on this has already passed a content
// needle, so "no stamp" means an `.acf` unlike any X-Plane writes — and excluding
// it would make an install quietly do nothing to a file it was meant to patch,
// which is the exact shape of the format trap at the top of this header.
bool IsForXp12(const std::string& text);

// Which of an aircraft's `.acf` variants an operation acts on.
//
// ⚠ PHOTON WRITES TO THE XP12 PAIR ONLY (2026-08-15). Nothing it builds is for
// X-Plane 11 — the exterior OBJ is literally `lights_out3xx_XP12.obj` — so
// patching the XP11 pair was work with no reader, and READING it was worse: the
// wing-mod check took the four variants' verdicts as a consensus, and RealWings'
// own instructions are a Plane Maker session on ONE file, so an XP11 variant left
// stock made a correct RealWings install report that its `.acf` files "do not
// agree".
//
// ⚠ A REVERSAL STILL REACHES EVERY VARIANT, and that is not an inconsistency.
// Photon ≤ 0.8.4 patched all four, so those edits exist in the wild: an uninstall
// scoped to XP12 would leave an XP11 file naming `lights_screens.obj` — a file the
// same uninstall deletes — and a cockpit spot block still disabled, with nothing
// on screen saying so. Same rule as the backup folder: write to the new place,
// clean up both.
enum class Variants {
    Xp12,    // the pair X-Plane 12 loads. Everything Photon WRITES.
    Every,   // ...and the XP11 pair, which older versions patched.
};

// The `.acf` files in an aircraft folder whose text contains `needle`.
//
// ⚠ By CONTENT, not by an assumed `a320{,_StdDef,_XP11,_XP11_StdDef}.acf` naming
// scheme — the folder may have been renamed, and detection-by-content is already
// the principle governing aircraft detection.
//
// ⚠ `.bak` files are EXCLUDED. Patching a backup would corrupt some other mod's
// uninstall — Durantula keeps an `a320.acf.durantula.bak` of this very file.
std::vector<std::string> AcfFilesContaining(const std::string& aircraftDirUtf8,
                                            const std::string& needle,
                                            Variants which);

}  // namespace acf
}  // namespace photon
