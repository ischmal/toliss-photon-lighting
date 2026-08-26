// The integral-lighting recolor engine.
//
// These cases pin the shape of the transform, not a golden image: the goldens
// (ToLiss's stock texture and Gus's finished one) are 8 MB apiece and live
// outside the repo, so the fidelity measurement against them is the studio
// tool's `compare` subcommand and `tests/test_lit_recolor.py`. What is pinned
// here is everything decidable without them — and every one of these properties
// is a bug that shipped or nearly did in some other patcher in this tree.
#include "test_harness.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/image_io.h"
#include "core/lit_recolor.h"

namespace fs = std::filesystem;
using namespace photon;
using photon::lit::Curve;
using photon::lit::Image;
using photon::lit::Rect;
using photon::lit::Spec;

namespace {

// A scratch directory that removes itself, so a failed run never poisons the
// next. Each test TU carries its own by convention.
class TempDir {
public:
    TempDir() {
        const auto base = fs::temp_directory_path();
        for (int i = 0; i < 1000; ++i) {
            fs::path candidate =
                base / ("photon_lit_test_" + std::to_string(::rand()) + "_" +
                        std::to_string(i));
            std::error_code ec;
            if (fs::create_directory(candidate, ec) && !ec) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error("could not create a temp directory");
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }
    fs::path operator/(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

Image Solid(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b,
            std::uint8_t a = 200) {
    Image img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
        img.pixels[i + 0] = r;
        img.pixels[i + 1] = g;
        img.pixels[i + 2] = b;
        img.pixels[i + 3] = a;
    }
    return img;
}

// The pixel at (x, y).
struct Px {
    std::uint8_t r, g, b, a;
};
Px At(const Image& img, int x, int y) {
    const std::size_t o = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.pixels[o], img.pixels[o + 1], img.pixels[o + 2],
            img.pixels[o + 3]};
}

}  // namespace

TEST("lit: an identity curve is a byte-for-byte no-op") {
    std::uint8_t lut[256];
    lit::BuildLut(Curve{}, lut);
    for (int i = 0; i < 256; ++i) CHECK_EQ(static_cast<int>(lut[i]), i);
    CHECK(Curve{}.IsIdentity());
}

TEST("lit: red is identity in the shipped incandescent look") {
    // ⚠ Measured bit-exact across all 100 red values present in Gus's file. If a
    // refit ever nudges red off identity, every amber on the atlas shifts hue
    // and the fit is wrong, not the aircraft.
    const Spec s = lit::GusIncandescent();
    CHECK(s.palette.r.IsIdentity());
    std::uint8_t lut[256];
    lit::BuildLut(s.palette.r, lut);
    for (int i = 0; i < 256; ++i) CHECK_EQ(static_cast<int>(lut[i]), i);
}

TEST("lit: the incandescent look drives stock amber to Gus's deep amber") {
    // ToLiss's washed-out placard amber, and what his file makes of it.
    Image img = Solid(8, 8, 255, 150, 89);
    Spec s = lit::GusIncandescent();
    s.preserve.clear();
    s.promote.clear();
    lit::Apply(img, nullptr, s);
    const Px p = At(img, 4, 4);
    CHECK_EQ(static_cast<int>(p.r), 255);
    // Green lands at 125 +/- 1 and blue is clipped clean off; those two facts
    // ARE the look.
    CHECK(p.g >= 123 && p.g <= 127);
    CHECK_EQ(static_cast<int>(p.b), 0);
}

TEST("lit: blue below the black point clips to zero, above it survives") {
    Spec s = lit::GusIncandescent();
    s.preserve.clear();
    s.promote.clear();
    Image low = Solid(4, 4, 255, 150, 80);
    Image high = Solid(4, 4, 255, 150, 140);
    lit::Apply(low, nullptr, s);
    lit::Apply(high, nullptr, s);
    CHECK_EQ(static_cast<int>(At(low, 1, 1).b), 0);
    CHECK(At(high, 1, 1).b > 80);
}

TEST("lit: alpha is roughness and is never touched outside a promote region") {
    // ⚠ Gus changed alpha on 2511 pixels, every one inside the artwork he added,
    // and left 16.7 M others byte-identical. Alpha here is a roughness map
    // (NORMAL_METALNESS), so writing it changes how the panel reflects.
    Image img = Solid(16, 16, 255, 150, 89, 37);
    Spec s = lit::GusIncandescent();
    s.preserve.clear();
    s.promote.clear();
    lit::Apply(img, nullptr, s);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) CHECK_EQ(static_cast<int>(At(img, x, y).a), 37);
}

TEST("lit: a preserve rect is left exactly as it was") {
    Image img = Solid(64, 64, 255, 150, 89);
    Spec s = lit::GusIncandescent();
    s.promote.clear();
    s.preserve.clear();
    // In reference-atlas coords; the image is 64 px, so this scales to the
    // left half.
    s.preserve.push_back(Rect{0, 0, lit::kReferenceSize / 2, lit::kReferenceSize});
    lit::Apply(img, nullptr, s);
    CHECK_EQ(static_cast<int>(At(img, 5, 5).b), 89);    // inside: untouched
    CHECK_EQ(static_cast<int>(At(img, 60, 5).b), 0);    // outside: recolored
}

TEST("lit: preserve rects scale with the atlas rather than being pixel-absolute") {
    // A half-size atlas must still get the annunciator block over the
    // annunciator block. Nothing has ever shipped one; the failure if ToLiss
    // ever does is a mask sitting across the middle of the texture.
    const int half = lit::kReferenceSize / 2;
    Image img = Solid(half, half, 255, 150, 89);
    Spec s = lit::GusIncandescent();
    s.promote.clear();
    lit::Apply(img, nullptr, s);
    // The first preserve rect is the upper-right legend block; its scaled
    // interior must be untouched.
    const Rect& r = lit::PreserveRects(lit::Texture::kPlacards)[0];
    const int x = (r.x0 + r.x1) / 4;  // midpoint, halved
    const int y = (r.y0 + r.y1) / 4;
    CHECK_EQ(static_cast<int>(At(img, x, y).b), 89);
}

TEST("lit: a Viewport places the rects for a crop") {
    // ⚠ Without this the studio's detail inspector would scale the preserve
    // table to a 384 px window and drop the legend block into the middle of it.
    const Rect& r = lit::PreserveRects(lit::Texture::kPlacards)[0];
    const int cropX = r.x0 + 16, cropY = r.y0 + 16, edge = 64;
    Image crop = Solid(edge, edge, 255, 150, 89);
    Spec s = lit::GusIncandescent();
    s.promote.clear();
    lit::Viewport vp;
    vp.originX = cropX;
    vp.originY = cropY;
    vp.fullWidth = lit::kReferenceSize;
    vp.fullHeight = lit::kReferenceSize;
    lit::Apply(crop, nullptr, s, vp);
    // The whole crop sits inside the legend block, so nothing may change.
    CHECK_EQ(static_cast<int>(At(crop, 8, 8).b), 89);

    // The same crop taken from open atlas recolors normally.
    Image other = Solid(edge, edge, 255, 150, 89);
    lit::Viewport vp2;
    vp2.originX = 100;
    vp2.originY = 100;
    vp2.fullWidth = lit::kReferenceSize;
    vp2.fullHeight = lit::kReferenceSize;
    lit::Apply(other, nullptr, s, vp2);
    CHECK_EQ(static_cast<int>(At(other, 8, 8).b), 0);
}

