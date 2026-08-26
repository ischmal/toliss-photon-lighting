#include "core/lit_recolor.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <thread>

#include "third_party/nlohmann/json.hpp"

namespace photon {
namespace lit {
namespace {

using json = nlohmann::json;

// The measured fit of Gus's file. Red came out bit-exact identity, which is why
// it is spelled as the default Curve rather than as three fitted numbers — a
// future refit must not be able to nudge it off identity by rounding.
constexpr double kGusGreenWhite = 258.25;
constexpr double kGusGreenGamma = 1.326;
constexpr double kGusBlueBlack = 92.8;
constexpr double kGusBlueWhite = 321.75;
constexpr double kGusBlueGamma = 0.574;

// ⚠ THESE ARE LAMP LEGENDS, NOT INTEGRAL LIGHTING. Each of these boxes holds
// artwork that is lit in its own signal color — a green CAPT, a red PULL UP, a
// blue ON — so running the amber curve over it would say the wrong thing about
// the aircraft. Reference-atlas coords.
//
// ⚠ THE BOUNDARY IS AUTHORED AGAINST THE ARTWORK, NOT DERIVED FROM GUS'S FILE
// (changed 2026-08-24). It used to be the bounding boxes of the connected runs
// of lit pixels he left alone, which is how one box came to be the whole
// 992x848 upper-right corner: a bounding box grows to its furthest member, and
// scattered legends have far-flung members. That corner also contains the
// COCKPIT / FWD CABIN / AFT CABIN placards, the pitch-trim ruling and part of
// the NOSE UP tapes, all of which ARE integral lighting and were being left
// stock. The five bands below trace the legend block's actual ragged left edge
// instead, so those come back into the recolor.
//
// Measured against the A319/A320 stock texture, both tables scored the same way
// (a "preserved" pixel being one the incandescent curve would change and Gus did
// not): of the 272,161 Gus preserved, this table keeps 254,277 and recolors
// 17,884 — and those 17,884 are EXACTLY the XPDR/TCAS block below, with zero
// leakage anywhere else. Its collateral, meanwhile, falls from 19,961 pixels to
// 717 (0.58% of the recolor to 0.02%), because the bands no longer swallow a
// tenth of the atlas corner to reach a legend in it.
//
// ⚠ A COLOR TEST STILL CANNOT REPLACE THEM. Inside these bands the legends run
// white, amber, red, green and blue, and a boxed amber OFF sits beside a placard
// amber that must move; no threshold separates them, which is the whole reason
// the rule is regional. What changed is the SHAPE of the regions, not the fact
// that they are regions.
//
// The rects travel in the spec JSON and the studio tool draws and edits them, so
// retuning a band by hand never needs a code change.
struct NamedRect {
    Rect rect;
    const char* name;
};

const NamedRect kPreserveTable[] = {
    // The annunciator/button legend block, in five bands down its stepped left
    // edge. Each band runs to the atlas edge except the last two, which are the
    // narrow callout column.
    {{3083, 0, 4096, 218}, "annunciator legends 1 (CAPT, MAN, OVRD)"},
    {{3209, 218, 4096, 425}, "annunciator legends 2 (G/S, SQUIB, ACTIV)"},
    {{3275, 425, 4096, 603}, "annunciator legends 3 (FIRE, AUTO LAND)"},
    {{3425, 603, 4096, 722}, "annunciator legends 4 (arrows, RTO ARM)"},
    // The 15 px underhang of that last row — the descenders of the arrow and
    // chevron artwork, which sit below the band that carries the rest of it.
    {{3397, 722, 4096, 737}, "annunciator legends 4 (underhang)"},
    // PUSH / ENG 1 2 / DECEL / PULL UP / boxed ON / OFF / ON — the warning and
    // memo callouts, red, green and blue, each in its true lamp color. This band
    // subsumes what used to be two separate rects (the callouts and a 32 px
    // sliver above them).
    {{3397, 737, 3626, 1427}, "warning callouts (PUSH, PULL UP)"},
    // A second, smaller callout column below it.
    {{3408, 1616, 3568, 1920}, "callouts (lower column)"},
    // ⚠ THE XPDR/TCAS PANEL IS DELIBERATELY *NOT* HERE (2026-08-24). Gus left it
    // alone and it was a preserve rect until then, on the reading that ToLiss
    // authored it at rgb(255,121,49) — already amber and already more saturated
    // than the atlas around it — so recoloring it would be re-cooking a color
    // that had been chosen. But STBY / ALT RPTG / TA ONLY / IDENT are integral
    // panel legends like any other, not lamp colors, and holding them back left
    // one panel visibly paler than its neighbours. The curve now carries them to
    // rgb(255,93,0) along with everything else: 17,884 pixels in the block, the
    // only place this table knowingly departs from Gus.
};

constexpr std::size_t kPreserveCount =
    sizeof(kPreserveTable) / sizeof(kPreserveTable[0]);

// "PEDAL DISC", on the pedestal by the trim wheel. Tight around the two words —
// the "SYS"/"ATC" column just to its right is already lit in stock and must not
// be re-inked.
const PromoteRegion kPromoteTable[] = {
    {{3880, 2145, 4040, 2330}, 19, 226, {250, 73, 32}},
};

constexpr std::size_t kPromoteCount =
    sizeof(kPromoteTable) / sizeof(kPromoteTable[0]);

// --- knobs_LIT.png -----------------------------------------------------------
// ⚠ A 2048 ATLAS. Every rect below is in ITS coordinates, which is why the size
// travels in the Spec — see `Spec::referenceSize`.
constexpr int kKnobsReferenceSize = 2048;

// ⚠ WHAT GUS DID HERE IS MOSTLY DELETION, NOT RECOLOR. Measured against the
// stock file 2026-08-24: of 4.19 M pixels he changed 88,744, and 82,778 of those
// he turned OFF. Both regions hold zero lit pixels in his file against 58,482
// and 24,279 in stock, so a rectangle reproduces them exactly.
const NamedRect kKnobsBlackoutTable[] = {
    // A wide, faint grey wash across a panel face that has no business
    // self-illuminating — it reads in the cockpit as a panel glowing from
    // nowhere.
    {{992, 517, 1524, 639}, "panel wash (spurious glow)"},
    // ⚠ THE COCKPIT LOUDSPEAKER, AND THIS RECT ALSO KILLS THE TWO PLACARDS
    // ABOVE IT. Stock lights the whole speaker body plus its two label strips;
    // Gus blacked out the lot, and this reproduces that. The strips are arguably
    // legitimate integral lighting, so if they should come back, shrink this
    // rect's top edge rather than adding a preserve rect — the tool edits it.
    {{1661, 682, 1821, 861}, "loudspeaker + its placards"},
};

constexpr std::size_t kKnobsBlackoutCount =
    sizeof(kKnobsBlackoutTable) / sizeof(kKnobsBlackoutTable[0]);

// Where the palette applies on this atlas. Everything else is left alone, which
// is what the positive region list is for.
//
// ⚠ TEN OF THESE ARE **NOT** THINGS GUS RECOLORED — they are an extension
// (2026-08-24, from a marked-up PNG). They are the white index markings on knobs
// and levers: the pointer stripe on a rotary, the position bar on a slider, the
// wedge on a selector. Stock lights them a pale salmon that sits visibly cooler
// and flatter than the placard amber around them, so they read as a different
// era of lamp on the same panel. They take the ORDINARY era palette — literally
// the same curve the placards get — which is the whole reason `Spec::palette`
// on this texture is the placard palette and the odd one out carries the
// override.
//
// The rects were derived from the marks rather than copied: each was flood-
// filled through the stock LIT footprint to the whole of the marking it touched,
// then padded 2 px for the antialiased halo the >24 threshold cuts off. Checked
// pairwise for overlap with each other, with the blue knob and with both
// blackouts: none.
struct NamedRegion {
    Rect rect;
    const char* name;
    bool ownPalette;
};

const NamedRegion kKnobsRegionTable[] = {
    // ⚠ THE ONE EXCEPTION, and it must stay first so the tests can name it.
    // Stock lights this knob icy blue-white; the era palette alone leaves it
    // blue, so it carries its own curve. See `KnobHighlightPalette`.
    {{1381, 91, 1411, 121}, "blue-backlit knob (own curve)", true},
    // The index markings, in atlas order.
    {{478, 79, 490, 124}, "index marking 1", false},
    {{1722, 167, 1781, 176}, "index marking 2", false},
    {{1208, 288, 1216, 334}, "index marking 3", false},
    {{1989, 467, 2005, 481}, "selector wedge", false},
    {{1470, 979, 1480, 1034}, "index marking 4", false},
    {{1496, 1040, 1540, 1049}, "index marking 5", false},
    {{1625, 1088, 1652, 1097}, "index marking 6", false},
    {{304, 1413, 349, 1421}, "index marking 7", false},
    {{1808, 1629, 1844, 1648}, "lever pointer", false},
    {{1688, 1701, 1694, 1717}, "index marking 8", false},
};

constexpr std::size_t kKnobsRegionCount =
    sizeof(kKnobsRegionTable) / sizeof(kKnobsRegionTable[0]);

// ⚠ THE TUNING ON TOP OF THE FIT, AND IT IS A JUDGEMENT, NOT A MEASUREMENT
// (approved 2026-08-24). Everything else about this texture is fitted to Gus's
// file; this is not, because the index markings are not something Gus recolored
// — they are Photon's extension (see `kKnobsRegionTable`), so there is nothing
// to fit them against and the only test is whether they look like markings on a
// warm-lit panel. Signed off in-sim at hue +13 / brightness +17, which takes the
// stock salmon rgb(204,130,101) to rgb(219,158,64) rather than to the placards'
// deep rgb(204,103,38): a pointer stripe reads as a marking, not as a placard,
// and running it all the way down to placard amber lost it against the knob.
//
// ⚠ IT LIVES ON THE KNOBS PALETTE ALONE AND THE PLACARDS DO NOT GET IT. The two
// textures still share the CURVE — that is what `GusKnobs` copying
// `GusIncandescent().palette` is for, and it is what stops the markings reading as a
// different era — but the same stack over the placards takes their amber from
// rgb(255,124,0) to rgb(255,192,43), which is a long way off the file that
// measures 95.7% within 2 levels of Gus. Sharing the curve and diverging on the
// tuning is the whole reason the two are separate things.
constexpr double kKnobsTuneHue = 13.0;
constexpr double kKnobsTuneBrightness = 17.0;

// --- the LED era -------------------------------------------------------------
// ⚠ AUTHORED, NOT FITTED. There is no LED reference file anywhere — these were
// tuned in the bench against the cockpit and signed off by eye (2026-08-24), so
// unlike every other number in this file they are a judgement and a retune is
// the honest way to change them, not a refit.
//
// ⚠ THE CURVE IS THE SAME MEASURED ONE; ONLY THE STACK DIFFERS. That is what
// makes LED a statement about the lamp rather than a second unrelated recolor,
// and it is why a future refit of Gus's curve reaches both eras for free.
//
// Placards go warmer and up: rgb(255,124,0) -> rgb(255,156,26), which reads as
// the cooler, less deeply saturated wash an LED backlight gives a placard.
constexpr double kLedPlacardHue = 5.0;
constexpr double kLedPlacardBrightness = 10.0;
// ⚠ The knobs keep the INCANDESCENT hue and brightness and change only SATURATION.
// The index markings are white artwork over a lit panel, so what separates an
// LED marking from an incandescent one is how much color the lamp pushes into it,
// where that color sits: rgb(219,158,64) -> rgb(188,152,95).
constexpr double kLedKnobsHue = kKnobsTuneHue;
constexpr double kLedKnobsSaturation = -40.0;
constexpr double kLedKnobsBrightness = kKnobsTuneBrightness;

// Fitted over that knob's 745 pixels. Red came out bit-exact identity again —
// the same signature as the placard fit, which is the main reason to believe
// this is the same author doing the same kind of edit.
constexpr double kKnobGreenBlack = 99.5;
constexpr double kKnobGreenWhite = 259.5;
constexpr double kKnobGreenGamma = 1.34;
constexpr double kKnobBlueBlack = 130.5;
constexpr double kKnobBlueWhite = 258.5;
constexpr double kKnobBlueGamma = 2.10;

int ScaleCoord(int v, int size, int reference) {
    if (size == reference || reference <= 0) return v;
    // Round to nearest so a rect keeps its width on a half-size atlas.
    return static_cast<int>(std::lround(static_cast<double>(v) * size /
                                        reference));
}

Rect ScaleRect(const Rect& r, int w, int h, int reference) {
    Rect s;
    s.x0 = std::max(0, std::min(w, ScaleCoord(r.x0, w, reference)));
    s.x1 = std::max(0, std::min(w, ScaleCoord(r.x1, w, reference)));
    s.y0 = std::max(0, std::min(h, ScaleCoord(r.y0, h, reference)));
    s.y1 = std::max(0, std::min(h, ScaleCoord(r.y1, h, reference)));
    return s;
}

// Scale a rect into the buffer's own coordinates and clip it there. Returns
// false when nothing of it lands in this buffer.
bool RectForBuffer(const Rect& r, int w, int h, int fullW, int fullH, int ox,
                   int oy, int reference, Rect& out) {
    Rect s = ScaleRect(r, fullW, fullH, reference);
    s.x0 = std::max(0, s.x0 - ox);
    s.y0 = std::max(0, s.y0 - oy);
    s.x1 = std::min(w, s.x1 - ox);
    s.y1 = std::min(h, s.y1 - oy);
    if (s.Empty()) return false;
    out = s;
    return true;
}

bool Covers(const std::vector<Rect>& rects, int x, int y) {
    for (const Rect& r : rects)
        if (y >= r.y0 && y < r.y1 && x >= r.x0 && x < r.x1) return true;
    return false;
}

json CurveToJson(const Curve& c) {
    return json{{"black", c.black}, {"white", c.white}, {"gamma", c.gamma}};
}

bool CurveFromJson(const json& j, Curve& c, std::string& error) {
    if (!j.is_object()) {
        error = "curve is not an object";
        return false;
    }
    c.black = j.value("black", 0.0);
    c.white = j.value("white", 255.0);
    c.gamma = j.value("gamma", 1.0);
    // A zero-width window divides by zero in BuildLut; a non-positive gamma
    // makes std::pow return inf at 0. Both are unreachable from the tool's
    // sliders and reachable from a hand-edited file.
    if (c.white - c.black < 1e-6) {
        error = "curve white point must exceed its black point";
        return false;
    }
    if (c.gamma <= 0.0) {
        error = "curve gamma must be positive";
        return false;
    }
    return true;
}

json HslToJson(const std::vector<HslOp>& ops) {
    json a = json::array();
    for (const HslOp& op : ops) {
        a.push_back(json{
            // ⚠ Spelled out, not an enum index. A hand-edited spec is a normal
            // thing here, and a stack that silently became colorize because a
            // number shifted would gray the whole atlas.
            {"kind", op.kind == HslOp::Kind::kColorize ? "colorize" : "adjust"},
            {"enabled", op.enabled},
            {"hue", op.hue},
            {"saturation", op.saturation},
            {"lightness", op.lightness},
        });
    }
    return a;
}

// ⚠ No failure path, deliberately, unlike CurveFromJson. Every value here is
// clamped where it is used and an out-of-range hue is just a rotation, so there
// is nothing a bad number can divide by zero or raise to an infinite power. An
// unrecognized kind falls back to `adjust`, which at worst does nothing —
// falling back to `colorize` would repaint the cockpit one flat hue.
void HslFromJson(const json& j, std::vector<HslOp>& out) {
    if (!j.is_array()) return;
    for (const json& e : j) {
        if (!e.is_object()) continue;
        HslOp op;
        op.kind = e.value("kind", std::string("adjust")) == "colorize"
                      ? HslOp::Kind::kColorize
                      : HslOp::Kind::kAdjust;
        op.enabled = e.value("enabled", true);
        op.hue = e.value("hue", 0.0);
        op.saturation = e.value("saturation", 0.0);
        op.lightness = e.value("lightness", 0.0);
        out.push_back(op);
    }
}

json RectToJson(const Rect& r) {
    return json{{"x0", r.x0}, {"y0", r.y0}, {"x1", r.x1}, {"y1", r.y1}};
}

Rect RectFromJson(const json& j) {
    Rect r;
    r.x0 = j.value("x0", 0);
    r.y0 = j.value("y0", 0);
    r.x1 = j.value("x1", 0);
    r.y1 = j.value("y1", 0);
    return r;
}

}  // namespace

bool Curve::IsIdentity() const {
    return std::abs(black) < 1e-9 && std::abs(white - 255.0) < 1e-9 &&
           std::abs(gamma - 1.0) < 1e-9;
}

void BuildLut(const Curve& curve, std::uint8_t out[256]) {
    const double span = curve.white - curve.black;
    for (int i = 0; i < 256; ++i) {
        if (span < 1e-6) {  // degenerate; treat as identity rather than crash
            out[i] = static_cast<std::uint8_t>(i);
            continue;
        }
        double d = (static_cast<double>(i) - curve.black) / span;
        d = std::min(1.0, std::max(0.0, d));
        const double v = 255.0 * std::pow(d, curve.gamma);
        out[i] = static_cast<std::uint8_t>(
            std::min(255.0, std::max(0.0, std::lround(v) * 1.0)));
    }
}

// --- the HSL tuning layer ----------------------------------------------------
namespace {

// Textbook HSL — the bicone — which is what Photoshop's Hue/Saturation dialog
// works in. ⚠ NOT HSV, and the difference is at the top end: in HSL, lightness 1
// is white at every hue and saturation, which is what makes the lightness gain
// wash a hot lamp toward white rather than merely toward a brighter amber. On
// this atlas that is the right behavior and it is why the model is stated here
// rather than left to whichever conversion came to hand.
void RgbToHsl(double r, double g, double b, double& h, double& s, double& l) {
    const double mx = std::max(r, std::max(g, b));
    const double mn = std::min(r, std::min(g, b));
    l = (mx + mn) * 0.5;
    const double d = mx - mn;
    if (d < 1e-12) {  // gray, including black and white: hue is undefined
        h = 0.0;
        s = 0.0;
        return;
    }
    // The denominator vanishes only as d does, and d is at least 1/255 here.
    s = d / (1.0 - std::abs(2.0 * l - 1.0));
    if (mx == r)
        h = 60.0 * std::fmod((g - b) / d, 6.0);
    else if (mx == g)
        h = 60.0 * ((b - r) / d + 2.0);
    else
        h = 60.0 * ((r - g) / d + 4.0);
    if (h < 0.0) h += 360.0;
}

double HueToChannel(double p, double q, double t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

void HslToRgb(double h, double s, double l, double& r, double& g, double& b) {
    if (s <= 0.0) {
        r = g = b = l;
        return;
    }
    h = std::fmod(h, 360.0);
    if (h < 0.0) h += 360.0;
    const double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    const double p = 2.0 * l - q;
    const double hk = h / 360.0;
    r = HueToChannel(p, q, hk + 1.0 / 3.0);
    g = HueToChannel(p, q, hk);
    b = HueToChannel(p, q, hk - 1.0 / 3.0);
}

// Photoshop's slider shape for saturation: positive drives toward the maximum,
// negative toward zero, so -100 is exactly gray and +100 exactly full. ⚠ NOT
// used for lightness — see HslOp::lightness for why that one multiplies.
double ScaleToward(double v, double amount) {
    const double a = std::min(100.0, std::max(-100.0, amount)) / 100.0;
    if (a >= 0.0) return v + (1.0 - v) * a;
    return v * (1.0 + a);
}

double Clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

std::uint8_t ToByte(double v) {
    return static_cast<std::uint8_t>(std::lround(Clamp01(v) * 255.0));
}

void ApplyOneOp(const HslOp& op, double& r, double& g, double& b) {
    double h, s, l;
    RgbToHsl(r, g, b, h, s, l);
    if (op.kind == HslOp::Kind::kColorize) {
        h = op.hue;
        s = Clamp01(op.saturation / 100.0);
    } else {
        h += op.hue;
        s = Clamp01(ScaleToward(s, op.saturation));
    }
    // ⚠ Multiplies. A black pixel therefore stays black at every setting, which
    // is the property the whole atlas rests on.
    l = Clamp01(l * (1.0 + std::min(100.0, std::max(-100.0, op.lightness)) /
                               100.0));
    HslToRgb(h, s, l, r, g, b);
}

}  // namespace

bool HslOp::IsIdentity() const {
    // ⚠ A colorize op states a hue, so it is never identity however small its
    // numbers are — zeros there mean "force everything to red at zero
    // saturation", i.e. gray, which is a very large change indeed.
    if (kind == Kind::kColorize) return false;
    return std::abs(hue) < 1e-9 && std::abs(saturation) < 1e-9 &&
           std::abs(lightness) < 1e-9;
}

const char* HslKindName(HslOp::Kind kind) {
    switch (kind) {
        case HslOp::Kind::kAdjust: return "Adjust";
        case HslOp::Kind::kColorize: return "Colorize";
    }
    return "Adjust";
}

bool HslStackIsIdentity(const std::vector<HslOp>& ops) {
    for (const HslOp& op : ops)
        if (op.enabled && !op.IsIdentity()) return false;
    return true;
}

void ApplyHslStack(const std::vector<HslOp>& ops, std::uint8_t rgb[3]) {
    double r = rgb[0] / 255.0, g = rgb[1] / 255.0, b = rgb[2] / 255.0;
    bool ran = false;
    for (const HslOp& op : ops) {
        if (!op.enabled || op.IsIdentity()) continue;
        ApplyOneOp(op, r, g, b);
        ran = true;
    }
    // ⚠ Nothing is written when nothing ran, so a stack of no-ops cannot cost a
    // byte to a round-trip rounding error. Same rule as `Apply`'s skip.
    if (!ran) return;
    rgb[0] = ToByte(r);
    rgb[1] = ToByte(g);
    rgb[2] = ToByte(b);
}

std::size_t Apply(Image& lit, const Image* albedo, const Spec& spec,
                  const Viewport& view) {
    if (!lit.Valid()) return 0;

    std::uint8_t lutR[256], lutG[256], lutB[256];
    BuildLut(spec.palette.r, lutR);
    BuildLut(spec.palette.g, lutG);
    BuildLut(spec.palette.b, lutB);

    const int w = lit.width, h = lit.height;
    // The atlas this buffer is a window onto, and where the window starts.
    const int fullW = view.fullWidth > 0 ? view.fullWidth : w;
    const int fullH = view.fullHeight > 0 ? view.fullHeight : h;
    const int ox = view.originX, oy = view.originY;

    // The spec's rects, scaled to the atlas and then shifted into buffer
    // coordinates. Doing the shift here keeps the inner loop on plain ints.
    const int ref = spec.referenceSize > 0 ? spec.referenceSize : kReferenceSize;
    auto prepare = [&](const std::vector<Rect>& src) {
        std::vector<Rect> outRects;
        outRects.reserve(src.size());
        Rect s;
        for (const Rect& r : src)
            if (RectForBuffer(r, w, h, fullW, fullH, ox, oy, ref, s))
                outRects.push_back(s);
        return outRects;
    };
    const std::vector<Rect> keep = prepare(spec.preserve);
    const std::vector<Rect> black = prepare(spec.blackout);
    // ⚠ An EMPTY region list means the whole atlas, so it cannot be prepared the
    // same way — `prepare` would drop rects that miss this buffer and an empty
    // result would then read as "everywhere" and recolor a crop it should not
    // touch. The flag is taken from the SPEC, before clipping.
    const bool wholeAtlas = spec.region.empty();

    // A region may carry its own palette, so each one resolves to a LUT triple
    // rather than to the spec's. ⚠ `owned` is reserved up front: the prepared
    // regions hold pointers into it, and a reallocation would dangle them.
    struct Luts {
        std::uint8_t r[256], g[256], b[256];
        // ⚠ NULL WHEN THE STACK DOES NOTHING, which is how "an empty stack is
        // bit-exact the old behavior" is enforced rather than merely intended:
        // there is no path that round-trips a pixel through HSL for free. It
        // points into the spec, which outlives this call.
        const std::vector<HslOp>* hsl = nullptr;
    };
    auto stackFor = [](const Palette& p) {
        return HslStackIsIdentity(p.hsl) ? nullptr : &p.hsl;
    };
    Luts base;
    std::memcpy(base.r, lutR, 256);
    std::memcpy(base.g, lutG, 256);
    std::memcpy(base.b, lutB, 256);
    base.hsl = stackFor(spec.palette);
    std::vector<Luts> owned;
    owned.reserve(spec.region.size());
    struct PreparedRegion {
        Rect rect;
        const Luts* luts;
    };
    std::vector<PreparedRegion> region;
    region.reserve(spec.region.size());
    for (const Region& rg : spec.region) {
        Rect s;
        if (!RectForBuffer(rg.rect, w, h, fullW, fullH, ox, oy, ref, s)) continue;
        const Luts* use = &base;
        if (rg.ownPalette) {
            Luts l;
            BuildLut(rg.palette.r, l.r);
            BuildLut(rg.palette.g, l.g);
            BuildLut(rg.palette.b, l.b);
            // ⚠ AN OVERRIDE REPLACES THE CURVE, NOT THE TUNING. A region
            // carrying no stack of its own inherits the spec's, and the icy-blue
            // knob is why: its override exists because that ONE lamp is authored
            // in a different color and needs a different curve to land where its
            // neighbours land — it is not a second era. Left on its own stack it
            // would hold still through every tuning move and be the one lamp on
            // the panel that did not change, which is the same failure the
            // profile-copy path in the studio already guards against. A region
            // that genuinely wants its own tuning states one and keeps it.
            l.hsl = stackFor(rg.palette.hsl.empty() ? spec.palette : rg.palette);
            owned.push_back(l);
            use = &owned.back();
        }
        region.push_back({s, use});
    }

    std::atomic<std::size_t> changed{0};

    auto band = [&](int yStart, int yEnd) {
        std::size_t local = 0;
        for (int y = yStart; y < yEnd; ++y) {
            std::uint8_t* row = lit.pixels.data() +
                                static_cast<std::size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
                // ⚠ Blackout wins over everything, including preserve: it is
                // "this must not glow", which is a stronger statement than "do
                // not change this color".
                if (!black.empty() && Covers(black, x, y)) {
                    if (p[0] || p[1] || p[2]) ++local;
                    p[0] = p[1] = p[2] = 0;
                    // ⚠ p[3] is roughness. Gus left it alone even here.
                    continue;
                }
                const Luts* use = &base;
                if (!wholeAtlas) {
                    use = nullptr;
                    for (const PreparedRegion& pr : region) {
                        if (y >= pr.rect.y0 && y < pr.rect.y1 &&
                            x >= pr.rect.x0 && x < pr.rect.x1) {
                            use = pr.luts;
                            break;
                        }
                    }
                    if (!use) continue;
                }
                if (Covers(keep, x, y)) continue;
                std::uint8_t nr = use->r[p[0]];
                std::uint8_t ng = use->g[p[1]];
                std::uint8_t nb = use->b[p[2]];
                // ⚠ Black is skipped, and that is a proof rather than an
                // approximation: HSL lightness 0 leaves every channel 0 for any
                // hue and any saturation, and the gain multiplies, so no op in
                // the stack can lift it. That matters here because ~90% of an
                // emissive atlas IS black background — running the round trip
                // over it would be almost all of the cost, for nothing.
                if (use->hsl && (nr | ng | nb)) {
                    std::uint8_t c[3] = {nr, ng, nb};
                    ApplyHslStack(*use->hsl, c);
                    nr = c[0];
                    ng = c[1];
                    nb = c[2];
                }
                if (nr != p[0] || ng != p[1] || nb != p[2]) ++local;
                p[0] = nr;
                p[1] = ng;
                p[2] = nb;
                // ⚠ p[3] is roughness, not opacity. Never written here.
            }
        }
        changed += local;
    };

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    const int bands = static_cast<int>(std::min<unsigned>(hw, 16));
    if (bands <= 1 || h < 64) {
        band(0, h);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(bands);
        const int step = (h + bands - 1) / bands;
        for (int i = 0; i < bands; ++i) {
            const int y0 = i * step;
            const int y1 = std::min(h, y0 + step);
            if (y0 >= y1) break;
            pool.emplace_back(band, y0, y1);
        }
        for (std::thread& t : pool) t.join();
    }

