// photon-lit-studio — the integral-lighting recolor bench.
//
// Two jobs, one binary. With a subcommand it runs headless (recolor a texture,
// or measure one against a reference); with no arguments it opens the Dear ImGui
// window for tuning a look by eye. Everything it can do headlessly is what the
// installer will eventually call `core/lit_recolor` for directly — the tool adds
// no color logic of its own, deliberately, so a look approved here is the look
// that ships.
//
// ⚠ DEV TOOL, NOT A SHIPPED ONE. It is built only under `-DPHOTON_TOOLS=ON`, is
// not staged into a bundle, and nothing in the release path refers to it.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/image_io.h"
#include "core/lit_recolor.h"
#include "tools/lit_studio/reference_set.h"

#if defined(PHOTON_LIT_STUDIO_GUI)
int RunStudioGui(const std::string& litPath, const std::string& albedoPath,
                 const std::string& referenceDir);
#endif

namespace {

namespace fs = std::filesystem;
using photon::lit::Image;
using photon::lit::Spec;

void Usage() {
    std::printf(
        "photon-lit-studio - ToLiss Photon integral-lighting recolor bench\n"
        "\n"
        "  photon-lit-studio                       open the tuning window\n"
        "  photon-lit-studio harvest  --xplane DIR [--out DIR]\n"
        "  photon-lit-studio apply    --xplane DIR [--spec F|--preset N] [--dry-run]\n"
        "  photon-lit-studio revert   --xplane DIR         put stock back\n"
        "  photon-lit-studio recolor  --lit F [--albedo F] --out F [options]\n"
        "  photon-lit-studio compare  --a F --b F [--rects]\n"
        "  photon-lit-studio preset   [--preset NAME]      print a spec as JSON\n"
        "\n"
        "options:\n"
        "  --aircraft DIR   take --lit/--albedo from DIR/objects/text{,_LIT}.png\n"
        "  --airframe KEY   take them from the harvested set instead (a319|a320|a321)\n"
        "  --preset NAME    incandescent (default) | led | stock\n"
        "  --spec FILE      a JSON spec, overriding --preset\n"
        "  --reference DIR  the harvested set (default: ../reference/toliss)\n"
        "  --quiet          errors only\n"
        "\n"
        "harvest copies each installed ToLiss A3xx's STOCK text.png/text_LIT.png\n"
        "into reference/toliss/, de-duplicating byte-identical airframes, so the\n"
        "window can load them from a dropdown. That folder is gitignored: the\n"
        "textures are ToLiss's payware, not ours to redistribute.\n"
        "\n"
        "apply recolors EVERY installed A3xx from its harvested stock and writes\n"
        "the result into the aircraft, so the next reload in X-Plane shows it.\n"
        "The GUI's 'Apply to sim' button runs the same thing with your live\n"
        "sliders. revert puts the harvested stock back.\n");
}

struct Args {
    std::string cmd;
    std::string lit, albedo, out, a, b, spec, preset = "incandescent", aircraft;
    std::string xplane, reference, airframe;
    // Which cockpit texture the single-file commands work on. `apply`/`revert`
    // always do every texture; this narrows `recolor`, `compare --rects` and the
    // window's starting selection.
    photon::lit::Texture texture = photon::lit::Texture::kPlacards;
    bool rects = false;
    bool quiet = false;
    bool dryRun = false;
};

// The harvested set, resolved from --reference, then by walking up from the
// executable. Empty when there is none.
fs::path ResolveReferenceDir(const Args& args, const char* argv0) {
    if (!args.reference.empty()) return fs::path(args.reference);
    std::error_code ec;
    fs::path here = fs::absolute(fs::path(argv0), ec).parent_path();
    if (ec) here = fs::current_path();
    fs::path found = photon::studio::FindReferenceDir(here);
    if (found.empty()) found = photon::studio::FindReferenceDir(fs::current_path());
    return found;
}

bool ParseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto need = [&](std::string& dst) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", s.c_str());
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (s == "-h" || s == "--help") {
            Usage();
            std::exit(0);
        } else if (s == "--rects") {
            args.rects = true;
        } else if (s == "--dry-run") {
            args.dryRun = true;
        } else if (s == "--quiet") {
            args.quiet = true;
        } else if (s == "--lit") {
            if (!need(args.lit)) return false;
        } else if (s == "--albedo") {
            if (!need(args.albedo)) return false;
        } else if (s == "--out") {
            if (!need(args.out)) return false;
        } else if (s == "--a") {
            if (!need(args.a)) return false;
        } else if (s == "--b") {
            if (!need(args.b)) return false;
        } else if (s == "--spec") {
            if (!need(args.spec)) return false;
        } else if (s == "--preset") {
            if (!need(args.preset)) return false;
        } else if (s == "--aircraft") {
            if (!need(args.aircraft)) return false;
        } else if (s == "--xplane") {
            if (!need(args.xplane)) return false;
        } else if (s == "--reference") {
            if (!need(args.reference)) return false;
        } else if (s == "--texture") {
            std::string key;
            if (!need(key)) return false;
            if (!photon::lit::TextureFromKey(key, args.texture)) {
                std::fprintf(stderr,
                             "unknown --texture '%s' (placards|knobs)\n",
                             key.c_str());
                return false;
            }
        } else if (s == "--airframe") {
            if (!need(args.airframe)) return false;
        } else if (!s.empty() && s[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", s.c_str());
            return false;
        } else if (args.cmd.empty()) {
            args.cmd = s;
        } else {
            std::fprintf(stderr, "unexpected argument %s\n", s.c_str());
            return false;
        }
    }
    // An aircraft folder is just a shorthand for the two paths inside it.
    if (!args.aircraft.empty()) {
        const fs::path root(args.aircraft);
        if (args.lit.empty())
            args.lit =
                (root / "objects" / photon::lit::TextureLitName(args.texture))
                    .string();
        if (args.albedo.empty())
            args.albedo =
                (root / "objects" / photon::lit::TextureDayName(args.texture))
                    .string();
    }
    return true;
}

