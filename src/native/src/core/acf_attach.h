// The `.acf` `_obja/*` attachment table — the generic row engine.
//
// Two features now add OBJs to an aircraft: the display glow
// (`core/patch_acf_screens`) and the switchable integral lighting
// (`core/patch_acf_integral`). Appending a row and dropping one again is the
// same job both times, and the parts of it that are easy to get wrong are
// exactly the parts that are silent:
//
//   * `_obja/count` — X-Plane reads rows 0..count-1, so a row appended without
//     bumping it attaches nothing at all, while looking installed to a grep.
//   * ⚠ RENUMBERING ON REMOVAL. Dropping row 7 out of 9 and leaving a hole makes
//     X-Plane read index 8 as though it were 7 — silently unloading whatever
//     some other mod attached there.
//   * ⚠ VALUES ARE COPIED VERBATIM AS STRINGS. An attachment row carries the
//     OBJ's origin against a per-airframe datum, in the host file's own float
//     style (XP12 writes `-70.000000000`, XP11 `-70.0`). Re-rendering a number
//     is how the format trap gets sprung; copying the template row's bytes
//     sidesteps it entirely. Only `count` is a number we compute, and it is an
//     integer written bare.
//
// ⚠ THE FRAME COMES FROM A TEMPLATE ROW, NEVER FROM A CONSTANT. Where an
// attached OBJ ends up is that row's business, and the datum differs per
// airframe (26.00 / 20.75 / 6.78 ft on the A319 / A320 / A321). Which row to
// copy is the CALLER's question — the glow copies `lights_inn.obj`'s because its
// lights are authored in that frame; integral lighting copies the row of the
// very OBJ it is twinning, which is the only frame that can be right. So this
// takes a resolved index and each caller owns its own refusal message.
//
// ⚠ ONE ROW PER CALL, DELIBERATELY. `Detach` renumbers everything above the row
// it drops, so two removals in one pass would need the second to know what the
// first shifted. Calling it twice re-reads the table each time and cannot get
// that wrong.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/acf.h"

namespace photon {
namespace acf_attach {

constexpr char kCountProp[] = "_obja/count";
constexpr char kFileField[] = "_v10_att_file_stl";

// The `_obja/*` table as {index: {field: raw value}}.
std::map<int, std::map<std::string, std::string>> AttachmentRows(
    const std::string& text);

// Index of the row attaching `filename`, or -1.
int FindObj(const std::string& text, const std::string& filename);

// `_obja/count`. Throws AcfError when absent.
int DeclaredCount(const std::string& text);

// ⚠ BOTH HALVES MATTER: the OBJ must be in the table AND inside the declared
// count. A row at index >= count is present in the file but never read.
bool IsAttached(const std::string& text, const std::string& filename);

// Append a row attaching `filename`, with every field copied from row
// `templateIndex`. Idempotent — an already-attached file comes back unchanged.
//
// ⚠ AT THE END OF THE TABLE, WHICH IS A STATEMENT ABOUT DRAW ORDER. X-Plane
// walks `_obja` in index order, so the last row draws last — after the
// aircraft's transparent geometry, which ToLiss deliberately puts at the end
// (`windows.obj`, `GlassInterior.obj`, `GlassInteriorWindows.obj`). That is
// right for an OBJ of nothing but `LIGHT_SPILL_CUSTOM` lights, which is what the
// display glow is. It is WRONG for blended mesh — see `InsertAfter`.
std::string Attach(const std::string& text, const std::string& filename,
                   int templateIndex, int& changed);

// The same, but placed DIRECTLY AFTER row `templateIndex`, renumbering every row
// above it up by one.
//
// ⚠ A ROW'S POSITION IS PART OF WHAT IT SAYS, exactly as its transform is, and
// this function exists because a first cut got one and not the other
// (2026-08-24). Integral lighting's twin OBJs were appended at the end with
// their source's frame copied — geometrically perfect, and visibly wrong in the
// cockpit: neither `lamps.obj` nor `knobs.obj` declares `ATTR_no_blend`, so both
// draw with alpha blending on, and at the end of the table they drew AFTER the
// glass instead of at index 10-12 with the rest of the flight deck. The reported
// symptom was the placard artwork showing through the engine selector switches.
// Appending also silently INVERTED the pair's own order — stock is knobs before
// lamps, and two appended twins came out lamps before knobs.
//
// So: a twin of an existing object belongs beside that object. Anything with no
// source to sit beside — the display glow — uses `Attach`.
//
// ⚠ RENUMBERING UP IS SAFE because nothing anywhere identifies an attachment by
// INDEX: our own detection, `wingmod`'s RealWings check and the glow scan all
// match on the filename. Same rule that lets `Detach` renumber down.
std::string InsertAfter(const std::string& text, const std::string& filename,
                        int templateIndex, int& changed);

// Drop the row attaching `filename` and renumber the rows above it down.
// Idempotent.
std::string Detach(const std::string& text, const std::string& filename,
                   int& changed);

// The `.acf` files in an aircraft folder carrying an attachment table. ⚠ `which`
// IS NOT DEFAULT-ABLE: writing acts on `Xp12` and reversing on `Every` — see
// `acf::Variants`, which carries the reason both are right.
std::vector<std::string> AcfFiles(const std::string& aircraftDirUtf8,
                                  acf::Variants which);

}  // namespace acf_attach
}  // namespace photon