TEST("lit: promotion reads the day texture's ALPHA, not its color") {
    // ⚠ In the PEDAL DISC rect the albedo's RGB is flat white on every glyph
    // pixel — the letterform lives entirely in alpha. A luminance mask yields a
    // rectangle of ink.
    Spec s = lit::GusIncandescent();
    s.preserve.clear();
    const Rect pr = s.promote.at(0).rect;
    Image lit_ = Solid(lit::kReferenceSize, lit::kReferenceSize, 0, 0, 0, 1);
    // Flat white albedo, alpha at the region's background level: no glyph.
    Image albedo = Solid(lit::kReferenceSize, lit::kReferenceSize, 255, 255, 255,
                         static_cast<std::uint8_t>(s.promote[0].alphaLo));
    lit::Apply(lit_, &albedo, s);
    const Px none = At(lit_, pr.x0 + 4, pr.y0 + 4);
    CHECK_EQ(static_cast<int>(none.r), 0);

    // Same color, alpha at ink level: the ink appears.
    Image lit2 = Solid(lit::kReferenceSize, lit::kReferenceSize, 0, 0, 0, 1);
    Image albedo2 = Solid(lit::kReferenceSize, lit::kReferenceSize, 255, 255, 255,
                          static_cast<std::uint8_t>(s.promote[0].alphaHi));
    lit::Apply(lit2, &albedo2, s);
    const Px ink = At(lit2, pr.x0 + 4, pr.y0 + 4);
    CHECK_EQ(static_cast<int>(ink.r), static_cast<int>(s.palette.ink[0]));
    CHECK_EQ(static_cast<int>(ink.g), static_cast<int>(s.palette.ink[1]));
}

TEST("lit: a missing day texture costs the promotion and nothing else") {
    // A null albedo must never abort the recolor — the promoted label is a
    // nicety, the recolor is the product.
    Image img = Solid(256, 256, 255, 150, 89);
    Spec s = lit::GusIncandescent();
    const std::size_t n = lit::Apply(img, nullptr, s);
    CHECK(n > 0);
    CHECK_EQ(static_cast<int>(At(img, 2, 2).b), 0);
}

TEST("lit: the stock passthrough changes nothing at all") {
    Image before = Solid(128, 128, 255, 150, 89, 77);
    Image img = before;
    const std::size_t n = lit::Apply(img, nullptr, lit::StockPassthrough(lit::Texture::kPlacards));
    CHECK_EQ(n, static_cast<std::size_t>(0));
    CHECK(img.pixels == before.pixels);
}

TEST("lit: BOTH profiles are defined, and a choice between them MOVES pixels") {
    // ⚠ THE PREDICATE AND THE SPEC ARE ONE CHANGE. `ProfileIsDefined` was false
    // for as long as LED resolved to incandescent, precisely so no surface could
    // offer a choice that did nothing; it went true in the same commit that gave
    // LED real numbers (2026-08-24). This case is what stops the two drifting
    // apart again in either direction — a defined profile that changes nothing
    // is the same lie as an undefined one that silently substitutes.
    CHECK(lit::ProfileIsDefined(lit::Profile::kIncandescent));
    CHECK(lit::ProfileIsDefined(lit::Profile::kLed));

    for (int t = 0; t < lit::kTextureCount; ++t) {
        const auto tex = static_cast<lit::Texture>(t);
        const Spec h = lit::SpecForProfile(lit::Profile::kIncandescent, tex);
        const Spec l = lit::SpecForProfile(lit::Profile::kLed, tex);
        // The stock colors each texture's job is about.
        Image a = tex == lit::Texture::kKnobs ? Solid(2048, 64, 204, 130, 101)
                                              : Solid(1024, 64, 255, 150, 89);
        Image b = a;
        lit::Apply(a, nullptr, h);
        lit::Apply(b, nullptr, l);
        CHECK(!(a.pixels == b.pixels));
    }
}

TEST("lit: LED shares the measured curve and differs ONLY in the stack") {
    // ⚠ This is what makes "LED" a statement about the LAMP rather than a second
    // unrelated recolor — and it is what lets a future refit of Gus's curve
    // reach both eras for free. If a future LED wants its own curve, that is a
    // real decision and this case is where it gets argued.
    for (int t = 0; t < lit::kTextureCount; ++t) {
        const auto tex = static_cast<lit::Texture>(t);
        const Spec h = lit::SpecForProfile(lit::Profile::kIncandescent, tex);
        const Spec l = lit::SpecForProfile(lit::Profile::kLed, tex);
        CHECK_EQ(h.palette.r.black, l.palette.r.black);
        CHECK_EQ(h.palette.r.white, l.palette.r.white);
        CHECK_EQ(h.palette.r.gamma, l.palette.r.gamma);
        CHECK_EQ(h.palette.g.white, l.palette.g.white);
        CHECK_EQ(h.palette.g.gamma, l.palette.g.gamma);
        CHECK_EQ(h.palette.b.black, l.palette.b.black);
        CHECK_EQ(h.palette.b.white, l.palette.b.white);
        CHECK_EQ(h.palette.b.gamma, l.palette.b.gamma);
        CHECK(l.palette.r.IsIdentity());
    }
}

TEST("lit: the LED look is pinned by value on both textures") {
    // ⚠ AUTHORED, NOT FITTED — there is no LED reference file, so these came
    // from a bench session signed off by eye (2026-08-24). Pinned by value for
    // exactly that reason: nothing else can catch them drifting.
    const Spec p = lit::SpecForProfile(lit::Profile::kLed,
                                       lit::Texture::kPlacards);
    CHECK_EQ(p.palette.hsl.size(), static_cast<std::size_t>(1));
    CHECK(std::abs(p.palette.hsl[0].hue - 5.0) < 1e-9);
    CHECK(std::abs(p.palette.hsl[0].saturation) < 1e-9);
    CHECK(std::abs(p.palette.hsl[0].lightness - 10.0) < 1e-9);

    const Spec k = lit::SpecForProfile(lit::Profile::kLed, lit::Texture::kKnobs);
    CHECK_EQ(k.palette.hsl.size(), static_cast<std::size_t>(1));
    CHECK(std::abs(k.palette.hsl[0].hue - 13.0) < 1e-9);
    CHECK(std::abs(k.palette.hsl[0].saturation - (-40.0)) < 1e-9);
    CHECK(std::abs(k.palette.hsl[0].lightness - 17.0) < 1e-9);

    // ⚠ EXACTLY ONE OP, not incandescent's plus another. The knobs incandescent spec
    // already carries one, so an LED built by APPENDING would compound the two
    // and an incandescent retune would silently drag LED along with it.
    const Spec kh = lit::SpecForProfile(lit::Profile::kIncandescent,
                                        lit::Texture::kKnobs);
    CHECK_EQ(kh.palette.hsl.size(), static_cast<std::size_t>(1));

    // The signed-off colors, end to end.
    std::uint8_t lutR[256], lutG[256], lutB[256];
    lit::BuildLut(p.palette.r, lutR);
    lit::BuildLut(p.palette.g, lutG);
    lit::BuildLut(p.palette.b, lutB);
    std::uint8_t amber[3] = {lutR[255], lutG[150], lutB[89]};
    lit::ApplyHslStack(p.palette.hsl, amber);
    CHECK_EQ(static_cast<int>(amber[0]), 255);
    CHECK_EQ(static_cast<int>(amber[1]), 156);
    CHECK_EQ(static_cast<int>(amber[2]), 26);

    std::uint8_t mark[3] = {lutR[204], lutG[130], lutB[101]};
    lit::ApplyHslStack(k.palette.hsl, mark);
    CHECK_EQ(static_cast<int>(mark[0]), 188);
    CHECK_EQ(static_cast<int>(mark[1]), 152);
    CHECK_EQ(static_cast<int>(mark[2]), 95);
}

TEST("lit: both profiles carry the same geometry, so one serves every airframe") {
    // The A319/A320 and A321 stock atlases hold byte-identical artwork inside
    // every preserve rect and the promote region (measured 2026-08-24, and
    // re-checked by `harvest`). That is what lets a profile be a palette only —
    // so neither profile may carry airframe-specific geometry.
    const Spec h = lit::SpecForProfile(lit::Profile::kIncandescent, lit::Texture::kPlacards);
    const Spec l = lit::SpecForProfile(lit::Profile::kLed, lit::Texture::kPlacards);
    CHECK_EQ(h.preserve.size(), lit::PreserveRects(lit::Texture::kPlacards).size());
    CHECK_EQ(l.preserve.size(), lit::PreserveRects(lit::Texture::kPlacards).size());
    CHECK_EQ(h.promote.size(), lit::PromoteRegions(lit::Texture::kPlacards).size());
    for (std::size_t i = 0; i < h.preserve.size(); ++i) {
        CHECK_EQ(h.preserve[i].x0, l.preserve[i].x0);
        CHECK_EQ(h.preserve[i].y1, l.preserve[i].y1);
    }
}