    // Promotion runs after, single-threaded: it is a few thousand pixels and it
    // must be able to write over what the curve just did.
    if (albedo && albedo->Valid() && albedo->width == w &&
        albedo->height == h) {
        // ⚠ The ink goes through the stack too, once, here. It is painted
        // artwork sitting among recolored artwork — see Palette::ink — so a hue
        // shift that skipped it would leave PEDAL DISC the one thing on the
        // pedestal still wearing the old era, inches from placards that moved.
        std::uint8_t ink[3] = {spec.palette.ink[0], spec.palette.ink[1],
                               spec.palette.ink[2]};
        ApplyHslStack(spec.palette.hsl, ink);
        for (const PromoteRegion& pr : spec.promote) {
            Rect r;
            if (!RectForBuffer(pr.rect, w, h, fullW, fullH, ox, oy, ref, r))
                continue;
            const double lo = pr.alphaLo;
            const double hi = pr.alphaHi;
            if (hi - lo < 1e-6) continue;
            for (int y = r.y0; y < r.y1; ++y) {
                for (int x = r.x0; x < r.x1; ++x) {
                    const std::size_t o =
                        (static_cast<std::size_t>(y) * w + x) * 4;
                    const double a = albedo->pixels[o + 3];
                    double cov = (a - lo) / (hi - lo);
                    cov = std::min(1.0, std::max(0.0, cov));
                    if (cov <= 0.0) continue;
                    std::uint8_t* p = lit.pixels.data() + o;
                    for (int c = 0; c < 3; ++c) {
                        const double v = ink[c] * cov;
                        const std::uint8_t nv = static_cast<std::uint8_t>(
                            std::min(255.0, std::max(0.0, std::lround(v) * 1.0)));
                        if (nv > p[c]) p[c] = nv;
                    }
                    // Alpha here IS the glyph's own coverage — the one place
                    // this module writes it, and only upward, so a roughness
                    // value already present under the glyph is never lowered.
                    const std::uint8_t na = static_cast<std::uint8_t>(
                        std::lround(cov * pr.alphaHi));
                    if (na > p[3]) p[3] = na;
                    ++changed;
                }
            }
        }
    }

