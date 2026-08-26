// Integral-lighting recolor — deriving a Photon `text_LIT.png` from whatever
// stock one the aircraft actually ships.
//
// `text_LIT.png` is the emissive layer for the cockpit placard atlas (bound by
// `objects/lamps.obj` + `lamps_Std.obj`). ToLiss authors it as a washed-out
// amber, rgb(255,150,89); Gus's mod deepens it to rgb(255,122,0). Photon used
// to ship Gus's finished 7.7 MB file and copy it onto every airframe, which is
// how the A321 came to wear the A319/A320 trim-scale artwork (see
// docs/interior_plan.md §1.4b). This module computes the same look instead, from
// the file already in the aircraft folder, so per-airframe artwork and any
// future ToLiss redraw carry through by construction.
//
// ⚠ WHAT GUS DID IS A PER-CHANNEL TONE CURVE, AND THAT IS NOT A GUESS. Measured
// over his A320 file 2026-08-24: of the 840 distinct stock colors he touched,
// 838 map to exactly ONE output color each (the two exceptions are pure black,
// where he ADDED artwork, and a 2-pixel speck). Red is bit-exact identity across
// all 100 of its input values; green and blue are each a function of their OWN
// input channel alone, verified with zero ambiguity over 3.15 M pixels. That is
// the signature of a Photoshop Levels/Curves adjustment, which is why `Curve`
// below is exactly Levels — black point, white point, gamma — rather than a
// hue/saturation model. An HSL model cannot express it: HSL saturation reads all
// three channels at once, and these three demonstrably do not talk to each other.
//
// The one operation does both halves of the look. Blue clips to zero below ~93,
// which deepens the amber; and because the SAME curve meets the green and cyan
// callouts elsewhere on the atlas, they come out more saturated too — the
// "non-orange parts should be slightly more saturated" requirement is not a
// second rule, it is this one applied to a different input color.
//
// ⚠ THE EXCLUSIONS ARE REGIONAL AND CANNOT BE EXPRESSED AS A COLOR TEST. The
// annunciator legends in the upper right are the reason: they carry a white
// `OFF`, an amber `OFF` in a box, and a `PUSH` in red, so "recolor everything
// that is not already saturated" catches some and "recolor everything amber"
// catches others — and the boxed amber `OFF` is indistinguishable by color from
// the placard amber beside it that must move. Gus masked by area, and so do we.
//
// ⚠ WHAT THE REGIONS ARE IS A SEPARATE QUESTION FROM WHETHER THEY ARE REGIONS,
// and the answer changed on 2026-08-24. `kPreserveTable` no longer reproduces
// Gus's mask: its bands trace the legend block's own stepped edge rather than
// bounding-boxing his untouched pixels, which stops the upper-right rect from
// swallowing the trim ruling and the cabin-call placards; and the XPDR/TCAS
// panel is deliberately no longer preserved at all. See the table's own comment
// for the measurements.
//
// ⚠ ALPHA IS ROUGHNESS AND MUST SURVIVE UNTOUCHED. Per Gus's own readme (§4),
// `NORMAL_METALNESS` + `GLOBAL_specular 1` make the alpha channel a roughness
// map, not opacity. He changed alpha on exactly 2511 pixels — every one of them
// inside the artwork he ADDED — and left all 16.7 M others byte-identical.
// `Apply` therefore writes alpha only inside a promote region.
//
// ⚠ HIS FILE ALSO CARRIES THREE PURE-WHITE EDGE BANDS (top, bottom and right
// margins, 68 k pixels) that are export artifacts, not artwork — one of them
// lands on the trim wheel's side face. They are NOT reproduced here, and that is
// deliberate: deriving from stock means we simply never create them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace photon {
namespace lit {

// The atlas the placard rect table is authored against, and `Spec`'s default. A
// texture of a different size is handled by scaling rather than refused — ToLiss
// has never shipped one, but a silent half-atlas recolor would be far worse than
// an approximate rect.
//
// ⚠ IT IS NOT THE ONLY SIZE ANY MORE. `knobs_LIT.png` is 2048, so the size a
// spec's rects are authored against travels in the SPEC (`Spec::referenceSize`),
// not in this constant. Scaling a 2048-authored rect as though it were 4096
// would land every knob region on a quarter of the atlas.
constexpr int kReferenceSize = 4096;

// The cockpit textures this module knows how to derive.
//
// ⚠ THEY ARE NOT VARIATIONS ON ONE JOB. The placard atlas is a recolor with
// exclusions; the knobs atlas is mostly a pair of BLACKOUTS with one small
// recolor. That is why `Spec` carries `region` and `blackout` — see their
// comments — rather than the knobs work being bolted on beside it.
enum class Texture {
    kPlacards,  // text_LIT.png  + text.png,  4096
    kKnobs,     // knobs_LIT.png + knobs.png, 2048
};

constexpr int kTextureCount = 2;

const char* TextureName(Texture texture);     // "Placards" / "Knobs"
const char* TextureLitName(Texture texture);  // "text_LIT.png" / "knobs_LIT.png"
const char* TextureDayName(Texture texture);  // "text.png" / "knobs.png"
int TextureReferenceSize(Texture texture);    // 4096 / 2048

// The stable spelling used in files and on the command line — unlike
// `TextureName`, which is a label and may be reworded.
const char* TextureKey(Texture texture);  // "placards" / "knobs"
bool TextureFromKey(const std::string& key, Texture& out);

// One channel's Levels adjustment: input `black`..`white` is stretched onto
// 0..255 and then raised to `gamma`. `white` may legitimately exceed 255 — it
// names where the ramp WOULD reach full scale, so a value above 255 simply means
// the channel never gets there. Gus's blue does exactly that.
struct Curve {
    double black = 0.0;
    double white = 255.0;
    double gamma = 1.0;

