#include "reference_set.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

#include "core/detect.h"
#include "core/fsutil.h"
#include "core/image_io.h"
#include "core/lit_recolor.h"
#include "third_party/nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace photon {
namespace studio {
namespace {

constexpr char kIndexName[] = "index.json";

// Only the airframes whose atlas this tool understands. The A339 is out: its
// cockpit is a different mod entirely and it has no `lights_inn.obj`.
const char* kWanted[] = {"a319", "a320", "a321"};

bool IsWanted(const std::string& key) {
    for (const char* w : kWanted)
        if (key == w) return true;
    return false;
}

std::string PrettyLabel(const std::string& key) {
    if (key.size() == 4 && key[0] == 'a') return "A" + key.substr(1);
    return key;
}

bool ReadWhole(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    return true;
}

// ⚠ IS THIS TEXTURE ALREADY RECOLORED? Asked of the PIXELS, never of the install
// marker — the codebase's standing rule, and here it is load-bearing twice over.
// A folder can carry a Photon manifest while its textures have been restored to
// stock by hand or by a ToLiss update (exactly the state this machine was in on
// 2026-08-24), and conversely a manifest can be missing from a folder whose
// textures are ours. Harvesting a recolored file would feed the curve back
// through itself: blue is already clipped to zero, so a second pass is a no-op
// that silently freezes the recolor in as "stock".
//
// The test is the one thing the recolor always does: it drives the placard
// amber's blue channel to 0. Stock sits near 89. Sampled outside the preserve
// rects, since those keep their stock colors either way.
bool Covers(const std::vector<lit::Rect>& rects, int x, int y) {
    for (const lit::Rect& r : rects)
        if (y >= r.y0 && y < r.y1 && x >= r.x0 && x < r.x1) return true;
    return false;
}

bool PixelIsLit(const std::uint8_t* p) { return p[0] + p[1] + p[2] > 24; }

// What fraction of comparable pixels disagree about whether there is ARTWORK
// here at all? This is how "did ToLiss redraw this texture?" is asked, because
// it is the one question a recolor cannot change the answer to.
//
// ⚠ BLACKOUT RECTS ARE EXCLUDED, or the knobs would always look redrawn: turning
// 82,778 pixels off is 1.97% of that atlas and is the whole point of the spec.
// Outside them the margin is wide — our own recolors shift the footprint by
// ~0.007% of the placard atlas (antialiased edges the curve rounds to black),
// where ToLiss's A321 trim-scale redraw shifts it by 0.10%.
double ArtworkDelta(const lit::Image& a, const lit::Image& b,
                    const lit::Spec& spec) {
    if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height)
        return 0.0;
    const int ref = spec.referenceSize > 0 ? spec.referenceSize
                                           : lit::kReferenceSize;
    std::vector<lit::Rect> skip;
    skip.reserve(spec.blackout.size());
    for (const lit::Rect& r : spec.blackout) {
        lit::Rect s;
        s.x0 = static_cast<int>(static_cast<long long>(r.x0) * a.width / ref);
        s.x1 = static_cast<int>(static_cast<long long>(r.x1) * a.width / ref);
        s.y0 = static_cast<int>(static_cast<long long>(r.y0) * a.height / ref);
        s.y1 = static_cast<int>(static_cast<long long>(r.y1) * a.height / ref);
        skip.push_back(s);
    }
    std::size_t compared = 0, differ = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            if (!skip.empty() && Covers(skip, x, y)) continue;
            const std::size_t o = (static_cast<std::size_t>(y) * a.width + x) * 4;
            ++compared;
            if (PixelIsLit(&a.pixels[o]) != PixelIsLit(&b.pixels[o])) ++differ;
        }
    }
    if (compared == 0) return 0.0;
    return static_cast<double>(differ) / static_cast<double>(compared);
}

// Above this, call it a redraw and say so. Measured margin above.
constexpr double kArtworkRedrawThreshold = 0.0005;  // 0.05%

// Does the whole rect hold identical bytes in both images?
bool RectIdentical(const lit::Image& a, const lit::Image& b, const lit::Rect& r) {
    if (a.width != b.width || a.height != b.height) return false;
    for (int y = std::max(0, r.y0); y < std::min(a.height, r.y1); ++y) {
        const std::size_t off = static_cast<std::size_t>(y) * a.width * 4;
        const std::size_t x0 = static_cast<std::size_t>(std::max(0, r.x0)) * 4;
        const std::size_t x1 = static_cast<std::size_t>(std::min(a.width, r.x1)) * 4;
        if (x1 <= x0) continue;
        if (std::memcmp(a.pixels.data() + off + x0, b.pixels.data() + off + x0,
                        x1 - x0) != 0)
            return false;
    }
    return true;
}

}  // namespace