// ⚠ "Incandescent", not "Halogen" — pinned by value because this string is what
// the sim's Integral Lights row offers, and it has to stay distinct from the
// "Old Halogen"/"New Halogen" labels on the five interior rows beside it. See
// the enum's comment for why the superset is the honest word here.
TEST("lit: every profile has a name") {
    CHECK_EQ(std::string(lit::ProfileName(lit::Profile::kIncandescent)),
             std::string("Incandescent"));
    CHECK_EQ(std::string(lit::ProfileName(lit::Profile::kLed)),
             std::string("LED"));
}

TEST("lit: a spec survives a JSON round trip") {
    const Spec original = lit::GusIncandescent();
    Spec back;
    std::string err;
    CHECK(lit::FromJson(lit::ToJson(original), back, err));
    CHECK_EQ(back.preserve.size(), original.preserve.size());
    CHECK_EQ(back.promote.size(), original.promote.size());
    for (std::size_t i = 0; i < back.preserve.size(); ++i) {
        CHECK_EQ(back.preserve[i].x0, original.preserve[i].x0);
        CHECK_EQ(back.preserve[i].y1, original.preserve[i].y1);
    }
    // Applying either must give the same pixels.
    Image a = Solid(512, 512, 255, 150, 89), b = a;
    lit::Apply(a, nullptr, original);
    lit::Apply(b, nullptr, back);
    CHECK(a.pixels == b.pixels);
}

TEST("lit: a degenerate curve is refused rather than dividing by zero") {
    Spec out;
    std::string err;
    CHECK(!lit::FromJson(
        "{\"palette\":{\"r\":{\"black\":10,\"white\":10,\"gamma\":1},"
        "\"g\":{},\"b\":{}}}",
        out, err));
    CHECK(!err.empty());
    CHECK(!lit::FromJson(
        "{\"palette\":{\"r\":{\"black\":0,\"white\":255,\"gamma\":0},"
        "\"g\":{},\"b\":{}}}",
        out, err));
    CHECK(!lit::FromJson("not json at all", out, err));
}

TEST("lit: every preserve rect is non-empty and named") {
    const auto& rects = lit::PreserveRects(lit::Texture::kPlacards);
    CHECK(!rects.empty());
    for (std::size_t i = 0; i < rects.size(); ++i) {
        CHECK(!rects[i].Empty());
        CHECK(rects[i].x1 <= lit::kReferenceSize);
        CHECK(rects[i].y1 <= lit::kReferenceSize);
        CHECK(std::string(lit::PreserveRectName(lit::Texture::kPlacards, i)).size() > 0);
    }
    // Out of range must not read off the end of the table.
    CHECK_EQ(std::string(lit::PreserveRectName(lit::Texture::kPlacards, rects.size())), std::string(""));
}

namespace {

bool AnyRectCovers(int x, int y) {
    for (const Rect& r : lit::PreserveRects(lit::Texture::kPlacards))
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) return true;
    return false;
}

}  // namespace

TEST("lit: the XPDR/TCAS panel is deliberately NOT preserved") {
    // ⚠ It was, until 2026-08-24. Gus left that block alone and ToLiss authored
    // it at rgb(255,121,49) — already amber, already more saturated than the
    // atlas around it — so preserving it looked right. But STBY / ALT RPTG /
    // TA ONLY / IDENT are integral panel legends, not lamp colors, and holding
    // them back left one panel visibly paler than its neighbours. This is the
    // ONE place the table knowingly departs from Gus's file (17,884 px), so a
    // rect creeping back over it would silently restore the old look while
    // every other number in the suite stayed green.
    const Rect block{3440, 2336, 4016, 2560};
    for (const Rect& r : lit::PreserveRects(lit::Texture::kPlacards)) {
        const bool overlaps = r.x0 < block.x1 && block.x0 < r.x1 &&
                              r.y0 < block.y1 && block.y0 < r.y1;
        CHECK(!overlaps);
    }
}

TEST("lit: the legend bands stop short of the integral lighting beside them") {
    // ⚠ THE REASON THE UPPER-RIGHT RECT IS FIVE BANDS AND NOT ONE BOX. It used
    // to be {3104, 0, 4096, 848} — the bounding box of the legends Gus left
    // alone — and a bounding box reaches its furthest member, so it also
    // swallowed the cabin-call placards and the pitch-trim ruling. Those are
    // ordinary integral lighting and MUST recolor; leaving them stock is what
    // "the boundary excludes parts that should be changed" meant.
    //
    // Each probe is the brightest lit pixel of that feature on the A319/A320
    // stock atlas, measured 2026-08-24. All four were inside the old rect.
    CHECK(!AnyRectCovers(3169, 281));  // COCKPIT call placard
    CHECK(!AnyRectCovers(3193, 507));  // FWD CABIN call placard
    CHECK(!AnyRectCovers(3169, 800));  // AFT CABIN call placard
    CHECK(!AnyRectCovers(3349, 610));  // the pitch-trim ruling

    // The legends themselves must still be covered, or the bands have been
    // trimmed past the point of doing their job.
    CHECK(AnyRectCovers(3300, 100));   // top legend row
    CHECK(AnyRectCovers(3500, 1000));  // the PUSH / PULL UP callout column
}

// --- knobs_LIT.png, and the two mechanisms it needed ------------------------

TEST("lit: a blackout forces RGB off and still leaves alpha alone") {
    // ⚠ Stock knobs_LIT glows on a panel wash and the cockpit loudspeaker, and
    // Gus's fix is deletion, not a color. No curve expresses "off" — one that
    // reached black here would reach black everywhere.
    Image img = Solid(64, 64, 200, 180, 170, 91);
    Spec s;
    s.referenceSize = 64;
    s.blackout.push_back(Rect{0, 0, 32, 64});
    lit::Apply(img, nullptr, s);
    const Px inside = At(img, 5, 5);
    CHECK_EQ(static_cast<int>(inside.r), 0);
    CHECK_EQ(static_cast<int>(inside.g), 0);
    CHECK_EQ(static_cast<int>(inside.b), 0);
    // ⚠ Alpha is roughness. Gus changed it on ZERO pixels of this texture,
    // including inside the blackouts.
    CHECK_EQ(static_cast<int>(inside.a), 91);
    CHECK_EQ(static_cast<int>(At(img, 40, 5).r), 200);  // outside: untouched
}

TEST("lit: a blackout beats a preserve rect over the same pixels") {
    // "This must not glow" is a stronger statement than "do not change this
    // color", and the loudspeaker rect deliberately covers lit placards.
    Image img = Solid(32, 32, 200, 180, 170);
    Spec s;
    s.referenceSize = 32;
    s.preserve.push_back(Rect{0, 0, 32, 32});
    s.blackout.push_back(Rect{0, 0, 16, 32});
    lit::Apply(img, nullptr, s);
    CHECK_EQ(static_cast<int>(At(img, 4, 4).r), 0);     // blacked out
    CHECK_EQ(static_cast<int>(At(img, 24, 4).r), 200);  // merely preserved
}

TEST("lit: an empty region means the whole atlas, a filled one confines it") {
    // ⚠ The placards recolor everything minus exceptions; the knobs recolor one
    // 30 px knob and must not touch the other 4.19 M pixels. If "empty" ever
    // stopped meaning "everywhere", every placard would go stock in silence.
    Spec whole = lit::GusIncandescent();
    whole.preserve.clear();
    whole.promote.clear();
    CHECK(whole.region.empty());
    Image a = Solid(64, 64, 255, 150, 89);
    lit::Apply(a, nullptr, whole);
    CHECK_EQ(static_cast<int>(At(a, 60, 60).b), 0);  // recolored

    Spec confined = whole;
    lit::Region half;
    half.rect = Rect{0, 0, lit::kReferenceSize / 2, lit::kReferenceSize};
    confined.region.push_back(half);
    Image b = Solid(64, 64, 255, 150, 89);
    lit::Apply(b, nullptr, confined);
    CHECK_EQ(static_cast<int>(At(b, 5, 5).b), 0);     // inside the region
    CHECK_EQ(static_cast<int>(At(b, 60, 5).b), 89);   // outside: untouched
}