    bool IsIdentity() const;
};

// --- the tuning layer --------------------------------------------------------
// ⚠ THIS DOES NOT REPLACE THE CURVE ABOVE, AND IT CANNOT. Everything the header
// says about Levels stands: the three channels in Gus's file provably do not
// talk to each other, and HSL saturation reads all three at once, so no stack of
// HSL ops reproduces his fit. The curve stays the measured baseline.
//
// What the stack is FOR is the next question, and it is a different one. Having
// landed on his look, moving it is asked in sentences like "a bit warmer", "less
// saturated", "brighter" — which are hue, saturation and lightness statements.
// Answering those by hand-solving three black/white/gamma triples means working
// out which of nine numbers to move and in which direction, and that is exactly
// the wall this layer exists to remove. So the pipeline is:
//
//     stock -> Levels (the measured Gus baseline) -> this stack -> out
//
// ⚠ AN EMPTY STACK IS BIT-EXACT THE OLD BEHAVIOR, and so is a stack whose ops
// all sit at zero. Identity ops are SKIPPED rather than computed, deliberately:
// an rgb -> hsl -> rgb round trip through doubles is not guaranteed to land back
// on the same byte, and "adding a control and not touching it changed 3 million
// pixels" is the kind of thing that would only be noticed in a diff against Gus
// six weeks later. Skipping is what lets every shipped profile keep an untouched
// fit while the stack sits there empty and available.
struct HslOp {
    enum class Kind {
        // Relative, and the ordinary one: rotate the hue, scale saturation and
        // lightness. Photoshop's Hue/Saturation sliders, in the same units.
        kAdjust,
        // Absolute: force one hue at one saturation, keeping each pixel's own
        // lightness. Photoshop's "Colorize" — the way to author a look from
        // scratch rather than nudge an existing one, which is what defining the
        // LED profile will want.
        kColorize,
    };

    Kind kind = Kind::kAdjust;
    // Off keeps an op in the stack without its effect, so a look can be A/B'd
    // without losing the numbers that made it.
    bool enabled = true;

