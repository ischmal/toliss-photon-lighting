// The photon-lit-studio tuning window.
//
// Everything here is presentation. The recolor itself is `core/lit_recolor`,
// called exactly the way the installer will call it — this file owns no color
// math of its own, so a look approved by eye here is reproducible by the
// headless `recolor` subcommand and, later, by an install.
//
// Two views, because a texture recolor fails in two different ways. The OVERVIEW
// is a downsampled proxy: fast enough to follow a slider, and the right scale for
// judging a palette. The DETAIL inspector is a full-resolution crop recolored
// through the same call with a `Viewport` — the only way to see what actually
// happens at a glyph edge or along an exclusion boundary, where a proxy's box
// filter has already averaged the answer away.
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/image_io.h"
#include "core/lit_recolor.h"
#include "imgui.h"
#include "imgui_win32_gl.h"
#include "tools/lit_studio/reference_set.h"

namespace {

namespace lit = photon::lit;
using photon::studio::TextureId;
using photon::studio::Window;

// The overview proxy edge. 1024 from a 4096 atlas is one recolor of ~1 M pixels
// per parameter change — a few milliseconds, so dragging stays live.
constexpr int kProxyEdge = 1024;
// The detail crop edge, in atlas pixels, recolored at 1:1.
constexpr int kDetailEdge = 384;

// ⚠ THE ORDER IS WIRE — the view radios pass their index straight through a
// cast, the way the settings window's tab enum does.
enum class View { kRecolored, kStock, kAlbedo, kGus, kDiff };

// Is there artwork at this pixel at all? A LIT texture is emissive over black,
// so "lit" versus "black" is the closest thing it has to a coverage channel.
// Deliberately a low bar: the question is whether the artist drew anything
// here, not how bright it is.
inline bool IsLit(const std::uint8_t* px) {
    return px[0] + px[1] + px[2] > 24;
}

struct Texture {
    TextureId id = 0;
    int w = 0, h = 0;

    void Ensure(int width, int height) {
        if (id && (w != width || h != height)) {
            Window::DestroyTexture(id);
            id = 0;
        }
        w = width;
        h = height;
    }
};

std::string OpenFileDialog(const char* title, HWND owner) {
    char buf[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "PNG textures\0*.png\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return std::string();
    return std::string(buf);
}

std::string SaveFileDialog(const char* title, const char* defExt, HWND owner) {
    char buf[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "PNG textures\0*.png\0JSON\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameA(&ofn)) return std::string();
    return std::string(buf);
}

// Box-filter downsample by an integer factor. Averaging in 8-bit sRGB is not
// colorimetrically correct, but it matches what the eye is being asked to judge
// here (and the detail view is the authority anyway).
lit::Image Downsample(const lit::Image& src, int factor) {
    lit::Image out;
    if (!src.Valid() || factor < 1) return out;
    out.width = src.width / factor;
    out.height = src.height / factor;
    if (out.width <= 0 || out.height <= 0) return out;
    out.pixels.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    const int f2 = factor * factor;
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            int acc[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < factor; ++dy) {
                const std::uint8_t* row =
                    src.pixels.data() +
                    (static_cast<std::size_t>(y * factor + dy) * src.width +
                     x * factor) * 4;
                for (int dx = 0; dx < factor; ++dx)
                    for (int c = 0; c < 4; ++c) acc[c] += row[dx * 4 + c];
            }
            std::uint8_t* d = out.pixels.data() +
                              (static_cast<std::size_t>(y) * out.width + x) * 4;
            for (int c = 0; c < 4; ++c)
                d[c] = static_cast<std::uint8_t>(acc[c] / f2);
        }
    }
    return out;
}

lit::Image Crop(const lit::Image& src, int x0, int y0, int w, int h) {
    lit::Image out;
    if (!src.Valid()) return out;
    out.width = w;
    out.height = h;
    out.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        const int sy = y0 + y;
        if (sy < 0 || sy >= src.height) continue;
        for (int x = 0; x < w; ++x) {
            const int sx = x0 + x;
            if (sx < 0 || sx >= src.width) continue;
            std::memcpy(&out.pixels[(static_cast<std::size_t>(y) * w + x) * 4],
                        &src.pixels[(static_cast<std::size_t>(sy) * src.width +
                                     sx) * 4],
                        4);
        }
    }
    return out;
}

// A LIT texture is emissive over black: shown as-is on a dark page it reads far
// dimmer than it does in the cockpit. The preview composites it over a flat
// backdrop and lets alpha through untouched so what is on screen is the RGB the
// sim will actually add.
void FlattenForDisplay(lit::Image& img) {
    for (std::size_t i = 0; i + 3 < img.pixels.size(); i += 4)
        img.pixels[i + 3] = 255;
}

struct App {
    // --- source data ---------------------------------------------------------
    lit::Image stockFull, albedoFull, referenceFull;
    lit::Image stockProxy, albedoProxy, referenceProxy;
    std::string litPath, albedoPath, referencePath;
    std::string status = "Load a stock text_LIT.png to begin.";
    int proxyFactor = 4;

    // --- the look ------------------------------------------------------------
    // ⚠ ONE SPEC PER TEXTURE, ALL LIVE AT ONCE. The tuning session edits
    // whichever is selected, but "Apply to sim" pushes every one of them: the
    // cockpit is not half a look, and a button that wrote only the texture that
    // happened to be on screen would leave the other one silently a version
    // behind — which is exactly the class of bug this subsystem exists to end.
    lit::Spec specs[lit::kTextureCount];
    bool edited[lit::kTextureCount] = {false, false};
    lit::Texture texture = lit::Texture::kPlacards;
    lit::Profile profile = lit::Profile::kIncandescent;
    bool dirty = true;

    App() {
        for (int i = 0; i < lit::kTextureCount; ++i)
            specs[i] = lit::SpecForProfile(lit::Profile::kIncandescent,
                                           static_cast<lit::Texture>(i));
    }

    lit::Spec& spec() { return specs[static_cast<int>(texture)]; }
    const lit::Spec& spec() const { return specs[static_cast<int>(texture)]; }
    bool& editedHere() { return edited[static_cast<int>(texture)]; }
    // The atlas the current spec's rects are authored against — 4096 or 2048.
    float refSize() const {
        return static_cast<float>(spec().referenceSize > 0
                                      ? spec().referenceSize
                                      : lit::kReferenceSize);
    }