TEST("lit: a crop outside the palette region is left alone, not recolored") {
    // ⚠ The empty-means-everywhere flag has to be read from the SPEC, before
    // the rects are clipped to the buffer. Clipping first would leave a crop
    // that misses every region with an empty list, which then reads as
    // "everywhere" and recolors exactly the pixels the region excluded.
    Spec s;
    s.referenceSize = lit::kReferenceSize;
    s.palette.b = Curve{200.0, 255.0, 1.0};
    lit::Region corner;
    corner.rect = Rect{0, 0, 64, 64};
    s.region.push_back(corner);
    Image crop = Solid(32, 32, 255, 150, 89);
    lit::Viewport vp;
    vp.originX = 2000;
    vp.originY = 2000;
    vp.fullWidth = lit::kReferenceSize;
    vp.fullHeight = lit::kReferenceSize;
    lit::Apply(crop, nullptr, s, vp);
    CHECK_EQ(static_cast<int>(At(crop, 8, 8).b), 89);
}

TEST("lit: the knobs markings take the SAME palette the placards do") {
    // ⚠ That is the whole point of recoloring them (2026-08-24). Stock lights
    // the index markings a pale salmon that reads as a different era of lamp
    // from the placards beside them; they were added to the spec so one era
    // palette moves both textures together. If these two ever drift apart,
    // tuning the placards would silently stop reaching the markings.
    const Spec p = lit::GusIncandescent();
    const Spec k = lit::GusKnobs();
    CHECK_EQ(k.palette.r.black, p.palette.r.black);
    CHECK_EQ(k.palette.g.white, p.palette.g.white);
    CHECK_EQ(k.palette.g.gamma, p.palette.g.gamma);
    CHECK_EQ(k.palette.b.black, p.palette.b.black);
    CHECK_EQ(k.palette.b.white, p.palette.b.white);
    CHECK_EQ(k.palette.b.gamma, p.palette.b.gamma);
}

TEST("lit: exactly one knobs region carries its own curve, and it is the knob") {
    // ⚠ The icy-blue-white knob is the one lamp the era palette cannot reach —
    // that curve alone leaves it blue. Everything else on this atlas must follow
    // the palette, so a second override appearing here means something was
    // special-cased that should not have been.
    const Spec k = lit::GusKnobs();
    std::size_t own = 0, first = 0;
    for (std::size_t i = 0; i < k.region.size(); ++i)
        if (k.region[i].ownPalette) {
            if (own == 0) first = i;
            ++own;
        }
    CHECK_EQ(own, static_cast<std::size_t>(1));
    CHECK_EQ(first, static_cast<std::size_t>(0));  // kept first, by convention
    CHECK(k.region.size() > 1);
    // And it genuinely differs from the palette it is overriding.
    CHECK(!(k.region[first].palette.b.black == k.palette.b.black &&
            k.region[first].palette.b.gamma == k.palette.b.gamma));
}

TEST("lit: a region's own palette applies to it and to nothing else") {
    Spec s;
    s.referenceSize = 64;
    s.palette.b = Curve{};                      // spec palette: identity
    lit::Region plain, own;
    plain.rect = Rect{0, 0, 16, 64};
    own.rect = Rect{32, 0, 48, 64};
    own.ownPalette = true;
    own.palette.b = Curve{200.0, 255.0, 1.0};   // crushes blue
    s.region.push_back(plain);
    s.region.push_back(own);
    Image img = Solid(64, 64, 255, 150, 89);
    lit::Apply(img, nullptr, s);
    CHECK_EQ(static_cast<int>(At(img, 8, 8).b), 89);   // spec palette: identity
    CHECK_EQ(static_cast<int>(At(img, 40, 8).b), 0);   // override: crushed
    CHECK_EQ(static_cast<int>(At(img, 24, 8).b), 89);  // outside every region
}

TEST("lit: switching profile changes the blue knob too, not just its neighbours") {
    // ⚠ AN OVERRIDE IS PART OF THE LOOK, NOT THE GEOMETRY. Left behind by a
    // profile switch, the icy-blue knob would be the one lamp on the panel that
    // did not change era.
    //
    // ⚠ THE MECHANISM CHANGED ON 2026-08-24 AND THE CONTRACT DID NOT. This used
    // to assert that the two profiles gave the override different CURVES, which
    // was true of the old LED seed. Both eras now share every curve and differ
    // only in the stack — so the override's curve is identical by design, and it
    // changes era by INHERITING the spec's stack instead. Asserting the outcome
    // rather than the mechanism is what keeps this case honest across that kind
    // of redesign; the old form would have gone green on a knob that was in fact
    // frozen.
    const Spec h = lit::SpecForProfile(lit::Profile::kIncandescent,
                                       lit::Texture::kKnobs);
    const Spec l = lit::SpecForProfile(lit::Profile::kLed, lit::Texture::kKnobs);
    CHECK(h.region[0].ownPalette);
    CHECK(l.region[0].ownPalette);
    // The geometry is untouched by the switch.
    CHECK_EQ(h.region.size(), l.region.size());
    CHECK_EQ(h.region[0].rect.x0, l.region[0].rect.x0);

    // Render just that knob's stock icy blue-white under each era.
    auto renderKnob = [](Spec s) {
        s.blackout.clear();
        s.preserve.clear();
        s.referenceSize = 64;
        s.region.resize(1);
        s.region[0].rect = Rect{0, 0, 32, 32};
        Image img = Solid(64, 64, 166, 208, 218);
        lit::Apply(img, nullptr, s);
        return At(img, 8, 8);
    };
    const Px hp = renderKnob(h);
    const Px lp = renderKnob(l);
    CHECK(!(hp.r == lp.r && hp.g == lp.g && hp.b == lp.b));
}

TEST("lit: the knobs spec is authored against its own 2048 atlas") {
    // ⚠ The reference size travels in the SPEC. Scaling a 2048-authored rect as
    // though it were 4096 would put every knobs region on a quarter of the
    // atlas — geometry that still "works", silently, on the wrong pixels.
    const Spec k = lit::SpecForProfile(lit::Profile::kIncandescent,
                                       lit::Texture::kKnobs);
    CHECK_EQ(k.referenceSize, 2048);
    CHECK_EQ(lit::TextureReferenceSize(lit::Texture::kKnobs), 2048);
    const Spec p = lit::SpecForProfile(lit::Profile::kIncandescent,
                                       lit::Texture::kPlacards);
    CHECK_EQ(p.referenceSize, lit::kReferenceSize);
    // And the two carry genuinely different geometry.
    CHECK(!k.blackout.empty());
    CHECK(!k.region.empty());
    CHECK(p.blackout.empty());
    CHECK(p.region.empty());
    CHECK(!p.preserve.empty());
}

TEST("lit: red stays bit-exact identity on the knobs too") {
    // The same signature as the placard fit, and the main reason to believe it
    // is the same author doing the same kind of edit.
    CHECK(lit::GusKnobs().palette.r.IsIdentity());
}

TEST("lit: passthrough drops the blackouts, or 'stock' would have holes in it") {
    const Spec s = lit::StockPassthrough(lit::Texture::kKnobs);
    CHECK(s.blackout.empty());
    CHECK(s.promote.empty());
    Image img = Solid(32, 32, 200, 180, 170);
    const Image before = img;
    lit::Apply(img, nullptr, s);
    CHECK(img.pixels == before.pixels);
}