    // kAdjust: -180..180 degrees of rotation. kColorize: the 0..360 target.
    double hue = 0.0;
    // kAdjust: -100..100, where -100 is exactly gray and +100 exactly full
    // saturation. kColorize: the 0..100 target.
    double saturation = 0.0;
    // ⚠ A GAIN ON LIGHTNESS, NOT PHOTOSHOP'S SLIDER, and the difference is the
    // whole texture. Photoshop drives Lightness TOWARD white, so +50 takes a
    // black pixel to mid gray — which on an emissive atlas that is ~90% black
    // background would light the entire cockpit up like a lamp. Here it
    // multiplies instead: -100..100 maps to 0x..2x, so black stays black at
    // every setting and there is no discontinuity just above it either. On a LIT
    // texture that is also the more honest operator, since it is what "the lamp
    // is brighter" physically means.
    double lightness = 0.0;

    // ⚠ A colorize op is never identity — it states a hue — so this asks about
    // kAdjust only.
    bool IsIdentity() const;
};

const char* HslKindName(HslOp::Kind kind);

// Does the whole stack do nothing? Disabled and identity ops both count as
// nothing, which makes this the exact test `Apply` uses to skip the stage.
bool HslStackIsIdentity(const std::vector<HslOp>& ops);

// Run a stack over one color, in place. Exposed because a stated color has to be
// able to show what it becomes — the tool previews the promote ink through it —
// and because the tests pin the formula here rather than through an image.
void ApplyHslStack(const std::vector<HslOp>& ops, std::uint8_t rgb[3]);

// A half-open rectangle in reference-atlas pixels: x0 <= x < x1.
struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    bool Empty() const { return x1 <= x0 || y1 <= y0; }
};

// Artwork lifted out of the ALBEDO (`text.png`) and lit up. Gus hand-added a
// backlit "PEDAL DISC" that the stock emissive layer does not carry; the day
// texture has always had it, so rather than vendoring his glyphs as a sprite we
// read the shape from the albedo and paint it. The aircraft's own artwork then
// stays the source of truth here too.
//
// ⚠ THE GLYPH SHAPE IS IN THE ALBEDO'S ALPHA, NOT ITS COLOR. In this rect the
// albedo's RGB is pure white on every one of the 2511 glyph pixels — flat, no
// antialiasing at all — while its alpha runs 19..226 and carries the whole
// letterform. Reading luminance here yields a hard-edged blob; Gus read alpha,
// and his output alpha still equals the albedo's exactly on 68% of pixels
// (correlation 0.95 over the rest).
struct PromoteRegion {
    Rect rect;
    // Albedo alpha mapped to coverage 0..1. The defaults are this region's own
    // measured background and ink levels.
    int alphaLo = 19;
    int alphaHi = 226;
    // The lit ink, at full coverage. Gus's measured (250,73,32) — redder than
    // the recolored amber around it, so it is stated rather than derived.
    std::uint8_t ink[3] = {250, 73, 32};
};

// One integral-lighting look.
struct Palette {
    Curve r, g, b;

    // ⚠ RUNS AFTER THE THREE CURVES, and is empty on every look this module
    // ships. See HslOp: the curves are a measurement, this is where a deliberate
    // departure from it goes.
    std::vector<HslOp> hsl;

    // The ink used by every promote region, so a variant recolors its added
    // artwork along with everything else. ⚠ THE STACK MOVES THIS TOO. It is
    // painted artwork sitting among recolored artwork, so an ink that held its
    // own hue through a hue shift would be the one thing on the pedestal that
    // did not change era — and it sits inches from placards that did. The tool
    // shows the stated color and what it becomes, side by side, so the swatch
    // never quietly disagrees with the pixels.
    std::uint8_t ink[3] = {250, 73, 32};
};

// A patch of atlas the palette applies to.
struct Region {
    Rect rect;

