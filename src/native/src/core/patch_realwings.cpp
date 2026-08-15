#include "core/patch_realwings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "core/fsutil.h"
#include "third_party/nlohmann/json.hpp"

namespace photon {
namespace patch_realwings {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// Format a corrected coordinate: trim trailing zeros but keep RealWings' own
// precision. ⚠ NOT the DSL's 3-dp `fmt` — that is far too coarse for a position,
// and rounding one here would move a light the offset was meant to leave alone.
std::string FormatPos(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.7f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s.empty() ? "0" : s;
}

std::string RequireString(const json& j, const char* key, const std::string& where) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        throw RealWingsError("RealWings patch data: missing string '" +
                             std::string(key) + "' at " + where);
    }
    return it->get<std::string>();
}

// Numbers reach the OBJ as text, and the JSON may carry either form (the
// generator writes `index` as a number and `cone` as a string).
std::string ScalarToString(const json& v, const char* key,
                           const std::string& where) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) {
        std::ostringstream os;
        os << v.get<double>();
        return os.str();
    }
    throw RealWingsError("RealWings patch data: '" + std::string(key) +
                         "' is neither a string nor a number at " + where);
}

Record ParseRecord(const json& j, const std::string& where) {
    if (!j.is_object()) {
        throw RealWingsError("RealWings patch data: record is not an object at " +
                             where);
    }
    Record rec;
    rec.category = RequireString(j, "category", where);
    rec.lightClass = RequireString(j, "class", where);
    rec.dir = RequireString(j, "dir", where);
    rec.index = ScalarToString(j.at("index"), "index", where);

    const auto off = j.find("offset");
    if (off != j.end() && off->is_array() && off->size() == 3) {
        bool nonZero = false;
        for (std::size_t i = 0; i < 3; ++i) {
            rec.offset[i] = (*off)[i].is_number() ? (*off)[i].get<double>() : 0.0;
            if (rec.offset[i] != 0.0) nonZero = true;
        }
        // ⚠ An all-zero offset is the same thing as no offset: leave RealWings'
        // bytes alone. Collapsing them here means the applier has one case.
        rec.hasOffset = nonZero;
    }

    const auto branches = j.find("branches");
    if (branches == j.end() || !branches->is_array() || branches->empty()) {
        throw RealWingsError("RealWings patch data: no branches at " + where);
    }
    for (const json& b : *branches) {
        Branch br;
        br.hide = RequireString(b, "hide", where);
        br.rgb = RequireString(b, "rgb", where);
        br.cone = ScalarToString(b.at("cone"), "cone", where);
        const auto inten = b.find("intensity");
        br.intensity = (inten != b.end() && inten->is_number())
                           ? static_cast<long long>(std::llround(inten->get<double>()))
                           : 0;
        rec.branches.push_back(std::move(br));
    }
    return rec;
}

}  // namespace

PatchData ParseJson(const std::string& text, const std::string& where) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& e) {
        throw RealWingsError("RealWings patch data is corrupt: " + where + " (" +
                             e.what() + ")");
    }
    if (!root.is_object()) {
        throw RealWingsError("RealWings patch data is not an object: " + where);
    }

    const auto schema = root.find("schema");
    const int got = (schema != root.end() && schema->is_number())
                        ? schema->get<int>()
                        : -1;
    if (got != kSchema) {
        // ⚠ Refused, not best-effort. Reading fields this build does not
        // understand would emit LIGHT_PARAM lines built from defaults — a wingtip
        // light that is subtly wrong and looks installed.
        throw RealWingsError(
            "RealWings patch data is schema " + std::to_string(got) +
            ", this installer applies schema " + std::to_string(kSchema) + " (" +
            where + ") — the bundle and the installer are out of step");
    }

    PatchData data;
    data.sourceAirframe = RequireString(root, "sourceAirframe", where);

    const auto classes = root.find("classes");
    if (classes == root.end() || !classes->is_object() || classes->empty()) {
        throw RealWingsError("RealWings patch data has no 'classes': " + where);
    }
    for (auto it = classes->begin(); it != classes->end(); ++it) {
        BakedClass bc;
        const auto side = it.value().find("side");
        if (side != it.value().end() && side->is_string()) {
            bc.side = side->get<std::string>();
        }
        const auto path = it.value().find("path");
        if (path != it.value().end() && path->is_string()) {
            bc.path = path->get<std::string>();
        }
        data.classes[it.key()] = std::move(bc);
    }

    const auto frames = root.find("airframes");
    if (frames == root.end() || !frames->is_object()) {
        throw RealWingsError("RealWings patch data has no 'airframes': " + where);
    }
    for (auto af = frames->begin(); af != frames->end(); ++af) {
        for (auto cls = af.value().begin(); cls != af.value().end(); ++cls) {
            for (auto wt = cls.value().begin(); wt != cls.value().end(); ++wt) {
                for (auto sd = wt.value().begin(); sd != wt.value().end(); ++sd) {
                    const std::string at = af.key() + "/" + cls.key() + "/" +
                                           wt.key() + "/" + sd.key();
                    data.airframes[af.key()][cls.key()][wt.key()][sd.key()] =
                        ParseRecord(sd.value(), at);
                }
            }
        }
    }
    if (data.airframes.find(data.sourceAirframe) == data.airframes.end()) {
        throw RealWingsError("RealWings patch data names sourceAirframe '" +
                             data.sourceAirframe + "' but carries no records for "
                             "it: " + where);
    }
    return data;
}