TEST("lit: every texture has a name, a stable key and its two file names") {
    for (int i = 0; i < lit::kTextureCount; ++i) {
        const auto t = static_cast<lit::Texture>(i);
        CHECK(std::string(lit::TextureName(t)).size() > 0);
        CHECK(std::string(lit::TextureLitName(t)).size() > 0);
        CHECK(std::string(lit::TextureDayName(t)).size() > 0);
        lit::Texture back;
        CHECK(lit::TextureFromKey(lit::TextureKey(t), back));
        CHECK(back == t);
    }
    lit::Texture ignored;
    CHECK(!lit::TextureFromKey("not-a-texture", ignored));
    // The names the harvest and the aircraft folder both depend on.
    CHECK_EQ(std::string(lit::TextureLitName(lit::Texture::kKnobs)),
             std::string("knobs_LIT.png"));
    CHECK_EQ(std::string(lit::TextureLitName(lit::Texture::kPlacards)),
             std::string("text_LIT.png"));
}

TEST("lit: a knobs spec survives JSON with its size and both new rect lists") {
    // ⚠ referenceSize is part of the recipe. A spec that lost it would come back
    // as a 4096 one and land every rect in the wrong place.
    const Spec original = lit::GusKnobs();
    Spec back;
    std::string err;
    CHECK(lit::FromJson(lit::ToJson(original), back, err));
    CHECK_EQ(back.referenceSize, original.referenceSize);
    CHECK_EQ(back.region.size(), original.region.size());
    CHECK_EQ(back.blackout.size(), original.blackout.size());
    // ⚠ The override rides along. A spec that lost it would come back applying
    // the placard curve to the icy-blue knob, which leaves it blue — a wrong
    // look that still looks deliberate.
    for (std::size_t i = 0; i < back.region.size(); ++i) {
        CHECK_EQ(back.region[i].ownPalette, original.region[i].ownPalette);
        CHECK_EQ(back.region[i].rect.x0, original.region[i].rect.x0);
        if (original.region[i].ownPalette)
            CHECK_EQ(back.region[i].palette.b.black,
                     original.region[i].palette.b.black);
    }
    for (std::size_t i = 0; i < back.blackout.size(); ++i) {
        CHECK_EQ(back.blackout[i].x0, original.blackout[i].x0);
        CHECK_EQ(back.blackout[i].y1, original.blackout[i].y1);
    }
    Image a = Solid(256, 256, 200, 180, 170), b = a;
    lit::Apply(a, nullptr, original);
    lit::Apply(b, nullptr, back);
    CHECK(a.pixels == b.pixels);

    // A spec written before knobs existed has no referenceSize and must read
    // back as a placard one.
    Spec old;
    CHECK(lit::FromJson(
        "{\"palette\":{\"r\":{},\"g\":{},\"b\":{}},\"preserve\":[]}", old, err));
    CHECK_EQ(old.referenceSize, lit::kReferenceSize);
}

TEST("lit: every blackout rect is non-empty, named and inside its atlas") {
    for (int i = 0; i < lit::kTextureCount; ++i) {
        const auto t = static_cast<lit::Texture>(i);
        const int ref = lit::TextureReferenceSize(t);
        const auto& rects = lit::BlackoutRects(t);
        for (std::size_t r = 0; r < rects.size(); ++r) {
            CHECK(!rects[r].Empty());
            CHECK(rects[r].x1 <= ref);
            CHECK(rects[r].y1 <= ref);
            CHECK(std::string(lit::BlackoutRectName(t, r)).size() > 0);
        }
        CHECK_EQ(std::string(lit::BlackoutRectName(t, rects.size())),
                 std::string(""));
        const auto& regions = lit::PaletteRegions(t);
        for (std::size_t r = 0; r < regions.size(); ++r) {
            CHECK(!regions[r].Empty());
            CHECK(regions[r].x1 <= ref);
            CHECK(std::string(lit::PaletteRegionName(t, r)).size() > 0);
        }
    }
}

// --- the HSL tuning stack ---------------------------------------------------
// It sits ON TOP of the Levels curve and cannot replace it (see lit_recolor.h).
// Everything here is about the two ways that arrangement can go wrong: the stack
// costing something when it should cost nothing, and the stack reaching a pixel
// it must never reach.

TEST("lit: the placards ship on the measured curve alone, with NO stack") {
    // ⚠ THE FIT IS THE PLACARDS' WHOLE CLAIM TO BEING RIGHT — 95.7% within 2
    // levels of Gus's own file. A stack here is a departure from a measurement,
    // and it is a big one: the knobs' approved tuning, run over this texture,
    // takes placard amber from rgb(255,124,0) to rgb(255,192,43). If this case
    // ever fails, the question to ask is whether the fidelity number was
    // knowingly given up, not whether the test is stale.
    const Spec s = lit::SpecForProfile(lit::Profile::kIncandescent,
                                       lit::Texture::kPlacards);
    CHECK(s.palette.hsl.empty());
    CHECK(lit::StockPassthrough(lit::Texture::kPlacards).palette.hsl.empty());
    // And the promote ink is therefore the stated one, untouched.
    std::uint8_t ink[3] = {s.palette.ink[0], s.palette.ink[1], s.palette.ink[2]};
    lit::ApplyHslStack(s.palette.hsl, ink);
    CHECK_EQ(static_cast<int>(ink[0]), 250);
    CHECK_EQ(static_cast<int>(ink[1]), 73);
    CHECK_EQ(static_cast<int>(ink[2]), 32);
}

TEST("lit: the knobs ship the shared curve PLUS exactly one tuning op") {
    // ⚠ APPROVED IN-SIM 2026-08-24, and a judgement rather than a fit — the
    // index markings are Photon's extension, so there is no Gus file to measure
    // them against. Pinned by value so a retune is a visible diff and not a
    // drift. hue +13 / brightness +17.
    const Spec s = lit::SpecForProfile(lit::Profile::kIncandescent,
                                       lit::Texture::kKnobs);
    CHECK_EQ(s.palette.hsl.size(), static_cast<std::size_t>(1));
    const lit::HslOp& op = s.palette.hsl[0];
    CHECK_EQ(static_cast<int>(op.kind),
             static_cast<int>(lit::HslOp::Kind::kAdjust));
    CHECK(op.enabled);
    CHECK(std::abs(op.hue - 13.0) < 1e-9);
    CHECK(std::abs(op.saturation) < 1e-9);
    CHECK(std::abs(op.lightness - 17.0) < 1e-9);

    // ⚠ THE CURVE IS STILL THE PLACARDS'. That sharing is what keeps the
    // markings in the same era as the placards; the stack is only where "a
    // pointer stripe is not a placard" gets said. Diverging on both would make
    // this a second look rather than a tuned one.
    const Spec p = lit::SpecForProfile(lit::Profile::kIncandescent,
                                       lit::Texture::kPlacards);
    CHECK_EQ(s.palette.g.white, p.palette.g.white);
    CHECK_EQ(s.palette.g.gamma, p.palette.g.gamma);
    CHECK_EQ(s.palette.b.black, p.palette.b.black);
    CHECK_EQ(s.palette.b.white, p.palette.b.white);
    CHECK_EQ(s.palette.b.gamma, p.palette.b.gamma);
    CHECK(s.palette.r.IsIdentity());

    // Stock index-marking salmon lands on the signed-off color.
    std::uint8_t lutR[256], lutG[256], lutB[256];
    lit::BuildLut(s.palette.r, lutR);
    lit::BuildLut(s.palette.g, lutG);
    lit::BuildLut(s.palette.b, lutB);
    std::uint8_t c[3] = {lutR[204], lutG[130], lutB[101]};
    lit::ApplyHslStack(s.palette.hsl, c);
    CHECK_EQ(static_cast<int>(c[0]), 219);
    CHECK_EQ(static_cast<int>(c[1]), 158);
    CHECK_EQ(static_cast<int>(c[2]), 64);

    // ⚠ Passthrough is still the untouched aircraft, tuning included.
    CHECK(lit::StockPassthrough(lit::Texture::kKnobs).palette.hsl.empty());
}

