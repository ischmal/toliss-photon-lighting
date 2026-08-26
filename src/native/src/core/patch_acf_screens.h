// The `.acf` attachment patcher for the screen-glow OBJ.
//
// `lights_screens.obj` is an OBJ ToLiss does not ship and does not reference, so
// nothing draws it until it is added to the aircraft's `_obja/*` attachment table.
// That is precisely what makes the feature reversible: an install that never runs
// this carries no trace of it, and the detach removes every line again.
//
// ⚠ WHY THE ATTACHMENT IS COPIED, NOT WRITTEN
// -------------------------------------------
// An attachment row carries the OBJ's origin in FEET against a per-airframe datum,
// and it differs per airframe — 26.00 / 20.75 / 6.78 ft on the A319 / A320 / A321.
// Our light coordinates are authored in `lights_inn.obj`'s frame, so the ONLY
// correct attachment is a copy of THAT OBJ's own row with the filename swapped.
// Hardcoding a datum would silently mis-place every screen light on two airframes
// out of three. If `lights_inn.obj` is not in the table we REFUSE rather than guess
// — and the refusal is logged, non-fatal, and the install continues: the glow is
// simply absent, which is its own honest degraded state.
//
// ⚠ THIS TOUCHES `_obja/*`, WHICH IS DURANTULA'S NAMESPACE. The cockpit-spot
// patcher deliberately stays inside `acf/_spot*` so it cannot collide with
// Durantula's 312-line rewrite of the attachment table. This one cannot: attaching
// an OBJ *is* an `_obja` edit. So detection is BY PRESENCE OF OUR FILENAME in a
// `_v10_att_file_stl` slot — never by index (ours is whatever was free at the time)
// and never by a hash (the table legitimately carries other mods' edits). If
// Durantula's uninstall restores a pre-Photon backup our row vanishes with it;
// `IsAttached` then reports false, which is the truth.
//
// ⚠ THE FORMAT TRAP IS SIDESTEPPED, NOT SOLVED, HERE. Values are copied from the
// template row VERBATIM AS STRINGS, so they are already in the host file's own
// float style and need no re-rendering. Only `_obja/count` is a number we compute,
// and it is an integer written bare.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/acf.h"
#include "core/acf_attach.h"
#include "core/progress.h"

namespace photon {
namespace patch_acf_screens {

// ⚠ THE ROW ENGINE MOVED TO `core/acf_attach` (2026-08-24), when the integral
// lighting feature became a second caller that appends and drops `_obja` rows.
// Everything below is this feature's own half — which OBJ, which frame to copy,
// and what to say when there is none — and the names are kept so the tests and
// `wingmod.cpp` read the table through the spelling they always have.
constexpr const char* kCountProp = acf_attach::kCountProp;
constexpr const char* kFileField = acf_attach::kFileField;

// Index of the row whose frame ours is copied from, or -1 if this file carries
// none of the candidates in ScreensFrameTemplates(). Exposed for the tests and
// for the refusal message, which names what it looked for.
//
// ⚠ Chosen BY CONTENT, in the candidate list's order, never from an airframe key
// passed down from detection — the same rule the installer follows everywhere
// else, and it keeps the answer correct for a file that carries more than one.
int FindFrameTemplate(const std::string& text);

// The `_obja/*` table as {index: {field: raw value}}.
std::map<int, std::map<std::string, std::string>> AttachmentRows(
    const std::string& text);

// Index of the row attaching `filename`, or -1.
int FindObj(const std::string& text, const std::string& filename);

// `_obja/count` — X-Plane reads rows 0..count-1, so appending a row without
// bumping this attaches nothing at all. Throws AcfError when absent.
int DeclaredCount(const std::string& text);

// ⚠ BOTH HALVES MATTER: our OBJ must be in the table AND inside the declared
// count. A row at index >= count is present in the file but never read — it looks
// installed to a naive grep and draws nothing in the sim.
bool IsAttached(const std::string& text);

// Attach / detach. Both idempotent. PatchText throws AcfError when there is no
// template row to copy a frame from.
std::string PatchText(const std::string& text, int& changed);
std::string UnpatchText(const std::string& text, int& changed);

// The `.acf` files in an aircraft folder carrying an attachment table. ⚠ `which`
// IS NOT A DEFAULT-ABLE ARGUMENT: attaching acts on `Xp12` and detaching on
// `Every` — see `acf::Variants`, which carries the reason both are right.
std::vector<std::string> AcfFiles(const std::string& aircraftDirUtf8,
                                  acf::Variants which);

struct RunResult {
    std::vector<std::string> touched;
    // ⚠ The subset that actually differed — see the note on `patch_acf::RunResult`.
    // Detaching visits four files and changes the two attaching wrote to.
    std::vector<std::string> changed;
    std::vector<std::string> log;
};

RunResult Run(const std::string& aircraftDirUtf8, bool reverse, bool dryRun,
              const progress::StepProgress& onProgress = {});

}  // namespace patch_acf_screens
}  // namespace photon