    // ⚠ AN OPTIONAL PALETTE OF ITS OWN, and the knobs atlas is why. Ten of its
    // eleven regions are index markings — the little white pointer stripes on
    // knobs and levers — which take the ordinary era palette, the same one the
    // placards get. The eleventh is a knob whose stock backlight is icy
    // blue-white rather than pale amber, so the same curve leaves it blue: it
    // needs a harder blue crush to land where its neighbours land. That is one
    // oddly-authored lamp, not a second look, so it rides as an exception on the
    // region rather than splitting the spec in two.
    //
    // ⚠ IT OVERRIDES THE CURVE, NOT THE TUNING STACK. `palette.hsl` empty means
    // the region runs the SPEC's stack — because the override says "this lamp is
    // authored differently", not "this lamp is a different era", and a tuning
    // move that skipped it would leave it the one thing on the panel holding
    // still. Same reasoning as the studio's profile copy, which reaches
    // overrides for the same reason. A region that genuinely wants its own
    // tuning states one and then keeps it.
    bool ownPalette = false;
    Palette palette;
};

// A complete recipe: the look plus the geometry it must respect.
struct Spec {
    Palette palette;

    // The atlas `region`, `preserve`, `blackout` and `promote` are authored
    // against. Rects scale from here to whatever size the buffer actually is.
    int referenceSize = kReferenceSize;

    // ⚠ WHERE THE PALETTE APPLIES. **Empty means the whole atlas**, which is the
    // placard case — recolor everything, minus `preserve`. The knobs atlas is
    // the other shape: a handful of small markings are recolored and the other
    // 4.19 M pixels must not be touched at all. Expressing that with `preserve`
    // alone would need rects covering the atlas minus eleven holes, so the
    // palette gets a positive region list and `preserve` keeps its job of
    // carving exceptions out of it.
    std::vector<Region> region;

    // Carved out of the above. Lamp legends, on the placards.
    std::vector<Rect> preserve;

    // ⚠ FORCED TO BLACK, RGB ONLY. Stock `knobs_LIT.png` glows on two things
    // that should not glow at all — a wide panel wash and the cockpit
    // loudspeaker — and Gus's fix is not a color adjustment, it is deletion. No
    // curve expresses "off": one that reached black everywhere here would reach
    // black everywhere else too.
    //
    // ⚠ ALPHA IS NOT CLEARED. It is roughness (see below), and Gus changed it on
    // exactly zero pixels of this texture — including inside these rects.
    std::vector<Rect> blackout;

    std::vector<PromoteRegion> promote;
};

// 8-bit RGBA, tightly packed, row-major from the top.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // width * height * 4

    bool Valid() const {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<std::size_t>(width) * height * 4;
    }
};

// The 256-entry table `Apply` actually runs on. Exposed because the preview tool
// draws it and the tests compare against it directly.
void BuildLut(const Curve& curve, std::uint8_t out[256]);

// Where a buffer sits inside the atlas it belongs to. The default — all zeros —
// means "this buffer IS the whole atlas", which is the ordinary case.
//
// It exists because a crop cannot place the rects on its own: a 384-pixel window
// onto the pedestal is not a 384-pixel atlas, and scaling the preserve table to
// its size would put the annunciator block across the middle of it. The tool's
// detail inspector recolors exactly such a crop at full resolution, and the
// installer can use the same door to work in tiles rather than holding a 67 MB
// buffer.
struct Viewport {
    int originX = 0, originY = 0;      // atlas coords of the buffer's pixel 0,0
    int fullWidth = 0, fullHeight = 0;  // 0 = take the buffer's own size
};

// Recolor `lit` in place. `albedo` may be null, in which case promote regions
// are skipped (and only those — a missing day texture must never cost the
// recolor). Returns the number of pixels whose RGB changed.
//
// Safe to call on any size; rects scale from kReferenceSize to the viewport's
// full size. Runs multithreaded over row bands, which is why it takes no
// callbacks. When `albedo` is given it must match `lit`'s dimensions, so a crop
// takes a matching crop of the day texture.
std::size_t Apply(Image& lit, const Image* albedo, const Spec& spec,
                  const Viewport& view = Viewport{});