TEST("lit: the knobs tuning reaches the icy-blue knob rather than skipping it") {
    // The override states no stack of its own, so it inherits — see the
    // inherit rule. Left behind it would be the one lamp on the panel still on
    // the untuned look, which is the failure the whole rule exists for.
    const Spec s = lit::SpecForProfile(lit::Profile::kIncandescent,
                                       lit::Texture::kKnobs);
    bool sawOverride = false;
    for (const auto& rg : s.region) {
        if (!rg.ownPalette) continue;
        sawOverride = true;
        CHECK(rg.palette.hsl.empty());  // empty == inherit, not == no tuning
    }
    CHECK(sawOverride);

    // Prove it end to end: the knob's stock icy blue-white must land warmer than
    // the untuned curve alone would put it.
    Spec tuned = s;
    tuned.blackout.clear();
    tuned.referenceSize = 64;
    tuned.region.resize(1);
    tuned.region[0].rect = Rect{0, 0, 32, 32};
    Spec untuned = tuned;
    untuned.palette.hsl.clear();
    Image a = Solid(64, 64, 166, 208, 218), b = a;
    lit::Apply(a, nullptr, tuned);
    lit::Apply(b, nullptr, untuned);
    CHECK(At(a, 8, 8).r > At(b, 8, 8).r);
    CHECK(At(a, 8, 8).g > At(b, 8, 8).g);
}

TEST("lit: an op at zero is SKIPPED, not computed, so it cannot cost a byte") {
    // ⚠ rgb -> hsl -> rgb through doubles is not guaranteed to land back on the
    // same byte. That is the entire reason identity ops are skipped rather than
    // run, and this is the case that would catch it being "simplified" away.
    Spec plain = lit::GusIncandescent();
    plain.preserve.clear();
    plain.promote.clear();
    Spec zeroed = plain;
    zeroed.palette.hsl.push_back(lit::HslOp{});  // enabled, all zeros
    CHECK(zeroed.palette.hsl[0].IsIdentity());
    CHECK(lit::HslStackIsIdentity(zeroed.palette.hsl));

    // Sweep the channel space rather than one convenient color.
    Image a = Solid(64, 64, 0, 0, 0);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * 64 + x) * 4;
            a.pixels[o + 0] = static_cast<std::uint8_t>(x * 4);
            a.pixels[o + 1] = static_cast<std::uint8_t>(y * 4);
            a.pixels[o + 2] = static_cast<std::uint8_t>((x + y) * 2);
        }
    Image b = a;
    lit::Apply(a, nullptr, plain);
    lit::Apply(b, nullptr, zeroed);
    CHECK(a.pixels == b.pixels);
}

TEST("lit: a disabled op does nothing however large its numbers are") {
    lit::HslOp op;
    op.enabled = false;
    op.hue = 120.0;
    op.saturation = -100.0;
    op.lightness = -100.0;
    std::vector<lit::HslOp> ops{op};
    CHECK(lit::HslStackIsIdentity(ops));
    std::uint8_t rgb[3] = {255, 122, 0};
    lit::ApplyHslStack(ops, rgb);
    CHECK_EQ(static_cast<int>(rgb[0]), 255);
    CHECK_EQ(static_cast<int>(rgb[1]), 122);
    CHECK_EQ(static_cast<int>(rgb[2]), 0);
}

TEST("lit: BLACK STAYS BLACK under every op, which the whole atlas rests on") {
    // ⚠ THE ONE PROPERTY THAT MAKES THIS LAYER SAFE HERE. A LIT texture is
    // emissive over black and ~90% of it IS black, so an op that lifted black
    // even slightly would light the entire cockpit up like a lamp — and it would
    // do it uniformly, which reads in-sim as a broken shader rather than as a
    // tuning mistake. It is also why `lightness` multiplies instead of driving
    // toward white the way Photoshop's slider does: Photoshop's +50 takes black
    // to mid gray.
    for (double hue : {-180.0, -37.0, 0.0, 90.0, 180.0}) {
        for (double sat : {-100.0, -1.0, 0.0, 55.0, 100.0}) {
            for (double light : {-100.0, -20.0, 0.0, 50.0, 100.0}) {
                for (int kind = 0; kind < 2; ++kind) {
                    lit::HslOp op;
                    op.kind = static_cast<lit::HslOp::Kind>(kind);
                    op.hue = kind ? std::abs(hue) : hue;
                    op.saturation = kind ? std::abs(sat) : sat;
                    op.lightness = light;
                    std::uint8_t rgb[3] = {0, 0, 0};
                    lit::ApplyHslStack({op}, rgb);
                    CHECK_EQ(static_cast<int>(rgb[0]), 0);
                    CHECK_EQ(static_cast<int>(rgb[1]), 0);
                    CHECK_EQ(static_cast<int>(rgb[2]), 0);
                }
            }
        }
    }
    // And through the real pipeline, on the real background color.
    Spec s = lit::GusIncandescent();
    s.preserve.clear();
    s.promote.clear();
    lit::HslOp op;
    op.saturation = 100.0;
    op.lightness = 100.0;
    op.hue = 90.0;
    s.palette.hsl.push_back(op);
    Image img = Solid(32, 32, 0, 0, 0, 0);
    CHECK_EQ(lit::Apply(img, nullptr, s), static_cast<std::size_t>(0));
    for (std::size_t i = 0; i < img.pixels.size(); ++i)
        CHECK_EQ(static_cast<int>(img.pixels[i]), 0);
}

TEST("lit: saturation -100 is exactly gray and +100 is exactly full") {
    lit::HslOp gray;
    gray.saturation = -100.0;
    std::uint8_t a[3] = {255, 122, 40};
    lit::ApplyHslStack({gray}, a);
    CHECK_EQ(static_cast<int>(a[0]), static_cast<int>(a[1]));
    CHECK_EQ(static_cast<int>(a[1]), static_cast<int>(a[2]));

    lit::HslOp full;
    full.saturation = 100.0;
    std::uint8_t b[3] = {200, 150, 120};
    lit::ApplyHslStack({full}, b);
    // Fully saturated means one channel at the top of its bicone and one at the
    // bottom; the exact values depend on the lightness, which is untouched.
    CHECK(b[0] > 200);
    CHECK(b[2] < 120);
}

TEST("lit: brightness is a GAIN, so -100 is black and +100 doubles lightness") {
    lit::HslOp off;
    off.lightness = -100.0;
    std::uint8_t a[3] = {255, 122, 0};
    lit::ApplyHslStack({off}, a);
    CHECK_EQ(static_cast<int>(a[0]), 0);
    CHECK_EQ(static_cast<int>(a[1]), 0);
    CHECK_EQ(static_cast<int>(a[2]), 0);

    // A mid-lightness color at +100 clips to white: its L was 0.5, so 2x is 1.0.
    lit::HslOp up;
    up.lightness = 100.0;
    std::uint8_t b[3] = {255, 122, 0};
    lit::ApplyHslStack({up}, b);
    CHECK_EQ(static_cast<int>(b[0]), 255);
    CHECK_EQ(static_cast<int>(b[1]), 255);
    CHECK_EQ(static_cast<int>(b[2]), 255);

    // And a dark one lands brighter without clipping.
    lit::HslOp half;
    half.lightness = 50.0;
    std::uint8_t c[3] = {80, 40, 0};
    lit::ApplyHslStack({half}, c);
    CHECK(c[0] > 80 && c[0] < 255);
}

TEST("lit: colorize forces one hue and is never treated as identity") {
    lit::HslOp op;
    op.kind = lit::HslOp::Kind::kColorize;
    op.hue = 0.0;
    op.saturation = 0.0;
    op.lightness = 0.0;
    // ⚠ All zeros, and still not identity: it says "red at zero saturation",
    // i.e. gray. Treating it as identity would silently drop it from a stack.
    CHECK(!op.IsIdentity());
    CHECK(!lit::HslStackIsIdentity({op}));

    op.hue = 120.0;  // green
    op.saturation = 100.0;
    std::uint8_t rgb[3] = {255, 122, 0};
    lit::ApplyHslStack({op}, rgb);
    CHECK(rgb[1] > rgb[0]);
    CHECK(rgb[1] > rgb[2]);
}