    // --- the harvested reference set -----------------------------------------
    photon::studio::ReferenceSet refSet;
    std::string refError;
    int airframeIndex = -1;

    // --- view state ----------------------------------------------------------
    View view = View::kRecolored;
    float zoom = 1.0f;
    ImVec2 pan{0.0f, 0.0f};
    bool showRects = true;
    int detailX = 3880, detailY = 2145;  // opens on PEDAL DISC
    bool detailDirty = true;

    Texture overview, detail;
    lit::Image overviewBuf, detailBuf;

    // --- measurements --------------------------------------------------------
    std::string compareText = "(Gus's texture not found)";
    // Set with `dirty`, but acted on only once no widget is being held — see
    // RecomputeCompare for why the measurement cannot ride the proxy.
    bool compareDirty = true;
    // Did the comparison come from the vendored file rather than a dialog?
    bool gusAuto = false;

    bool HaveSource() const { return stockFull.Valid(); }

    // Load the harvested index and select an airframe, if there is one.
    void OpenReferenceSet(const std::string& dir) {
        if (dir.empty()) {
            refError =
                "no harvested set found - run: photon-lit-studio harvest "
                "--xplane <X-Plane root>";
            // Gus's file is committed and independent of the harvest, so it is
            // still worth finding — the comparison works on a hand-loaded stock
            // texture too.
            LoadGusFor(texture);
            return;
        }
        if (!photon::studio::LoadReferenceSet(dir, refSet, refError)) return;
        refError.clear();
        if (!refSet.airframes.empty()) SelectAirframe(0);
    }

    void SelectAirframe(int index) {
        if (index < 0 || index >= static_cast<int>(refSet.airframes.size()))
            return;
        airframeIndex = index;
        LoadSelected();
    }

    void SelectTexture(lit::Texture t) {
        texture = t;
        // The detail inspector's coordinates are atlas coordinates, and the two
        // atlases are different sizes — so a crop that was over the pedestal at
        // 4096 is off the edge at 2048. Park it on something worth seeing.
        if (!spec().region.empty()) {
            detailX = spec().region.front().rect.x0;
            detailY = spec().region.front().rect.y0;
        } else if (!spec().preserve.empty()) {
            detailX = spec().preserve.front().x0;
            detailY = spec().preserve.front().y0;
        }
        LoadSelected();
    }

    // Load the current airframe + texture pair out of the harvested set.
    void LoadSelected() {
        if (airframeIndex < 0 ||
            airframeIndex >= static_cast<int>(refSet.airframes.size()))
            return;
        const auto& a = refSet.airframes[airframeIndex];
        const photon::studio::ReferenceTexture* rt = a.Find(texture);
        if (!rt) {
            status = std::string(lit::TextureName(texture)) +
                     " is not in the harvested set - re-run harvest";
            return;
        }
        LoadLit(rt->lit.string());
        LoadAlbedo(rt->albedo.string());
        LoadGusFor(texture);
        status = "Loaded stock " + a.label + " " + lit::TextureName(texture) +
                 (a.shared ? " (shared set " + a.set + ")" : "");
    }

    // Reset the palette to a profile's own values, on EVERY texture — the
    // profile names an era, and an era that only reached half the cockpit is
    // the bug, not a feature. ⚠ Keeps the geometry the user may have edited: the
    // profile is a palette, not the whole spec.
    void ApplyProfile(lit::Profile p) {
        profile = p;
        for (int i = 0; i < lit::kTextureCount; ++i) {
            const lit::Spec fresh =
                lit::SpecForProfile(p, static_cast<lit::Texture>(i));
            specs[i].palette = fresh.palette;
            // ⚠ A region's own palette is part of the LOOK, not the geometry, so
            // a profile change has to reach it too. Left behind, the icy-blue
            // knob would keep its incandescent curve through a switch to LED and be
            // the one lamp on the panel that did not change.
            for (std::size_t r = 0;
                 r < specs[i].region.size() && r < fresh.region.size(); ++r) {
                if (!fresh.region[r].ownPalette) continue;
                specs[i].region[r].ownPalette = true;
                specs[i].region[r].palette = fresh.region[r].palette;
            }
            edited[i] = false;
        }
        dirty = detailDirty = true;
    }

    // Gus's own file for a texture, found with no dialog.
    void LoadGusFor(lit::Texture t) {
        const std::filesystem::path base =
            refSet.root.empty() ? std::filesystem::current_path() : refSet.root;
        const std::filesystem::path p =
            photon::studio::FindGusTexture(base, t);
        referenceFull = lit::Image{};
        referenceProxy = lit::Image{};
        referencePath.clear();
        gusAuto = false;
        if (p.empty()) return;
        LoadReference(p.string());
        gusAuto = true;
    }

    void LoadLit(const std::string& path) {
        std::string err;
        lit::Image img;
        if (!photon::image::LoadPng(path, img, err)) {
            status = err;
            return;
        }
        stockFull = std::move(img);
        litPath = path;
        proxyFactor = std::max(1, stockFull.width / kProxyEdge);
        stockProxy = Downsample(stockFull, proxyFactor);
        dirty = detailDirty = true;
        status = "Loaded " + path + " (" + std::to_string(stockFull.width) +
                 "x" + std::to_string(stockFull.height) + ")";
    }

    void LoadAlbedo(const std::string& path) {
        std::string err;
        lit::Image img;
        if (!photon::image::LoadPng(path, img, err)) {
            status = err;
            return;
        }
        albedoFull = std::move(img);
        albedoPath = path;
        albedoProxy = Downsample(albedoFull, std::max(1, albedoFull.width / kProxyEdge));
        dirty = detailDirty = true;
        status = "Loaded day texture " + path;
    }

    void LoadReference(const std::string& path) {
        std::string err;
        lit::Image img;
        if (!photon::image::LoadPng(path, img, err)) {
            status = err;
            return;
        }
        referenceFull = std::move(img);
        referencePath = path;
        referenceProxy =
            Downsample(referenceFull, std::max(1, referenceFull.width / kProxyEdge));
        dirty = true;
        status = "Loaded reference " + path;
    }

