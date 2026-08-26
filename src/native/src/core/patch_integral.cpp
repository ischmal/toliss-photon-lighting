#include "core/patch_integral.h"

#include <fstream>

#include "core/fsutil.h"
#include "core/marker.h"

namespace photon {
namespace integral {
namespace {

// Line 7 of an OBJ8 header at the outside; 4 KB is generous even for a file
// carrying a long comment block above POINT_COUNTS.
constexpr std::size_t kHeadBytes = 4096;

// ⚠ DATA, NOT DRAWING. These belong between POINT_COUNTS and the first command
// and must stay outside the animation block — see the header.
bool IsTableToken(const std::string& tok) {
    return tok == "VT" || tok == "VLINE" || tok == "VLIGHT" || tok == "IDX" ||
           tok == "IDX10";
}

std::string FirstToken(const std::string& s, std::size_t pos, std::size_t eol) {
    std::size_t a = pos;
    while (a < eol && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    std::size_t b = a;
    while (b < eol && s[b] != ' ' && s[b] != '\t' && s[b] != '\r') ++b;
    return s.substr(a, b - a);  // short enough to live in the SSO buffer
}

// ⚠ THE FILE'S OWN FLAVOR, never a hard-coded "\n". These OBJs are read and
// rewritten by other tools; flipping every line ending turns a three-line patch
// into a whole-file diff. Same rule `marker::Inject` states.
std::string DetectEol(const std::string& text) {
    const std::size_t nl = text.find('\n');
    if (nl != std::string::npos && nl > 0 && text[nl - 1] == '\r') return "\r\n";
    return "\n";
}

bool ReadHead(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(kHeadBytes, '\0');
    in.read(out.data(), static_cast<std::streamsize>(kHeadBytes));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return true;
}

// Offsets into `text`: just past the POINT_COUNTS line, and the first real draw
// command after the vertex/index tables. Both npos when the file is not an OBJ8
// we recognize.
struct Sections {
    std::size_t headerEnd = std::string::npos;
    std::size_t drawStart = std::string::npos;
};

Sections FindSections(const std::string& text) {
    Sections s;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t eol = text.find('\n', pos);
        const std::size_t lineEnd = (eol == std::string::npos) ? text.size() : eol;
        const std::size_t next = (eol == std::string::npos) ? text.size() : eol + 1;
        const std::string tok = FirstToken(text, pos, lineEnd);
        if (s.headerEnd == std::string::npos) {
            if (tok == "POINT_COUNTS") s.headerEnd = next;
        } else if (!tok.empty() && tok[0] != '#' && !IsTableToken(tok)) {
            s.drawStart = pos;
            return s;
        }
        if (eol == std::string::npos) break;
        pos = next;
    }
    return s;
}

// Swap the value on the `TEXTURE_LIT` line, keeping the file's own separator
// (these are tab-separated) and its line ending.
bool SwapTextureLit(std::string& header, const std::string& litName) {
    std::size_t pos = 0;
    while (pos <= header.size()) {
        std::size_t eol = header.find('\n', pos);
        const std::size_t lineEnd = (eol == std::string::npos) ? header.size() : eol;
        if (FirstToken(header, pos, lineEnd) == "TEXTURE_LIT") {
            // Keep everything up to and including the whitespace run after the
            // token; replace only the value, and leave any trailing \r alone.
            std::size_t a = pos;
            while (a < lineEnd && (header[a] == ' ' || header[a] == '\t')) ++a;
            a += std::string("TEXTURE_LIT").size();
            std::size_t v = a;
            while (v < lineEnd && (header[v] == ' ' || header[v] == '\t')) ++v;
            std::size_t e = lineEnd;
            while (e > v && (header[e - 1] == '\r')) --e;
            header = header.substr(0, v) + litName + header.substr(e);
            return true;
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return false;
}

}  // namespace

const char kDataRef[] = "ToLissPhoton/interior/integral";
const char kObjNeedle[] = "ToLissPhoton/interior/integral";
const char kLedSuffix[] = "_photon_led";

const char* EraKey(Era era) {
    return era == Era::kLed ? "led" : "incandescent";
}

const char* EraLabel(Era era) {
    return era == Era::kLed ? "LED" : "Incandescent";
}

lit::Profile ProfileFor(Era era) {
    return era == Era::kLed ? lit::Profile::kLed : lit::Profile::kIncandescent;
}

std::string LedNameFor(const std::string& stockName) {
    const std::size_t dot = stockName.rfind('.');
    if (dot == std::string::npos) return stockName + kLedSuffix;
    return stockName.substr(0, dot) + kLedSuffix + stockName.substr(dot);
}

std::string TextureLitOf(const std::string& objHead) {
    std::size_t pos = 0;
    while (pos <= objHead.size()) {
        std::size_t eol = objHead.find('\n', pos);
        const std::size_t lineEnd =
            (eol == std::string::npos) ? objHead.size() : eol;
        if (FirstToken(objHead, pos, lineEnd) == "TEXTURE_LIT") {
            std::size_t a = pos;
            while (a < lineEnd && (objHead[a] == ' ' || objHead[a] == '\t')) ++a;
            a += std::string("TEXTURE_LIT").size();
            while (a < lineEnd && (objHead[a] == ' ' || objHead[a] == '\t')) ++a;
            std::size_t e = lineEnd;
            while (e > a && (objHead[e - 1] == '\r' || objHead[e - 1] == ' ' ||
                             objHead[e - 1] == '\t')) {
                --e;
            }
            return objHead.substr(a, e - a);
        }
        // POINT_COUNTS ends the header; anything after it is tables and drawing.
        if (FirstToken(objHead, pos, lineEnd) == "POINT_COUNTS") break;
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return std::string();
}

std::string TextureLitOfFile(const fs::path& objPath) {
    std::string head;
    if (!ReadHead(objPath, head)) return std::string();
    return TextureLitOf(head);
}

std::vector<Fixture> FixturesIn(const fs::path& objectsDir) {
    std::vector<Fixture> out;
    std::error_code ec;
    if (!fs::is_directory(objectsDir, ec) || ec) return out;

    for (int t = 0; t < lit::kTextureCount; ++t) {
        const auto texture = static_cast<lit::Texture>(t);
        const std::string want = lit::TextureLitName(texture);
        // ⚠ Sorted by the directory iterator's own order, then filtered — the
        // list is small and the caller logs it, so stability matters more than
        // speed here.
        for (const auto& entry : fs::directory_iterator(objectsDir, ec)) {
            if (ec) break;
            std::error_code fec;
            if (!entry.is_regular_file(fec) || fec) continue;
            const std::string name = fsutil::PathToUtf8(entry.path().filename());
            if (!fsutil::EndsWith(fsutil::ToLower(name), ".obj")) continue;
            // ⚠ Skip our own output, or a re-run would find `lamps_photon_led`
            // and generate a LED twin OF THE LED TWIN.
            if (name.find(kLedSuffix) != std::string::npos) continue;
            if (TextureLitOfFile(entry.path()) != want) continue;

            Fixture f;
            f.obj = name;
            f.ledObj = LedNameFor(name);
            f.stockLit = want;
            f.ledLit = LedNameFor(want);
            f.texture = texture;
            out.push_back(f);
        }
    }
    return out;
}

bool IsPatched(const std::string& objText) {
    return objText.find(kObjNeedle) != std::string::npos;
}

bool PatchText(const std::string& stockObj, Era era, const std::string& litName,
               const std::string& version, std::string& out,
               std::string& error) {
    error.clear();
    out.clear();
    if (IsPatched(stockObj)) {
        // ⚠ Not a warning to swallow. The installer sources from the backup
        // precisely so this cannot happen, so reaching it means the stock copy
        // was lost — and wrapping twice would hide the branch forever.
        error = "this OBJ already carries the integral-lighting gate";
        return false;
    }
    const Sections s = FindSections(stockObj);
    if (s.headerEnd == std::string::npos) {
        error = "no POINT_COUNTS line — not an OBJ8 file";
        return false;
    }
    if (s.drawStart == std::string::npos) {
        error = "no draw commands after the vertex tables";
        return false;
    }

    const std::string eol = DetectEol(stockObj);
    // The incandescent branch is value 0 and hides at 1; the LED branch is
    // value 1 and hides at 0. Two-valued gating, exactly the exterior form —
    // one hide per branch, at the OTHER branch's value.
    const char* hideAt = (era == Era::kLed) ? "0" : "1";

    std::string header = stockObj.substr(0, s.headerEnd);
    if (!litName.empty() && !SwapTextureLit(header, litName)) {
        error = "no TEXTURE_LIT line to repoint at " + litName;
        return false;
    }

    out.reserve(stockObj.size() + 512);
    out += header;
    out += marker::Line(version, "integral");
    out += eol;
    out += stockObj.substr(s.headerEnd, s.drawStart - s.headerEnd);
    out += "# ToLissPhoton integral lighting: this branch draws only when ";
    out += kDataRef;
    out += " != ";
    out += hideAt;
    out += " (";
    out += EraLabel(era);
    out += ")";
    out += eol;
    out += "ANIM_begin";
    out += eol;
    out += "\tANIM_hide\t";
    out += hideAt;
    out += "\t";
    out += hideAt;
    out += "\t";
    out += kDataRef;
    out += eol;
    out += stockObj.substr(s.drawStart);
    if (out.size() < eol.size() ||
        out.compare(out.size() - eol.size(), eol.size(), eol) != 0) {
        out += eol;
    }
    out += "ANIM_end";
    out += eol;
    return true;
}

}  // namespace integral
}  // namespace photon