    return changed.load();
}

namespace {

// One table lookup, so a new texture adds rows rather than branches.
struct TextureTables {
    const char* name;
    const char* key;
    const char* litFile;
    const char* dayFile;
    int referenceSize;
    const NamedRect* preserve;
    std::size_t preserveCount;
    const NamedRect* blackout;
    std::size_t blackoutCount;
    const NamedRegion* region;
    std::size_t regionCount;
    const PromoteRegion* promote;
    std::size_t promoteCount;
};

const TextureTables kTextures[kTextureCount] = {
    {"Placards", "placards", "text_LIT.png", "text.png", kReferenceSize,
     kPreserveTable, kPreserveCount, nullptr, 0, nullptr, 0, kPromoteTable,
     kPromoteCount},
    {"Knobs", "knobs", "knobs_LIT.png", "knobs.png", kKnobsReferenceSize,
     nullptr, 0, kKnobsBlackoutTable, kKnobsBlackoutCount, kKnobsRegionTable,
     kKnobsRegionCount, nullptr, 0},
};

const TextureTables& Tables(Texture texture) {
    const int i = static_cast<int>(texture);
    return kTextures[(i >= 0 && i < kTextureCount) ? i : 0];
}

const std::vector<Rect>& RectVector(const NamedRect* table, std::size_t count,
                                    std::vector<Rect>& cache) {
    if (cache.size() != count) {
        cache.clear();
        cache.reserve(count);
        for (std::size_t i = 0; i < count; ++i) cache.push_back(table[i].rect);
    }
    return cache;
}

}  // namespace

const char* TextureName(Texture texture) { return Tables(texture).name; }
const char* TextureKey(Texture texture) { return Tables(texture).key; }

bool TextureFromKey(const std::string& key, Texture& out) {
    for (int i = 0; i < kTextureCount; ++i) {
        if (key == kTextures[i].key) {
            out = static_cast<Texture>(i);
            return true;
        }
    }
    return false;
}

const char* TextureLitName(Texture texture) { return Tables(texture).litFile; }
const char* TextureDayName(Texture texture) { return Tables(texture).dayFile; }
int TextureReferenceSize(Texture texture) {
    return Tables(texture).referenceSize;
}

const std::vector<Rect>& PreserveRects(Texture texture) {
    static std::vector<Rect> cache[kTextureCount];
    const TextureTables& t = Tables(texture);
    return RectVector(t.preserve, t.preserveCount,
                      cache[static_cast<int>(texture)]);
}

const char* PreserveRectName(Texture texture, std::size_t index) {
    const TextureTables& t = Tables(texture);
    if (index >= t.preserveCount) return "";
    return t.preserve[index].name;
}

const std::vector<Rect>& BlackoutRects(Texture texture) {
    static std::vector<Rect> cache[kTextureCount];
    const TextureTables& t = Tables(texture);
    return RectVector(t.blackout, t.blackoutCount,
                      cache[static_cast<int>(texture)]);
}

const char* BlackoutRectName(Texture texture, std::size_t index) {
    const TextureTables& t = Tables(texture);
    if (index >= t.blackoutCount) return "";
    return t.blackout[index].name;
}

const std::vector<Rect>& PaletteRegions(Texture texture) {
    static std::vector<Rect> cache[kTextureCount];
    const TextureTables& t = Tables(texture);
    std::vector<Rect>& v = cache[static_cast<int>(texture)];
    if (v.size() != t.regionCount) {
        v.clear();
        v.reserve(t.regionCount);
        for (std::size_t i = 0; i < t.regionCount; ++i)
            v.push_back(t.region[i].rect);
    }
    return v;
}

const char* PaletteRegionName(Texture texture, std::size_t index) {
    const TextureTables& t = Tables(texture);
    if (index >= t.regionCount) return "";
    return t.region[index].name;
}

const std::vector<PromoteRegion>& PromoteRegions(Texture texture) {
    static std::vector<PromoteRegion> cache[kTextureCount];
    const TextureTables& t = Tables(texture);
    std::vector<PromoteRegion>& v = cache[static_cast<int>(texture)];
    if (v.size() != t.promoteCount) {
        v.clear();
        v.reserve(t.promoteCount);
        for (std::size_t i = 0; i < t.promoteCount; ++i)
            v.push_back(t.promote[i]);
    }
    return v;
}

namespace {

// The curve for the one knob whose stock backlight is icy blue-white. The era
// palette alone leaves it blue — its green and blue both need a black point that
// the placard curve does not have.
Palette KnobHighlightPalette() {
    Palette p;
    p.r = Curve{};  // identity, measured
    p.g = Curve{kKnobGreenBlack, kKnobGreenWhite, kKnobGreenGamma};
    p.b = Curve{kKnobBlueBlack, kKnobBlueWhite, kKnobBlueGamma};
    return p;
}

// The geometry every look on a texture shares. Only the palette varies.
void FillGeometry(Spec& s, Texture texture) {
    const TextureTables& t = Tables(texture);
    s.referenceSize = t.referenceSize;
    s.region.clear();
    s.region.reserve(t.regionCount);
    for (std::size_t i = 0; i < t.regionCount; ++i) {
        Region rg;
        rg.rect = t.region[i].rect;
        if (t.region[i].ownPalette) {
            rg.ownPalette = true;
            rg.palette = KnobHighlightPalette();
        }
        s.region.push_back(rg);
    }
    s.preserve = PreserveRects(texture);
    s.blackout = BlackoutRects(texture);
    s.promote = PromoteRegions(texture);
}

}  // namespace

Spec GusIncandescent() {
    Spec s;
    s.palette.r = Curve{};  // identity, measured bit-exact
    s.palette.g = Curve{0.0, kGusGreenWhite, kGusGreenGamma};
    s.palette.b = Curve{kGusBlueBlack, kGusBlueWhite, kGusBlueGamma};
    s.palette.ink[0] = 250;
    s.palette.ink[1] = 73;
    s.palette.ink[2] = 32;
    FillGeometry(s, Texture::kPlacards);
    return s;
}

Spec GusKnobs() {
    Spec s;
    // ⚠ THE PLACARD PALETTE, DELIBERATELY. Ten of the eleven regions here are
    // index markings that must take the same transformation the placards get —
    // that is the entire point of recoloring them — so tuning the era look moves
    // both textures together. Only the icy-blue knob departs, and it says so on
    // its own region.
    s.palette = GusIncandescent().palette;
    // ⚠ AND THEN DIVERGES, BY EXACTLY ONE OP. See kKnobsTuneHue: the curve is
    // shared so the markings sit in the same era as the placards, and the stack
    // is where "but a pointer stripe is not a placard" gets said. Copying the
    // palette first and appending after is the order that keeps a future placard
    // retune flowing through to here.
    HslOp warm;
    warm.hue = kKnobsTuneHue;
    warm.lightness = kKnobsTuneBrightness;
    s.palette.hsl.push_back(warm);
    FillGeometry(s, Texture::kKnobs);
    return s;
}

Spec StockPassthrough(Texture texture) {
    Spec s;
    FillGeometry(s, texture);
    // ⚠ Passthrough must mean the file the aircraft shipped with, so it drops
    // everything that WRITES — the promotions and the blackouts alike. Leaving
    // the blackouts in would make "stock" a texture with two holes in it.
    s.promote.clear();
    s.blackout.clear();
    return s;
}

Spec Led(Texture texture) {
    // ⚠ START FROM THE INCANDESCENT SPEC OF THE SAME TEXTURE, then replace the
    // stack.
    // Starting from the *fit* is what keeps the two eras on one curve — see the
    // constants above — and taking the whole spec (rather than assembling a new
    // one) is what keeps their geometry identical, which
    // `lit: both profiles carry the same geometry` depends on.
    Spec s = texture == Texture::kKnobs ? GusKnobs() : GusIncandescent();
    HslOp op;
    if (texture == Texture::kKnobs) {
        op.hue = kLedKnobsHue;
        op.saturation = kLedKnobsSaturation;
        op.lightness = kLedKnobsBrightness;
    } else {
        op.hue = kLedPlacardHue;
        op.lightness = kLedPlacardBrightness;
    }
    // ⚠ ASSIGNED, NOT APPENDED. The knobs incandescent spec already carries an op,
    // and appending would make LED "incandescent and then some" — two compounding,
    // where a retune of the incandescent one would silently drag LED with it.
    s.palette.hsl = {op};
    return s;
}

namespace {

bool CoversPx(const std::vector<Rect>& rects, int x, int y) {
    for (const Rect& r : rects)
        if (y >= r.y0 && y < r.y1 && x >= r.x0 && x < r.x1) return true;
    return false;
}

bool PixelIsLit(const std::uint8_t* p) { return p[0] + p[1] + p[2] > 24; }

// Blue as a fraction of red, x255, at or below which the placards have been
// recolored. ⚠ IT HAS TO CLEAR **BOTH** SHIPPED ERAS, and until 2026-08-24 it did
// not. Measured on the placard amber each look produces:
//
//     stock         rgb(255,150,89)  ->  89
//     Incandescent  rgb(255,124,0)   ->   0
//     LED           rgb(255,156,26)  ->  26
//
// The old value was 20 — set when incandescent was the only look there was — so
// our OWN LED output read as stock. That is the expensive direction: a caller
// that believes our output is stock derives from it, and the curve compounds
// SILENTLY, because blue is already clipped and the second pass looks like a
// no-op. 48 is the geometric mean of 26 and 89, which is the most margin
// available on both sides at once (1.8x from LED, 1.9x from stock).
constexpr int kPlacardRecoloredRatio = 48;

// The placard test. Stock placard amber is rgb(255,150,89) — blue at 35% of red
// — and every recolor drives blue to zero. Sampled OUTSIDE the preserve rects,
// which keep their stock colors either way and would drag the median back up.
bool PlacardsLookRecolored(const Image& img) {
    const std::vector<Rect>& keep = PreserveRects(Texture::kPlacards);
    std::vector<int> ratios;
    ratios.reserve(1 << 16);
    // A coarse stride: this is a population statistic, not a pixel audit.
    for (int y = 0; y < img.height; y += 3) {
        for (int x = 0; x < img.width; x += 3) {
            if (CoversPx(keep, x, y)) continue;
            const std::size_t o = (static_cast<std::size_t>(y) * img.width + x) * 4;
            const int r = img.pixels[o], g = img.pixels[o + 1],
                      b = img.pixels[o + 2];
            // The placard-amber family, stated in RELATIVE terms: red leads,
            // green is a middling fraction of it, blue trails green.
            if (r < 64 || g <= b) continue;
            if (g * 100 < r * 30 || g * 100 > r * 90) continue;
            ratios.push_back(b * 255 / r);
        }
    }
    if (ratios.size() < 500) return false;  // too little amber to judge
    std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2,
                     ratios.end());
    return ratios[ratios.size() / 2] <= kPlacardRecoloredRatio;
}

// The knobs test: stock glows on 82,761 pixels inside the blackout rects and a
// recolored file glows on none.
bool KnobsLookRecolored(const Image& img) {
    const std::vector<Rect>& black = BlackoutRects(Texture::kKnobs);
    const int ref = TextureReferenceSize(Texture::kKnobs);
    std::size_t area = 0, litPx = 0;
    for (const Rect& r : black) {
        for (int y = r.y0; y < r.y1; ++y) {
            const int sy =
                static_cast<int>(static_cast<long long>(y) * img.height / ref);
            if (sy < 0 || sy >= img.height) continue;
            for (int x = r.x0; x < r.x1; ++x) {
                const int sx =
                    static_cast<int>(static_cast<long long>(x) * img.width / ref);
                if (sx < 0 || sx >= img.width) continue;
                ++area;
                if (PixelIsLit(&img.pixels[(static_cast<std::size_t>(sy) *
                                                img.width + sx) * 4]))
                    ++litPx;
            }
        }
    }
    if (area < 1000) return false;
    return litPx * 100 < area;  // under 1% still glowing = blacked out
}

}  // namespace

bool LooksRecolored(const Image& img, Texture texture) {
    if (!img.Valid()) return false;
    return texture == Texture::kKnobs ? KnobsLookRecolored(img)
                                      : PlacardsLookRecolored(img);
}

const char* ProfileName(Profile profile) {
    switch (profile) {
        case Profile::kIncandescent: return "Incandescent";
        case Profile::kLed: return "LED";
    }
    return "Incandescent";
}

bool ProfileIsDefined(Profile profile) {
    // ⚠ BOTH, since 2026-08-24, and this line moved in the SAME change that gave
    // kLed a real spec. It was false while LED resolved to incandescent: a
    // defined-but-identical profile tells the user a choice took effect when
    // nothing moved. Do not flip it back without also removing `Led`.
    (void)profile;
    return true;
}

Spec SpecForProfile(Profile profile, Texture texture) {
    switch (profile) {
        case Profile::kLed:
            return Led(texture);
        case Profile::kIncandescent:
            break;
    }
    return texture == Texture::kKnobs ? GusKnobs() : GusIncandescent();
}

std::string ToJson(const Spec& spec) {
    json j;
    j["version"] = 1;
    j["palette"] = {
        {"r", CurveToJson(spec.palette.r)},
        {"g", CurveToJson(spec.palette.g)},
        {"b", CurveToJson(spec.palette.b)},
        {"hsl", HslToJson(spec.palette.hsl)},
        {"ink", {spec.palette.ink[0], spec.palette.ink[1], spec.palette.ink[2]}},
    };
    // ⚠ The reference size is part of the recipe, not a constant: a knobs spec's
    // rects are 2048-authored and reading them back as 4096 would put every one
    // of them on a quarter of the atlas.
    j["referenceSize"] = spec.referenceSize;
    json rects = json::array();
    for (const Rect& r : spec.preserve) rects.push_back(RectToJson(r));
    j["preserve"] = rects;
    json regions = json::array();
    for (const Region& rg : spec.region) {
        json rj = RectToJson(rg.rect);
        // ⚠ The override travels with the region. A spec that lost it would come
        // back applying the placard curve to the icy-blue knob, which leaves it
        // blue — a wrong look that still looks deliberate.
        if (rg.ownPalette) {
            rj["palette"] = {
                {"r", CurveToJson(rg.palette.r)},
                {"g", CurveToJson(rg.palette.g)},
                {"b", CurveToJson(rg.palette.b)},
                {"hsl", HslToJson(rg.palette.hsl)},
            };
        }
        regions.push_back(rj);
    }
    j["region"] = regions;
    json blacks = json::array();
    for (const Rect& r : spec.blackout) blacks.push_back(RectToJson(r));
    j["blackout"] = blacks;
    json proms = json::array();
    for (const PromoteRegion& p : spec.promote) {
        json pj = RectToJson(p.rect);
        pj["alphaLo"] = p.alphaLo;
        pj["alphaHi"] = p.alphaHi;
        proms.push_back(pj);
    }
    j["promote"] = proms;
    return j.dump(2);
}

bool FromJson(const std::string& text, Spec& out, std::string& error) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "not valid JSON";
        return false;
    }
    Spec s;
    const json& p = j["palette"];
    if (!p.is_object()) {
        error = "missing \"palette\"";
        return false;
    }
    if (!CurveFromJson(p["r"], s.palette.r, error) ||
        !CurveFromJson(p["g"], s.palette.g, error) ||
        !CurveFromJson(p["b"], s.palette.b, error)) {
        return false;
    }
    // Absent in every spec written before the stack existed, and an absent
    // stack is exactly the right answer for those: they were tuned on the curves
    // alone, so they reload bit-for-bit as what they were.
    if (p.contains("hsl")) HslFromJson(p["hsl"], s.palette.hsl);
    if (p.contains("ink") && p["ink"].is_array() && p["ink"].size() == 3) {
        for (int i = 0; i < 3; ++i)
            s.palette.ink[i] = static_cast<std::uint8_t>(
                std::min(255, std::max(0, p["ink"][i].get<int>())));
    }
    // Absent in a spec written before knobs existed, and those were all placard
    // specs — so the default is right for exactly the files that lack it.
    s.referenceSize = j.value("referenceSize", kReferenceSize);
    if (s.referenceSize <= 0) {
        error = "\"referenceSize\" must be positive";
        return false;
    }
    if (j.contains("preserve") && j["preserve"].is_array())
        for (const json& r : j["preserve"]) s.preserve.push_back(RectFromJson(r));
    if (j.contains("region") && j["region"].is_array()) {
        for (const json& r : j["region"]) {
            Region rg;
            rg.rect = RectFromJson(r);
            if (r.contains("palette") && r["palette"].is_object()) {
                const json& rp = r["palette"];
                if (!CurveFromJson(rp["r"], rg.palette.r, error) ||
                    !CurveFromJson(rp["g"], rg.palette.g, error) ||
                    !CurveFromJson(rp["b"], rg.palette.b, error))
                    return false;
                if (rp.contains("hsl")) HslFromJson(rp["hsl"], rg.palette.hsl);
                rg.ownPalette = true;
            }
            s.region.push_back(rg);
        }
    }
    if (j.contains("blackout") && j["blackout"].is_array())
        for (const json& r : j["blackout"]) s.blackout.push_back(RectFromJson(r));
    if (j.contains("promote") && j["promote"].is_array()) {
        for (const json& r : j["promote"]) {
            PromoteRegion pr;
            pr.rect = RectFromJson(r);
            pr.alphaLo = r.value("alphaLo", 19);
            pr.alphaHi = r.value("alphaHi", 226);
            pr.ink[0] = s.palette.ink[0];
            pr.ink[1] = s.palette.ink[1];
            pr.ink[2] = s.palette.ink[2];
            s.promote.push_back(pr);
        }
    }
    out = s;
    return true;
}

}  // namespace lit
}  // namespace photon