// Does this buffer already carry a Photon recolor?
//
// ⚠ DECIDED FROM PIXELS, NEVER FROM THE INSTALL MARKER. An aircraft can carry a
// Photon manifest with stock textures restored under it (a ToLiss update), and
// the marker the other way round would feed a recolored file back through the
// curve — which compounds SILENTLY, because blue is already clipped to zero, so
// the second pass looks like a no-op while freezing the result in as if it were
// stock. A PNG has nowhere to put a `# ToLissPhoton orig` marker, so the artwork
// is the only honest witness.
//
// ⚠ AND IT IS A RATIO, NOT A LEVEL. It used to test the placard amber for
// `r >= 200 && blue <= 20` — true of the shipped palette and of nothing else —
// so a tuning session that pulled red down to 180 made our OWN output read as
// stock. Stock amber is rgb(255,150,89), i.e. blue at 35% of red, and every
// recolor pulls blue far below that whatever it does to red. On the knobs the
// test is structural instead and needs no palette knowledge at all: are the
// blackout rects already dark?
//
// ⚠ THE RATIO HAS TO CLEAR BOTH ERAS, and the threshold moved on 2026-08-24 when
// LED became a shipped profile: LED's amber leaves blue at 26/255 of red where
// incandescent leaves 0, so a bound tuned to incandescent alone read our own LED
// output as stock. See `kPlacardRecoloredRatio` for the three measured points.
//
// ⚠ IT NEVER ANSWERS "YES" ON A FILE IT CANNOT JUDGE. Too little amber to
// sample, or too small a blackout area, reads FALSE — "not recolored" — because
// the caller's response to false is to derive from this file, and deriving from
// a stock file is the correct outcome in every ambiguous case.
bool LooksRecolored(const Image& img, Texture texture);

// --- the user-facing profiles ------------------------------------------------
// The two looks an install offers. ⚠ ONE PROFILE SERVES ALL THREE A3xx
// AIRFRAMES, and that is a measured property rather than an assumption: the
// preserve rects and the promote region contain byte-identical artwork in the
// A319/A320 and A321 stock textures (checked 2026-08-24, and re-checked by
// `photon-lit-studio harvest` every time it runs). The airframes differ only in
// the trim-scale ruling and the flap placard, neither of which any rect touches
// — so the profile is a palette, and the artwork comes from the aircraft.
// ⚠ `kIncandescent` WAS `kHalogen` UNTIL 2026-08-24, and the rename is not
// cosmetic. This profile is now offered to the user as one of TWO choices on the
// Integral Lights row, where it has to cover both of the cockpit's halogen eras
// — the interior categories beside it still say "Old Halogen" and "New Halogen",
// and a row reading "Halogen / LED" next to them would read as a third,
// contradictory answer to the same question. "Incandescent" is the honest
// superset: a halogen lamp IS an incandescent one, so the name says "this is the
// filament era" without claiming which. The *fit* is unchanged — `GusIncandescent`
// is still a measurement of Gus's file, only its name follows the profile.
enum class Profile {
    kIncandescent,  // Gus's deep amber
    kLed,      // paler and cooler; authored, not fitted. See Led().
};

// ⚠ The knobs atlas is byte-identical on all three A3xx too (A319/A320/A321 all
// hash the same stock `knobs_LIT.png`, checked 2026-08-24), so the same
// statement holds for both textures and `harvest` re-checks both.

const char* ProfileName(Profile profile);