PatchData Load(const std::string& jsonPathUtf8) {
    std::string text;
    if (!fsutil::ReadFileBytes(fsutil::PathFromUtf8(jsonPathUtf8), text)) {
        throw RealWingsError("RealWings patch data not found: " + jsonPathUtf8);
    }
    return ParseJson(text, jsonPathUtf8);
}

const Record& RecordFor(const PatchData& data, const std::string& airframe,
                        const std::string& cls, const std::string& wingtip,
                        const std::string& side) {
    auto af = data.airframes.find(airframe);
    if (af == data.airframes.end()) af = data.airframes.find(data.sourceAirframe);
    if (af == data.airframes.end()) {
        throw RealWingsError("RealWings patch data has no airframe '" + airframe +
                             "' and no fallback");
    }
    const auto c = af->second.find(cls);
    if (c == af->second.end()) {
        throw RealWingsError("RealWings patch data has no class '" + cls + "'");
    }
    const auto w = c->second.find(wingtip);
    if (w == c->second.end()) {
        throw RealWingsError("RealWings patch data has no wingtip '" + wingtip +
                             "' for '" + cls + "'");
    }
    const auto s = w->second.find(side);
    if (s == w->second.end()) {
        throw RealWingsError("RealWings patch data has no side '" + side +
                             "' for '" + cls + "/" + wingtip + "'");
    }
    return s->second;
}

std::string SideFor(const PatchData& data, const std::string& cls,
                    const std::string& x) {
    const auto it = data.classes.find(cls);
    if (it != data.classes.end() && !it->second.side.empty()) return it->second.side;
    return std::atof(x.c_str()) < 0.0 ? "port" : "starboard";
}

void ShiftPos(const Record& rec, std::string& x, std::string& y, std::string& z) {
    if (!rec.hasOffset) return;
    std::string* coords[3] = {&x, &y, &z};
    for (int i = 0; i < 3; ++i) {
        if (rec.offset[i] == 0.0) continue;   // untouched axis keeps its exact bytes
        *coords[i] = FormatPos(std::atof(coords[i]->c_str()) + rec.offset[i]);
    }
}