// Fill --lit/--albedo from the harvested set when --airframe names one.
bool ResolveAirframe(Args& args, const char* argv0) {
    if (args.airframe.empty()) return true;
    const fs::path dir = ResolveReferenceDir(args, argv0);
    photon::studio::ReferenceSet set;
    std::string err;
    if (dir.empty() || !photon::studio::LoadReferenceSet(dir, set, err)) {
        std::fprintf(stderr, "%s\n",
                     err.empty() ? "no harvested reference set found"
                                 : err.c_str());
        return false;
    }
    for (const auto& a : set.airframes) {
        if (a.key != args.airframe) continue;
        const photon::studio::ReferenceTexture* rt = a.Find(args.texture);
        if (!rt) {
            std::fprintf(stderr, "%s has no harvested %s; re-run harvest\n",
                         a.label.c_str(),
                         photon::lit::TextureLitName(args.texture));
            return false;
        }
        if (args.lit.empty()) args.lit = rt->lit.string();
        if (args.albedo.empty()) args.albedo = rt->albedo.string();
        return true;
    }
    std::fprintf(stderr, "no airframe '%s' in %s\n", args.airframe.c_str(),
                 dir.string().c_str());
    return false;
}

bool SpecFor(const Args& args, photon::lit::Texture texture, Spec& spec) {
    if (!args.spec.empty()) {
        std::string text;
        {
            std::FILE* f = std::fopen(args.spec.c_str(), "rb");
            if (!f) {
                std::fprintf(stderr, "cannot open %s\n", args.spec.c_str());
                return false;
            }
            char buf[4096];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                text.append(buf, n);
            std::fclose(f);
        }
        std::string err;
        if (!photon::lit::FromJson(text, spec, err)) {
            std::fprintf(stderr, "%s: %s\n", args.spec.c_str(), err.c_str());
            return false;
        }
        return true;
    }
    // ⚠ `halogen` and `gus` STAY as aliases. The profile was called Halogen
    // until 2026-08-24 and the name is in saved scripts, shell history and the
    // spec JSONs of every tuning session so far; the rename is about what the
    // *user* is offered in the sim, and breaking a dev tool's spelling buys
    // nothing. `incandescent` is the canonical one and the only one advertised.
    if (args.preset == "incandescent" || args.preset == "halogen" ||
        args.preset == "gus") {
        spec = photon::lit::SpecForProfile(photon::lit::Profile::kIncandescent, texture);
    } else if (args.preset == "led") {
        spec = photon::lit::SpecForProfile(photon::lit::Profile::kLed, texture);
        if (!photon::lit::ProfileIsDefined(photon::lit::Profile::kLed) &&
            !args.quiet) {
            // ⚠ Kept, though it is unreachable while both profiles are defined.
            // It is the guard for the state this tool spent weeks in, and the
            // cost of a branch that never fires is nothing against a silent
            // substitution reading as the profile having been applied.
            std::fprintf(stderr,
                         "note: the LED profile is not defined yet - using "
                         "Incandescent\n");
        }
    } else if (args.preset == "stock") {
        spec = photon::lit::StockPassthrough(texture);
    } else {
        // ⚠ `led-seed` is GONE (2026-08-24). It was scaffolding for defining the
        // LED look, and now that `led` IS that look a second, untuned LED beside
        // it is a trap rather than a convenience.
        std::fprintf(stderr, "unknown preset '%s' (incandescent|led|stock)\n",
                     args.preset.c_str());
        return false;
    }
    return true;
}