bool LooksRecolored(const lit::Image& img, lit::Texture texture) {
    // ⚠ FORWARDS. The test moved into core/lit_recolor on 2026-08-24, when
    // the INSTALLER became a caller: it must know whether the file in
    // objects/ is stock before deriving an era from it, and this bench is
    // dev-only and never linked into the installer. One statement of the rule,
    // in the module that owns the palette it is a statement about.
    return lit::LooksRecolored(img, texture);
}

const ReferenceTexture* ReferenceAirframe::Find(lit::Texture t) const {
    for (const ReferenceTexture& rt : textures)
        if (rt.texture == t) return &rt;
    return nullptr;
}

bool LoadReferenceSet(const fs::path& dir, ReferenceSet& out,
                      std::string& error) {
    const fs::path index = dir / kIndexName;
    std::string text;
    if (!ReadWhole(index, text)) {
        error = "no harvested reference set at " + dir.string() +
                " (run: photon-lit-studio harvest --xplane <X-Plane root>)";
        return false;
    }
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = index.string() + " is not valid JSON";
        return false;
    }
    // ⚠ A version-1 index holds only the placards, and nothing can invent the
    // knobs pair from it — so this says so plainly rather than half-loading and
    // leaving every knobs operation mysteriously absent.
    if (j.value("version", 1) < 2) {
        error = "the harvested set at " + dir.string() +
                " is version 1 (placards only). Re-run: photon-lit-studio "
                "harvest --xplane <X-Plane root>  to pick up knobs_LIT.png";
        return false;
    }
    ReferenceSet set;
    set.root = dir;
    set.xplaneRoot = j.value("xplane_root", "");
    std::map<std::string, int> setUse;
    for (const json& a : j.value("airframes", json::array()))
        setUse[a.value("set", "")]++;
    for (const json& a : j.value("airframes", json::array())) {
        ReferenceAirframe ra;
        ra.key = a.value("key", "");
        ra.label = a.value("label", PrettyLabel(ra.key));
        ra.set = a.value("set", "");
        ra.shared = setUse[ra.set] > 1;
        std::error_code ec;
        for (const json& t : a.value("textures", json::array())) {
            ReferenceTexture rt;
            if (!lit::TextureFromKey(t.value("texture", ""), rt.texture))
                continue;  // a texture this build does not know about
            rt.albedo = dir / t.value("albedo", "");
            rt.lit = dir / t.value("lit", "");
            if (!fs::exists(rt.albedo, ec) || !fs::exists(rt.lit, ec)) {
                error = "the index names files that are not there (" +
                        rt.lit.string() + "); re-run harvest";
                return false;
            }
            ra.textures.push_back(std::move(rt));
        }
        if (ra.textures.empty()) {
            error = ra.label + " has no harvested textures; re-run harvest";
            return false;
        }
        set.airframes.push_back(std::move(ra));
    }
    if (set.airframes.empty()) {
        error = index.string() + " lists no airframes";
        return false;
    }
    out = std::move(set);
    return true;
}

fs::path FindReferenceDir(const fs::path& start) {
    std::error_code ec;
    fs::path dir = fs::absolute(start, ec);
    if (ec) dir = start;
    for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
        const fs::path candidate = dir / "reference" / "toliss";
        if (fs::exists(candidate / kIndexName, ec)) return candidate;
        const fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return fs::path();
}

fs::path FindGusTexture(const fs::path& start, lit::Texture texture) {
    std::error_code ec;
    fs::path dir = fs::absolute(start, ec);
    if (ec) dir = start;
    for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
        const fs::path candidate = dir / "reference" / "gus" / "textures" /
                                   lit::TextureLitName(texture);
        if (fs::exists(candidate, ec)) return candidate;
        const fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return fs::path();
}

