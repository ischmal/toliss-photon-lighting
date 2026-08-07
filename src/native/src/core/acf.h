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

// The `.acf` files in an aircraft folder whose text contains `needle`.
//
// ⚠ By CONTENT, not by an assumed `a320{,_StdDef,_XP11,_XP11_StdDef}.acf` naming
// scheme — the folder may have been renamed, and detection-by-content is already
// the principle governing aircraft detection.
//
// ⚠ `.bak` files are EXCLUDED. Patching a backup would corrupt some other mod's
// uninstall — Durantula keeps an `a320.acf.durantula.bak` of this very file.
std::vector<std::string> AcfFilesContaining(const std::string& aircraftDirUtf8,
                                            const std::string& needle);

}  // namespace acf
}  // namespace photon