    // The overview image for the current view mode.
    void RebuildOverview() {
        if (!stockProxy.Valid()) return;
        lit::Image img;
        switch (view) {
            case View::kStock:
                img = stockProxy;
                break;
            case View::kAlbedo:
                // The day texture. Shown flat rather than composited: the point
                // of looking at it here is to find the artwork a promote region
                // is going to lift out of it.
                img = albedoProxy.Valid() ? albedoProxy : stockProxy;
                break;
            case View::kGus:
                img = referenceProxy.Valid() ? referenceProxy : stockProxy;
                break;
            case View::kDiff: {
                img = stockProxy;
                lit::Image rec = stockProxy;
                lit::Apply(rec, albedoProxy.Valid() ? &albedoProxy : nullptr, spec());
                const lit::Image& ref =
                    referenceProxy.Valid() ? referenceProxy : stockProxy;
                for (std::size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
                    int d = 0;
                    for (int c = 0; c < 3; ++c)
                        d = std::max(d, std::abs(static_cast<int>(rec.pixels[i + c]) -
                                                 static_cast<int>(ref.pixels[i + c])));
                    // x8 so a 1-2 level difference is visible at all; saturating
                    // rather than scaling keeps "identical" unambiguously black.
                    const std::uint8_t v =
                        static_cast<std::uint8_t>(std::min(255, d * 8));
                    // ⚠ TWO KINDS OF DISAGREEMENT, TWO COLORS. Where the stock
                    // atlas and Gus's file disagree about whether there is
                    // artwork here at all, the aircraft has been REDRAWN under
                    // him — his three white export bands everywhere, and on an
                    // A321 the whole trim-scale block, which ToLiss authored
                    // differently. Painting that red would make a correct
                    // palette look broken on one airframe and send someone
                    // hunting for a curve bug that is not there.
                    const bool artwork = IsLit(&stockProxy.pixels[i]) !=
                                         IsLit(&ref.pixels[i]);
                    img.pixels[i + 0] =
                        artwork ? static_cast<std::uint8_t>(v / 3) : v;
                    img.pixels[i + 1] = static_cast<std::uint8_t>(v / 3);
                    img.pixels[i + 2] =
                        artwork ? v : static_cast<std::uint8_t>(v / 3);
                }
                break;
            }
            case View::kRecolored:
            default:
                img = stockProxy;
                lit::Apply(img, albedoProxy.Valid() ? &albedoProxy : nullptr, spec());
                break;
        }
        FlattenForDisplay(img);
        overviewBuf = std::move(img);
        overview.Ensure(overviewBuf.width, overviewBuf.height);
        if (!overview.id)
            overview.id = Window::CreateTexture(overview.w, overview.h,
                                                overviewBuf.pixels.data());
        else
            Window::UpdateTexture(overview.id, overview.w, overview.h,
                                  overviewBuf.pixels.data());
        // One place to mark the measurement stale: this runs on exactly the
        // changes that could move it (palette, geometry, airframe).
        compareDirty = true;
    }

    void RebuildDetail() {
        if (!stockFull.Valid()) return;
        const int x0 = std::clamp(detailX, 0, std::max(0, stockFull.width - kDetailEdge));
        const int y0 = std::clamp(detailY, 0, std::max(0, stockFull.height - kDetailEdge));
        lit::Image crop = Crop(stockFull, x0, y0, kDetailEdge, kDetailEdge);
        if (view == View::kRecolored || view == View::kDiff) {
            lit::Image acrop;
            const lit::Image* ap = nullptr;
            if (albedoFull.Valid()) {
                acrop = Crop(albedoFull, x0, y0, kDetailEdge, kDetailEdge);
                ap = &acrop;
            }
            // ⚠ The Viewport is what makes a crop legal: without it the preserve
            // rects would be scaled to 384 px and land in the middle of it.
            lit::Viewport vp;
            vp.originX = x0;
            vp.originY = y0;
            vp.fullWidth = stockFull.width;
            vp.fullHeight = stockFull.height;
            lit::Apply(crop, ap, spec(), vp);
        } else if (view == View::kAlbedo && albedoFull.Valid()) {
            crop = Crop(albedoFull, x0, y0, kDetailEdge, kDetailEdge);
        } else if (view == View::kGus && referenceFull.Valid()) {
            crop = Crop(referenceFull, x0, y0, kDetailEdge, kDetailEdge);
        }
        FlattenForDisplay(crop);
        detailBuf = std::move(crop);
        detail.Ensure(detailBuf.width, detailBuf.height);
        if (!detail.id)
            detail.id = Window::CreateTexture(detail.w, detail.h,
                                              detailBuf.pixels.data());
        else
            Window::UpdateTexture(detail.id, detail.w, detail.h,
                                  detailBuf.pixels.data());
    }

    // How close is the current palette to Gus's finished file?
    //
    // ⚠ MEASURED AT FULL RESOLUTION, NEVER ON THE PROXY. The overview proxy is a
    // 4x4 box average, and averaging then curving is not curving then averaging:
    // scoring the proxy against Gus's downsampled file reported the error of
    // that reordering, not of the palette. It read 73.9% within 2 levels where
    // the truth is 95.7% — a number low enough to send someone tuning a curve
    // that was already right. A full pass costs ~0.1 s, which is why it runs
    // when a slider is RELEASED rather than every frame it moves.
    //
    // ⚠ PIXELS WHERE THE AIRCRAFT HAS BEEN REDRAWN UNDER HIM ARE EXCLUDED, NOT
    // SCORED. Gus worked from the A319/A320 atlas, and two things differ from it
    // through no fault of any palette: his export left three pure-white edge
    // bands we deliberately never create, and the A321's trim-scale and flap
    // artwork is ToLiss's own redraw. Counting either as palette error would put
    // a floor under the numbers that no amount of tuning could reach, and would
    // read as "the A321 is worse" when nothing about it is.
    void RecomputeCompare() {
        if (!referenceFull.Valid() || !stockFull.Valid() ||
            referenceFull.width != stockFull.width ||
            referenceFull.height != stockFull.height) {
            compareText = "(Gus's texture not found)";
            return;
        }
        lit::Image rec = stockFull;
        lit::Apply(rec, albedoFull.Valid() ? &albedoFull : nullptr, spec());
        const std::size_t n = static_cast<std::size_t>(rec.width) * rec.height;
        std::size_t within2 = 0, within8 = 0, scored = 0, artwork = 0;
        long long sum = 0;
        int worst = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (IsLit(&stockFull.pixels[i * 4]) !=
                IsLit(&referenceFull.pixels[i * 4])) {
                ++artwork;
                continue;
            }
            int d = 0;
            for (int c = 0; c < 3; ++c)
                d = std::max(d, std::abs(static_cast<int>(rec.pixels[i * 4 + c]) -
                                         static_cast<int>(
                                             referenceFull.pixels[i * 4 + c])));
            ++scored;
            sum += d;
            worst = std::max(worst, d);
            if (d <= 2) ++within2;
            if (d <= 8) ++within8;
        }
        if (scored == 0) {
            compareText = "(nothing comparable)";
            return;
        }
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "mean %.3f  worst %d\n"
                      "within 2: %.2f%%   within 8: %.2f%%\n"
                      "%.2f%% skipped as redrawn artwork",
                      static_cast<double>(sum) / scored, worst,
                      100.0 * within2 / scored, 100.0 * within8 / scored,
                      100.0 * artwork / n);
        compareText = buf;
    }
};

