// Which wing mod — Durantula, RealWings, neither — is ACTUALLY DRAWING on an
// aircraft, and whether it was installed correctly.
//
// ⚠ WHAT IS IN THE FOLDER IS NOT WHAT IS ON THE AEROPLANE, and that is the whole
// reason this file exists. Both mods leave their distribution folders and their
// `.bak` files behind forever: a real A320 here carries `RealWings320/`,
// `Durantula_ToLiss_Wingflex_V1.3/`, `wingL.obj.durantula.bak` and
// `a320.acf.durantula.bak` — and is running STOCK wings. Any check that answers
// from a folder listing says "both mods are installed" about an aircraft with
// neither. So every question below is asked of the two things X-Plane itself
// reads: the `.acf` attachment table, and the contents of the OBJs it names.
//
// ─── how each mod actually works ────────────────────────────────────────────
//
// DURANTULA is an EDIT TO THE AIRCRAFT'S OWN WING OBJ. Its manual (DATA.txt) is
// six find-and-replace blocks, three per wing, that swap ToLiss's `anim/winglex`
// rotation chain for one driven by `sim/flightmodel2/wing/wing_tip_deflection_deg`.
// The attachment table is untouched — `wingL.obj` is still `wingL.obj` — so the
// signal is the DRIVER TOKEN inside it, and nothing else.
//
// REALWINGS REPLACES THE WING GEOMETRY WHOLESALE. Its ReadMe is a Plane Maker
// session: delete the `wingL` / `wingR` / `wings_glass` attachment rows, add six
// objects out of `objects/RealWings<nnn>/`. The aircraft's own wing OBJs stay on
// disk untouched and simply stop being drawn — so here the signal is the
// ATTACHMENT TABLE, and reading the wing OBJ would report stock forever.
//
// The two therefore need two different questions, and asking either one alone gets
// the other mod wrong. RealWings wins when both are present: if the table does not
// name `wingL.obj`, whatever Durantula did to `wingL.obj` does not draw.
//
// ⚠ THIS IS EXACTLY THE FIELD PHOTON'S OWN BUILD MIRRORS. `lights.layout.phdsl`'s
// `wing_left`/`wing_right` mounts are copies of that same rotation chain, so a
// Photon built for `stock` and installed on a Durantula aircraft hangs its wingtip
// lights off a driver the wing no longer uses: the lights sit still while the wing
// flexes. Nothing errors, nothing looks broken on the ground, and the aircraft has
// to be airborne and loaded for anyone to see it. Hence the check.
//
// ⚠ READ-ONLY AND NON-THROWING, ALWAYS. This runs inside an install; a detector
// that can abort one would be a worse bug than the mismatch it reports. Anything
// it cannot read becomes an undetermined answer with a line saying so.
#pragma once

#include <string>
#include <vector>

namespace photon {
namespace wingmod {

// ⚠ THE SAME TOKENS AS `--wing`, so an answer can be compared with
// `actions::Options::wing` and with the manifest's `wing` field directly. A second
// vocabulary here would need a translation table, and a translation table is what
// eventually disagrees with `WingsFor()`.
constexpr char kStock[] = "stock";
constexpr char kDurantula[] = "durantula";
constexpr char kRealWings[] = "realwings";

// The two wing-flex drivers. ⚠ `kDurantulaFlexRef` is matched as a PREFIX: the
// real lines carry an index (`…wing_tip_deflection_deg[2]`) and the index differs
// per block and per side, so matching the whole token would find nothing.
constexpr char kStockFlexRef[] = "anim/winglex";
constexpr char kDurantulaFlexRef[] =
    "sim/flightmodel2/wing/wing_tip_deflection_deg";

// How many blocks Durantula's manual replaces in ONE wing OBJ: six cases, three
// per side (V1.3, the current release). ⚠ USED FOR A NOTE, NEVER FOR THE VERDICT.
// A later version may add a case, and an installer that called that "wrong" would
// cry wolf on a correct install. What IS a hard problem is asymmetry — the mod is
// symmetric by construction, so one wing with more converted blocks than the other
// is a paste that was missed, whatever the version.
constexpr int kDurantulaBlocksPerWing = 3;

enum class Severity {
    Note,       // worth saying; nothing is wrong
    Problem,    // this aircraft will draw or fly wrong, and the user can fix it
};

struct Finding {
    Severity severity = Severity::Note;
    std::string text;
};

struct Result {
    // One of the three tokens, or empty when nothing could be established.
    std::string wing;
    bool determined = false;

    // ⚠ A SEPARATE AXIS FROM `determined`. A ToLiss folder holds FOUR `.acf`
    // variants and RealWings' install is a manual Plane Maker edit of ONE file, so
    // "the main `.acf` says RealWings and the StdDef one says stock" is the single
    // most likely mis-install there is — and it is not an undetermined answer, it
    // is a determined answer per variant that disagree. `wing` then holds what the
    // majority of the readable files say, so the install still picks something
    // sensible, and the disagreement is a Problem finding naming both sides.
    bool agreed = true;

    // The one-line verdict, always set — including when nothing could be read.
    std::string summary;

    // ⚠ SEVERITY IS CARRIED, NOT BAKED INTO THE STRING, because the caller picks
    // the log level from it and the GUI's run log marks a WARN line with `!`
    // instead of a check. A pre-formatted "! …" would look right in the file log
    // and print a doubled marker on screen.
    std::vector<Finding> findings;

    bool Healthy() const;
    // Findings of one severity, in order.
    std::vector<std::string> Texts(Severity s) const;
};

// The answer for one aircraft folder. `airframeKey` decides which RealWings folder
// name is looked for; an airframe with no wing mods at all (a339) short-circuits to
// a determined `stock` with one explanatory line and reads nothing.
Result Detect(const std::string& aircraftDirUtf8, const std::string& airframeKey);

// ─── the pieces, exposed because each is a rule worth pinning on its own ─────

// Occurrences of Durantula's driver in one OBJ's text. 0 = stock chain.
int CountDurantulaBlocks(const std::string& objText);

// True for an attachment naming one of the aircraft's own flex-carrying wing
// meshes — `wingL.obj`, `wingR.obj`, and the A321's `wing321L.obj`/`wing321R.obj`.
//
// ⚠ MATCHED ON THE BASENAME, and `wings_glass.obj` is deliberately NOT one: it
// carries no flex chain to classify. It is still stock wing geometry for the
// conflict check below, which is why that has its own predicate.
bool IsStockWingObj(const std::string& attachmentPath);
bool IsStockWingGlassObj(const std::string& attachmentPath);

// True when `attachmentPath`'s first path segment is `realWingsDir`
// (case-insensitively, either slash). ⚠ Segment-wise, never `find()`: an aircraft
// OBJ called `RealWings320_old.obj` in objects/ would match a substring test and
// be counted as the mod being live.
bool IsUnderDir(const std::string& attachmentPath, const std::string& dirName);

}  // namespace wingmod
}  // namespace photon