int CmdHarvest(const Args& args, const char* argv0) {
    if (args.xplane.empty()) {
        std::fprintf(stderr, "harvest needs --xplane <X-Plane root>\n");
        return 2;
    }
    fs::path out = args.reference.empty() ? fs::path() : fs::path(args.reference);
    if (out.empty()) {
        out = ResolveReferenceDir(args, argv0);
        if (out.empty()) {
            // Nothing harvested yet, so nothing to find: put it beside the repo
            // this binary was built in.
            std::error_code ec;
            fs::path dir = fs::absolute(fs::path(argv0), ec).parent_path();
            for (int i = 0; i < 8 && !dir.empty(); ++i) {
                if (fs::exists(dir / "reference", ec)) {
                    out = dir / "reference" / "toliss";
                    break;
                }
                const fs::path p = dir.parent_path();
                if (p == dir) break;
                dir = p;
            }
        }
    }
    if (out.empty()) {
        std::fprintf(stderr,
                     "cannot work out where to put the set; pass --out or "
                     "--reference\n");
        return 2;
    }
    std::string report, err;
    std::printf("harvesting into %s\n", out.string().c_str());
    if (!photon::studio::HarvestReferenceSet(args.xplane, out, report, err)) {
        std::fputs(report.c_str(), stdout);
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::fputs(report.c_str(), stdout);
    std::printf("done - the window now opens with an airframe dropdown\n");
    return 0;
}

int CmdRecolor(const Args& args) {
    if (args.lit.empty() || args.out.empty()) {
        std::fprintf(stderr, "recolor needs --lit and --out\n");
        return 2;
    }
    Spec spec;
    if (!SpecFor(args, args.texture, spec)) return 2;

    Image lit, albedo;
    std::string err;
    if (!photon::image::LoadPng(args.lit, lit, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    bool haveAlbedo = false;
    if (!args.albedo.empty()) {
        if (photon::image::LoadPng(args.albedo, albedo, err)) {
            haveAlbedo = true;
        } else {
            // Not fatal: a missing day texture costs the promoted artwork only.
            std::fprintf(stderr, "warning: %s (promoted artwork skipped)\n",
                         err.c_str());
        }
    }
    const std::size_t changed =
        photon::lit::Apply(lit, haveAlbedo ? &albedo : nullptr, spec);
    if (!photon::image::SavePng(args.out, lit, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!args.quiet) {
        std::error_code ec;
        const auto sz = fs::file_size(args.out, ec);
        std::printf("recolored %dx%d, %zu pixels changed -> %s (%.1f MB)\n",
                    lit.width, lit.height, changed, args.out.c_str(),
                    ec ? 0.0 : static_cast<double>(sz) / (1024.0 * 1024.0));
    }
    return 0;
}

int CmdCompare(const Args& args) {
    if (args.a.empty() || args.b.empty()) {
        std::fprintf(stderr, "compare needs --a and --b\n");
        return 2;
    }
    Image A, B;
    std::string err;
    if (!photon::image::LoadPng(args.a, A, err) ||
        !photon::image::LoadPng(args.b, B, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (A.width != B.width || A.height != B.height) {
        std::fprintf(stderr, "size mismatch: %dx%d vs %dx%d\n", A.width,
                     A.height, B.width, B.height);
        return 1;
    }
    const std::size_t n = static_cast<std::size_t>(A.width) * A.height;
    std::size_t diff = 0, alphaDiff = 0;
    std::size_t within[5] = {0, 0, 0, 0, 0};
    const int kBands[5] = {0, 1, 2, 4, 8};
    long long sum = 0;
    int worst = 0;
    std::size_t worstAt = 0;
    for (std::size_t i = 0; i < n; ++i) {
        int d = 0;
        for (int c = 0; c < 3; ++c) {
            const int e = std::abs(static_cast<int>(A.pixels[i * 4 + c]) -
                                   static_cast<int>(B.pixels[i * 4 + c]));
            if (e > d) d = e;
        }
        if (A.pixels[i * 4 + 3] != B.pixels[i * 4 + 3]) ++alphaDiff;
        if (d) ++diff;
        sum += d;
        if (d > worst) {
            worst = d;
            worstAt = i;
        }
        for (int k = 0; k < 5; ++k)
            if (d <= kBands[k]) ++within[k];
    }
    std::printf("compare %s\n     vs %s\n", args.a.c_str(), args.b.c_str());
    std::printf("  %dx%d, RGB differs on %zu px (%.4f%%), alpha on %zu px\n",
                A.width, A.height, diff, 100.0 * diff / n, alphaDiff);
    std::printf("  mean |err| %.4f, worst %d at (%zu,%zu)\n",
                static_cast<double>(sum) / n, worst,
                worstAt % A.width, worstAt / A.width);
    for (int k = 0; k < 5; ++k)
        std::printf("  within %d levels: %.4f%%\n", kBands[k],
                    100.0 * within[k] / n);
    if (args.rects) {
        const auto& rects = photon::lit::PreserveRects(args.texture);
        for (std::size_t r = 0; r < rects.size(); ++r) {
            const auto& q = rects[r];
            std::size_t bad = 0, tot = 0;
            for (int y = q.y0; y < q.y1 && y < A.height; ++y)
                for (int x = q.x0; x < q.x1 && x < A.width; ++x) {
                    const std::size_t i =
                        (static_cast<std::size_t>(y) * A.width + x) * 4;
                    ++tot;
                    for (int c = 0; c < 3; ++c)
                        if (A.pixels[i + c] != B.pixels[i + c]) {
                            ++bad;
                            break;
                        }
                }
            std::printf("  preserve[%zu] %-34s %zu/%zu differ\n", r,
                        photon::lit::PreserveRectName(args.texture, r), bad, tot);
        }
    }
    return 0;
}

int CmdPreset(const Args& args) {
    Spec spec;
    if (!SpecFor(args, args.texture, spec)) return 2;
    std::printf("%s\n", photon::lit::ToJson(spec).c_str());
    return 0;
}

int CmdApplyOrRevert(const Args& args, const char* argv0, bool revert) {
    if (args.xplane.empty()) {
        std::fprintf(stderr, "%s needs --xplane <X-Plane root>\n",
                     revert ? "revert" : "apply");
        return 2;
    }
    const fs::path dir = ResolveReferenceDir(args, argv0);
    photon::studio::ReferenceSet set;
    std::string err;
    if (dir.empty() || !photon::studio::LoadReferenceSet(dir, set, err)) {
        std::fprintf(stderr, "%s\n",
                     err.empty() ? "no harvested reference set found - run "
                                   "harvest first"
                                 : err.c_str());
        return 1;
    }
    std::vector<photon::studio::ApplyResult> results;
    bool ok;
    if (revert) {
        ok = photon::studio::RevertAircraft(args.xplane, set, args.airframe,
                                            results, err);
    } else {
        // ⚠ EVERY texture, not `args.texture`. A half-applied cockpit is the
        // failure this whole subsystem exists to prevent, and an `apply` that
        // silently did one file because a flag defaulted would be exactly that.
        // ⚠ A hand-written --spec is a spec for ONE texture; it is routed by the
        // reference size it carries rather than being pushed onto both, which
        // would put 4096-authored rects on the 2048 atlas.
        photon::studio::SpecSet push;
        if (!args.spec.empty()) {
            Spec one;
            if (!SpecFor(args, args.texture, one)) return 2;
            photon::lit::Texture target = args.texture;
            for (int i = 0; i < photon::lit::kTextureCount; ++i) {
                const auto t = static_cast<photon::lit::Texture>(i);
                if (photon::lit::TextureReferenceSize(t) == one.referenceSize)
                    target = t;
            }
            push.Set(target, one);
            if (!args.quiet)
                std::printf("  (--spec applies to %s only)\n",
                            photon::lit::TextureName(target));
        } else {
            for (int i = 0; i < photon::lit::kTextureCount; ++i) {
                const auto t = static_cast<photon::lit::Texture>(i);
                Spec one;
                if (!SpecFor(args, t, one)) return 2;
                push.Set(t, one);
            }
        }
        ok = photon::studio::ApplyToAircraft(args.xplane, set, push,
                                             args.dryRun, args.airframe,
                                             results, err);
    }
    for (const auto& r : results) {
        std::printf("  %-5s %-9s %s  (%s)\n", r.label.c_str(),
                    r.texture.c_str(), r.wrote ? "written" : "-",
                    r.note.c_str());
        if (!r.warning.empty())
            std::printf("        ⚠ %s\n", r.warning.c_str());
    }
    if (!ok) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!revert && !args.dryRun)
        std::printf("done - reload the aircraft in X-Plane to see it "
                    "(Developer > Reload the Current Aircraft)\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, args)) return 2;
    const char* self = argc > 0 ? argv[0] : "";

    if (args.cmd.empty()) {
#if defined(PHOTON_LIT_STUDIO_GUI)
        const fs::path ref = ResolveReferenceDir(args, self);
        return RunStudioGui(args.lit, args.albedo, ref.string());
#else
        Usage();
        std::fprintf(stderr,
                     "\n(this build has no window; it was compiled without "
                     "PHOTON_LIT_STUDIO_GUI)\n");
        return 2;
#endif
    }
    if (args.cmd == "harvest") return CmdHarvest(args, self);
    if (args.cmd == "apply") return CmdApplyOrRevert(args, self, false);
    if (args.cmd == "revert") return CmdApplyOrRevert(args, self, true);
    if (!ResolveAirframe(args, self)) return 2;
    if (args.cmd == "recolor") return CmdRecolor(args);
    if (args.cmd == "compare") return CmdCompare(args);
    if (args.cmd == "preset") return CmdPreset(args);
    std::fprintf(stderr, "unknown command '%s'\n", args.cmd.c_str());
    Usage();
    return 2;
}