std::string PatchText(const std::string& text, const PatchData& data,
                      const std::string& airframe, const std::string& objStem,
                      int& changed) {
    changed = 0;
    const std::string nl = fsutil::DetectNewline(text);
    const std::vector<std::string> lines = fsutil::SplitLines(text);
    // RealWings ships CEO and NEO as separate OBJs (Main.obj vs MainNEO.obj); the
    // baked lights carry no marker, so the wingtip variant — which keys the DSL
    // `@realwings` offset (fence = CEO, sharklet = NEO) — is read from the filename.
    const std::string stem = fsutil::ToLower(objStem);
    const std::string wingtip =
        (stem.find("neo") != std::string::npos) ? "sharklet" : "fence";

    std::vector<std::string> out;
    out.reserve(lines.size() * 2);
    for (const std::string& line : lines) {
        const std::string stripped = fsutil::Trim(line);
        std::istringstream iss(stripped);
        std::vector<std::string> tok;
        std::string t;
        while (iss >> t) tok.push_back(t);

        const bool isBaked =
            tok.size() >= 5 && tok[0] == "LIGHT_PARAM" &&
            data.classes.find(tok[1]) != data.classes.end();
        if (!isBaked) {
            out.push_back(line);
            continue;
        }

        std::string indent;
        for (char c : line) {
            if (c == ' ' || c == '\t') {
                indent.push_back(c);
            } else {
                break;
            }
        }

        const std::string cls = tok[1];
        std::string x = tok[2], y = tok[3], z = tok[4];
        const std::string side = SideFor(data, cls, x);
        const Record& rec = RecordFor(data, airframe, cls, wingtip, side);
        ShiftPos(rec, x, y, z);

        out.push_back(indent + "#" + stripped);   // keep the original, commented

        std::string srcPath;
        const auto bc = data.classes.find(cls);
        if (bc != data.classes.end()) srcPath = bc->second.path;
        out.push_back(indent + kMarker + " — " + cls + " -> " + srcPath + " (" +
                      side + "); edit it in lights.style.phdsl / "
                      "lights.layout.phdsl");
        for (const Branch& br : rec.branches) {
            out.push_back(indent + "ANIM_begin");
            out.push_back(indent + "\tANIM_hide " + br.hide +
                          " ToLissPhoton/exterior/" + rec.category);
            out.push_back(indent + "\tLIGHT_PARAM " + rec.lightClass + " " + x +
                          " " + y + " " + z + " " + br.rgb + " " + rec.index + " " +
                          std::to_string(br.intensity) + "cd " + rec.dir + " " +
                          br.cone);
            out.push_back(indent + "ANIM_end");
        }
        ++changed;
    }
    return fsutil::JoinLines(out, nl);
}

namespace {

std::vector<fs::path> ObjsUnder(const fs::path& root,
                                const std::vector<std::string>& needles,
                                const progress::StepProgress& onProgress) {
    std::vector<fs::path> hits;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return hits;
    std::vector<fs::path> objs;
    std::vector<std::uintmax_t> sizes;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;
        const std::string name =
            fsutil::ToLower(fsutil::PathToUtf8(it->path().filename()));
        if (!fsutil::EndsWith(name, ".obj")) continue;
        std::error_code se;
        const std::uintmax_t sz = it->file_size(se);
        objs.push_back(it->path());
        sizes.push_back(se ? 0 : sz);
    }
    // Keep sizes beside their paths through the sort — the reported fraction has to
    // follow the order the files are actually read in.
    std::vector<std::size_t> order(objs.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&objs](std::size_t a, std::size_t b) { return objs[a] < objs[b]; });

    std::uintmax_t totalBytes = 0;
    for (std::uintmax_t s : sizes) totalBytes += s;

    std::uintmax_t readBytes = 0;
    for (std::size_t idx : order) {
        const fs::path& p = objs[idx];
        std::string text;
        const bool got = fsutil::ReadFileBytes(p, text);
        readBytes += sizes[idx];
        if (onProgress && totalBytes > 0) {
            onProgress(static_cast<double>(readBytes) /
                       static_cast<double>(totalBytes));
        }
        if (!got) continue;
        for (const std::string& n : needles) {
            if (text.find(n) != std::string::npos) {
                hits.push_back(p);
                break;
            }
        }
    }
    return hits;
}

}  // namespace