// ⚠ TRUE FOR BOTH SINCE 2026-08-24, and it became true in the SAME change that
// gave kLed a real spec — that pairing is the whole point of the predicate. It
// was false for as long as LED resolved to incandescent, because a defined-but-
// identical profile tells the user a choice took effect when nothing moved,
// and every surface offering the choice was expected to say it was substituting.
// Do not flip it back without also removing the spec, or vice versa.
bool ProfileIsDefined(Profile profile);

// The spec an install would apply for `profile` on `texture`, fallback included.
//
// ⚠ AN INSTALL MUST DO EVERY TEXTURE. They are separate files with separate
// geometry, so there is no single call that covers the cockpit; iterate
// `kTextureCount`. Doing one and not the other leaves a cockpit half converted,
// which reads in-sim as the mod being broken rather than half-applied.
Spec SpecForProfile(Profile profile, Texture texture);

// --- the raw specs -----------------------------------------------------------

// Gus's own placard look, fitted to his file. Reproduces it within 2 levels on
// 95.7% of comparable pixels and within 8 on 99.6%.
Spec GusIncandescent();

// Gus's knobs look: the two blackouts, plus a curve over the one knob whose
// stock backlight is icy blue.
//
// ⚠ THE BLACKOUTS ARE EXACT AND THE CURVE IS NOT. Inside both blackout rects
// stock has 58,482 and 24,279 lit pixels and Gus has zero, so a rect reproduces
// them bit for bit. The knob curve is an approximation: unlike the placards —
// where 838 of 840 colors map 1:1 and Levels fits with no ambiguity — the knob's
// 745 pixels do form a per-channel LUT (red again bit-exact identity) but not a
// clean Levels one, fitting to within 11 levels on green and 16 on blue. It is a
// specular highlight 30 px across, so that is invisible; it is recorded here so
// nobody later reads it as measured to the placards' standard.
Spec GusKnobs();

// Identity — no curve, no promotion, everything preserved. The A/B baseline in
// the tool, and what an aircraft looks like with Photon's texture work off.
Spec StockPassthrough(Texture texture);

// The LED look, on either texture. Paler and cooler than incandescent.
//
// ⚠ AUTHORED, NOT FITTED, AND THAT IS THE DIFFERENCE FROM EVERYTHING ABOVE.
// `GusIncandescent` and `GusKnobs` carry his name because they are *measurements* of
// a file that exists — 838 of 840 colors mapping 1:1, red bit-exact. There is no
// LED file to measure: this look was tuned in the bench against the cockpit and
// signed off by eye on 2026-08-24. So it is a judgement, and the honest place to
// change it is a tuning session, not a refit.
//
// ⚠ IT SHARES THE MEASURED CURVE AND DIVERGES ONLY IN THE STACK. Both textures
// keep Gus's fitted Levels and add HSL ops on top (see `Palette::hsl`) — which
// is what makes "LED" a statement about the LAMP rather than a second, unrelated
// recolor, and what keeps a future refit of the curve flowing into both eras.
Spec Led(Texture texture);

// The geometry tables for a texture, shared by every look. Named so a future
// edit can say which rect it is changing.
const std::vector<Rect>& PreserveRects(Texture texture);
const char* PreserveRectName(Texture texture, std::size_t index);

const std::vector<Rect>& BlackoutRects(Texture texture);
const char* BlackoutRectName(Texture texture, std::size_t index);

// Where the palette applies. Empty for the placards — meaning everywhere.
const std::vector<Rect>& PaletteRegions(Texture texture);
const char* PaletteRegionName(Texture texture, std::size_t index);

// The promote regions, likewise.
const std::vector<PromoteRegion>& PromoteRegions(Texture texture);

// --- persistence -------------------------------------------------------------
// Round-trips a Spec through JSON so the tool can save a tuning session and the
// installer can carry a chosen look. Rects and promote regions are written too:
// a preset that recolored a different set of areas than it was tuned with would
// be a preset for a different texture.
std::string ToJson(const Spec& spec);
bool FromJson(const std::string& text, Spec& out, std::string& error);

}  // namespace lit
}  // namespace photon