bool HarvestReferenceSet(const std::string& xplaneRootUtf8,
                         const fs::path& outDir, std::string& report,
                         std::string& error) {
    auto say = [&report](const std::string& line) { report += line + "\n"; };

    const std::vector<detect::Aircraft> found =
        detect::DetectAircraft(xplaneRootUtf8);
    if (found.empty()) {
        error = "no ToLiss aircraft found under " + xplaneRootUtf8;
        return false;
    }

    // airframe key -> every texture pair we harvest from it
    struct TexBytes {
        bool present = false;
        std::string albedoBytes, litBytes;
    };
    struct Source {
        std::string key;
        TexBytes tex[lit::kTextureCount];
    };
    std::vector<Source> sources;
    std::set<std::string> seenKeys;
    for (const detect::Aircraft& ac : found) {
        if (!IsWanted(ac.airframe)) continue;
        if (!seenKeys.insert(ac.airframe).second) {
            say("  note: more than one " + PrettyLabel(ac.airframe) +
                " installed; using the first");
            continue;
        }
        Source s;
        s.key = ac.airframe;
        bool anyTexture = false;
        for (int ti = 0; ti < lit::kTextureCount; ++ti) {
            const lit::Texture tx = static_cast<lit::Texture>(ti);
            // ⚠ objects/, never Photon Backup Files/ — see the header.
            const fs::path albedo =
                fs::path(ac.path) / "objects" / lit::TextureDayName(tx);
            const fs::path litPath =
                fs::path(ac.path) / "objects" / lit::TextureLitName(tx);
            TexBytes tb;
            if (!ReadWhole(albedo, tb.albedoBytes) ||
                !ReadWhole(litPath, tb.litBytes)) {
                say("  skipped " + PrettyLabel(s.key) + " " +
                    lit::TextureName(tx) + ": cannot read " +
                    lit::TextureDayName(tx) + " / " + lit::TextureLitName(tx));
                continue;
            }
            // ⚠ Content, not the marker. An aircraft can carry a Photon manifest
            // and still hold stock textures (restored by hand, or replaced by a
            // ToLiss update); refusing on the marker would make this unusable in
            // exactly the state a tuning session starts from.
            lit::Image probe;
            std::string e;
            // ⚠ Qualified: `studio::LooksRecolored` now forwards to the core one
            // and both are reachable by ADL on a `lit::Image`, so an unqualified
            // call is ambiguous.
            if (image::LoadPng(litPath, probe, e) &&
                lit::LooksRecolored(probe, tx)) {
                say("  ⚠ skipped " + PrettyLabel(s.key) + " " +
                    lit::TextureName(tx) + ": its " + lit::TextureLitName(tx) +
                    " is already recolored. Restore the stock texture first.");
                continue;
            }
            tb.present = true;
            s.tex[ti] = std::move(tb);
            anyTexture = true;
        }
        if (!anyTexture) continue;
        if (!ac.photon.empty())
            say("  note: " + PrettyLabel(s.key) + " carries a Photon " +
                ac.photon + " marker, but its textures read as stock - taking "
                "them.");
        sources.push_back(std::move(s));
    }
    if (sources.empty()) {
        error = "found ToLiss aircraft but none with readable stock textures";
        return false;
    }

    // De-duplicate: airframes whose every harvested texture is byte-identical
    // become one set on disk.
    auto sameTextures = [](const Source& a, const Source& b) {
        for (int t = 0; t < lit::kTextureCount; ++t) {
            if (a.tex[t].present != b.tex[t].present) return false;
            if (!a.tex[t].present) continue;
            if (a.tex[t].albedoBytes != b.tex[t].albedoBytes ||
                a.tex[t].litBytes != b.tex[t].litBytes)
                return false;
        }
        return true;
    };
    std::vector<std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        bool placed = false;
        for (auto& g : groups) {
            if (sameTextures(sources[g.front()], sources[i])) {
                g.push_back(i);
                placed = true;
                break;
            }
        }
        if (!placed) groups.push_back({i});
    }

    std::error_code ec;
    fs::create_directories(outDir, ec);

    json index;
    index["version"] = 2;  // v2 = per-texture entries; v1 held placards only
    index["note"] =
        "ToLiss stock textures, harvested locally. NOT redistributable - "
        "this folder is gitignored on purpose. See README.md.";
    // Remembered so the window's "Apply to sim" needs no path prompt. A moved
    // install just means re-running harvest, which is already the answer to
    // every other way the set can go stale.
    index["xplane_root"] = xplaneRootUtf8;
    json airframes = json::array();
    // One per group per texture, for the invariant check below.
    std::vector<lit::Image> litImages[lit::kTextureCount];
    std::vector<std::string> groupIds;

    for (const auto& g : groups) {
        std::string id;
        for (std::size_t i : g) id += (id.empty() ? "" : "-") + sources[i].key;
        groupIds.push_back(id);
        const fs::path dir = outDir / id;
        fs::create_directories(dir, ec);
        const Source& rep = sources[g.front()];

        json textures = json::array();
        for (int ti = 0; ti < lit::kTextureCount; ++ti) {
            if (!rep.tex[ti].present) continue;
            const lit::Texture tx = static_cast<lit::Texture>(ti);
            const std::string dayName = lit::TextureDayName(tx);
            const std::string litName = lit::TextureLitName(tx);
            {
                std::ofstream a(dir / dayName,
                                std::ios::binary | std::ios::trunc);
                a.write(rep.tex[ti].albedoBytes.data(),
                        static_cast<std::streamsize>(
                            rep.tex[ti].albedoBytes.size()));
                std::ofstream l(dir / litName,
                                std::ios::binary | std::ios::trunc);
                l.write(rep.tex[ti].litBytes.data(),
                        static_cast<std::streamsize>(
                            rep.tex[ti].litBytes.size()));
                if (!a || !l) {
                    error = "cannot write into " + dir.string();
                    return false;
                }
            }
            json t;
            t["texture"] = lit::TextureKey(tx);
            t["albedo"] = id + "/" + dayName;
            t["lit"] = id + "/" + litName;
            textures.push_back(t);
            say("  " + id + "/" + litName + "  (" +
                std::to_string(rep.tex[ti].litBytes.size() / (1024 * 1024)) +
                " MB + " +
                std::to_string(rep.tex[ti].albedoBytes.size() / (1024 * 1024)) +
                " MB)");
            lit::Image img;
            std::string e;
            if (image::LoadPng(dir / litName, img, e))
                litImages[ti].push_back(std::move(img));
        }

        std::string label;
        for (std::size_t i : g)
            label += (label.empty() ? "" : " + ") + PrettyLabel(sources[i].key);
        say("  " + id + "/  <- " + label);
        for (std::size_t i : g) {
            json a;
            a["key"] = sources[i].key;
            a["label"] = PrettyLabel(sources[i].key);
            a["set"] = id;
            a["textures"] = textures;
            airframes.push_back(a);
        }
    }
    // Keep the dropdown in a sensible order rather than filesystem order.
    std::sort(airframes.begin(), airframes.end(),
              [](const json& a, const json& b) {
                  return a.value("key", "") < b.value("key", "");
              });
    index["airframes"] = airframes;

    {
        std::ofstream o(outDir / kIndexName, std::ios::trunc);
        if (!o) {
            error = "cannot write " + (outDir / kIndexName).string();
            return false;
        }
        o << index.dump(2) << "\n";
    }

    // ⚠ The invariant every shared profile rests on. If two harvested sets ever
    // differ inside a preserve rect or the promote region, one rect table can no
    // longer serve every airframe.
    // ⚠ A MODIFIED *DAY* TEXTURE IS INVISIBLE TO `LooksRecolored`, WHICH ONLY
    // EVER READS THE LIT FILE — and that is not hypothetical: Photon's own
    // interior install replaces `knobs.png`, where Gus changed nothing but the
    // alpha (roughness) channel, so its RGB is bit-identical to stock and no
    // amount of looking at colors would catch it. Harvested as stock, it would
    // become what `revert` restores.
    //
    // The signature is precise and needs no threshold: two airframes whose LIT
    // files agree byte for byte have the same artwork, so their day textures
    // should agree too. Where they do not, something replaced one of them.
    for (int ti = 0; ti < lit::kTextureCount; ++ti) {
        for (std::size_t a = 0; a < groups.size(); ++a) {
            for (std::size_t b = a + 1; b < groups.size(); ++b) {
                const Source& sa = sources[groups[a].front()];
                const Source& sb = sources[groups[b].front()];
                if (!sa.tex[ti].present || !sb.tex[ti].present) continue;
                if (sa.tex[ti].litBytes != sb.tex[ti].litBytes) continue;
                if (sa.tex[ti].albedoBytes == sb.tex[ti].albedoBytes) continue;
                const lit::Texture tx = static_cast<lit::Texture>(ti);
                say(std::string("  ⚠ ") + PrettyLabel(sa.key) + " and " +
                    PrettyLabel(sb.key) + " share an identical " +
                    lit::TextureLitName(tx) + " but their " +
                    lit::TextureDayName(tx) +
                    " differ - one of them is NOT stock (a mod replaced it). "
                    "Restore it from that aircraft's Photon Backup Files and "
                    "re-run harvest.");
            }
        }
    }

    bool invariantOk = true, invariantChecked = false;
    for (int ti = 0; ti < lit::kTextureCount; ++ti) {
        const std::vector<lit::Image>& imgs = litImages[ti];
        if (imgs.size() < 2) continue;
        invariantChecked = true;
        const lit::Texture tx = static_cast<lit::Texture>(ti);
        const lit::Spec spec = lit::SpecForProfile(lit::Profile::kIncandescent, tx);
        const std::string what = lit::TextureName(tx);
        for (std::size_t i = 1; i < imgs.size(); ++i) {
            for (std::size_t r = 0; r < spec.preserve.size(); ++r)
                if (!RectIdentical(imgs[0], imgs[i], spec.preserve[r])) {
                    say("  ⚠ " + what + " preserve rect '" +
                        std::string(lit::PreserveRectName(tx, r)) +
                        "' DIFFERS between " + groupIds[0] + " and " +
                        groupIds[i] + " - one profile can no longer serve both");
                    invariantOk = false;
                }
            // ⚠ The knobs rest on these two just as hard: a palette region whose
            // artwork moved would recolor the wrong knob, and a blackout whose
            // artwork moved would delete the wrong object.
            for (std::size_t r = 0; r < spec.region.size(); ++r)
                if (!RectIdentical(imgs[0], imgs[i], spec.region[r].rect)) {
                    say("  ⚠ " + what + " palette region '" +
                        std::string(lit::PaletteRegionName(tx, r)) +
                        "' differs between " + groupIds[0] + " and " +
                        groupIds[i]);
                    invariantOk = false;
                }
            for (std::size_t r = 0; r < spec.blackout.size(); ++r)
                if (!RectIdentical(imgs[0], imgs[i], spec.blackout[r])) {
                    say("  ⚠ " + what + " blackout rect '" +
                        std::string(lit::BlackoutRectName(tx, r)) +
                        "' differs between " + groupIds[0] + " and " +
                        groupIds[i]);
                    invariantOk = false;
                }
            for (const auto& p : spec.promote)
                if (!RectIdentical(imgs[0], imgs[i], p.rect)) {
                    say("  ⚠ " + what + " promote region differs between " +
                        groupIds[0] + " and " + groupIds[i]);
                    invariantOk = false;
                }
        }
    }
    if (invariantChecked && invariantOk)
        say("  checked: every authored rect on every texture is identical across "
            "sets, so one profile serves all " +
            std::to_string(airframes.size()) + " airframes");
    return true;
}