RunResult Run(const std::string& rootUtf8, const PatchData& data,
              const std::string& airframe, bool dryRun,
              const progress::StepProgress& onProgress) {
    RunResult result;
    const fs::path root = fsutil::PathFromUtf8(rootUtf8);
    std::error_code ec;
    if (!fs::exists(root, ec)) throw RealWingsError("not found: " + rootUtf8);

    std::vector<std::string> needles;
    needles.reserve(data.classes.size());
    for (const auto& kv : data.classes) needles.push_back(kv.first);

    // ⚠ THE SCAN IS ~90% OF THIS PHASE AND IT IS THE PART THAT ANIMATES. `ObjsUnder`
    // reads every `.obj` under the mod folder — 140.6 MB on the A320's RealWings320,
    // against roughly 14 MB of hits the loop below re-reads and rewrites. So the
    // scan is mapped over the step's first 0.9 and the patch loop is left as one
    // step at the end: sub-reporting a tenth of a 1.5 s phase is not worth threading
    // a second callback through the refresh/backup/write branches below.
    const std::vector<fs::path> objs =
        ObjsUnder(root, needles,
                  onProgress ? progress::StepProgress([&onProgress](double f) {
                      onProgress(f * 0.9);
                  })
                             : progress::StepProgress{});
    if (objs.empty()) {
        throw RealWingsError(
            "no RealWings OBJs with baked wingtip lights found under root");
    }

    for (const fs::path& p : objs) {
        std::string raw;
        if (!fsutil::ReadFileBytes(p, raw)) continue;
        const std::string name = fsutil::PathToUtf8(p.filename());

        // ⚠ An already-patched file is REFRESHED, not skipped: re-derive from the
        // pristine `.bak` so tuned values reach a mod patched under older ones.
        // Without this, re-running the patch (an installer upgrade, a dev rebuild)
        // left the stale patch in place. The `.bak` is the refresh SOURCE and is
        // never re-copied — that would clobber the true original with patched
        // content.
        const bool refreshing = raw.find(kMarker) != std::string::npos;
        // `+=` on a path appends without a separator and keeps the native
        // encoding — `p.string() + ".bak"` would mangle a non-ASCII folder name
        // on Windows.
        fs::path bak = p;
        bak += ".bak";
        if (refreshing) {
            std::error_code be;
            if (!fs::is_regular_file(bak, be) || be) {
                result.log.push_back("  = " + name +
                                     ": patched but no .bak to refresh from, skipping");
                continue;
            }
            std::string bakText;
            if (!fsutil::ReadFileBytes(bak, bakText)) continue;
            if (bakText.find(kMarker) != std::string::npos) {
                result.log.push_back("  = " + name +
                                     ": .bak itself contains a patch (?), skipping");
                continue;
            }
            raw = bakText;
        }

        int changed = 0;
        const std::string out =
            PatchText(raw, data, airframe, fsutil::PathToUtf8(p.stem()), changed);
        if (changed > 0 && !dryRun) {
            if (!refreshing) {
                std::error_code ce;
                fs::copy_file(p, bak, fs::copy_options::overwrite_existing, ce);
                if (ce) {
                    result.log.push_back("  ! " + name +
                                         ": could not write .bak, skipped");
                    continue;
                }
            }
            std::error_code we;
            if (!fsutil::AtomicWriteBytes(p, out, we)) {
                result.log.push_back("  ! " + name + ": could not write");
                continue;
            }
        }
        const char* verb = refreshing
                               ? (dryRun ? "would refresh" : "refreshed (from .bak)")
                               : (dryRun ? "would patch" : "patched");
        std::ostringstream line;
        line << "  " << (changed ? '~' : '.') << " " << name << ": " << verb << " "
             << changed << " wingtip light(s)";
        result.log.push_back(line.str());
        result.lights += changed;
        ++result.files;
    }
    if (onProgress) onProgress(1.0);
    std::ostringstream tail;
    tail << (dryRun ? "would patch " : "patched ") << result.lights
         << " light(s) across " << objs.size() << " file(s)";
    result.log.push_back(tail.str());
    return result;
}

RunResult RunReverse(const std::string& rootUtf8, bool dryRun) {
    RunResult result;
    const fs::path root = fsutil::PathFromUtf8(rootUtf8);
    std::error_code ec;
    if (!fs::exists(root, ec)) throw RealWingsError("not found: " + rootUtf8);

    std::vector<fs::path> baks;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;
        if (fsutil::EndsWith(fsutil::ToLower(
                fsutil::PathToUtf8(it->path().filename())), ".obj.bak")) {
            baks.push_back(it->path());
        }
    }
    std::sort(baks.begin(), baks.end());

    for (const fs::path& bak : baks) {
        fs::path orig = bak;
        orig.replace_extension();                // strip the trailing .bak
        const std::string name = fsutil::PathToUtf8(orig.filename());
        if (dryRun) {
            result.log.push_back("  < " + name + ": would restore from .bak");
            ++result.files;
            continue;
        }
        std::string data;
        if (!fsutil::ReadFileBytes(bak, data)) continue;
        std::error_code we;
        if (!fsutil::AtomicWriteBytes(orig, data, we)) {
            result.log.push_back("  ! " + name + ": could not restore");
            continue;
        }
        fs::remove(bak, we);
        result.log.push_back("  < " + name + ": restored from .bak");
        ++result.files;
    }
    std::ostringstream tail;
    tail << (dryRun ? "would restore " : "restored ") << result.files
         << " file(s)";
    result.log.push_back(tail.str());
    return result;
}

}  // namespace patch_realwings
}  // namespace photon