bool CurveEditor(const char* label, lit::Curve& c) {
    ImGui::PushID(label);  // ⚠ identical slider labels in three channels
    ImGui::SeparatorText(label);
    bool dirty = false;
    float black = static_cast<float>(c.black);
    float white = static_cast<float>(c.white);
    float gamma = static_cast<float>(c.gamma);
    // The blue channel's white point genuinely exceeds 255 in Gus's fit, so the
    // range has to allow it — see Curve's comment.
    if (ImGui::SliderFloat("black", &black, 0.0f, 200.0f, "%.2f")) {
        c.black = black;
        dirty = true;
    }
    if (ImGui::SliderFloat("white", &white, 64.0f, 512.0f, "%.2f")) {
        c.white = white;
        dirty = true;
    }
    if (ImGui::SliderFloat("gamma", &gamma, 0.20f, 3.0f, "%.3f")) {
        c.gamma = gamma;
        dirty = true;
    }
    if (c.white - c.black < 1.0) c.white = c.black + 1.0;

    std::uint8_t lut[256];
    lit::BuildLut(c, lut);
    float plot[256];
    for (int i = 0; i < 256; ++i) plot[i] = lut[i] / 255.0f;
    ImGui::PlotLines("##curve", plot, 256, 0, nullptr, 0.0f, 1.0f,
                     ImVec2(0, 60));
    ImGui::PopID();
    return dirty;
}

void Swatch(const std::uint8_t rgb[3], const char* id) {
    ImGui::ColorButton(id,
                       ImVec4(rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f,
                              1.0f),
                       ImGuiColorEditFlags_NoAlpha, ImVec2(44, 26));
}

// The pipeline, on one color, as three swatches.
//
// ⚠ THIS IS HERE BECAUSE "I CANNOT TELL WHAT THESE NUMBERS DO" IS THE PROBLEM
// THE PANEL EXISTS TO SOLVE. A black/white/gamma triple per channel is a fine
// way to STORE a measured tone curve and a miserable way to steer one by hand:
// there is no reading of the nine numbers that tells you which way "warmer" is.
// Stock -> curved -> tuned answers it in a glance, and it is the same code path
// the atlas runs, so the swatches cannot drift from the pixels.
void PipelineSwatches(const lit::Palette& pal, const std::uint8_t stock[3],
                      const char* label) {
    std::uint8_t lutR[256], lutG[256], lutB[256];
    lit::BuildLut(pal.r, lutR);
    lit::BuildLut(pal.g, lutG);
    lit::BuildLut(pal.b, lutB);
    const std::uint8_t curved[3] = {lutR[stock[0]], lutG[stock[1]],
                                    lutB[stock[2]]};
    std::uint8_t tuned[3] = {curved[0], curved[1], curved[2]};
    lit::ApplyHslStack(pal.hsl, tuned);

    ImGui::PushID(label);
    Swatch(stock, "##stock");
    ImGui::SameLine();
    Swatch(curved, "##curved");
    ImGui::SameLine();
    Swatch(tuned, "##tuned");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", label);
    ImGui::TextDisabled("stock %d,%d,%d  curve %d,%d,%d  tuned %d,%d,%d",
                        stock[0], stock[1], stock[2], curved[0], curved[1],
                        curved[2], tuned[0], tuned[1], tuned[2]);
    ImGui::PopID();
}

// The tuning stack.
//
// ⚠ THE PAGE LEADS WITH THIS AND COLLAPSES THE CURVES UNDER IT, which is a
// deliberate inversion of what the data says is primary. The Levels triple is
// the measurement and the stack is a departure from it — but the measurement is
// already correct and finished, and the departure is the thing a tuning session
// is actually for. Ordering the panel by what is being EDITED rather than by
// what is authoritative is the whole point.
bool HslStackEditor(std::vector<lit::HslOp>& ops) {
    bool dirty = false;
    ImGui::PushID("hsl");  // ⚠ identical slider labels in every row
    int remove = -1;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        lit::HslOp& op = ops[i];

        if (ImGui::Checkbox("##on", &op.enabled)) dirty = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        int kind = static_cast<int>(op.kind);
        const char* kinds[] = {"Adjust", "Colorize"};
        if (ImGui::Combo("##kind", &kind, kinds, 2)) {
            op.kind = static_cast<lit::HslOp::Kind>(kind);
            dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("remove")) remove = static_cast<int>(i);

        ImGui::BeginDisabled(!op.enabled);
        float hue = static_cast<float>(op.hue);
        float sat = static_cast<float>(op.saturation);
        float light = static_cast<float>(op.lightness);
        const bool colorize = op.kind == lit::HslOp::Kind::kColorize;
        if (ImGui::SliderFloat("hue", &hue, colorize ? 0.0f : -180.0f,
                               colorize ? 360.0f : 180.0f,
                               colorize ? "%.0f deg" : "%+.0f deg")) {
            op.hue = hue;
            dirty = true;
        }
        if (ImGui::SliderFloat("saturation", &sat, colorize ? 0.0f : -100.0f,
                               100.0f, colorize ? "%.0f" : "%+.0f")) {
            op.saturation = sat;
            dirty = true;
        }
        if (ImGui::SliderFloat("brightness", &light, -100.0f, 100.0f,
                               "%+.0f")) {
            op.lightness = light;
            dirty = true;
        }
        ImGui::EndDisabled();

        // ⚠ Say what brightness actually is. It is labelled for what it does on
        // an emissive atlas — a gain — and someone arriving from Photoshop's
        // Lightness slider would otherwise expect +50 to gray the background,
        // find it does not, and read that as the control being broken.
        if (colorize)
            ImGui::TextDisabled("forces one hue; black stays black");
        else
            ImGui::TextDisabled("brightness multiplies (0x at -100, 2x at +100)");
        ImGui::Separator();
        ImGui::PopID();
    }
    if (remove >= 0) {
        ops.erase(ops.begin() + remove);
        dirty = true;
    }

    if (ImGui::SmallButton("+ Adjust")) {
        ops.push_back(lit::HslOp{});
        dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Colorize")) {
        lit::HslOp op;
        op.kind = lit::HslOp::Kind::kColorize;
        // Land on the amber already on screen rather than on red at zero
        // saturation, which is gray and reads as the button having broken
        // something.
        op.hue = 25.0;
        op.saturation = 100.0;
        ops.push_back(op);
        dirty = true;
    }
    if (!ops.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear stack")) {
            ops.clear();
            dirty = true;
        }
    }
    ImGui::PopID();
    return dirty;
}

}  // namespace