TEST("lit: the stack runs AFTER the curve, not instead of it or before it") {
    // Order is the whole contract. Computing it by hand — curve first, then the
    // stack on the result — must equal what Apply does to the same pixel.
    Spec s = lit::GusIncandescent();
    s.preserve.clear();
    s.promote.clear();
    lit::HslOp op;
    op.hue = 20.0;
    op.saturation = -30.0;
    op.lightness = 15.0;
    s.palette.hsl.push_back(op);

    const std::uint8_t stock[3] = {255, 150, 89};
    std::uint8_t lutR[256], lutG[256], lutB[256];
    lit::BuildLut(s.palette.r, lutR);
    lit::BuildLut(s.palette.g, lutG);
    lit::BuildLut(s.palette.b, lutB);
    std::uint8_t want[3] = {lutR[stock[0]], lutG[stock[1]], lutB[stock[2]]};
    lit::ApplyHslStack(s.palette.hsl, want);

    Image img = Solid(8, 8, stock[0], stock[1], stock[2]);
    lit::Apply(img, nullptr, s);
    const Px p = At(img, 4, 4);
    CHECK_EQ(static_cast<int>(p.r), static_cast<int>(want[0]));
    CHECK_EQ(static_cast<int>(p.g), static_cast<int>(want[1]));
    CHECK_EQ(static_cast<int>(p.b), static_cast<int>(want[2]));
}

TEST("lit: ops compose in order, so two halves are not one whole") {
    lit::HslOp a, b;
    a.hue = 30.0;
    b.hue = 30.0;
    lit::HslOp both;
    both.hue = 60.0;
    std::uint8_t twice[3] = {255, 122, 0};
    std::uint8_t once[3] = {255, 122, 0};
    lit::ApplyHslStack({a, b}, twice);
    lit::ApplyHslStack({both}, once);
    // Hue rotation is additive, so these agree to within rounding — which is
    // what proves the ops are being run in sequence rather than the last one
    // winning.
    for (int i = 0; i < 3; ++i) CHECK(std::abs(twice[i] - once[i]) <= 2);
}

TEST("lit: the promote ink travels through the stack with everything else") {
    // ⚠ PEDAL DISC is painted artwork sitting among recolored artwork. An ink
    // that held its own hue through a hue shift would be the one thing on the
    // pedestal still wearing the old era, inches from placards that moved.
    Spec s = lit::GusIncandescent();
    s.preserve.clear();
    s.region.clear();
    s.promote.clear();
    lit::PromoteRegion pr;
    pr.rect = Rect{0, 0, 4096, 4096};
    pr.alphaLo = 0;
    pr.alphaHi = 255;
    s.promote.push_back(pr);
    lit::HslOp op;
    op.kind = lit::HslOp::Kind::kColorize;
    op.hue = 120.0;  // green, which the amber ink is nowhere near
    op.saturation = 100.0;
    s.palette.hsl.push_back(op);

    Image lit_ = Solid(8, 8, 0, 0, 0, 0);
    const Image albedo = Solid(8, 8, 255, 255, 255, 255);
    lit::Apply(lit_, &albedo, s);
    const Px p = At(lit_, 4, 4);
    CHECK(p.g > p.r);
    CHECK(p.g > p.b);
}

TEST("lit: a stack survives JSON with its kind, its switch and its numbers") {
    Spec s = lit::GusKnobs();
    lit::HslOp a;
    a.hue = -12.5;
    a.saturation = 33.0;
    a.lightness = -7.0;
    lit::HslOp b;
    b.kind = lit::HslOp::Kind::kColorize;
    b.enabled = false;  // ⚠ off must round-trip, or a loaded look changes
    b.hue = 200.0;
    b.saturation = 45.0;
    s.palette.hsl = {a, b};
    // A region override carries its own, independently.
    for (auto& rg : s.region)
        if (rg.ownPalette) rg.palette.hsl.push_back(a);

    Spec back;
    std::string err;
    CHECK(lit::FromJson(lit::ToJson(s), back, err));
    CHECK_EQ(back.palette.hsl.size(), static_cast<std::size_t>(2));
    CHECK_EQ(static_cast<int>(back.palette.hsl[0].kind),
             static_cast<int>(lit::HslOp::Kind::kAdjust));
    CHECK(back.palette.hsl[0].enabled);
    CHECK(std::abs(back.palette.hsl[0].hue - (-12.5)) < 1e-9);
    CHECK(std::abs(back.palette.hsl[0].saturation - 33.0) < 1e-9);
    CHECK(std::abs(back.palette.hsl[0].lightness - (-7.0)) < 1e-9);
    CHECK_EQ(static_cast<int>(back.palette.hsl[1].kind),
             static_cast<int>(lit::HslOp::Kind::kColorize));
    CHECK(!back.palette.hsl[1].enabled);
    for (std::size_t i = 0; i < back.region.size(); ++i)
        if (back.region[i].ownPalette)
            CHECK_EQ(back.region[i].palette.hsl.size(),
                     static_cast<std::size_t>(1));

    Image x = Solid(128, 128, 200, 180, 170), y = x;
    lit::Apply(x, nullptr, s);
    lit::Apply(y, nullptr, back);
    CHECK(x.pixels == y.pixels);
}

TEST("lit: a spec written before the stack existed loads with an empty one") {
    Spec old;
    std::string err;
    CHECK(lit::FromJson("{\"palette\":{\"r\":{},\"g\":{},\"b\":{}}}", old, err));
    CHECK(old.palette.hsl.empty());
}

TEST("lit: an unrecognized op kind falls back to adjust, never to colorize") {
    // ⚠ A colorize fallback would repaint the whole atlas one flat hue off a
    // typo. Adjust with the same numbers is at worst a no-op.
    Spec s;
    std::string err;
    CHECK(lit::FromJson(
        "{\"palette\":{\"r\":{},\"g\":{},\"b\":{},"
        "\"hsl\":[{\"kind\":\"sepia\",\"hue\":0,\"saturation\":0,"
        "\"lightness\":0}]}}",
        s, err));
    CHECK_EQ(s.palette.hsl.size(), static_cast<std::size_t>(1));
    CHECK_EQ(static_cast<int>(s.palette.hsl[0].kind),
             static_cast<int>(lit::HslOp::Kind::kAdjust));
    CHECK(lit::HslStackIsIdentity(s.palette.hsl));
}

TEST("lit: a region's stack applies to it and to nothing else") {
    Spec s = lit::GusKnobs();
    s.blackout.clear();
    s.preserve.clear();
    // Two regions far apart, one of them carrying an override with a stack.
    s.region.clear();
    lit::Region plain;
    plain.rect = Rect{0, 0, 16, 16};
    lit::Region tinted;
    tinted.rect = Rect{32, 0, 48, 16};
    tinted.ownPalette = true;
    lit::HslOp op;
    op.kind = lit::HslOp::Kind::kColorize;
    op.hue = 120.0;
    op.saturation = 100.0;
    tinted.palette.hsl.push_back(op);
    s.region = {plain, tinted};
    s.referenceSize = 64;

    Image img = Solid(64, 64, 220, 140, 90);
    lit::Apply(img, nullptr, s);
    const Px inPlain = At(img, 8, 8);
    const Px inTinted = At(img, 40, 8);
    const Px outside = At(img, 24, 8);
    CHECK(inTinted.g > inTinted.r);           // colorized green
    CHECK(inPlain.r >= inPlain.g);            // still amber
    CHECK_EQ(static_cast<int>(outside.r), 220);  // untouched entirely
    CHECK_EQ(static_cast<int>(outside.g), 140);
}