bool ApplyToAircraft(const std::string& xplaneRootUtf8, const ReferenceSet& set,
                     const SpecSet& specs, bool dryRun,
                     const std::string& onlyAirframe,
                     std::vector<ApplyResult>& out, std::string& error) {
    const std::vector<detect::Aircraft> found =
        detect::DetectAircraft(xplaneRootUtf8);
    if (found.empty()) {
        error = "no ToLiss aircraft found under " + xplaneRootUtf8;
        return false;
    }
    bool any = false;
    for (const detect::Aircraft& ac : found) {
        const ReferenceAirframe* ref = nullptr;
        for (const auto& a : set.airframes)
            if (a.key == ac.airframe) { ref = &a; break; }
        if (!ref) continue;  // an airframe the set does not cover (a339)
        if (!onlyAirframe.empty() && ac.airframe != onlyAirframe) continue;

        for (int ti = 0; ti < lit::kTextureCount; ++ti) {
            if (!specs.has[ti]) continue;
            const lit::Texture tx = static_cast<lit::Texture>(ti);
            const ReferenceTexture* rt = ref->Find(tx);

            ApplyResult r;
            r.label = ref->label;
            r.texture = lit::TextureName(tx);
            if (!rt) {
                r.note = "not in the harvested set - re-run harvest";
                out.push_back(r);
                continue;
            }
            const fs::path target =
                fs::path(ac.path) / "objects" / lit::TextureLitName(tx);
            r.path = target.string();

            // ⚠ Source = the HARVESTED stock, never the file being overwritten
            // — recoloring our own output would compound the curve, silently.
            lit::Image stockLit, albedo;
            std::string e;
            if (!image::LoadPng(rt->lit, stockLit, e)) {
                r.note = "cannot read the harvested set: " + e;
                out.push_back(r);
                continue;
            }
            const bool haveAlbedo = image::LoadPng(rt->albedo, albedo, e);

            // ⚠ Staleness is DETECTED AND REPORTED, NEVER ACTED ON. See the
            // header: this is a bench and refusing to write costs an iteration.
            // The question is whether ToLiss REDREW the texture since the
            // harvest, which is asked of the artwork — a byte compare cannot
            // answer it, because the file we are looking at is one we wrote.
            lit::Image current;
            if (image::LoadPng(target, current, e)) {
                const double delta = ArtworkDelta(current, stockLit,
                                                  specs.spec[ti]);
                if (delta > kArtworkRedrawThreshold) {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf),
                                  "artwork differs from the harvest on %.2f%% "
                                  "of pixels (ToLiss update?) - re-run harvest; "
                                  "applied anyway",
                                  delta * 100.0);
                    r.warning = buf;
                }
            }

            lit::Image work = stockLit;
            const std::size_t n =
                lit::Apply(work, haveAlbedo ? &albedo : nullptr,
                           specs.spec[ti]);
            if (dryRun) {
                r.note = "dry run - would change " + std::to_string(n) +
                         " px and write";
                out.push_back(r);
                any = true;
                continue;
            }
            if (!image::SavePng(target, work, e)) {
                r.note = e;
                out.push_back(r);
                continue;
            }
            r.wrote = true;
            r.note = std::to_string(n) + " px changed";
            out.push_back(r);
            any = true;
        }
    }
    if (!any && out.empty()) {
        error = "no installed aircraft matches the harvested set";
        return false;
    }
    return true;
}