int RunStudioGui(const std::string& litPath, const std::string& albedoPath,
                 const std::string& referenceDir) {
    Window win;
    std::string err;
    if (!win.Create("ToLiss Photon - integral lighting studio", 1500, 940, err)) {
        std::fprintf(stderr, "cannot open the tuning window: %s\n", err.c_str());
        return 1;
    }
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 2.0f;

    App app;
    // The harvested set first, so the dropdown is populated and an airframe is
    // already loaded; explicit paths then override it.
    app.OpenReferenceSet(referenceDir);
    if (!litPath.empty()) app.LoadLit(litPath);
    if (!albedoPath.empty()) app.LoadAlbedo(albedoPath);

    HWND hwnd = GetActiveWindow();

    while (win.Pump()) {
        win.BeginFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(
            ImVec2(static_cast<float>(win.Width()), static_cast<float>(win.Height())));
        ImGui::Begin("root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        // --- left column: the controls ---------------------------------------
        ImGui::BeginChild("controls", ImVec2(400, 0), ImGuiChildFlags_Borders);

        ImGui::SeparatorText("Aircraft");
        if (!app.refSet.airframes.empty()) {
            std::vector<const char*> labels;
            labels.reserve(app.refSet.airframes.size());
            for (const auto& a : app.refSet.airframes)
                labels.push_back(a.label.c_str());
            int sel = app.airframeIndex;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##airframe", &sel, labels.data(),
                             static_cast<int>(labels.size())))
                app.SelectAirframe(sel);
            if (sel >= 0 && app.refSet.airframes[sel].shared)
                ImGui::TextDisabled("stock textures shared with the rest of set '%s'",
                                    app.refSet.airframes[sel].set.c_str());
            else
                ImGui::TextDisabled("this airframe has its own stock textures");
        } else {
            ImGui::TextWrapped("%s", app.refError.c_str());
        }

        ImGui::SeparatorText("Texture");
        {
            int t = static_cast<int>(app.texture);
            const char* names[lit::kTextureCount];
            for (int i = 0; i < lit::kTextureCount; ++i)
                names[i] = lit::TextureName(static_cast<lit::Texture>(i));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##texture", &t, names, lit::kTextureCount))
                app.SelectTexture(static_cast<lit::Texture>(t));
            // ⚠ Say that the other one is still live. This selector changes what
            // is on SCREEN; Apply writes them all, and a user who reads it as
            // "the knobs are off" would tune the placards and wonder why the
            // knobs changed too.
            ImGui::TextDisabled("%s - editing one; Apply writes both",
                                lit::TextureLitName(app.texture));
        }

        ImGui::SeparatorText("Profile");
        {
            int p = static_cast<int>(app.profile);
            const char* names[] = {"Incandescent", "LED"};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##profile", &p, names, 2))
                app.ApplyProfile(static_cast<lit::Profile>(p));
            // ⚠ Say the fallback out loud. A silent substitution would read as
            // the LED profile having been applied and looking identical.
            if (!lit::ProfileIsDefined(app.profile))
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "LED is not defined yet - showing Incandescent");
            else if (app.editedHere())
                ImGui::TextDisabled("edited - no longer the stock %s palette",
                                    lit::ProfileName(app.profile));
            else
                ImGui::TextDisabled("the palette an install would apply");
            if (ImGui::SmallButton("Reset to profile")) app.ApplyProfile(app.profile);
            // ⚠ "Start from LED seed" is GONE (2026-08-24). It existed to give a
            // tuning session somewhere to begin while LED resolved to
            // incandescent;
            // now that LED is a real entry in the dropdown above, a button
            // loading a second, untuned LED beside it reads as two LED looks.
            ImGui::SameLine();
            if (ImGui::SmallButton("Identity")) {
                app.spec().palette = lit::StockPassthrough(app.texture).palette;
                app.editedHere() = true;
                app.dirty = app.detailDirty = true;
            }
        }

        ImGui::SeparatorText("Push to X-Plane");
        {
            const bool canApply = !app.refSet.airframes.empty() &&
                                  !app.refSet.xplaneRoot.empty();
            if (!canApply) {
                ImGui::TextDisabled(
                    "run harvest first - it records where X-Plane is");
            } else {
                // ⚠ One button writes ALL airframes. The whole point of the
                // shared profile is that the look is one decision; a per-
                // aircraft button would reintroduce the drift this exists to
                // end.
                if (ImGui::Button("Apply to sim (all aircraft)",
                                  ImVec2(-1, 0))) {
                    photon::studio::SpecSet push;
                    for (int i = 0; i < lit::kTextureCount; ++i)
                        push.Set(static_cast<lit::Texture>(i), app.specs[i]);
                    std::vector<photon::studio::ApplyResult> results;
                    std::string e;
                    if (photon::studio::ApplyToAircraft(
                            app.refSet.xplaneRoot, app.refSet, push,
                            /*dryRun=*/false, std::string(), results, e)) {
                        app.status.clear();
                        for (const auto& r : results) {
                            app.status += r.label + " " + r.texture + ": " +
                                          r.note;
                            if (!r.warning.empty())
                                app.status += "  [" + r.warning + "]";
                            app.status += "\n";
                        }
                        app.status +=
                            "Reload the aircraft in X-Plane to see it "
                            "(Developer > Reload the Current Aircraft).";
                    } else {
                        app.status = e;
                    }
                }
                if (ImGui::Button("Revert sim to stock", ImVec2(-1, 0))) {
                    std::vector<photon::studio::ApplyResult> results;
                    std::string e;
                    if (photon::studio::RevertAircraft(app.refSet.xplaneRoot,
                                                       app.refSet, std::string(),
                                                       results, e)) {
                        app.status.clear();
                        for (const auto& r : results)
                            app.status += r.label + ": " + r.note + "\n";
                    } else {
                        app.status = e;
                    }
                }
                ImGui::TextDisabled(
                    "writes every texture into every A3xx; then reload the\n"
                    "aircraft in X-Plane to see it");
            }
        }

        if (ImGui::CollapsingHeader("Other sources")) {
            if (ImGui::Button("Stock text_LIT.png...")) {
                const std::string p = OpenFileDialog("Stock text_LIT.png", hwnd);
                if (!p.empty()) app.LoadLit(p);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", app.litPath.empty() ? "(none)" : "loaded");
            if (ImGui::Button("Day text.png...")) {
                const std::string p = OpenFileDialog("Day text.png", hwnd);
                if (!p.empty()) app.LoadAlbedo(p);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", app.albedoPath.empty()
                                          ? "(none - PEDAL DISC off)"
                                          : "loaded");
            if (ImGui::Button("Comparison reference...")) {
                const std::string p =
                    OpenFileDialog("Reference text_LIT.png", hwnd);
                if (!p.empty()) {
                    app.LoadReference(p);
                    app.gusAuto = false;
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", app.referencePath.empty()
                                          ? "(none)"
                                          : (app.gusAuto ? "Gus (auto)" : "loaded"));
        }

        ImGui::SeparatorText("Tuning (hue / saturation / brightness)");
        {
            // The sample is ToLiss's own stock placard amber on the placards and
            // the stock index-marking salmon on the knobs — in each case the one
            // color that texture's whole job is about.
            static const std::uint8_t kPlacardStock[3] = {255, 150, 89};
            static const std::uint8_t kKnobStock[3] = {204, 130, 101};
            PipelineSwatches(app.spec().palette,
                             app.texture == lit::Texture::kKnobs ? kKnobStock
                                                                 : kPlacardStock,
                             app.texture == lit::Texture::kKnobs
                                 ? "index marking"
                                 : "placard amber");
            if (HslStackEditor(app.spec().palette.hsl))
                app.dirty = app.detailDirty = app.editedHere() = true;
            if (app.spec().palette.hsl.empty())
                ImGui::TextDisabled(
                    "empty - the look is Gus's measured curve, untouched");
        }

        // ⚠ COLLAPSED, AND UNDER THE STACK. These nine numbers are a MEASUREMENT
        // of Gus's file (see lit_recolor.h), not a set of taste knobs: they are
        // fitted, they are correct, and the tuning a session actually wants
        // belongs in the stack above. Leading with them is what made this panel
        // unusable to anyone who had not done the fit.
        if (ImGui::CollapsingHeader("Base curve - measured, advanced")) {
            ImGui::TextDisabled(
                "Levels fitted to Gus's own file: 838 of 840 colors\n"
                "map 1:1 and the channels are independent. An HSL\n"
                "model cannot express this, which is why both exist.");
            bool curveMoved = CurveEditor("Red", app.spec().palette.r);
            curveMoved |= CurveEditor("Green", app.spec().palette.g);
            curveMoved |= CurveEditor("Blue", app.spec().palette.b);
            if (curveMoved) app.dirty = app.detailDirty = app.editedHere() = true;
        }

        // ⚠ Only where there IS promoted artwork. The ink is meaningless on a
        // texture with no promote region, and a live color swatch labelled
        // PEDAL DISC on the knobs atlas reads as a control that does nothing.
        if (!app.spec().promote.empty()) {
            float ink[3] = {app.spec().palette.ink[0] / 255.0f,
                            app.spec().palette.ink[1] / 255.0f,
                            app.spec().palette.ink[2] / 255.0f};
            ImGui::SeparatorText("Promoted artwork");
            if (ImGui::ColorEdit3("PEDAL DISC ink", ink)) {
                for (int i = 0; i < 3; ++i)
                    app.spec().palette.ink[i] =
                        static_cast<std::uint8_t>(std::lround(ink[i] * 255.0f));
                app.dirty = app.detailDirty = true;
            }
            // ⚠ The swatch above is the STATED ink and the stack moves it — see
            // Palette::ink — so show what actually lands. A color picker that
            // quietly disagreed with the pixels would be read as the promote
            // region being broken, not as the stack working.
            if (!lit::HslStackIsIdentity(app.spec().palette.hsl)) {
                std::uint8_t become[3] = {app.spec().palette.ink[0],
                                          app.spec().palette.ink[1],
                                          app.spec().palette.ink[2]};
                lit::ApplyHslStack(app.spec().palette.hsl, become);
                Swatch(become, "##inkafter");
                ImGui::SameLine();
                ImGui::TextDisabled("what the stack makes of it");
            }
            int lo = app.spec().promote[0].alphaLo;
            int hi = app.spec().promote[0].alphaHi;
            if (ImGui::SliderInt("mask low", &lo, 0, 255) ||
                ImGui::SliderInt("mask high", &hi, 1, 255)) {
                for (auto& p : app.spec().promote) {
                    p.alphaLo = lo;
                    p.alphaHi = std::max(hi, lo + 1);
                }
                app.dirty = app.detailDirty = true;
            }
            ImGui::TextDisabled("mask reads the DAY texture's alpha channel");
        }

        // One editor for all three rect lists — they differ only in what they
        // mean and what color they outline in.
        enum class RectKind { kPreserve, kRegion, kBlackout };
        auto rectRows = [&](const char* title, std::vector<lit::Rect>& rects,
                            RectKind kind) {
            if (rects.empty()) return;
            ImGui::SeparatorText(title);
            ImGui::PushID(title);  // ⚠ identical "##rect" labels across lists
            for (std::size_t i = 0; i < rects.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                lit::Rect& r = rects[i];
                int v[4] = {r.x0, r.y0, r.x1, r.y1};
                ImGui::TextUnformatted(
                    kind == RectKind::kBlackout
                        ? lit::BlackoutRectName(app.texture, i)
                        : kind == RectKind::kRegion
                              ? lit::PaletteRegionName(app.texture, i)
                              : lit::PreserveRectName(app.texture, i));
                if (ImGui::DragInt4("##rect", v, 4.0f, 0,
                                    static_cast<int>(app.refSize()))) {
                    r.x0 = std::min(v[0], v[2]);
                    r.y0 = std::min(v[1], v[3]);
                    r.x1 = std::max(v[0], v[2]);
                    r.y1 = std::max(v[1], v[3]);
                    app.dirty = app.detailDirty = true;
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        };

        ImGui::SeparatorText("Areas");
        ImGui::Checkbox("Outline in preview", &app.showRects);
        ImGui::TextDisabled("editable - they ship in the spec JSON");
        rectRows("Preserved (kept stock)", app.spec().preserve,
                 RectKind::kPreserve);
        rectRows("Blacked out (forced off)", app.spec().blackout,
                 RectKind::kBlackout);

        if (!app.spec().region.empty()) {
            ImGui::SeparatorText("Palette applies only here");
            ImGui::PushID("regions");
            for (std::size_t i = 0; i < app.spec().region.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                lit::Region& rg = app.spec().region[i];
                int v[4] = {rg.rect.x0, rg.rect.y0, rg.rect.x1, rg.rect.y1};
                ImGui::TextUnformatted(lit::PaletteRegionName(app.texture, i));
                if (ImGui::DragInt4("##rect", v, 4.0f, 0,
                                    static_cast<int>(app.refSize()))) {
                    rg.rect.x0 = std::min(v[0], v[2]);
                    rg.rect.y0 = std::min(v[1], v[3]);
                    rg.rect.x1 = std::max(v[0], v[2]);
                    rg.rect.y1 = std::max(v[1], v[3]);
                    app.dirty = app.detailDirty = true;
                }
                // ⚠ Say which rows do NOT follow the sliders above. Without it,
                // dragging the palette and watching one region sit still reads
                // as the region being broken.
                if (rg.ownPalette)
                    ImGui::TextDisabled(
                        "  ^ own curve; the tuning above still reaches it");
                ImGui::PopID();
            }
            ImGui::PopID();
        }

        ImGui::SeparatorText("Against Gus's texture");
        if (app.referenceFull.Valid()) {
            ImGui::TextDisabled("%s", app.gusAuto ? "reference/gus (loaded automatically)"
                                                  : "loaded by hand");
            ImGui::TextUnformatted(app.compareText.c_str());
            ImGui::TextDisabled("Diff view: red = palette, blue = redrawn artwork");
            // ⚠ Say this out loud. The XPDR/TCAS block is no longer preserved,
            // so it differs from Gus BY DESIGN and lights up in every diff. Left
            // unexplained it reads as the one region where the palette is wrong.
            ImGui::TextDisabled(
                "XPDR/TCAS differs on purpose - it is no longer\n"
                "preserved, so it recolors where Gus kept it stock");
        } else {
            ImGui::TextUnformatted(app.compareText.c_str());
        }

        ImGui::SeparatorText("Output");
        if (ImGui::Button("Export recolored PNG...")) {
            if (!app.HaveSource()) {
                app.status = "Load a stock texture first.";
            } else {
                const std::string p =
                    SaveFileDialog("Export recolored text_LIT.png", "png", hwnd);
                if (!p.empty()) {
                    lit::Image out = app.stockFull;
                    const std::size_t n = lit::Apply(
                        out, app.albedoFull.Valid() ? &app.albedoFull : nullptr,
                        app.spec());
                    std::string e;
                    app.status = photon::image::SavePng(p, out, e)
                                     ? ("Wrote " + p + " (" +
                                        std::to_string(n) + " px changed)")
                                     : e;
                }
            }
        }
        if (ImGui::Button("Save spec JSON...")) {
            const std::string p = SaveFileDialog("Save spec", "json", hwnd);
            if (!p.empty()) {
                const std::string js = lit::ToJson(app.spec());
                std::FILE* f = std::fopen(p.c_str(), "wb");
                if (f) {
                    std::fwrite(js.data(), 1, js.size(), f);
                    std::fclose(f);
                    app.status = "Wrote " + p;
                } else {
                    app.status = "Cannot write " + p;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load spec...")) {
            const std::string p = OpenFileDialog("Load spec", hwnd);
            if (!p.empty()) {
                std::string text;
                std::FILE* f = std::fopen(p.c_str(), "rb");
                if (f) {
                    char b[4096];
                    std::size_t k;
                    while ((k = std::fread(b, 1, sizeof(b), f)) > 0)
                        text.append(b, k);
                    std::fclose(f);
                    std::string e;
                    if (lit::FromJson(text, app.spec(), e)) {
                        app.dirty = app.detailDirty = true;
                        app.status = "Loaded " + p;
                    } else {
                        app.status = e;
                    }
                }
            }
        }

        ImGui::EndChild();

        // --- right column: the previews --------------------------------------
        ImGui::SameLine();
        ImGui::BeginChild("previews", ImVec2(0, 0));

        int v = static_cast<int>(app.view);
        bool viewChanged = ImGui::RadioButton("Recolored", &v, 0);
        ImGui::SameLine();
        viewChanged |= ImGui::RadioButton("Stock LIT", &v, 1);
        ImGui::SameLine();
        viewChanged |= ImGui::RadioButton("Day text", &v, 2);
        ImGui::SameLine();
        viewChanged |= ImGui::RadioButton("Gus", &v, 3);
        ImGui::SameLine();
        viewChanged |= ImGui::RadioButton("Diff x8", &v, 4);
        if (viewChanged) {
            app.view = static_cast<View>(v);
            app.dirty = app.detailDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit")) {
            app.zoom = 1.0f;
            app.pan = ImVec2(0, 0);
        }
        ImGui::SameLine();
        ImGui::Text("| zoom %.2fx", app.zoom);

        if (app.dirty) {
            app.RebuildOverview();
            app.dirty = false;
        }
        // ⚠ A full-resolution pass over a 4096 atlas, so it waits until nothing
        // is being held. Dragging a slider stays live off the proxy preview; the
        // numbers catch up the moment it is let go.
        if (app.compareDirty && !ImGui::IsAnyItemActive()) {
            app.RecomputeCompare();
            app.compareDirty = false;
        }
        // The detail crop is cheap; rebuilding it whenever the palette could
        // have moved keeps it honest without a second dirty path. The three
        // static views do not follow the palette, so they rebuild only when
        // something actually changed.
        const bool followsPalette =
            app.view == View::kRecolored || app.view == View::kDiff;
        if (app.detailDirty || followsPalette) {
            app.RebuildDetail();
            app.detailDirty = false;
        }

        const float detailPane = 400.0f;
        ImGui::BeginChild("overview", ImVec2(-detailPane, -60),
                          ImGuiChildFlags_Borders);
        if (app.overview.id) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float side = std::min(avail.x, avail.y);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 size(side, side);
            // Pan/zoom by moving the UVs rather than the quad, so the image
            // always fills the same square and the overlay math stays simple.
            const float span = 1.0f / app.zoom;
            ImVec2 uv0(app.pan.x, app.pan.y);
            uv0.x = std::clamp(uv0.x, 0.0f, 1.0f - span);
            uv0.y = std::clamp(uv0.y, 0.0f, 1.0f - span);
            app.pan = uv0;
            const ImVec2 uv1(uv0.x + span, uv0.y + span);
            ImGui::Image(static_cast<ImTextureID>(
                             static_cast<std::intptr_t>(app.overview.id)),
                         size, uv0, uv1);

            const bool hovered = ImGui::IsItemHovered();
            if (hovered) {
                ImGuiIO& io = ImGui::GetIO();
                if (io.MouseWheel != 0.0f) {
                    const float before = app.zoom;
                    app.zoom = std::clamp(app.zoom * (io.MouseWheel > 0 ? 1.2f : 1.0f / 1.2f),
                                          1.0f, 32.0f);
                    // Keep the point under the cursor put.
                    const ImVec2 m = io.MousePos;
                    const float fx = (m.x - origin.x) / size.x;
                    const float fy = (m.y - origin.y) / size.y;
                    const float oldSpan = 1.0f / before, newSpan = 1.0f / app.zoom;
                    app.pan.x += fx * (oldSpan - newSpan);
                    app.pan.y += fy * (oldSpan - newSpan);
                }
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    const ImVec2 d = ImGui::GetIO().MouseDelta;
                    app.pan.x -= d.x / size.x * span;
                    app.pan.y -= d.y / size.y * span;
                }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                    app.stockFull.Valid()) {
                    const ImVec2 m = ImGui::GetIO().MousePos;
                    const float fx = uv0.x + (m.x - origin.x) / size.x * span;
                    const float fy = uv0.y + (m.y - origin.y) / size.y * span;
                    app.detailX = static_cast<int>(fx * app.stockFull.width) -
                                  kDetailEdge / 2;
                    app.detailY = static_cast<int>(fy * app.stockFull.height) -
                                  kDetailEdge / 2;
                    app.detailDirty = true;
                }
            }

            if (app.showRects && app.stockFull.Valid()) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                // ⚠ Against the SPEC's reference size, not a constant: the
                // knobs atlas is 2048, and dividing its rects by 4096 would draw
                // every outline in the top-left quarter of the preview.
                const float ref = app.refSize();
                auto toScreen = [&](int ax, int ay) {
                    const float fx = static_cast<float>(ax) / ref;
                    const float fy = static_cast<float>(ay) / ref;
                    return ImVec2(origin.x + (fx - uv0.x) / span * size.x,
                                  origin.y + (fy - uv0.y) / span * size.y);
                };
                dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                                 true);
                for (const lit::Rect& r : app.spec().preserve)
                    dl->AddRect(toScreen(r.x0, r.y0), toScreen(r.x1, r.y1),
                                IM_COL32(80, 190, 255, 200));
                for (const lit::Region& rg : app.spec().region)
                    dl->AddRect(toScreen(rg.rect.x0, rg.rect.y0),
                                toScreen(rg.rect.x1, rg.rect.y1),
                                rg.ownPalette ? IM_COL32(180, 140, 255, 220)
                                              : IM_COL32(255, 210, 90, 220));
                for (const lit::Rect& r : app.spec().blackout)
                    dl->AddRect(toScreen(r.x0, r.y0), toScreen(r.x1, r.y1),
                                IM_COL32(255, 90, 90, 220));
                for (const lit::PromoteRegion& p : app.spec().promote)
                    dl->AddRect(toScreen(p.rect.x0, p.rect.y0),
                                toScreen(p.rect.x1, p.rect.y1),
                                IM_COL32(120, 255, 140, 220));
                dl->PopClipRect();
            }
        } else {
            ImGui::TextDisabled("No texture loaded.");
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("detail", ImVec2(0, -60), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted("Detail (1:1, full resolution)");
        ImGui::TextDisabled("right-click the overview to move it");
        const int detailMax =
            std::max(0, (app.stockFull.Valid() ? app.stockFull.width
                                               : lit::kReferenceSize) -
                            kDetailEdge);
        if (ImGui::DragInt("x", &app.detailX, 4.0f, 0, detailMax))
            app.detailDirty = true;
        if (ImGui::DragInt("y", &app.detailY, 4.0f, 0, detailMax))
            app.detailDirty = true;
        if (app.detail.id) {
            const float side = std::min(ImGui::GetContentRegionAvail().x,
                                        static_cast<float>(kDetailEdge));
            ImGui::Image(static_cast<ImTextureID>(
                             static_cast<std::intptr_t>(app.detail.id)),
                         ImVec2(side, side));
        }
        ImGui::EndChild();

        ImGui::BeginChild("status", ImVec2(0, 0));
        ImGui::TextWrapped("%s", app.status.c_str());
        ImGui::EndChild();

        ImGui::EndChild();
        ImGui::End();

        const float clear[3] = {0.10f, 0.11f, 0.12f};
        win.EndFrame(clear);
    }

    win.Destroy();
    return 0;
}