TEST("lit: an override region inherits the spec's stack when it states none") {
    // ⚠ THE OVERRIDE IS A CURVE, NOT AN ERA. The icy-blue knob carries its own
    // curve because that ONE lamp is authored in a different color, not because
    // it is a different look — so a tuning move has to reach it, or it is the
    // one lamp on the panel that holds still while everything around it warms.
    // Same rule the studio's profile copy follows.
    Spec s = lit::GusKnobs();
    s.blackout.clear();
    s.preserve.clear();
    s.referenceSize = 64;
    lit::Region plain;
    plain.rect = Rect{0, 0, 16, 16};
    lit::Region odd;
    odd.rect = Rect{32, 0, 48, 16};
    odd.ownPalette = true;  // own curve, NO stack of its own
    s.region = {plain, odd};
    lit::HslOp op;
    op.kind = lit::HslOp::Kind::kColorize;
    op.hue = 120.0;
    op.saturation = 100.0;
    s.palette.hsl.push_back(op);

    Image img = Solid(64, 64, 220, 140, 90);
    lit::Apply(img, nullptr, s);
    const Px inherited = At(img, 40, 8);
    CHECK(inherited.g > inherited.r);  // the spec's stack reached it
    CHECK(inherited.g > inherited.b);

    // And a region that DOES state its own keeps it, rather than being
    // overwritten by the spec's.
    lit::HslOp mine;
    mine.kind = lit::HslOp::Kind::kColorize;
    mine.hue = 240.0;  // blue, nowhere near the spec's green
    mine.saturation = 100.0;
    s.region[1].palette.hsl.push_back(mine);
    Image img2 = Solid(64, 64, 220, 140, 90);
    lit::Apply(img2, nullptr, s);
    const Px own = At(img2, 40, 8);
    CHECK(own.b > own.g);
    CHECK(own.b > own.r);
}

TEST("lit: a blackout is not reachable by the stack, however bright it goes") {
    // Blackout means "this must not glow" and it wins over everything — a
    // +100 brightness op must not resurrect the loudspeaker.
    Spec s = lit::GusKnobs();
    s.preserve.clear();
    s.region.clear();
    s.blackout = {Rect{0, 0, 32, 32}};
    s.referenceSize = 64;
    lit::HslOp op;
    op.lightness = 100.0;
    op.saturation = 100.0;
    s.palette.hsl.push_back(op);
    Image img = Solid(64, 64, 200, 180, 170);
    lit::Apply(img, nullptr, s);
    const Px p = At(img, 8, 8);
    CHECK_EQ(static_cast<int>(p.r), 0);
    CHECK_EQ(static_cast<int>(p.g), 0);
    CHECK_EQ(static_cast<int>(p.b), 0);
    CHECK_EQ(static_cast<int>(p.a), 200);  // roughness, still untouched
}

TEST("lit: a preserve rect is not reachable by the stack either") {
    Spec s = lit::GusIncandescent();
    s.promote.clear();
    s.preserve = {Rect{0, 0, 32, 32}};
    s.referenceSize = 64;
    lit::HslOp op;
    op.kind = lit::HslOp::Kind::kColorize;
    op.hue = 120.0;
    op.saturation = 100.0;
    s.palette.hsl.push_back(op);
    Image img = Solid(64, 64, 255, 150, 89);
    lit::Apply(img, nullptr, s);
    const Px kept = At(img, 8, 8);
    CHECK_EQ(static_cast<int>(kept.r), 255);
    CHECK_EQ(static_cast<int>(kept.g), 150);
    CHECK_EQ(static_cast<int>(kept.b), 89);
    CHECK(At(img, 40, 40).g > At(img, 40, 40).r);  // and elsewhere it did run
}

TEST("lit: every op kind has a name") {
    CHECK_EQ(std::string(lit::HslKindName(lit::HslOp::Kind::kAdjust)),
             std::string("Adjust"));
    CHECK_EQ(std::string(lit::HslKindName(lit::HslOp::Kind::kColorize)),
             std::string("Colorize"));
}

TEST("image: a PNG round-trips through our own encoder") {
    // ⚠ The encoder is ours (there is no stb_image_write in third_party/), so
    // "does a real decoder read it back byte-for-byte" is the load-bearing test.
    Image img;
    img.width = 61;  // deliberately not a multiple of anything
    img.height = 37;
    img.pixels.resize(static_cast<std::size_t>(61) * 37 * 4);
    for (std::size_t i = 0; i < img.pixels.size(); ++i)
        img.pixels[i] = static_cast<std::uint8_t>((i * 7 + (i >> 5) * 13) & 0xFF);

    const std::vector<std::uint8_t> bytes = image::EncodePng(img);
    CHECK(bytes.size() > 8);
    // PNG signature.
    CHECK_EQ(static_cast<int>(bytes[0]), 137);
    CHECK_EQ(static_cast<int>(bytes[1]), 'P');

    TempDir tmp;
    const auto path = tmp / "roundtrip.png";
    std::string err;
    CHECK(image::SavePng(path, img, err));
    Image back;
    CHECK(image::LoadPng(path, back, err));
    CHECK_EQ(back.width, img.width);
    CHECK_EQ(back.height, img.height);
    CHECK(back.pixels == img.pixels);
}

TEST("image: large runs of identical pixels round-trip and actually compress") {
    // The real atlas is ~79% transparent black; if the LZ77 match path were
    // broken this would still decode but the file would be enormous.
    Image img = Solid(512, 512, 0, 0, 0, 0);
    const std::vector<std::uint8_t> bytes = image::EncodePng(img);
    CHECK(bytes.size() < static_cast<std::size_t>(512) * 512 * 4 / 50);
    TempDir tmp;
    const auto path = tmp / "flat.png";
    std::string err;
    CHECK(image::SavePng(path, img, err));
    Image back;
    CHECK(image::LoadPng(path, back, err));
    CHECK(back.pixels == img.pixels);
}

// ─────────────────────────────────────────────────────────────────────────────
// "is this already ours?" — the gate the installer reads before deriving
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// A flat atlas in ToLiss's own stock placard amber. Big enough that the placard
// test clears its 500-sample floor and the knobs test clears its 1000-pixel
// blackout-area floor.
lit::Image FlatAmber(int side) {
    lit::Image img;
    img.width = side;
    img.height = side;
    img.pixels.assign(static_cast<std::size_t>(side) * side * 4, 0);
    for (std::size_t i = 0; i < img.pixels.size(); i += 4) {
        img.pixels[i] = 255;
        img.pixels[i + 1] = 150;
        img.pixels[i + 2] = 89;
        img.pixels[i + 3] = 255;
    }
    return img;
}

}  // namespace

TEST("lit: stock reads as stock, and BOTH eras read as ours") {
    // ⚠ THE SECOND HALF IS THE ONE THAT BROKE. The threshold was tuned when
    // incandescent was the only look there was, so LED — which leaves blue at
    // 26/255 of red where incandescent leaves 0 — read as STOCK. That is the
    // expensive direction: a caller that believes our output is stock derives
    // from it, and the curve compounds silently, because blue is already clipped
    // and the second pass looks like a no-op.
    for (int t = 0; t < lit::kTextureCount; ++t) {
        const auto texture = static_cast<lit::Texture>(t);
        const lit::Image stock = FlatAmber(512);
        CHECK(!lit::LooksRecolored(stock, texture));

        for (const lit::Profile p : {lit::Profile::kIncandescent,
                                     lit::Profile::kLed}) {
            lit::Image work = stock;
            lit::Apply(work, nullptr, lit::SpecForProfile(p, texture));
            CHECK(lit::LooksRecolored(work, texture));
        }
    }
}

TEST("lit: a buffer too small to judge reads as NOT recolored") {
    // ⚠ THE SAFE DIRECTION IS FALSE. The caller's response to false is "derive
    // from this file", and deriving from a stock file is right in every
    // ambiguous case; answering true would send it to a backup that may be a
    // ToLiss revision behind, or make it refuse outright.
    const lit::Image tiny = FlatAmber(4);
    CHECK(!lit::LooksRecolored(tiny, lit::Texture::kPlacards));
    CHECK(!lit::LooksRecolored(tiny, lit::Texture::kKnobs));
    CHECK(!lit::LooksRecolored(lit::Image{}, lit::Texture::kPlacards));
}