bool RevertAircraft(const std::string& xplaneRootUtf8, const ReferenceSet& set,
                    const std::string& onlyAirframe,
                    std::vector<ApplyResult>& out, std::string& error) {
    const std::vector<detect::Aircraft> found =
        detect::DetectAircraft(xplaneRootUtf8);
    if (found.empty()) {
        error = "no ToLiss aircraft found under " + xplaneRootUtf8;
        return false;
    }
    for (const detect::Aircraft& ac : found) {
        const ReferenceAirframe* ref = nullptr;
        for (const auto& a : set.airframes)
            if (a.key == ac.airframe) { ref = &a; break; }
        if (!ref) continue;
        if (!onlyAirframe.empty() && ac.airframe != onlyAirframe) continue;

        for (int ti = 0; ti < lit::kTextureCount; ++ti) {
            const lit::Texture tx = static_cast<lit::Texture>(ti);
            const ReferenceTexture* rt = ref->Find(tx);
            if (!rt) continue;

            ApplyResult r;
            r.label = ref->label;
            r.texture = lit::TextureName(tx);
            const fs::path target =
                fs::path(ac.path) / "objects" / lit::TextureLitName(tx);
            r.path = target.string();

            std::string refBytes;
            if (!ReadWhole(rt->lit, refBytes)) {
                r.note = "cannot read the harvested stock copy";
                out.push_back(r);
                continue;
            }
            // ⚠ Revert DOES still hold back, unlike apply, and for a reason that
            // is not symmetric with it: writing the harvested bytes over a file
            // ToLiss has since redrawn is a DOWNGRADE wearing a "revert" label,
            // and unlike a re-apply it is not one more keystroke to undo — the
            // newer stock file is gone. So an aircraft whose current texture is
            // stock artwork we do not have a copy of is left exactly as it is.
            std::string curBytes;
            if (ReadWhole(target, curBytes) && curBytes == refBytes) {
                r.note = "already stock";
                out.push_back(r);
                continue;
            }
            lit::Image current, harvested;
            std::string e;
            if (image::LoadPng(target, current, e) &&
                !lit::LooksRecolored(current, tx) &&
                image::LoadPng(rt->lit, harvested, e)) {
                const lit::Spec spec = lit::SpecForProfile(
                    lit::Profile::kIncandescent, tx);
                if (ArtworkDelta(current, harvested, spec) >
                    kArtworkRedrawThreshold) {
                    r.note =
                        "already stock, and NEWER than the harvest - left alone";
                    out.push_back(r);
                    continue;
                }
            }
            std::ofstream o(target, std::ios::binary | std::ios::trunc);
            if (!o || !o.write(refBytes.data(),
                               static_cast<std::streamsize>(refBytes.size()))) {
                r.note = "cannot write " + target.string();
                out.push_back(r);
                continue;
            }
            r.wrote = true;
            r.note = "stock restored";
            out.push_back(r);
        }
    }
    return true;
}

}  // namespace studio
}  // namespace photon
