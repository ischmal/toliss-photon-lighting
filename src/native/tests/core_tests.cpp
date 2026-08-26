// photoncore_tests — the pure-logic layer of the installer port.
//
// What belongs here: anything decidable from a string or a temp directory. The
// `.acf` engine (⚠ the XP11/XP12 format trap gets exhaustive cases — it is the one
// bug in this codebase that fails SILENTLY on half the files it touches), marker
// inject/parse, the glow token swap, the attachment table's renumbering, and the
// fsutil primitives everything else stands on.
//
// What does NOT belong here: anything that needs a real release payload or a real
// X-Plane tree. That is the Python integration layer, which drives the compiled
// binary through its CLI (docs/installer_cpp_plan.md §6.2).
#include "test_harness.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/acf.h"
#include "core/backup.h"
#include "core/constants.h"
#include "core/fsutil.h"
#include "core/manifest.h"
#include "core/marker.h"
#include "core/neomod.h"
#include "core/patch_acf.h"
#include "core/patch_acf_screens.h"
#include "core/patch_glow.h"
#include "core/patch_integral.h"
#include "core/patch_intensity.h"
#include "core/patch_realwings.h"
#include "core/support.h"
#include "core/version.h"
#include "installer/tui.h"

namespace fs = std::filesystem;
using namespace photon;

namespace {

// A scratch directory that removes itself. Tests that touch the filesystem use it
// rather than a fixed path, so a failed run never poisons the next one.
class TempDir {
public:
    TempDir() {
        const auto base = fs::temp_directory_path();
        for (int i = 0; i < 1000; ++i) {
            fs::path candidate =
                base / ("photoncore_test_" + std::to_string(::rand()) + "_" +
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

void Write(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    CHECK(fsutil::AtomicWriteBytes(p, data));
}

std::string Read(const fs::path& p) {
    std::string out;
    CHECK(fsutil::ReadFileBytes(p, out));
    return out;
}

bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && !ec;
}

// ── .acf fixtures ────────────────────────────────────────────────────────────
// The two float styles, same values, as the two file generations really write
// them. Every `.acf` test runs against BOTH: the format trap's whole nature is
// that a patch works on one and silently no-ops on the other.
// ⚠ THE SECOND LINE IS A REAL STAMP, NOT DECORATION. `1200 Version` / `1100
// Version` is how X-Plane marks an `.acf`'s format, and it is what `acf::IsForXp12`
// reads to decide which variants Photon writes to. These two fixtures said
// `1 version` until 2026-08-15, which is neither.
const char* kAcfXp12 =
    "I\r\n"
    "1200 Version\r\n"
    "ACF\r\n"
    "P acf/_spot1_3d_xyz/1 1.000000000\r\n"
    "P acf/_spot_angle/0 0.000000000\r\n"
    "P acf/_spot_size/0 20.000000000\r\n"
    "P acf/_spot_size/1 28.010000229\r\n"
    "P acf/_spot_size/2 38.009998322\r\n"
    "P acf/_spot_the/0 -70.000000000\r\n"
    "P acf/_spot_psi/0 -20.000000000\r\n"
    "P acf/_spot_name_3d/0 sim/cockpit2/electrical/panel_brightness_ratio[0]\r\n"
    "P acf/_spot_name_3d/1 sim/cockpit2/electrical/panel_brightness_ratio[3]\r\n"
    "P acf/_spot_name_3d/2 ckpt/lights/map\r\n"
    "P acf/_unrelated/0 3.140000000\r\n";

const char* kAcfXp11 =
    "I\r\n"
    "1100 Version\r\n"
    "ACF\r\n"
    "P acf/_spot1_3d_xyz/1 1.0\r\n"
    "P acf/_spot_angle/0 0.0\r\n"
    "P acf/_spot_size/0 20.0\r\n"
    "P acf/_spot_size/1 28.010000229\r\n"
    "P acf/_spot_size/2 38.009998322\r\n"
    "P acf/_spot_the/0 -70.0\r\n"
    "P acf/_spot_psi/0 -20.0\r\n"
    "P acf/_spot_name_3d/0 sim/cockpit2/electrical/panel_brightness_ratio[0]\r\n"
    "P acf/_spot_name_3d/1 sim/cockpit2/electrical/panel_brightness_ratio[3]\r\n"
    "P acf/_spot_name_3d/2 ckpt/lights/map\r\n"
    "P acf/_unrelated/0 3.14\r\n";

std::string AttachmentAcf(bool xp12) {
    const std::string y = xp12 ? "20.750000000" : "20.75";
    return std::string("I\r\n") + (xp12 ? "1200" : "1100") + " Version\r\n"
        "ACF\r\n"
        "P _obja/0/_v10_att_file_stl fuselage.obj\r\n"
        "P _obja/0/_v10_att_y_acf_prt_ref " + y + "\r\n"
        "P _obja/1/_v10_att_file_stl lights_inn.obj\r\n"
        "P _obja/1/_v10_att_y_acf_prt_ref " + y + "\r\n"
        "P _obja/count 2\r\n";
}

// The A330-900 shape: no `lights_inn.obj` anywhere (that file is the A3xx's), and
// its cockpit-lighting row carries an `_obj_hide_dataref` the A3xx template does
// not — both faithful to the real `A330-900.acf`.
std::string AttachmentAcfA339() {
    return std::string(
        "I\r\n"
        "1200 Version\r\n"
        "ACF\r\n"
        "P _obja/0/_obj_hide_dataref AirbusFBW/ObjectKill\r\n"
        "P _obja/0/_v10_att_file_stl Fuselage_Fwd.obj\r\n"
        "P _obja/0/_v10_att_y_acf_prt_ref -3.000000000\r\n"
        "P _obja/1/_obj_hide_dataref AirbusFBW/ObjectKill\r\n"
        "P _obja/1/_v10_att_file_stl CockpitLighting_XP12.obj\r\n"
        "P _obja/1/_v10_att_y_acf_prt_ref -3.000000000\r\n"
        "P _obja/1/_v10_att_z_acf_prt_ref -3.359999895\r\n"
        "P _obja/count 2\r\n");
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// fsutil
// ─────────────────────────────────────────────────────────────────────────────
TEST("fsutil: atomic write round-trips bytes verbatim") {
    TempDir tmp;
    // Includes a NUL and a CR, because one stock A321 `.acf` really does carry a
    // stray NUL and every one of them is CRLF. A round trip that "cleans" either
    // is a whole-file diff in a file other mods also back up.
    const std::string blob("a\r\nb\0c\r\n", 8);
    Write(tmp / "x.bin", blob);
    CHECK_EQ(Read(tmp / "x.bin"), blob);
}

TEST("fsutil: atomic write creates missing parent directories") {
    TempDir tmp;
    Write(tmp.path() / "deep" / "deeper" / "f.txt", "hi");
    CHECK_EQ(Read(tmp.path() / "deep" / "deeper" / "f.txt"), std::string("hi"));
}

TEST("fsutil: FilesIdentical is byte-exact and safe on missing files") {
    TempDir tmp;
    Write(tmp / "a", "same");
    Write(tmp / "b", "same");
    Write(tmp / "c", "diff");
    Write(tmp / "d", "sam");            // shorter: the size fast path
    CHECK(fsutil::FilesIdentical(tmp / "a", tmp / "b"));
    CHECK(!fsutil::FilesIdentical(tmp / "a", tmp / "c"));
    CHECK(!fsutil::FilesIdentical(tmp / "a", tmp / "d"));
    // ⚠ Missing counts as NOT identical — callers treat that as needs-(re)write,
    // which is the safe direction. The opposite would skip writing a file we could
    // not prove was already correct.
    CHECK(!fsutil::FilesIdentical(tmp / "a", tmp / "nope"));
    CHECK(!fsutil::FilesIdentical(tmp / "nope", tmp / "alsonope"));
}

TEST("fsutil: newline detection and round trip preserve the file's own flavor") {
    CHECK_EQ(fsutil::DetectNewline("a\r\nb"), std::string("\r\n"));
    CHECK_EQ(fsutil::DetectNewline("a\nb"), std::string("\n"));
    const std::string crlf = "one\r\ntwo\r\nthree";
    CHECK_EQ(fsutil::JoinLines(fsutil::SplitLines(crlf), "\r\n"), crlf);
    const std::string lf = "one\ntwo\nthree";
    CHECK_EQ(fsutil::JoinLines(fsutil::SplitLines(lf), "\n"), lf);
}

TEST("fsutil: SplitLinesKeepEnds reassembles byte-for-byte") {
    for (const std::string& text : {std::string("a\r\nb\r\n"),
                                    std::string("a\nb"),
                                    std::string("a\r\nb\nc\r\n"),
                                    std::string(""),
                                    std::string("\n")}) {
        std::string back;
        for (const std::string& l : fsutil::SplitLinesKeepEnds(text)) back += l;
        CHECK_EQ(back, text);
    }
}

TEST("fsutil: GlobMatch handles the airframe folder patterns") {
    CHECK(fsutil::GlobMatch("ToLissA319*", "ToLissA319_V1p2p3"));
    CHECK(fsutil::GlobMatch("ToLissA319*", "tolissa319"));       // case-insensitive
    CHECK(!fsutil::GlobMatch("ToLissA319*", "ToLissA320_V1"));
    CHECK(fsutil::GlobMatch("*", "anything"));
    CHECK(fsutil::GlobMatch("a?c", "abc"));
    CHECK(!fsutil::GlobMatch("a?c", "ac"));
}

TEST("fsutil: UTF-8 paths survive a round trip") {
    // ⚠ An aircraft folder named by a user in any language has to survive the
    // scan, the manifest and the TUI table. On Windows that means the wide API,
    // never the ANSI code page.
    TempDir tmp;
    const std::string name = u8"ToLissÅ320 日本語";
    const fs::path dir = tmp.path() / fsutil::PathFromUtf8(name);
    std::error_code ec;
    fs::create_directory(dir, ec);
    CHECK(!ec);
    Write(dir / "f.txt", "ok");
    CHECK_EQ(Read(dir / "f.txt"), std::string("ok"));
    CHECK_EQ(fsutil::PathToUtf8(fsutil::PathFromUtf8(name)), name);
}

TEST("fsutil: atomic write handles a non-ASCII FILENAME, not just directory") {
    // ⚠ Regression: the tmp-sibling name was built via `filename().string()`,
    // which narrows through the ANSI code page on Windows — an unrepresentable
    // character makes the conversion throw (or yields an unwritable name), so the
    // write failed on exactly the files a flattened livery backup produces (the
    // livery folder's user-chosen name is part of the backup filename). The dir
    // case above never caught it because only the FILENAME was narrowed.
    TempDir tmp;
    const fs::path p =
        tmp.path() / fsutil::PathFromUtf8(u8"liveries__Тест 機体__chairs_LIT.dds");
    Write(p, "bytes");
    CHECK_EQ(Read(p), std::string("bytes"));
    // Overwrite through the same tmp-sibling path — the rename branch.
    Write(p, "bytes2");
    CHECK_EQ(Read(p), std::string("bytes2"));
}

// ─────────────────────────────────────────────────────────────────────────────
// marker
// ─────────────────────────────────────────────────────────────────────────────
TEST("marker: injected after POINT_COUNTS, keeping the file's newlines") {
    const std::string obj = "I\r\n800\r\nOBJ\r\nPOINT_COUNTS\t0\t0\t0\t0\r\nVT 1\r\n";
    const std::string out = marker::Inject(obj, "0.8", "stock");
    CHECK(out.find("POINT_COUNTS\t0\t0\t0\t0\r\n# ToLissPhoton version: 0.8 wing: stock\r\nVT 1") !=
          std::string::npos);
    CHECK(out.find('\n') != std::string::npos);
    CHECK(out.find("\n\n") == std::string::npos);
    // LF stays LF
    const std::string lf = "POINT_COUNTS 0\nVT 1\n";
    const std::string outLf = marker::Inject(lf, "0.8", "interior");
    CHECK(outLf.find("\r\n") == std::string::npos);
}

TEST("marker: with no POINT_COUNTS it goes to the top rather than nowhere") {
    const std::string out = marker::Inject("VT 1\nVT 2\n", "0.8", "screens");
    CHECK(fsutil::StartsWith(out, "# ToLissPhoton version: 0.8 wing: screens\n"));
}

TEST("marker: parse round-trips inject") {
    const std::string out = marker::Inject("POINT_COUNTS 0\n", "1.2.3", "realwings");
    const marker::Marker m = marker::Parse(out);
    CHECK(m.found);
    CHECK_EQ(m.version, std::string("1.2.3"));
    CHECK_EQ(m.wing, std::string("realwings"));
}

TEST("marker: a stock OBJ reports no marker") {
    // ⚠ This is the STALE-INSTALL signal. Manifest intact + marker gone means an
    // aircraft update reverted the OBJ out from under us, so a surviving manifest
    // alone is not proof the edit is still live.
    CHECK(!marker::Parse("I\n800\nOBJ\nPOINT_COUNTS 0\nVT 1\n").found);
}

// ─────────────────────────────────────────────────────────────────────────────
// the .acf engine — the format trap
// ─────────────────────────────────────────────────────────────────────────────
TEST("acf: style detection votes by majority, not by one probe line") {
    CHECK(acf::DetectStyle(kAcfXp12) == acf::Style::Xp12);
    CHECK(acf::DetectStyle(kAcfXp11) == acf::Style::Xp11);
    // ⚠ 28.010000229 and 38.009998322 need nine decimals in BOTH styles, so they
    // read as XP12 on their own. They are present in the XP11 fixture above and it
    // still votes XP11 — which is the whole reason detection is a majority vote
    // over every numeric acf/ property rather than a look at one line.
}

TEST("acf: Render writes each style the way its files really do") {
    CHECK_EQ(acf::Render(-70.0, acf::Style::Xp12), std::string("-70.000000000"));
    CHECK_EQ(acf::Render(-70.0, acf::Style::Xp11), std::string("-70.0"));
    CHECK_EQ(acf::Render(0.0, acf::Style::Xp12), std::string("0.000000000"));
    // ⚠ XP11 ALWAYS keeps the decimal point: 1.0, never 1. Dropping it is exactly
    // the kind of "equivalent" rewrite that turns a patch into a whole-file diff.
    CHECK_EQ(acf::Render(1.0, acf::Style::Xp11), std::string("1.0"));
    CHECK_EQ(acf::Render(50.0, acf::Style::Xp11), std::string("50.0"));
    CHECK_EQ(acf::Render(28.010000229, acf::Style::Xp11),
             std::string("28.010000229"));
}

TEST("acf: the XP11 renderer matches Python's repr exactly") {
    // ⚠ XP11 `.acf` files were written by X-Plane, but the PYTHON installer is what
    // has been rewriting them correctly for two releases, and its renderer is
    // `repr(float)`. So the C++ port's obligation is not "produce a valid float" —
    // it is "produce the same bytes the Python installer would", or the two
    // installers disagree on a file and the parity harness (§6.3) is the only thing
    // that would ever notice.
    //
    // Generated from CPython, including the cases that broke the first attempt:
    // `%g` renders -70.0 as "-7e+01" (shortest, round-trips, and completely wrong
    // for this file format), and Python only goes exponential when the decimal
    // point falls outside (-4, 16].
    struct Row { double value; const char* want; };
    static const Row kRows[] = {
        {1.0, "1.0"},                     {0.0, "0.0"},
        {-70.0, "-70.0"},                 {-60.0, "-60.0"},
        {50.0, "50.0"},                   {20.0, "20.0"},
        {28.010000229, "28.010000229"},   {38.009998322, "38.009998322"},
        {26.0, "26.0"},                   {20.75, "20.75"},
        {6.78, "6.78"},                   {-20.0, "-20.0"},
        {4.0, "4.0"},                     {0.5, "0.5"},
        {0.1, "0.1"},                     {0.001, "0.001"},
        {0.0001, "0.0001"},               {1e-05, "1e-05"},
        {123456789.0, "123456789.0"},     {1e16, "1e+16"},
        {1000000000000000.0, "1000000000000000.0"},
        {-4.48, "-4.48"},                 {9.49, "9.49"},
        {14.75, "14.75"},                 {3.14159265358979, "3.14159265358979"},
        {1234567890123456.0, "1234567890123456.0"},
        {0.30000000000000004, "0.30000000000000004"},
    };
    for (const Row& r : kRows) {
        CHECK_EQ(acf::Render(r.value, acf::Style::Xp11), std::string(r.want));
    }
    // Every one of them must also read back as the same double — a renderer that
    // matched the bytes but lost precision would be worse than one that did not.
    for (const Row& r : kRows) {
        CHECK(acf::NumericEquals(acf::Render(r.value, acf::Style::Xp11), r.value));
    }
}

TEST("acf: SetProps preserves spacing, terminators and untouched lines") {
    const std::string text =
        "P acf/_a    1.0\r\nrandom line\r\nP acf/_b 2.0\n";   // mixed endings
    int changed = 0;
    const std::string out =
        acf::SetProps(text, {{"acf/_a", "9.0"}}, changed);
    CHECK_EQ(changed, 1);
    CHECK(out.find("P acf/_a    9.0\r\n") != std::string::npos);
    CHECK(out.find("random line\r\n") != std::string::npos);
    CHECK(out.find("P acf/_b 2.0\n") != std::string::npos);
}

TEST("acf: SetProps refuses rather than silently skipping a missing property") {
    // ⚠ A silent skip IS the format trap's failure mode, so the engine must not be
    // able to report success over a file it did not fully patch.
    int changed = 0;
    CHECK_THROWS(acf::SetProps("P acf/_a 1.0\r\n", {{"acf/_nope", "1"}}, changed),
                 acf::AcfError);
}

TEST("acf: NumericEquals compares numerically, never as a string") {
    CHECK(acf::NumericEquals("-70.000000000", -70.0));
    CHECK(acf::NumericEquals("-70.0", -70.0));
    CHECK(acf::NumericEquals("-70", -70.0));
    CHECK(!acf::NumericEquals("-60.0", -70.0));
    CHECK(!acf::NumericEquals("none", -70.0));
    CHECK(!acf::NumericEquals("", -70.0));
}

// ─────────────────────────────────────────────────────────────────────────────
// patch_acf — cockpit spots
// ─────────────────────────────────────────────────────────────────────────────
TEST("patch_acf: patches BOTH float styles and round-trips each") {
    // ⚠ THE REGRESSION THIS EXISTS FOR: a literal string patch works on the two
    // XP12 files and silently no-ops on the two XP11 ones.
    //
    // ⚠ STILL BOTH STYLES, EVEN THOUGH AN INSTALL NOW ONLY WRITES XP12 FILES
    // (2026-08-15). The text engine is what a REVERSAL runs on, and a reversal
    // still reaches the XP11 pair to undo what Photon ≤ 0.8.4 wrote there — so a
    // renderer that lost the narrow style would leave those files half-reverted.
    // Which files each direction visits is `acf::Variants`, tested separately.
    for (const char* fixture : {kAcfXp12, kAcfXp11}) {
        const std::string original(fixture);
        CHECK(!patch_acf::IsPatched(original));

        int changed = 0;
        const std::string patched = patch_acf::PatchText(original, changed);
        CHECK(changed > 0);
        CHECK(patch_acf::IsPatched(patched));
        CHECK(patch_acf::VerifyGeometry(patched).empty());

        // rendered in THIS file's own style
        const bool xp12 = (acf::DetectStyle(original) == acf::Style::Xp12);
        CHECK(patched.find(xp12 ? "P acf/_spot_the/0 -60.000000000"
                                : "P acf/_spot_the/0 -60.0") != std::string::npos);

        // untouched properties survive verbatim
        CHECK(patched.find(xp12 ? "P acf/_unrelated/0 3.140000000"
                                : "P acf/_unrelated/0 3.14") != std::string::npos);

        // ⚠ Reversal writes STOCK VALUES BACK rather than restoring a snapshot, so
        // it composes with whatever else edited the file since — and here that
        // means it lands byte-for-byte back on the original.
        int rchanged = 0;
        const std::string reverted = patch_acf::UnpatchText(patched, rchanged);
        CHECK(rchanged > 0);
        CHECK(!patch_acf::IsPatched(reverted));
        CHECK_EQ(reverted, original);
    }
}

TEST("patch_acf: patching is idempotent") {
    int c1 = 0, c2 = 0;
    const std::string once = patch_acf::PatchText(kAcfXp12, c1);
    const std::string twice = patch_acf::PatchText(once, c2);
    CHECK_EQ(twice, once);
    CHECK_EQ(c2, 0);
}

TEST("patch_acf: the sentinel is the disabled spot NAMES, not the geometry") {
    // ⚠ Presence-based, and specifically a STRING compare — immune to the float
    // format trap, unambiguous (stock is a dataref path), and the property that
    // actually means "our patch is live". Geometry is corroboration only.
    int changed = 0;
    std::string patched = patch_acf::PatchText(kAcfXp12, changed);
    // Something reverted one geometry value but not the names: still "patched",
    // and the geometry check is what says it is partial.
    patched = acf::SetProps(patched, {{"acf/_spot_the/0", "-70.000000000"}}, changed);
    CHECK(patch_acf::IsPatched(patched));
    const auto bad = patch_acf::VerifyGeometry(patched);
    CHECK_EQ(bad.size(), static_cast<std::size_t>(1));
    CHECK_EQ(bad[0], std::string("acf/_spot_the/0"));
}

TEST("patch_acf: Run finds .acf files by content and skips .bak siblings") {
    TempDir tmp;
    Write(tmp / "a320.acf", kAcfXp12);
    Write(tmp / "a320_XP11.acf", kAcfXp11);
    Write(tmp / "unrelated.acf", "P acf/_nothing 1.0\r\n");
    // ⚠ Durantula keeps an `a320.acf.durantula.bak` of this very file; patching it
    // would corrupt that mod's uninstall.
    Write(tmp / "a320.acf.durantula.bak", kAcfXp12);

    const auto files =
        patch_acf::AcfFiles(fsutil::PathToUtf8(tmp.path()), acf::Variants::Xp12);
    CHECK_EQ(files.size(), static_cast<std::size_t>(1));
    for (const std::string& f : files) CHECK(f.find(".bak") == std::string::npos);

    const auto res = patch_acf::Run(fsutil::PathToUtf8(tmp.path()), false, false);
    CHECK_EQ(res.touched.size(), static_cast<std::size_t>(1));
    CHECK(patch_acf::IsPatched(Read(tmp / "a320.acf")));
    // ⚠ The XP11 variant is NOT patched (2026-08-15). Nothing Photon builds is for
    // X-Plane 11, so writing to a file that sim alone loads is work with no reader.
    CHECK(!patch_acf::IsPatched(Read(tmp / "a320_XP11.acf")));
    CHECK_EQ(Read(tmp / "a320_XP11.acf"), std::string(kAcfXp11));
    CHECK_EQ(Read(tmp / "a320.acf.durantula.bak"), std::string(kAcfXp12));
}

TEST("patch_acf: a REVERSAL still reaches the XP11 pair an older Photon patched") {
    // ⚠ THE ASYMMETRY IS THE POINT, and it is the same rule as the backup folder:
    // write to the new place, clean up both. Photon <= 0.8.4 patched all four
    // variants, so an uninstall scoped to XP12 would leave a cockpit spot block
    // permanently disabled in a file nothing this installer will ever touch again.
    TempDir tmp;
    int changed = 0;
    Write(tmp / "a320.acf", patch_acf::PatchText(kAcfXp12, changed));
    Write(tmp / "a320_XP11.acf", patch_acf::PatchText(kAcfXp11, changed));

    CHECK_EQ(patch_acf::AcfFiles(fsutil::PathToUtf8(tmp.path()),
                                 acf::Variants::Every).size(),
             static_cast<std::size_t>(2));

    const auto res = patch_acf::Run(fsutil::PathToUtf8(tmp.path()), true, false);
    CHECK_EQ(res.touched.size(), static_cast<std::size_t>(2));
    // Byte-for-byte back to stock, in each file's OWN float style.
    CHECK_EQ(Read(tmp / "a320.acf"), std::string(kAcfXp12));
    CHECK_EQ(Read(tmp / "a320_XP11.acf"), std::string(kAcfXp11));
}

TEST("acf: the format stamp decides, not the filename") {
    // ⚠ CONTENT, NOT `_XP11` IN THE NAME. The four-variant naming is ToLiss's
    // convention; the stamp on line 2 is X-Plane's own.
    CHECK_EQ(acf::FormatVersion(kAcfXp12), 1200);
    CHECK_EQ(acf::FormatVersion(kAcfXp11), 1100);
    CHECK(acf::IsForXp12(kAcfXp12));
    CHECK(!acf::IsForXp12(kAcfXp11));

    // A later format is still XP12-or-newer, not "unrecognized".
    CHECK(acf::IsForXp12("I\r\n1300 Version\r\nACF\r\n"));

    // ⚠ NO STAMP READS AS XP12, because the failure to avoid is a SILENT SKIP —
    // an install that quietly does nothing to a file it was meant to patch. A
    // caller has already matched a content needle by the time this is asked.
    CHECK_EQ(acf::FormatVersion("I\r\nACF\r\nP acf/_a 1.0\r\n"), 0);
    CHECK(acf::IsForXp12("I\r\nACF\r\nP acf/_a 1.0\r\n"));

    // ⚠ THE HEADER ONLY. A whole-file search would find this and call the file
    // XP11 on the strength of a livery name.
    CHECK_EQ(acf::FormatVersion(
                 "I\r\n1200 Version\r\nACF\r\nP acf/_name 1100 Version\r\n"),
             1200);
}

TEST("patch_acf: a dry run writes nothing") {
    TempDir tmp;
    Write(tmp / "a320.acf", kAcfXp12);
    patch_acf::Run(fsutil::PathToUtf8(tmp.path()), false, true);
    CHECK_EQ(Read(tmp / "a320.acf"), std::string(kAcfXp12));
}

// ─────────────────────────────────────────────────────────────────────────────
// patch_acf_screens — the attachment table
// ─────────────────────────────────────────────────────────────────────────────
TEST("screens: attach copies the lights_inn frame and bumps the count") {
    for (bool xp12 : {true, false}) {
        const std::string original = AttachmentAcf(xp12);
        CHECK(!patch_acf_screens::IsAttached(original));

        int changed = 0;
        const std::string patched = patch_acf_screens::PatchText(original, changed);
        CHECK(changed > 0);
        CHECK(patch_acf_screens::IsAttached(patched));
        CHECK_EQ(patch_acf_screens::DeclaredCount(patched), 3);

        // ⚠ The frame is COPIED from lights_inn.obj's own row. Hardcoding a datum
        // would mis-place every screen light on two airframes out of three — and
        // copying verbatim as a string is also what sidesteps the float-format
        // trap here, so the value keeps the host file's own style.
        const auto rows = patch_acf_screens::AttachmentRows(patched);
        const int idx = patch_acf_screens::FindObj(patched, kScreensObj);
        CHECK_EQ(idx, 2);
        CHECK_EQ(rows.at(2).at("_v10_att_y_acf_prt_ref"),
                 rows.at(1).at("_v10_att_y_acf_prt_ref"));
        CHECK(patched.find(xp12 ? "20.750000000" : "20.75") != std::string::npos);

        int rchanged = 0;
        const std::string detached =
            patch_acf_screens::UnpatchText(patched, rchanged);
        CHECK(!patch_acf_screens::IsAttached(detached));
        CHECK_EQ(detached, original);
    }
}

TEST("screens: attach is idempotent") {
    int c1 = 0, c2 = 0;
    const std::string once = patch_acf_screens::PatchText(AttachmentAcf(true), c1);
    const std::string twice = patch_acf_screens::PatchText(once, c2);
    CHECK_EQ(c2, 0);
    CHECK_EQ(twice, once);
}

TEST("screens: refuses when there is no frame to copy") {
    // ⚠ Logged, non-fatal, install continues — the glow is simply absent, which is
    // its own honest degraded state. Guessing a datum is the one thing it must not
    // do.
    const std::string noTemplate =
        "P _obja/0/_v10_att_file_stl fuselage.obj\r\n"
        "P _obja/0/_v10_att_y_acf_prt_ref 1.0\r\n"
        "P _obja/count 1\r\n";
    int changed = 0;
    CHECK_THROWS(patch_acf_screens::PatchText(noTemplate, changed), acf::AcfError);
}

// ⚠ THE A330-900 HAS NO `lights_inn.obj`, so the single-name template that served
// the A3xx would make the patcher refuse this airframe outright — the glow would
// install as a file on disk that nothing ever draws, which looks exactly like a
// broken install and is the failure a `screens.added[]` entry cannot explain.
TEST("screens: the a339 frame comes from CockpitLighting_XP12.obj") {
    const std::string original = AttachmentAcfA339();
    CHECK_EQ(patch_acf_screens::FindFrameTemplate(original), 1);

    int changed = 0;
    const std::string patched = patch_acf_screens::PatchText(original, changed);
    CHECK(changed > 0);
    CHECK(patch_acf_screens::IsAttached(patched));
    CHECK_EQ(patch_acf_screens::DeclaredCount(patched), 3);

    const auto rows = patch_acf_screens::AttachmentRows(patched);
    const int idx = patch_acf_screens::FindObj(patched, kScreensObj);
    CHECK_EQ(idx, 2);
    // The a339's own datum, copied verbatim — NOT the A3xx's 20.75.
    CHECK_EQ(rows.at(2).at("_v10_att_z_acf_prt_ref"),
             std::string("-3.359999895"));
    CHECK_EQ(rows.at(2).at("_v10_att_y_acf_prt_ref"),
             rows.at(1).at("_v10_att_y_acf_prt_ref"));
    // ⚠ Every field is copied, INCLUDING `_obj_hide_dataref`, which the A3xx
    // template does not carry. That is the point of copying verbatim: our OBJ
    // gets exactly the visibility behavior the aircraft's own cockpit-lighting
    // OBJ has, rather than a field set invented from the other airframe.
    CHECK_EQ(rows.at(2).at("_obj_hide_dataref"),
             std::string("AirbusFBW/ObjectKill"));

    int rchanged = 0;
    CHECK_EQ(patch_acf_screens::UnpatchText(patched, rchanged), original);
}

TEST("screens: the A3xx template still wins where both are present") {
    // Order in ScreensFrameTemplates() is the tie-break, and it has to be stable:
    // the two frames are different datums, so "whichever was found first" must be
    // a decision rather than a map iteration order.
    std::string both = AttachmentAcf(true);
    both = both.substr(0, both.find("P _obja/count")) +
           "P _obja/2/_v10_att_file_stl CockpitLighting_XP12.obj\r\n"
           "P _obja/2/_v10_att_y_acf_prt_ref -3.000000000\r\n"
           "P _obja/count 3\r\n";
    CHECK_EQ(patch_acf_screens::FindFrameTemplate(both),
             patch_acf_screens::FindObj(both, "lights_inn.obj"));
}

TEST("screens: a row beyond the declared count does not read as attached") {
    // ⚠ Present in the file but never read: X-Plane reads 0..count-1. It looks
    // installed to a naive grep and draws nothing in the sim.
    const std::string text =
        "P _obja/0/_v10_att_file_stl lights_inn.obj\r\n"
        "P _obja/1/_v10_att_file_stl lights_screens.obj\r\n"
        "P _obja/count 1\r\n";
    CHECK_EQ(patch_acf_screens::FindObj(text, kScreensObj), 1);
    CHECK(!patch_acf_screens::IsAttached(text));
}

TEST("screens: attaching past a stale row beyond the count never overwrites it") {
    const std::string text =
        "P _obja/0/_v10_att_file_stl lights_inn.obj\r\n"
        "P _obja/0/_v10_att_y_acf_prt_ref 1.0\r\n"
        "P _obja/5/_v10_att_file_stl someone_elses.obj\r\n"
        "P _obja/count 1\r\n";
    int changed = 0;
    const std::string out = patch_acf_screens::PatchText(text, changed);
    CHECK_EQ(patch_acf_screens::FindObj(out, kScreensObj), 6);
    CHECK_EQ(patch_acf_screens::FindObj(out, "someone_elses.obj"), 5);
}

TEST("screens: detaching a middle row renumbers the ones above it down") {
    // ⚠ A hole in 0..count-1 would silently unload whatever the table says that
    // index is — some other mod's OBJ.
    const std::string text =
        "P _obja/0/_v10_att_file_stl lights_inn.obj\r\n"
        "P _obja/1/_v10_att_file_stl lights_screens.obj\r\n"
        "P _obja/2/_v10_att_file_stl wing.obj\r\n"
        "P _obja/count 3\r\n";
    int changed = 0;
    const std::string out = patch_acf_screens::UnpatchText(text, changed);
    CHECK_EQ(patch_acf_screens::FindObj(out, kScreensObj), -1);
    CHECK_EQ(patch_acf_screens::FindObj(out, "wing.obj"), 1);
    CHECK_EQ(patch_acf_screens::DeclaredCount(out), 2);
}

TEST("screens: detach on an unattached file is a no-op") {
    const std::string text = AttachmentAcf(true);
    int changed = 0;
    CHECK_EQ(patch_acf_screens::UnpatchText(text, changed), text);
    CHECK_EQ(changed, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// patch_glow
// ─────────────────────────────────────────────────────────────────────────────
TEST("glow: only the driven indices are redirected") {
    // ⚠ A redirected index Photon does NOT drive goes DARK — our array reads 0
    // where we never write. Index 0 here is such an index and must be left alone.
    const std::string mesh =
        "ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[0] 5000\n"
        "ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[12] 5000\n"
        "ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[13] 5000\n";
    int changed = 0;
    const std::string out = patch_glow::PatchText(mesh, "a339", false, changed);
    CHECK_EQ(changed, 2);
    CHECK(out.find("ToLissPhoton/exterior/glow[12]") != std::string::npos);
    CHECK(out.find("ToLissPhoton/exterior/glow[13]") != std::string::npos);
    CHECK(out.find("AirbusFBW/ExternalLightBrightnesses[0]") != std::string::npos);
    CHECK(out.find("ToLissPhoton/exterior/glow[0]") == std::string::npos);

    int back = 0;
    CHECK_EQ(patch_glow::PatchText(out, "a339", true, back), mesh);
    CHECK_EQ(back, 2);
}

TEST("glow: the closing bracket keeps [1] from matching inside [12]") {
    // If the token were `…Brightnesses[1` the swap would corrupt [12]..[16].
    const std::string mesh =
        "ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[1] 1\n"
        "ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[12] 1\n";
    int changed = 0;
    const std::string out = patch_glow::PatchText(mesh, "a339", false, changed);
    CHECK_EQ(changed, 1);
    CHECK(out.find("AirbusFBW/ExternalLightBrightnesses[1] 1") != std::string::npos);
}

TEST("glow: an airframe with no redirect is untouched") {
    const std::string mesh =
        "ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[12] 1\n";
    int changed = 0;
    CHECK_EQ(patch_glow::PatchText(mesh, "a320", false, changed), mesh);
    CHECK_EQ(changed, 0);
    CHECK(patch_glow::IndicesFor("a320") == nullptr);
}

TEST("glow: patching is idempotent") {
    const std::string mesh =
        "ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[12] 1\n";
    int c1 = 0, c2 = 0;
    const std::string once = patch_glow::PatchText(mesh, "a339", false, c1);
    CHECK_EQ(patch_glow::PatchText(once, "a339", false, c2), once);
    CHECK_EQ(c2, 0);
}

TEST("glow: TargetFiles finds only the OBJs that still need work") {
    TempDir tmp;
    Write(tmp / "WingL.obj",
          "ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[12] 1\n");
    Write(tmp / "Nose.obj",
          "ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[0] 1\n");
    const auto hits = patch_glow::TargetFiles(fsutil::PathToUtf8(tmp.path()), "a339");
    CHECK_EQ(hits.size(), static_cast<std::size_t>(1));
    CHECK(hits[0].find("WingL.obj") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// manifest — the file an uninstall depends on
// ─────────────────────────────────────────────────────────────────────────────
TEST("manifest: missing and corrupt both read as ABSENT, never as an error") {
    // ⚠ A manifest we cannot parse must read as "no install found" — which the
    // caller reports honestly — rather than aborting an uninstall the user still
    // needs.
    TempDir tmp;
    const fs::path objects = tmp.path() / "objects";
    const fs::path mf = tmp.path() / kBackupDirName / kManifestName;
    CHECK(!manifest::Read(fsutil::PathToUtf8(objects)).present);
    Write(mf, "{not json at all");
    CHECK(!manifest::Read(fsutil::PathToUtf8(objects)).present);
    Write(mf, "[1,2,3]");
    CHECK(!manifest::Read(fsutil::PathToUtf8(objects)).present);
}

TEST("manifest: a Python-installer manifest round-trips") {
    // Byte-for-byte what installer/actions.py writes. ⚠ THE SCHEMA DOES NOT CHANGE
    // IN THE PORT: a user who installed with the Python installer and uninstalls
    // with this one gets their files back only if this reads what that wrote.
    const std::string legacy = R"({
  "version": "0.7",
  "wing": "realwings",
  "installed_at": "2026-08-01T12:00:00",
  "backed_up": [
    "lights_out320_XP12.obj",
    "lights_inn.obj"
  ],
  "realwings_dir": "C:\\X-Plane 12\\Aircraft\\ToLissA320\\objects\\RealWings320",
  "realwings_patched": true,
  "interior": {
    "added": [ "pedals_details_1.png", "pedals_details_2.png" ],
    "liveries_patched": [ "liveries/Air France/objects/chairs_LIT.dds" ],
    "acf_patched": [ "a320.acf", "a320_XP11.acf" ],
    "installed": true,
    "version": "0.7",
    "airframe": "a320"
  },
  "screens": {
    "added": [ "lights_screens.obj" ],
    "acf_patched": [ "a320.acf" ],
    "installed": true,
    "version": "0.7"
  }
})";
    const manifest::Manifest m = manifest::Parse(legacy);
    CHECK(m.present);
    CHECK_EQ(m.version, std::string("0.7"));
    CHECK_EQ(m.wing, std::string("realwings"));
    CHECK_EQ(m.backedUp.size(), static_cast<std::size_t>(2));
    CHECK(m.realwingsPatched);
    CHECK(m.realwingsDir.find("RealWings320") != std::string::npos);

    CHECK(m.interior.installed);
    CHECK_EQ(m.interior.airframe, std::string("a320"));
    CHECK_EQ(m.interior.added.size(), static_cast<std::size_t>(2));
    CHECK_EQ(m.interior.liveriesPatched.size(), static_cast<std::size_t>(1));
    CHECK_EQ(m.interior.acfPatched.size(), static_cast<std::size_t>(2));

    CHECK(m.screens.installed);
    CHECK_EQ(m.screens.added[0], std::string("lights_screens.obj"));

    // and back out again, losing nothing
    const manifest::Manifest again = manifest::Parse(manifest::Serialize(m));
    CHECK_EQ(again.version, m.version);
    CHECK_EQ(again.backedUp, m.backedUp);
    CHECK_EQ(again.interior.liveriesPatched, m.interior.liveriesPatched);
    CHECK_EQ(again.screens.acfPatched, m.screens.acfPatched);
    CHECK_EQ(again.realwingsDir, m.realwingsDir);
}

TEST("manifest: an exterior-only install writes no realwings or interior keys") {
    // ⚠ Matching the Python side exactly: an install that never touched RealWings
    // leaves NO key at all, so an older installer reading this file sees what it
    // has always seen.
    manifest::Manifest m;
    m.present = true;
    m.version = "0.8";
    m.wing = "stock";
    m.backedUp = {"lights_out320_XP12.obj"};
    const std::string out = manifest::Serialize(m);
    CHECK(out.find("realwings") == std::string::npos);
    CHECK(out.find("interior") == std::string::npos);
    CHECK(out.find("screens") == std::string::npos);
    CHECK(out.find("\"wing\": \"stock\"") != std::string::npos);
    // ⚠ `added` follows the same rule. Every file an ordinary install writes
    // replaces a ToLiss one, so the list is empty and the KEY IS ABSENT — an
    // ordinary manifest is byte for byte what every prior version wrote.
    CHECK(out.find("added") == std::string::npos);
}

TEST("manifest: `added` round-trips once something actually lands in it") {
    // The other half of the rule above: written when non-empty, and read back, or
    // an uninstall after an upgrade would forget which files it must delete.
    manifest::Manifest m;
    m.present = true;
    m.version = "0.8.1";
    m.added = {"lights_out320_XP12.obj"};
    const std::string out = manifest::Serialize(m);
    CHECK(out.find("\"added\"") != std::string::npos);
    const manifest::Manifest back = manifest::Parse(out);
    CHECK_EQ(back.added.size(), std::size_t(1));
    CHECK_EQ(back.added[0], std::string("lights_out320_XP12.obj"));
    // ⚠ And it must NOT leak into the carry-through bag — a key we now model
    // being treated as unknown would write it twice.
    CHECK(back.unknownJson.find("added") == std::string::npos);
}

TEST("manifest: keys this build does not model survive a round trip") {
    // A newer installer's bookkeeping must not be silently dropped by an older
    // one — that would strand whatever it recorded.
    const manifest::Manifest m =
        manifest::Parse(R"({"version":"9.9","future_axis":{"installed":true}})");
    CHECK(m.present);
    const std::string out = manifest::Serialize(m);
    CHECK(out.find("future_axis") != std::string::npos);
}

TEST("manifest: write then read through a real directory") {
    TempDir tmp;
    const std::string objects = fsutil::PathToUtf8(tmp.path() / "objects");
    manifest::Manifest m;
    m.present = true;
    m.version = kPhotonVersion;
    m.wing = "durantula";
    m.backedUp = {"a.obj", "b.obj"};
    m.screens.installed = true;
    m.screens.added = {kScreensObj};
    std::string err;
    CHECK(manifest::Write(objects, m, err));
    // ⚠ WHERE it landed is half the test. The manifest indexes the backups and has
    // to sit with them, and they are no longer in the aircraft's asset folder.
    CHECK(Exists(tmp.path() / kBackupDirName / kManifestName));
    CHECK(!Exists(tmp.path() / "objects" / kBackupDirName / kManifestName));
    const manifest::Manifest back = manifest::Read(objects);
    CHECK(back.present);
    CHECK_EQ(back.wing, std::string("durantula"));
    CHECK_EQ(back.backedUp.size(), static_cast<std::size_t>(2));
    CHECK(back.screens.installed);
}

TEST("manifest: an older layout's manifest inside objects/ is still FOUND") {
    // ⚠ THE EXPENSIVE FAILURE, and the reason the location is resolved rather than
    // spelled out. An installer that cannot see an existing manifest believes
    // nothing is installed — and then backs up the OBJ it wrote last time as though
    // it were ToLiss's, which no later uninstall can undo.
    TempDir tmp;
    const std::string objects = fsutil::PathToUtf8(tmp.path() / "objects");
    Write(tmp.path() / "objects" / kBackupDirName / kManifestName,
          R"({"version":"0.8.4","wing":"stock","backed_up":["lights_out320_XP12.obj"]})");
    const manifest::Manifest m = manifest::Read(objects);
    CHECK(m.present);
    CHECK_EQ(m.version, std::string("0.8.4"));
    CHECK_EQ(m.backedUp.size(), static_cast<std::size_t>(1));
}

// ─────────────────────────────────────────────────────────────────────────────
// backup — which folder the bookkeeping lives in, and moving an old one out
// ─────────────────────────────────────────────────────────────────────────────
TEST("backup: the folder sits beside objects/, not inside it") {
    TempDir tmp;
    CHECK_EQ(backup::Dir(tmp.path()), tmp.path() / kBackupDirName);
    CHECK_EQ(backup::DirForObjects(tmp.path() / "objects"),
             tmp.path() / kBackupDirName);
}

TEST("backup: a trailing separator does not resolve one level short") {
    // `…/objects/` has an EMPTY filename, so a bare `parent_path()` answers
    // `…/objects` — which would put the backups back inside the asset folder, by
    // way of a path that came in as text from a config file or a CLI flag.
    TempDir tmp;
    const fs::path withSlash =
        fsutil::PathFromUtf8(fsutil::PathToUtf8(tmp.path() / "objects") + "/");
    CHECK_EQ(backup::DirForObjects(withSlash), tmp.path() / kBackupDirName);
}

TEST("backup: a legacy folder WINS while it still holds files") {
    // One resolver, so the manifest and the files it indexes can never end up in
    // two different folders — including when a migration fails halfway.
    TempDir tmp;
    Write(tmp.path() / "objects" / kBackupDirName / "lights_out320_XP12.obj", "stock");
    CHECK(backup::LegacyDirInUse(tmp.path()));
    CHECK_EQ(backup::Dir(tmp.path()), backup::LegacyDir(tmp.path()));
}

TEST("backup: an EMPTY legacy folder does not keep sending backups into objects/") {
    // ⚠ Files, not merely a folder. An uninstall that could not remove the old
    // directory, or a half-finished move, leaves an empty one — and treating that
    // as "still in use" would put every future backup right back where this whole
    // change is getting them out of.
    TempDir tmp;
    std::error_code ec;
    fs::create_directories(backup::LegacyDir(tmp.path()), ec);
    CHECK(!backup::LegacyDirInUse(tmp.path()));
    CHECK_EQ(backup::Dir(tmp.path()), backup::PreferredDir(tmp.path()));
}

TEST("backup: migrate moves the whole folder, liveries subfolder and all") {
    TempDir tmp;
    const fs::path legacy = backup::LegacyDir(tmp.path());
    Write(legacy / "lights_out320_XP12.obj", "STOCK OBJ");
    Write(legacy / kManifestName, R"({"version":"0.8.4"})");
    Write(legacy / "liveries" / "liveries__Air France__objects__chairs_LIT.dds",
          "STOCK DDS");

    const backup::MigrateResult r = backup::Migrate(tmp.path(), /*dryRun=*/false);
    CHECK(r.needed);
    CHECK(r.complete);
    CHECK_EQ(r.files, 3);
    CHECK(!Exists(legacy));
    const fs::path now = backup::PreferredDir(tmp.path());
    CHECK_EQ(Read(now / "lights_out320_XP12.obj"), std::string("STOCK OBJ"));
    CHECK_EQ(Read(now / "liveries" /
                      "liveries__Air France__objects__chairs_LIT.dds"),
             std::string("STOCK DDS"));
    CHECK(Exists(now / kManifestName));
}

TEST("backup: a legacy file OVERWRITES a same-named one at the destination") {
    // ⚠ The merge rule, and it runs the opposite way to "newest wins". Nothing has
    // written to objects/ since the upgrade, so the legacy copy is the OLDER one —
    // and under the backup-once rule the oldest copy is the closest to stock. A
    // newer file at the destination can only be a backup of Photon's own work.
    TempDir tmp;
    Write(backup::LegacyDir(tmp.path()) / "lights_out320_XP12.obj", "TRUE STOCK");
    Write(backup::PreferredDir(tmp.path()) / "lights_out320_XP12.obj",
          "PHOTON'S OWN OBJ");

    CHECK(backup::Migrate(tmp.path(), /*dryRun=*/false).complete);
    CHECK_EQ(Read(backup::PreferredDir(tmp.path()) / "lights_out320_XP12.obj"),
             std::string("TRUE STOCK"));
    CHECK(!Exists(backup::LegacyDir(tmp.path())));
}

TEST("backup: a dry run reports the move and performs none of it") {
    TempDir tmp;
    Write(backup::LegacyDir(tmp.path()) / "lights_out320_XP12.obj", "STOCK");
    const backup::MigrateResult r = backup::Migrate(tmp.path(), /*dryRun=*/true);
    CHECK(r.needed);
    CHECK(!r.complete);
    CHECK_EQ(r.files, 1);
    CHECK(Exists(backup::LegacyDir(tmp.path()) / "lights_out320_XP12.obj"));
    CHECK(!Exists(backup::PreferredDir(tmp.path())));
}

TEST("backup: migrating with nothing to move is a no-op that tidies an empty one") {
    TempDir tmp;
    const backup::MigrateResult none = backup::Migrate(tmp.path(), false);
    CHECK(!none.needed);
    CHECK(none.complete);
    CHECK(!Exists(backup::PreferredDir(tmp.path())));   // nothing is CREATED

    std::error_code ec;
    fs::create_directories(backup::LegacyDir(tmp.path()), ec);
    CHECK(backup::Migrate(tmp.path(), false).complete);
    CHECK(!Exists(backup::LegacyDir(tmp.path())));
}

// ─────────────────────────────────────────────────────────────────────────────
// patch_realwings — applying the bundle-time data
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// A minimal but structurally real patch table: one class, both wingtips, both
// sides, with a fence-only offset so the placement correction is exercised.
const char* kRwJson = R"({
  "_generated": "GENERATED — DO NOT HAND-EDIT",
  "schema": 1,
  "marker": "# >>> Photon RealWings",
  "sourceAirframe": "a320",
  "classes": {
    "airplane_nav_left_size": { "side": "port", "path": "wing_nav/fence/nav_beam" },
    "airplane_strobe_size":   { "side": null,   "path": "wing_strobe/fence/strobe_beam" }
  },
  "airframes": {
    "a320": {
      "airplane_nav_left_size": {
        "fence": {
          "port":      { "category": "nav", "class": "airplane_nav_bb", "index": 0,
                         "dir": "-0.574 0 -0.819", "offset": [0.1, -0.15, 0],
                         "branches": [
                           {"hide": "1 1", "rgb": "0.12 0.77 0.25", "intensity": 4000, "cone": "0.2"},
                           {"hide": "0 0", "rgb": "0.06 1 0",       "intensity": 5000, "cone": "0.2"}]},
          "starboard": { "category": "nav", "class": "airplane_nav_bb", "index": 0,
                         "dir": "0.574 0 -0.819", "offset": [-0.1, -0.15, 0],
                         "branches": [
                           {"hide": "1 1", "rgb": "1 0.03 0", "intensity": 4000, "cone": "0.2"},
                           {"hide": "0 0", "rgb": "1 0.1 0",  "intensity": 5000, "cone": "0.2"}]}
        },
        "sharklet": {
          "port":      { "category": "nav", "class": "airplane_nav_bb", "index": 0,
                         "dir": "-0.574 0 -0.819", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "0.12 0.77 0.25", "intensity": 4000, "cone": "0.2"},
                           {"hide": "0 0", "rgb": "0.06 1 0",       "intensity": 5000, "cone": "0.2"}]},
          "starboard": { "category": "nav", "class": "airplane_nav_bb", "index": 0,
                         "dir": "0.574 0 -0.819", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "1 0.03 0", "intensity": 4000, "cone": "0.2"},
                           {"hide": "0 0", "rgb": "1 0.1 0",  "intensity": 5000, "cone": "0.2"}]}
        }
      },
      "airplane_strobe_size": {
        "fence": {
          "port":      { "category": "strobe", "class": "airplane_strobe_pm", "index": 0,
                         "dir": "-0.895 -0.094 -0.436", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "0.75 0.75 1", "intensity": 8000, "cone": "0.091"},
                           {"hide": "0 0", "rgb": "0.66 0.91 1", "intensity": 8000, "cone": "0.091"}]},
          "starboard": { "category": "strobe", "class": "airplane_strobe_pm", "index": 0,
                         "dir": "0.895 -0.094 -0.436", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "0.75 0.75 1", "intensity": 8000, "cone": "0.091"},
                           {"hide": "0 0", "rgb": "0.66 0.91 1", "intensity": 8000, "cone": "0.091"}]}
        },
        "sharklet": {
          "port":      { "category": "strobe", "class": "airplane_strobe_pm", "index": 0,
                         "dir": "-0.895 -0.094 -0.436", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "0.75 0.75 1", "intensity": 8000, "cone": "0.091"},
                           {"hide": "0 0", "rgb": "0.66 0.91 1", "intensity": 8000, "cone": "0.091"}]},
          "starboard": { "category": "strobe", "class": "airplane_strobe_pm", "index": 0,
                         "dir": "0.895 -0.094 -0.436", "offset": null,
                         "branches": [
                           {"hide": "1 1", "rgb": "0.75 0.75 1", "intensity": 8000, "cone": "0.091"},
                           {"hide": "0 0", "rgb": "0.66 0.91 1", "intensity": 8000, "cone": "0.091"}]}
        }
      }
    }
  }
})";

// A real baked RealWings port-nav position (A320 CEO Main.obj) — 7-8 dp strings,
// so byte-for-byte preservation is distinguishable from a reformatted round trip.
const char* kRwObj =
    "A\n800\nOBJ\n\n"
    "TRIS 0 3\n"
    "\t\t\tLIGHT_PARAM\tairplane_nav_left_size "
    "-3.0584354 0.19549444 0.64423001 0.1 0\n"
    "TRIS 3 6\n";

}  // namespace

TEST("realwings: a zero or absent offset keeps the baked position byte-for-byte") {
    // ⚠ THE VERBATIM-POSITION GUARANTEE. RealWings' animation chain owns these
    // coordinates; an "equivalent" reformat moves the light.
    const auto data = patch_realwings::ParseJson(kRwJson, "<test>");
    int changed = 0;
    // MainNEO -> sharklet, whose offset is null in the fixture
    const std::string out =
        patch_realwings::PatchText(kRwObj, data, "a320", "MainNEO", changed);
    CHECK_EQ(changed, 1);
    // once in the commented original + once per branch
    std::size_t n = 0;
    for (std::size_t at = out.find("-3.0584354"); at != std::string::npos;
         at = out.find("-3.0584354", at + 1)) {
        ++n;
    }
    CHECK_EQ(n, static_cast<std::size_t>(3));
    CHECK(out.find("0.19549444") != std::string::npos);
}

TEST("realwings: the fence offset moves only the CEO file, and only moved axes") {
    const auto data = patch_realwings::ParseJson(kRwJson, "<test>");
    int changed = 0;
    const std::string out =
        patch_realwings::PatchText(kRwObj, data, "a320", "Main", changed);
    CHECK_EQ(changed, 1);
    CHECK(out.find("-2.9584354") != std::string::npos);   // x + 0.1
    CHECK(out.find("0.0454944") != std::string::npos);    // y - 0.15
    // ⚠ The untouched axis keeps its EXACT bytes rather than being re-rendered.
    CHECK(out.find("0.64423001") != std::string::npos);
    // the baked x survives only in the commented-out original
    std::size_t n = 0;
    for (std::size_t at = out.find("-3.0584354"); at != std::string::npos;
         at = out.find("-3.0584354", at + 1)) {
        ++n;
    }
    CHECK_EQ(n, static_cast<std::size_t>(1));
    CHECK(out.find("#LIGHT_PARAM") != std::string::npos);
    CHECK(out.find("ToLissPhoton/exterior/nav") != std::string::npos);
    CHECK(out.find(patch_realwings::kMarker) != std::string::npos);
}

TEST("realwings: the side of a strobe comes from the sign of the baked X") {
    // ⚠ RealWings bakes ONE strobe class for both wingtips and distinguishes them
    // only by position, so the side is not recoverable any other way. The nav
    // classes name their own side and pin it.
    const auto data = patch_realwings::ParseJson(kRwJson, "<test>");
    CHECK_EQ(patch_realwings::SideFor(data, "airplane_strobe_size", "-5.2"),
             std::string("port"));
    CHECK_EQ(patch_realwings::SideFor(data, "airplane_strobe_size", "5.2"),
             std::string("starboard"));
    // fixed by the class, whatever the X says
    CHECK_EQ(patch_realwings::SideFor(data, "airplane_nav_left_size", "5.2"),
             std::string("port"));
}

TEST("realwings: an unknown airframe falls back to the source airframe") {
    // `patch-realwings --root` without `--airframe` passes "?"; it must resolve
    // the way the DSL-side resolver did, not throw.
    const auto data = patch_realwings::ParseJson(kRwJson, "<test>");
    const auto& fallback = patch_realwings::RecordFor(
        data, "?", "airplane_nav_left_size", "fence", "port");
    const auto& direct = patch_realwings::RecordFor(
        data, "a320", "airplane_nav_left_size", "fence", "port");
    CHECK_EQ(fallback.category, direct.category);
    CHECK_EQ(fallback.dir, direct.dir);
}

TEST("realwings: a wrong schema is refused rather than half-applied") {
    // ⚠ Reading fields this build does not understand would emit LIGHT_PARAM lines
    // built from defaults: a wingtip light that is subtly wrong and looks installed.
    CHECK_THROWS(patch_realwings::ParseJson(
                     R"({"schema":999,"classes":{},"airframes":{},)"
                     R"("sourceAirframe":"a320"})", "<test>"),
                 patch_realwings::RealWingsError);
    CHECK_THROWS(patch_realwings::ParseJson("{not json", "<test>"),
                 patch_realwings::RealWingsError);
}

TEST("realwings: Run patches, refreshes from .bak, and reverses") {
    TempDir tmp;
    const auto data = patch_realwings::ParseJson(kRwJson, "<test>");
    Write(tmp / "Main.obj", kRwObj);
    const std::string root = fsutil::PathToUtf8(tmp.path());

    const auto first = patch_realwings::Run(root, data, "a320", false);
    CHECK_EQ(first.lights, 1);
    CHECK(Read(tmp / "Main.obj").find(patch_realwings::kMarker) !=
          std::string::npos);
    // the .bak is the TRUE original
    CHECK_EQ(Read(tmp / "Main.obj.bak"), std::string(kRwObj));

    // ⚠ Re-running REFRESHES from the .bak rather than double-patching, and never
    // clobbers that .bak with patched content.
    const std::string afterFirst = Read(tmp / "Main.obj");
    const auto second = patch_realwings::Run(root, data, "a320", false);
    CHECK_EQ(second.lights, 1);
    CHECK_EQ(Read(tmp / "Main.obj"), afterFirst);
    CHECK_EQ(Read(tmp / "Main.obj.bak"), std::string(kRwObj));

    const auto rev = patch_realwings::RunReverse(root, false);
    CHECK_EQ(rev.files, 1);
    CHECK_EQ(Read(tmp / "Main.obj"), std::string(kRwObj));
    CHECK(!fs::exists(tmp / "Main.obj.bak"));
}

TEST("realwings: a patched file with no .bak is skipped, never double-patched") {
    TempDir tmp;
    const auto data = patch_realwings::ParseJson(kRwJson, "<test>");
    Write(tmp / "Main.obj", kRwObj);
    const std::string root = fsutil::PathToUtf8(tmp.path());
    patch_realwings::Run(root, data, "a320", false);
    const std::string patched = Read(tmp / "Main.obj");
    fs::remove(tmp / "Main.obj.bak");
    const auto again = patch_realwings::Run(root, data, "a320", false);
    CHECK_EQ(again.lights, 0);
    CHECK_EQ(Read(tmp / "Main.obj"), patched);
}

TEST("realwings: dry runs write nothing, in both directions") {
    TempDir tmp;
    const auto data = patch_realwings::ParseJson(kRwJson, "<test>");
    Write(tmp / "Main.obj", kRwObj);
    const std::string root = fsutil::PathToUtf8(tmp.path());

    patch_realwings::Run(root, data, "a320", true);
    CHECK_EQ(Read(tmp / "Main.obj"), std::string(kRwObj));
    CHECK(!fs::exists(tmp / "Main.obj.bak"));

    // ⚠ Regression: the reverse once ignored `dry`, so a dry-run uninstall of a
    // RealWings install really did restore and delete the mod's .bak files.
    patch_realwings::Run(root, data, "a320", false);
    const std::string patched = Read(tmp / "Main.obj");
    patch_realwings::RunReverse(root, true);
    CHECK_EQ(Read(tmp / "Main.obj"), patched);
    CHECK(fs::exists(tmp / "Main.obj.bak"));
}

// ─────────────────────────────────────────────────────────────────────────────
// constants — the facts three files used to each own a copy of
// ─────────────────────────────────────────────────────────────────────────────
TEST("constants: the interior OBJ is never an airframe fingerprint") {
    // ⚠ lights_inn.obj is byte-identical across A319/A320/A321, so putting it in
    // the `objName` slot would make every A3xx match every other one.
    for (const Airframe& af : Airframes()) {
        CHECK(std::string(af.objName) != std::string(kInteriorObj));
    }
}

TEST("constants: a339 offers stock only and has screens but no interior") {
    // ⚠ Without this gate, the DSL's mount lookup falls back to whatever mount
    // exists and ships a stock OBJ mislabeled as a mod build.
    CHECK(WingIsOfferedFor("a339", "stock"));
    CHECK(!WingIsOfferedFor("a339", "durantula"));
    CHECK(!WingIsOfferedFor("a339", "realwings"));
    CHECK(!AirframeHasInterior("a339"));
    // ⚠ THE TWO EXCLUSIONS WERE NEVER THE SAME ARGUMENT, though they were written
    // as one line and lived together until 2026-08-11. The interior needs Gus's
    // source data, which does not cover this airframe, and its rheostat indices
    // differ; the display glow needs six positions and AirbusFBW/DUBrightness,
    // both of which the A330-900 has. So this one is IN and that one stays out.
    CHECK(AirframeHasScreens("a339"));
    CHECK(RealWingsDir("a339").empty());
    for (const std::string& af : {"a319", "a320", "a321"}) {
        CHECK(WingIsOfferedFor(af, "realwings"));
        CHECK(AirframeHasInterior(af));
        CHECK(AirframeHasScreens(af));
        CHECK(!RealWingsDir(af).empty());
    }
}

TEST("constants: the glow redirect table is the one the patcher reads") {
    // ⚠ GlowMapForIcao in plugin.cpp reads this same vector, which is what turns
    // "keep three files in sync or regions go dark" into a fact about one.
    const std::vector<int>* a339 = GlowIndicesFor("a339");
    CHECK(a339 != nullptr);
    CHECK_EQ(a339->size(), static_cast<std::size_t>(5));
    CHECK(patch_glow::IndicesFor("a339") == a339);
}

TEST("constants: plugin user dirs are matched on the first path segment") {
    // ⚠ A `.xpl` named `overlays.xpl` must NOT be caught by a prefix test.
    CHECK(PluginPathIsUserOwned("overlays/scanlines.png"));
    CHECK(PluginPathIsUserOwned("overlays"));
    CHECK(!PluginPathIsUserOwned("overlays.xpl"));
    CHECK(!PluginPathIsUserOwned("win_x64/ToLissPhoton.xpl"));
}

TEST("constants: exactly two interior textures have no stock counterpart") {
    // ⚠ These must be DELETED on uninstall, not restored — restoring a backup that
    // never existed leaves them in the aircraft folder forever.
    int added = 0;
    for (const std::string& t : InteriorTextures()) {
        if (InteriorTextureIsAdded(t)) ++added;
    }
    CHECK_EQ(added, 2);
    CHECK(InteriorTextureIsAdded("pedals_details_1.png"));
    CHECK(InteriorTextureIsAdded("pedals_details_2.png"));
    CHECK(!InteriorTextureIsAdded("knobs.png"));
}

TEST("constants: the version is a plain dotted number") {
    // It reaches a filename and the OBJ marker line, so a stray space or a "v"
    // prefix would land on disk.
    const std::string v = kPhotonVersion;
    CHECK(!v.empty());
    for (char c : v) CHECK((c >= '0' && c <= '9') || c == '.');
    CHECK(v.front() != '.');
    CHECK(v.back() != '.');
}

// ─── the TUI string engine ───────────────────────────────────────────────────
// ⚠ EVERY EXPECTATION BELOW WAS GENERATED BY RUNNING `installer/tui.py` — this is
// a 1:1 port of somebody else's index arithmetic, and "looks right" is not a test
// of it. Delete these cases only when the Python original goes (§9), and only
// after confirming the C++ ones still pass on their own.
using namespace photon::installer;

TEST("tui: VisLen ignores ANSI and counts CODEPOINTS, not bytes") {
    // ⚠ The failure this pins: the rules and arrows are multi-byte UTF-8 literals,
    // so a byte-counting VisLen makes `Repeat("▁", width)` measure 3x too long and
    // Clip cuts every row to a third of the window.
    CHECK_EQ(tui::VisLen("[1mab[0m"), 2);
    CHECK_EQ(tui::VisLen("▁▁▁"), 3);
    CHECK_EQ(tui::VisLen("[2mx[0my"), 2);
    CHECK_EQ(tui::VisLen(""), 0);
    CHECK_EQ(tui::VisLen("► ✓ ⚠"), 5);
}

TEST("tui: Clip preserves color and ALWAYS terminates with RESET") {
    // Even when nothing was cut — a row that ends mid-color would otherwise bleed
    // into the next one, and the frame is blitted as a single string.
    CHECK_EQ(tui::Clip("[92mabcdef[0m", 3), "[92mabc[0m");
    CHECK_EQ(tui::Clip("abc", 10), "abc[0m");
    CHECK_EQ(tui::Clip("▔▔▔▔▔", 2), "▔▔[0m");
}

TEST("tui: ActiveSgr accumulates codes and a RESET clears the set") {
    CHECK_EQ(tui::ActiveSgr("[1m[92mx[0m[97m"), "[97m");
    CHECK_EQ(tui::ActiveSgr("[1mabc"), "[1m");
    CHECK_EQ(tui::ActiveSgr("plain"), "");
    CHECK_EQ(tui::ActiveSgr("[1ma[mb"), "");   // the short form resets too
}

TEST("tui: WrapAnsi breaks at the last space that fits") {
    const std::vector<std::string> want = {"hello world", "this is a", "wrapped line"};
    CHECK_EQ(tui::WrapAnsi("hello world this is a wrapped line", 12), want);
    CHECK_EQ(tui::WrapAnsi("hello world", 5),
             std::vector<std::string>({"hello", "world"}));
}

TEST("tui: WrapAnsi hard-breaks a run with no space in it") {
    CHECK_EQ(tui::WrapAnsi("abcdefghijklmnop", 5),
             std::vector<std::string>({"abcde", "fghij", "klmno", "p"}));
}

TEST("tui: WrapAnsi drops the separator when a line is EXACTLY full") {
    // The (width+1)-th column being a space means the word before it fit exactly;
    // the space is the separator and must not start the next line.
    CHECK_EQ(tui::WrapAnsi("aaa bbb", 3), std::vector<std::string>({"aaa", "bbb"}));
    CHECK_EQ(tui::WrapAnsi("word wrap", 4), std::vector<std::string>({"word", "wrap"}));
}

TEST("tui: WrapAnsi re-opens the live color on every continuation line") {
    // ⚠ Without this a wrapped colored paragraph loses its color from line two
    // down, because the SGR that opened it is back on line one.
    const std::vector<std::string> want = {
        "[92mgreen text", "[92mthat wraps", "[92maround",
        "[92mhere[0m"};
    CHECK_EQ(tui::WrapAnsi("[92mgreen text that wraps around here[0m", 10),
             want);
}

TEST("tui: WrapAnsi carries a MULTI-code color and honours a mid-string reset") {
    const std::vector<std::string> want = {
        "[1m[97mbold", "[1m[97mwhite",
        "[1m[97mwraps[0m", "tail"};
    CHECK_EQ(tui::WrapAnsi("[1m[97mbold white wraps[0m tail", 8), want);
}

TEST("tui: WrapAnsi measures multi-byte glyphs as one column each") {
    const std::vector<std::string> want = {"▁ wide", "glyphs ▔", "wrap by", "codepoint"};
    CHECK_EQ(tui::WrapAnsi("▁ wide glyphs ▔ wrap by codepoint", 10), want);
}

TEST("tui: WrapAnsi returns a short string untouched") {
    CHECK_EQ(tui::WrapAnsi("", 5), std::vector<std::string>({""}));
    CHECK_EQ(tui::WrapAnsi("exact fit", 9), std::vector<std::string>({"exact fit"}));
    CHECK_EQ(tui::WrapAnsi("anything", 0), std::vector<std::string>({"anything"}));
}

TEST("tui: Row drops the right fragment rather than overflowing the line") {
    // ⚠ Overflowing is not a cosmetic problem: the footer's key hints are laid out
    // with this, and one column too many wraps the row and shears the whole frame.
    CHECK_EQ(tui::Row("ab", "cd", 10), "ab      cd");
    CHECK_EQ(tui::Row("abcde", "fghij", 8), "abcde");
}

TEST("tui: an option row is numbered from 1 and marks only the selection") {
    CHECK(tui::Option(0, "first", true).find("1.") != std::string::npos);
    CHECK(tui::Option(0, "first", true).find("►") != std::string::npos);
    CHECK(tui::Option(2, "third", false).find("3.") != std::string::npos);
    CHECK(tui::Option(2, "third", false).find("►") == std::string::npos);
}

TEST("tui: the breadcrumb has the five stages the screens index into") {
    // The screens address these POSITIONALLY (kStageRoot … kStageComplete), so the
    // order is wire, not taste.
    const std::vector<std::string> want = {"Root", "Aircraft", "Configure", "Run",
                                           "Complete"};
    CHECK_EQ(tui::Stages(), want);
}

// The rows of a composed frame, with the cursor-positioning and clear escapes
// stripped, so a test can assert on what the user would see.
std::vector<std::string> FrameRows(const std::string& frame) {
    std::vector<std::string> rows;
    std::size_t i = frame.find("[1;1H");
    while (i != std::string::npos) {
        const std::size_t start = frame.find('H', i) + 1;
        const std::size_t end = frame.find("[K", start);
        if (end == std::string::npos) break;
        rows.push_back(frame.substr(start, end - start));
        i = frame.find("[", end + 3);
        if (i != std::string::npos && frame.compare(i, 3, "[J") == 0) break;
    }
    return rows;
}

TEST("tui: a composed frame is exactly the terminal's height, every row clipped") {
    // ⚠ ONE ROW TOO MANY SCROLLS THE HEADER OFF THE TOP, and one column too many
    // wraps and shears everything below it. Both are invisible in the helper tests
    // and obvious the moment a real terminal draws the frame.
    std::vector<std::string> body;
    for (int i = 0; i < 200; ++i) body.push_back("row " + std::to_string(i));
    const std::string frame = tui::ComposeFrame(1, body);
    const std::vector<std::string> rows = FrameRows(frame);

    int width = 0;
    int height = 0;
    tui::Dims(width, height);
    CHECK_EQ(static_cast<int>(rows.size()), height);
    for (const std::string& r : rows) CHECK(tui::VisLen(r) <= width);
}

TEST("tui: the frame pins the header at the top and the footer at the bottom") {
    tui::RenderOpts o;
    o.back = "go back";
    o.cont = "continue";
    std::vector<std::string> body;
    for (int i = 0; i < 200; ++i) body.push_back("row " + std::to_string(i));
    const std::vector<std::string> rows = FrameRows(tui::ComposeFrame(1, body, o));

    CHECK(rows[0].find("ToLiss Photon") != std::string::npos);
    CHECK(rows[0].find("Aircraft") != std::string::npos);   // the breadcrumb
    CHECK(rows[1].find("▁") != std::string::npos);          // the header rule
    // Footer: the ▔ rule, the key hints, then an always-present blank line so the
    // hints never sit flush against the terminal's bottom edge.
    // Blank as the user sees it — Clip always appends a RESET, so the row is not
    // an empty STRING and asserting that instead is the trap here.
    CHECK_EQ(tui::VisLen(rows[rows.size() - 1]), 0);
    CHECK(rows[rows.size() - 2].find("ESC") != std::string::npos);
    CHECK(rows[rows.size() - 2].find("ENTER") != std::string::npos);
    CHECK(rows[rows.size() - 3].find("▔") != std::string::npos);
}

TEST("tui: an over-long body scrolls to keep the FOCUSED row on screen") {
    // ⚠ The focus index addresses the PRE-WRAP body and has to be remapped onto the
    // physical line wrapping put it on. Without that remap the scroll window keeps
    // the wrong row visible and the highlighted option is simply off screen — which
    // reads in a terminal as the arrow keys doing nothing.
    std::vector<std::string> body;
    for (int i = 0; i < 200; ++i) body.push_back("row " + std::to_string(i));
    tui::RenderOpts o;
    o.focus = 180;
    const std::string frame = tui::ComposeFrame(1, body, o);
    CHECK(frame.find("row 180") != std::string::npos);
    CHECK(frame.find("more above") != std::string::npos);

    o.focus = 0;
    const std::string top = tui::ComposeFrame(1, body, o);
    CHECK(top.find("row 0") != std::string::npos);
    CHECK(top.find("more below") != std::string::npos);
    CHECK(top.find("more above") == std::string::npos);
}

TEST("tui: a body that fits shows no scroll markers at all") {
    const std::string frame = tui::ComposeFrame(1, {"just", "a", "few", "rows"});
    CHECK(frame.find("more above") == std::string::npos);
    CHECK(frame.find("more below") == std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// intensity — the cockpit-light multiplier baked into lights_inn.obj
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// A miniature of the real generated file: CRLF, tab-indented light lines of all
// three kinds inside an ANIM gate, the interior needle carried by the gate's
// dataref, and a POINT_COUNTS line for the factor marker to anchor on. The
// numbers are real ones from dist/A320/objects/lights_inn.obj.
std::string InnObjFixture() {
    return
        "I\r\n800\r\nOBJ\r\n\r\nTEXTURE\t\r\nPOINT_COUNTS\t0 0 1 0\r\n"
        "# ToLiss Photon Lighting - interior (cockpit) lights\r\n"
        "ANIM_begin\r\n"
        "\tANIM_hide 0.5 2.5 ToLissPhoton/interior/dome\r\n"
        "\t# Dome Lights -> Old Halogen\r\n"
        "\tLIGHT_PARAM\tairplane_panel_sp -0.472 1.09 -3.35 1 0.37 0.16 0 "
        "1.414 0 0 0 1\r\n"
        "\tLIGHT_PARAM\tairplane_inst_sp -1.147 0.033 -4.2 1 0.37 0.16 22 "
        "0.4 0 -1 0 0.4\r\n"
        "\tLIGHT_SPILL_CUSTOM\t0.244 1.097 -3.463 1 0.37 0.16 0.25 1 "
        "0.146 -0.799 -0.584 0.97 ToLissPhoton/interior/spill/map\r\n"
        "ANIM_end\r\n";
}

bool HasLine(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

TEST("intensity: PatchText scales slot 8 of every light kind and nothing else") {
    int scaled = 0;
    const std::string out = intensity::PatchText(InnObjFixture(), 2.0, scaled);
    CHECK_EQ(scaled, 3);
    // slot 8 doubled; position, color, rheostat index, aim and cone untouched
    CHECK(HasLine(out, "airplane_panel_sp -0.472 1.09 -3.35 1 0.37 0.16 0 "
                       "2.828 0 0 0 1"));
    CHECK(HasLine(out, "airplane_inst_sp -1.147 0.033 -4.2 1 0.37 0.16 22 "
                       "0.8 0 -1 0 0.4"));
    CHECK(HasLine(out, "LIGHT_SPILL_CUSTOM\t0.244 1.097 -3.463 1 0.37 0.16 "
                       "0.25 2 0.146 -0.799 -0.584 0.97 "
                       "ToLissPhoton/interior/spill/map"));
    // each scaled line rides with its authored original...
    CHECK(HasLine(out, std::string(intensity::kOrigPrefix) +
                           "LIGHT_PARAM\tairplane_panel_sp -0.472"));
    // ...the gate (the cockpit-submenu sentinel) is untouched...
    CHECK(HasLine(out, "ANIM_hide 0.5 2.5 ToLissPhoton/interior/dome"));
    CHECK(intensity::CanPatch(out));
    // ...and the applied factor reads back.
    CHECK(intensity::SameFactor(intensity::FactorIn(out), 2.0));
    CHECK(intensity::SameFactor(intensity::FactorIn(InnObjFixture()), 1.0));
}

TEST("intensity: repatching recomputes from the originals, never compounds") {
    // 2x then 3x must equal 3x applied once — byte for byte, not merely
    // numerically — or every adjustment would drift the file a little further
    // from Gus's authored values.
    int n = 0;
    const std::string direct = intensity::PatchText(InnObjFixture(), 3.0, n);
    const std::string stepped = intensity::PatchText(
        intensity::PatchText(InnObjFixture(), 2.0, n), 3.0, n);
    CHECK_EQ(stepped, direct);
}

TEST("intensity: 1x restores the authored bytes exactly") {
    // The mechanism must leave NO trace at 1x — no marker, no orig comments —
    // so a user who tried the slider and put it back has the file the
    // installer staged, and a byte-diff against the payload stays clean.
    int n = 0;
    const std::string patched = intensity::PatchText(InnObjFixture(), 2.0, n);
    const std::string restored = intensity::PatchText(patched, 1.0, n);
    CHECK_EQ(restored, InnObjFixture());
    CHECK_EQ(n, 3);   // three lines restored
}

TEST("intensity: newline flavor is preserved both ways") {
    // The shipped file is CRLF; a `--target interior --write` dev build may be
    // LF. Either way the patch must not be a whole-file line-ending diff.
    int n = 0;
    CHECK(intensity::PatchText(InnObjFixture(), 2.0, n).find("\r\n") !=
          std::string::npos);
    std::string lf = InnObjFixture();
    std::string noCr;
    for (char c : lf) {
        if (c != '\r') noCr.push_back(c);
    }
    CHECK(intensity::PatchText(noCr, 2.0, n).find('\r') == std::string::npos);
}

TEST("intensity: CanPatch refuses a stock OBJ and a --debug build") {
    // The stock ToLiss lights_inn.obj binds none of our datarefs; a --debug
    // build reads its lights from the dev tuning datarefs, so baked numbers do
    // nothing there and the tuning session must not meet a rewritten file.
    CHECK(intensity::CanPatch(InnObjFixture()));
    CHECK(!intensity::CanPatch(
        "I\r\n800\r\nOBJ\r\nLIGHT_PARAM\tairplane_panel_sp 0 0 0 1 1 1 0 1 "
        "0 0 0 1\r\n"));
    CHECK(!intensity::CanPatch(InnObjFixture() +
                               "# ToLissPhoton/debug/light/0\r\n"));
}

TEST("intensity: out-of-range and garbage factors clamp to the harmless end") {
    CHECK(intensity::SameFactor(intensity::Clamp(0.0), 1.0));
    CHECK(intensity::SameFactor(intensity::Clamp(-3.0), 1.0));
    CHECK(intensity::SameFactor(intensity::Clamp(99.0), 4.0));
    int n = 0;
    // A clamped-to-1x patch is a no-op that leaves no marker behind.
    CHECK_EQ(intensity::PatchText(InnObjFixture(), 0.5, n), InnObjFixture());
}

TEST("intensity: SavedFactor reads the plugin's prefs, absent means 1x") {
    TempDir tmp;
    const std::string root = fsutil::PathToUtf8(tmp.path());
    // no prefs file at all — a user who never touched the slider
    CHECK(intensity::SameFactor(intensity::SavedFactor(root), 1.0));
    const fs::path prefs =
        tmp / "Output" / "preferences" / kProfilesJsonName;
    // the shape the plugin writes, livery entries and all
    Write(prefs,
          "{\n  \"$displays\": {\"enabled\": 1},\n"
          "  \"$cockpit\": {\"optimized\": 0, \"intensity\": 2.50},\n"
          "  \"C:/Aircraft/a320.acf#0\": {\"profile\": 3}\n}\n");
    CHECK(intensity::SameFactor(intensity::SavedFactor(root), 2.5));
    // a factor outside the range clamps rather than propagating
    Write(prefs, "{\"$cockpit\": {\"intensity\": 99}}");
    CHECK(intensity::SameFactor(intensity::SavedFactor(root), 4.0));
    // ⚠ corrupt reads as "never set", never as an error — same rule as the
    // manifest's parser.
    Write(prefs, "{not json");
    CHECK(intensity::SameFactor(intensity::SavedFactor(root), 1.0));
}

// ============================ the support report =============================
// The Help tab's "Save Log" — see core/support.h. Everything here is decidable
// from a string or a temp directory, which is the whole reason the collection
// rules live in core rather than in plugin.cpp.

TEST("support: a log names the airframe its install-target tag states") {
    const std::string log =
        "04:18:55 [INFO ] ToLiss Photon 0.9.2 installer log\n"
        "04:19:00 [INFO ] " + std::string(support::kInstallTargetTag) +
        "a321 (ToLiss A321) - install - C:/X-Plane 12/Aircraft/ToLissA321\n"
        "04:19:01 [INFO ] writing .../objects/lights_out321_XP12.obj (34007 bytes)\n";
    const std::vector<std::string> keys = support::AirframesNamedIn(log);
    CHECK_EQ(keys.size(), (size_t)1);
    CHECK_EQ(keys[0], std::string("a321"));
}

TEST("support: an untagged log still classifies, by the OBJ fingerprint") {
    // ⚠ THE FALLBACK IS NOT DEAD CODE. Every installer log already on a user's
    // disk predates the tag, and those are exactly the logs a first bug report
    // carries.
    const std::string log =
        "01:42:32 [INFO ] writing C:/X/Aircraft/A319/objects/lights_out319_XP12.obj\n"
        "01:42:33 [INFO ] writing C:/X/Aircraft/A319/objects/lights_inn.obj\n";
    const std::vector<std::string> keys = support::AirframesNamedIn(log);
    CHECK_EQ(keys.size(), (size_t)1);
    CHECK_EQ(keys[0], std::string("a319"));
}

TEST("support: a tagged log does NOT also answer from the fingerprint") {
    // ⚠ THE A340 CASE, one level up. `ExternalLights_XP12.obj` is ToLiss's name
    // for every newer airframe's exterior lights OBJ, so the fingerprint alone
    // reads an A340 run — which the installer REFUSES — as an installed a339.
    // Where the run stated its target, that statement is the answer.
    const std::string log =
        std::string("[INFO ] ") + support::kInstallTargetTag +
        "a320 (ToLiss A320) - install - C:/X/Aircraft/A320\n"
        "[INFO ] detect: skipped ExternalLights_XP12.obj (ICAO A342, not A339)\n"
        "[INFO ] writing .../objects/lights_out320_XP12.obj\n";
    const std::vector<std::string> keys = support::AirframesNamedIn(log);
    CHECK_EQ(keys.size(), (size_t)1);
    CHECK_EQ(keys[0], std::string("a320"));
}

TEST("support: the newest log per airframe wins, and one file is listed once") {
    TempDir tmp;
    const std::string dir = fsutil::PathToUtf8(tmp.path());
    // Oldest first, so the mtimes come out in the order they are written.
    const std::string prefix = support::kInstallerLogPrefix;
    Write(tmp / (prefix + "20260101_000000.log"),
          std::string("[INFO ] ") + support::kInstallTargetTag + "a321 (x) - install\n");
    Write(tmp / (prefix + "20260102_000000.log"),
          std::string("[INFO ] ") + support::kInstallTargetTag + "a319 (x) - install\n"
          "[INFO ] " + support::kInstallTargetTag + "a321 (x) - install\n");
    // Not ours: a same-folder file that merely ends in .log.
    Write(tmp / "SomeOtherInstaller.log", "not ours\n");

    const std::vector<support::LogFile> logs = support::InstallerLogs(dir);
    // The a319/a321 run is newest and covers BOTH, so it is the only file taken:
    // the older a321-only run is superseded, and the foreign .log is not a
    // candidate at all.
    CHECK_EQ(logs.size(), (size_t)1);
    CHECK_EQ(logs[0].name, prefix + "20260102_000000.log");
    CHECK_EQ(logs[0].airframes.size(), (size_t)2);
    CHECK_EQ(logs[0].kind, std::string("installer"));
}

TEST("support: a run that named no aircraft is still carried, as the newest") {
    // A detection-only run, or one that failed before it chose anything. It is
    // the only evidence there is about an installer that would not start.
    TempDir tmp;
    Write(tmp / (std::string(support::kInstallerLogPrefix) + "20260103_000000.log"),
          "[ERROR] no installable payload found\n");
    const std::vector<support::LogFile> logs =
        support::InstallerLogs(fsutil::PathToUtf8(tmp.path()));
    CHECK_EQ(logs.size(), (size_t)1);
    CHECK(logs[0].airframes.empty());
}

TEST("support: X-Plane's rotated logs come back newest first, missing ones skipped") {
    TempDir tmp;
    Write(tmp / "Log.txt", "current\n");
    Write(tmp / "Log.1.txt", "one\n");
    // No Log.2.txt on purpose — a sim that has only been run twice.
    Write(tmp / "Log.3.txt", "three\n");
    Write(tmp / "Log.9.txt", "out of range\n");
    const std::vector<support::LogFile> logs =
        support::XPlaneLogs(fsutil::PathToUtf8(tmp.path()), 3);
    CHECK_EQ(logs.size(), (size_t)3);
    CHECK_EQ(logs[0].name, std::string("Log.txt"));
    CHECK_EQ(logs[1].name, std::string("Log.1.txt"));
    CHECK_EQ(logs[2].name, std::string("Log.3.txt"));
    CHECK_EQ(logs[0].note, std::string("current session"));
}

TEST("support: a CDATA section survives a log that contains ]]>") {
    // ⚠ SILENT OTHERWISE. `]]>` is a byte sequence, not an escape: it simply ENDS
    // the section, and everything after it becomes markup the reader rejects.
    const std::string xml = support::XmlCdata("before ]]> after");
    CHECK(xml.find("]]]]><![CDATA[>") != std::string::npos ||
          xml.find("]]><![CDATA[>") != std::string::npos);
    // The payload is still all there, in order.
    CHECK(xml.find("before ") != std::string::npos);
    CHECK(xml.find(" after") != std::string::npos);
    // ...and no bare `]]>` remains except the one that closes the last section.
    size_t last = xml.rfind("]]>");
    CHECK_EQ(last, xml.size() - 3);
}

TEST("support: bytes XML cannot carry are dropped rather than emitted") {
    std::string dirty = "ok\ttab\nline\r\n";
    dirty += (char)0x01;          // a C0 control XML 1.0 forbids
    dirty += (char)0x7F;          // DEL
    dirty += "end";
    const std::string clean = support::SanitizeXmlText(dirty);
    CHECK(clean.find((char)0x01) == std::string::npos);
    CHECK(clean.find((char)0x7F) == std::string::npos);
    // Tab, LF and CR are the three that stay.
    CHECK(clean.find('\t') != std::string::npos);
    CHECK(clean.find('\n') != std::string::npos);
    CHECK(clean.find("okend") == std::string::npos);   // nothing else was eaten
    CHECK(clean.find("end") != std::string::npos);
}

TEST("support: invalid UTF-8 becomes a placeholder, valid UTF-8 is untouched") {
    // A scenery name written in some other code page is the realistic source.
    std::string mangled = "caf";
    mangled += (char)0xE9;        // ISO-8859-1 'e-acute', not UTF-8
    const std::string clean = support::SanitizeXmlText(mangled);
    CHECK_EQ(clean, std::string("caf?"));
    const std::string good = "caf\xC3\xA9 \xE2\x9A\xA0";   // é and the warning sign
    CHECK_EQ(support::SanitizeXmlText(good), good);
}

TEST("support: an over-long log keeps BOTH its head and its tail") {
    // ⚠ Both ends, never one. The system block — GPU, driver, plugin list — is at
    // the top and the failure is at the bottom; a tail-only excerpt loses which
    // driver it was and a head-only one loses the error.
    std::string big = "FIRST LINE\n";
    big += std::string(2 * 1024 * 1024, 'a') + "\n";
    big += std::string(2 * 1024 * 1024, 'b') + "\n";
    big += "LAST LINE\n";
    bool truncated = false;
    const std::string cut = support::Excerpt(big, truncated);
    CHECK(truncated);
    CHECK(cut.size() < big.size());
    CHECK(cut.find("FIRST LINE") != std::string::npos);
    CHECK(cut.find("LAST LINE") != std::string::npos);
    CHECK(cut.find("bytes omitted from the middle") != std::string::npos);

    // A log that fits comes back byte for byte, and says so.
    bool small = true;
    CHECK_EQ(support::Excerpt("short\n", small), std::string("short\n"));
    CHECK(!small);
}

TEST("support: the report is well formed and carries every log it was given") {
    TempDir tmp;
    Write(tmp / "Log.txt", "x-plane says <hello> & \"goodbye\"\n");
    Write(tmp / "Log.1.txt", "an older session\n");
    const std::vector<support::LogFile> logs =
        support::XPlaneLogs(fsutil::PathToUtf8(tmp.path()), 3);
    std::vector<support::ReportFact> facts;
    facts.push_back(support::ReportFact{"Aircraft", "C:/X/A320 & \"friends\".acf"});

    const std::string xml = support::BuildReportXml(facts, logs);
    CHECK(fsutil::StartsWith(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
    CHECK(fsutil::EndsWith(xml, "</photonReport>\n"));
    CHECK(xml.find("<logs count=\"2\">") != std::string::npos);
    // The attribute is escaped...
    CHECK(xml.find("A320 &amp; &quot;friends&quot;.acf") != std::string::npos);
    // ...and the log body is NOT, because it is inside CDATA. That asymmetry is
    // the point: a log full of angle brackets would otherwise triple in size.
    CHECK(xml.find("x-plane says <hello> & \"goodbye\"") != std::string::npos);
    CHECK(xml.find("excerpt=\"complete\"") != std::string::npos);
    // Every element opened is closed — crude, but it catches a stray tag.
    CHECK_EQ(std::count(xml.begin(), xml.end(), '<'),
             std::count(xml.begin(), xml.end(), '>'));

    // And it lands on disk under a name that sorts by time.
    std::string err;
    const std::string written =
        support::WriteReport(fsutil::PathToUtf8(tmp.path()), xml, err);
    CHECK(err.empty());
    CHECK(!written.empty());
    std::string back;
    CHECK(fsutil::ReadFileBytes(fsutil::PathFromUtf8(written), back));
    CHECK_EQ(back, xml);
    CHECK(fsutil::StartsWith(
        fsutil::PathToUtf8(fsutil::PathFromUtf8(written).filename()),
        "ToLissPhoton_Report_"));
    CHECK(fsutil::EndsWith(written, ".xml"));
}

// ─────────────────────────────────────────────────────────────────────────────
// integral lighting — the OBJ gate (core/patch_integral)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Shaped like the real `objects/lamps.obj`: tab-separated, CRLF, a header ending
// in POINT_COUNTS, then the vertex and index tables, then drawing — including
// geometry both inside and outside an existing ANIM block, and a trailing
// exporter comment. Every one of those is a place the wrap can go wrong.
std::string LampsObjFixture() {
    return
        "I\r\n"
        "800\r\n"
        "OBJ\r\n"
        "\r\n"
        "GLOBAL_cockpit_lit\r\n"
        "TEXTURE\ttext.png\r\n"
        "TEXTURE_LIT\ttext_LIT.png\r\n"
        "POINT_COUNTS\t3\t0\t0\t3\r\n"
        "VT\t0 0 0\t0 1 0\t0 0\r\n"
        "VT\t1 0 0\t0 1 0\t1 0\r\n"
        "VT\t0 1 0\t0 1 0\t0 1\r\n"
        "IDX10\t0 1 2 0 1 2 0 1 2 0\r\n"
        "ATTR_light_level\t0\t1\tckpt/brt/pedistal\t1500\r\n"
        "ANIM_begin\r\n"
        "\tANIM_hide\t0\t0\tckpt/radio/2/radioNavCover\r\n"
        "\tTRIS\t0\t3\r\n"
        "ANIM_end\r\n"
        "TRIS\t0\t3\r\n"
        "# Build with Blender 3.6.20\r\n";
}

// The offset of `needle`, or npos — so a test can say one thing comes before
// another, which is the whole contract for where the block opens.
std::size_t At(const std::string& text, const std::string& needle) {
    return text.find(needle);
}

}  // namespace

TEST("integral: the gate opens after the vertex tables, never above them") {
    // ⚠ THE ONE STRUCTURAL RULE. VT/IDX are data and OBJ8 requires them directly
    // after POINT_COUNTS; an ANIM_begin above them is not a harmless extra
    // block, it is a malformed file — and it would fail at load, silently, on a
    // 300 k-line file nobody is going to read.
    std::string out, err;
    CHECK(integral::PatchText(LampsObjFixture(), integral::Era::kIncandescent,
                              "", kPhotonVersion, out, err));
    CHECK(err.empty());
    CHECK(At(out, "POINT_COUNTS") < At(out, "VT\t0 0 0"));
    CHECK(At(out, "IDX10") < At(out, "ANIM_begin\r\n\tANIM_hide\t1"));
    CHECK(At(out, "ANIM_begin\r\n\tANIM_hide\t1") <
          At(out, "ATTR_light_level"));
    // …and it closes past everything, exporter comment included. ⚠ `rfind`: the
    // file already contains ToLiss's own ANIM_end blocks, so the first one says
    // nothing about ours.
    CHECK(At(out, "# Build with Blender") < out.rfind("ANIM_end\r\n"));
    CHECK(fsutil::EndsWith(out, "ANIM_end\r\n"));
}

TEST("integral: each branch hides at the OTHER branch's value") {
    // Two-valued gating, the exterior form: one hide per branch, at the value
    // that is not its own. Get this backwards and BOTH branches draw at once —
    // two cockpits of placards in the same place, which reads as a broken mod
    // rather than as an inverted comparison.
    std::string inc, led, err;
    CHECK(integral::PatchText(LampsObjFixture(), integral::Era::kIncandescent,
                              "", kPhotonVersion, inc, err));
    CHECK(integral::PatchText(LampsObjFixture(), integral::Era::kLed,
                              "text_LIT_photon_led.png", kPhotonVersion, led,
                              err));
    CHECK(HasLine(inc, "ANIM_hide\t1\t1\tToLissPhoton/interior/integral"));
    CHECK(HasLine(led, "ANIM_hide\t0\t0\tToLissPhoton/interior/integral"));
}

TEST("integral: a missing plugin fails OPEN to incandescent") {
    // An OBJ binds dataref NAMES at load and an unresolved one reads 0 forever.
    // So the branch that must survive a plugin that did not start is the one
    // whose hide does NOT match 0 — incandescent. This is the same fail-open
    // argument as `interior/optimized`, and it is why the polarity is not
    // arbitrary: reversed, an older plugin would give a cockpit nobody chose.
    std::string inc, led, err;
    CHECK(integral::PatchText(LampsObjFixture(), integral::Era::kIncandescent,
                              "", kPhotonVersion, inc, err));
    CHECK(integral::PatchText(LampsObjFixture(), integral::Era::kLed, "",
                              kPhotonVersion, led, err));
    // hide bounds are [v v]; at a dataref reading 0, incandescent's [1 1] does
    // not match (drawn) and LED's [0 0] does (hidden).
    CHECK(HasLine(inc, "ANIM_hide\t1\t1\t"));
    CHECK(HasLine(led, "ANIM_hide\t0\t0\t"));
}

TEST("integral: only the LED copy is repointed, and only its TEXTURE_LIT") {
    std::string inc, led, err;
    CHECK(integral::PatchText(LampsObjFixture(), integral::Era::kIncandescent,
                              "", kPhotonVersion, inc, err));
    CHECK(integral::PatchText(LampsObjFixture(), integral::Era::kLed,
                              "text_LIT_photon_led.png", kPhotonVersion, led,
                              err));
    // The in-place branch keeps the stock name — that file now holds the
    // incandescent recolor, so repointing it would orphan the recolor.
    CHECK(HasLine(inc, "TEXTURE_LIT\ttext_LIT.png\r\n"));
    CHECK(HasLine(led, "TEXTURE_LIT\ttext_LIT_photon_led.png\r\n"));
    // ⚠ The DAY texture is shared by both copies and must not be touched: it is
    // one file on disk and duplicating it would double 5 MB for no difference.
    CHECK(HasLine(led, "TEXTURE\ttext.png\r\n"));
}

TEST("integral: an already-gated OBJ is refused, never wrapped twice") {
    // A double wrap is invisible in a diff of a 300 k-line file and shows up in
    // the cockpit as the branch never drawing at all.
    std::string once, twice, err;
    CHECK(integral::PatchText(LampsObjFixture(), integral::Era::kIncandescent,
                              "", kPhotonVersion, once, err));
    CHECK(integral::IsPatched(once));
    CHECK(!integral::PatchText(once, integral::Era::kIncandescent, "",
                               kPhotonVersion, twice, err));
    CHECK(!err.empty());
    CHECK(twice.empty());
}

TEST("integral: the marker lands in the header, where a head-scan finds it") {
    // `marker::Parse` callers pass only the first few KB of these files — the
    // draw section is 300 k lines down. A marker written beside the gate would
    // parse in a test and be invisible to every real reader.
    std::string out, err;
    CHECK(integral::PatchText(LampsObjFixture(), integral::Era::kLed,
                              "text_LIT_photon_led.png", kPhotonVersion, out,
                              err));
    CHECK(At(out, "ToLissPhoton version:") < At(out, "VT\t0 0 0"));
    const marker::Marker m = marker::Parse(out.substr(0, 512));
    CHECK(m.found);
    CHECK_EQ(m.version, std::string(kPhotonVersion));
    CHECK_EQ(m.wing, std::string("integral"));
}

TEST("integral: newline flavor survives the wrap") {
    std::string crlf, lf, err;
    CHECK(integral::PatchText(LampsObjFixture(), integral::Era::kIncandescent,
                              "", kPhotonVersion, crlf, err));
    CHECK(crlf.find("ANIM_begin\r\n") != std::string::npos);

    std::string noCr;
    for (char c : LampsObjFixture()) {
        if (c != '\r') noCr.push_back(c);
    }
    CHECK(integral::PatchText(noCr, integral::Era::kIncandescent, "",
                              kPhotonVersion, lf, err));
    CHECK(lf.find('\r') == std::string::npos);
}

TEST("integral: TextureLitOf reads the binding and stops at POINT_COUNTS") {
    CHECK_EQ(integral::TextureLitOf(LampsObjFixture()),
             std::string("text_LIT.png"));
    // ⚠ Stops at the header's end so a TEXTURE_LIT-shaped string further down —
    // a comment, or our own gate comment — cannot answer for the binding.
    const std::string noBinding =
        "I\r\n800\r\nOBJ\r\nPOINT_COUNTS\t0 0 0 0\r\n"
        "# TEXTURE_LIT\tnot_a_binding.png\r\n";
    CHECK(integral::TextureLitOf(noBinding).empty());
}

TEST("integral: the LED name goes before the extension, OBJ and PNG alike") {
    CHECK_EQ(integral::LedNameFor("lamps.obj"),
             std::string("lamps_photon_led.obj"));
    CHECK_EQ(integral::LedNameFor("text_LIT.png"),
             std::string("text_LIT_photon_led.png"));
    CHECK_EQ(integral::LedNameFor("knobs_LIT.png"),
             std::string("knobs_LIT_photon_led.png"));
}

TEST("integral: a file that is not an OBJ8 is refused rather than mangled") {
    std::string out, err;
    CHECK(!integral::PatchText("not an obj at all\r\n",
                               integral::Era::kIncandescent, "", kPhotonVersion,
                               out, err));
    CHECK(!err.empty());
    // And an OBJ with a header but no drawing — every line a table line.
    CHECK(!integral::PatchText("I\r\n800\r\nOBJ\r\nPOINT_COUNTS\t1 0 0 0\r\n"
                               "VT\t0 0 0\t0 1 0\t0 0\r\n",
                               integral::Era::kIncandescent, "", kPhotonVersion,
                               out, err));
    CHECK(!err.empty());
}

TEST("integral: an era names one profile, and LED is not incandescent") {
    CHECK(integral::ProfileFor(integral::Era::kIncandescent) ==
          lit::Profile::kIncandescent);
    CHECK(integral::ProfileFor(integral::Era::kLed) == lit::Profile::kLed);
    CHECK_EQ(std::string(integral::EraKey(integral::Era::kLed)),
             std::string("led"));
    CHECK_EQ(std::string(integral::EraLabel(integral::Era::kIncandescent)),
             std::string("Incandescent"));
}

int main(int argc, char** argv) { return photontest::RunAll(argc, argv); }

// ─── neomod: detecting a spill-dimming lighting mod ──────────────────────────
//
// The whole group asks the same question from different angles: does this text
// WRITE `spill_cutoff_level`, and can we read the value back? Everything else
// about BSS NEO — its scattering maths, its textures, its scenery — is
// irrelevant to Photon. See core/neomod.h.

namespace {

// The shape of the real script's one line that matters, plus both corroborators.
std::string NeoLuaFixture() {
    return
        "-- some header\n"
        "dataref(\"sun_pitch\", \"sim/graphics/scenery/sun_pitch_degrees\", \"readonly\")\n"
        "set(\"sim/private/controls/lights/spill_cutoff_level\", 0.09)\n"
        "set(\"sim/private/controls/lights/photobb/hack_ev_lo\", 6.0)\n"
        "dataref(\"turbidity\", \"sim/private/controls/scattering/override_turbidity_t\", "
        "\"writable\")\n";
}

}  // namespace

TEST("neomod: a set() of the spill cutoff is a write, and the value parses") {
    const std::string lua = NeoLuaFixture();
    CHECK(neomod::WritesSpillCutoff(lua));
    CHECK(neomod::LooksRecognized(lua));
    double v = -1.0;
    CHECK(neomod::ParseSpillCutoff(lua, v));
    CHECK(std::fabs(v - 0.09) < 1e-9);
}

TEST("neomod: READING the dataref is not writing it") {
    // The distinction the whole detector rests on. A script that only watches
    // the cutoff changes nothing, and reporting it would tell a user to
    // compensate for a mod they do not have.
    const std::string lua =
        "dataref(\"cut\", \"sim/private/controls/lights/spill_cutoff_level\", "
        "\"readonly\")\nprint(cut)\n";
    CHECK(!neomod::WritesSpillCutoff(lua));
    double v = -1.0;
    CHECK(!neomod::ParseSpillCutoff(lua, v));
}

TEST("neomod: a writable dataref() binding counts as a write") {
    const std::string lua =
        "dataref(\"cut\", \"sim/private/controls/lights/spill_cutoff_level\", "
        "\"writable\")\ncut = 0.2\n";
    CHECK(neomod::WritesSpillCutoff(lua));
    // But the VALUE is not parseable from a binding: the next token is a mode
    // string, not a number. Detection must survive that; only the number is lost.
    double v = -1.0;
    CHECK(!neomod::ParseSpillCutoff(lua, v));
}

TEST("neomod: a call whose name merely ENDS in set is not a write") {
    // ⚠ `offset(`, `reset(`, `asset(` all end in "set". A false positive here
    // does not merely mis-parse — it tells a user to brighten a cockpit that
    // nothing is dimming.
    const std::string lua =
        "local v = offset(\"sim/private/controls/lights/spill_cutoff_level\")\n";
    CHECK(!neomod::WritesSpillCutoff(lua));
}

TEST("neomod: a commented-out write is not a write") {
    // A user who has already disabled the line by hand must not be warned about
    // a mod that is no longer doing anything.
    const std::string lua =
        "-- set(\"sim/private/controls/lights/spill_cutoff_level\", 0.09)\n";
    CHECK(!neomod::WritesSpillCutoff(lua));
    CHECK(!neomod::LooksRecognized(lua));
}

TEST("neomod: a cutoff of zero parses as zero, not as a parse failure") {
    // Zero is a LEGAL cutoff meaning "cull nothing", and strtod returns 0.0 on
    // failure too. Conflating them would report a mod that had switched culling
    // off as though it had switched it on.
    const std::string lua =
        "set(\"sim/private/controls/lights/spill_cutoff_level\", 0)\n";
    double v = -1.0;
    CHECK(neomod::ParseSpillCutoff(lua, v));
    CHECK(v == 0.0);
}

TEST("neomod: the cutoff alone is detected but not RECOGNIZED as BSS NEO") {
    const std::string lua =
        "set(\"sim/private/controls/lights/spill_cutoff_level\", 0.12)\n";
    CHECK(neomod::WritesSpillCutoff(lua));
    CHECK(!neomod::LooksRecognized(lua));
}

TEST("neomod: Detect reads Scripts/ only, never the disabled siblings") {
    TempDir tmp;
    const fs::path fwl = tmp.path() / "Resources" / "plugins" / "FlyWithLua";
    fs::create_directories(fwl / "Scripts");
    fs::create_directories(fwl / "Scripts (disabled)");
    fs::create_directories(fwl / "Scripts (Quarantine)");
    Write(fwl / "Scripts (disabled)" / "NEO.lua", NeoLuaFixture());
    Write(fwl / "Scripts (Quarantine)" / "NEO.lua", NeoLuaFixture());
    Write(fwl / "Scripts" / "harmless.lua", "-- nothing to see\n");

    const neomod::Result r = neomod::Detect(fsutil::PathToUtf8(tmp.path()));
    CHECK(!r.Detected());
    CHECK(r.Token() == std::string("none"));
}

TEST("neomod: Detect finds a RENAMED script, because the name is never the test") {
    // NEO.lua is what it ships as; a troubleshooting user renames things, and
    // repackaged copies arrive called anything at all.
    TempDir tmp;
    const fs::path scripts =
        tmp.path() / "Resources" / "plugins" / "FlyWithLua" / "Scripts";
    fs::create_directories(scripts);
    Write(scripts / "my lighting tweaks.lua", NeoLuaFixture());

    const neomod::Result r = neomod::Detect(fsutil::PathToUtf8(tmp.path()));
    CHECK(r.Detected());
    CHECK(r.Token() == std::string("bss_neo"));
    CHECK(r.scriptName == std::string("my lighting tweaks.lua"));
    CHECK(std::fabs(r.spillCutoff - 0.09) < 1e-9);
    CHECK(r.Texts(neomod::Severity::Problem).size() == 1u);
}

TEST("neomod: a recognized script outranks an unrecognized one whatever the order") {
    TempDir tmp;
    const fs::path scripts =
        tmp.path() / "Resources" / "plugins" / "FlyWithLua" / "Scripts";
    fs::create_directories(scripts);
    // "aaa" sorts first and is the WEAKER answer; the verdict must still name
    // the mod, because the name is the only part the user can act on.
    Write(scripts / "aaa.lua",
              "set(\"sim/private/controls/lights/spill_cutoff_level\", 0.2)\n");
    Write(scripts / "zzz.lua", NeoLuaFixture());

    const neomod::Result r = neomod::Detect(fsutil::PathToUtf8(tmp.path()));
    CHECK(r.Detected());
    CHECK(r.Token() == std::string("bss_neo"));
    CHECK(r.scriptName == std::string("zzz.lua"));
}

TEST("neomod: no FlyWithLua at all is a clean, quiet answer") {
    TempDir tmp;
    fs::create_directories(tmp.path() / "Resources");
    const neomod::Result r = neomod::Detect(fsutil::PathToUtf8(tmp.path()));
    CHECK(!r.Detected());
    CHECK(r.findings.empty());
    CHECK(!r.summary.empty());
}

// ─── the compensation ────────────────────────────────────────────────────────

TEST("intensity: EffectiveFactor multiplies only when the NEO flag is set") {
    CHECK(intensity::SameFactor(intensity::EffectiveFactor(1.0, false), 1.0));
    CHECK(intensity::SameFactor(intensity::EffectiveFactor(2.0, false), 2.0));
    CHECK(intensity::SameFactor(intensity::EffectiveFactor(1.0, true),
                                intensity::kNeoBoost));
    CHECK(intensity::SameFactor(intensity::EffectiveFactor(2.0, true),
                                2.0 * intensity::kNeoBoost));
}

TEST("intensity: the compensation may exceed the SLIDER's ceiling") {
    // The whole point of the separate bound: clamping the product to kMax would
    // silently cancel the compensation for exactly the users who had already
    // turned the brightness up because of the mod.
    const double e = intensity::EffectiveFactor(intensity::kMax, true);
    CHECK(e > intensity::kMax);
    CHECK(intensity::SameFactor(e, intensity::kEffectiveMax));
    // ...but it is still bounded, and still NaN-safe on the low end.
    CHECK(intensity::SameFactor(intensity::ClampEffective(1e9),
                                intensity::kEffectiveMax));
    CHECK(intensity::SameFactor(intensity::ClampEffective(-5.0), intensity::kMin));
}

TEST("intensity: PatchText accepts a compensated factor above kMax") {
    int scaled = 0;
    const double f = intensity::EffectiveFactor(intensity::kMax, true);
    const std::string out = intensity::PatchText(InnObjFixture(), f, scaled);
    CHECK(scaled > 0);
    CHECK(intensity::SameFactor(intensity::FactorIn(out), f));
    // and it still round-trips back to the authored file byte for byte
    int back = 0;
    const std::string restored = intensity::PatchText(out, 1.0, back);
    CHECK(restored == InnObjFixture());
}

TEST("intensity: SaveNeo/SavedNeo round-trip and preserve everything else") {
    TempDir tmp;
    const fs::path prefs =
        tmp.path() / "Output" / "preferences" / photon::kProfilesJsonName;
    Write(prefs,
              "{\n  \"$cockpit\": {\"optimized\": 1, \"intensity\": 2.50},\n"
              "  \"MyLivery\": {\"profile\": 3}\n}\n");
    const std::string root = fsutil::PathToUtf8(tmp.path());

    CHECK(!intensity::SavedNeo(root));
    CHECK(intensity::SaveNeo(root, true));
    CHECK(intensity::SavedNeo(root));
    // THE SURGICAL PART. An install that wiped the user's per-livery profiles to
    // record one boolean would be a far worse bug than the one it fixes.
    CHECK(Read(prefs).find("MyLivery") != std::string::npos);
    CHECK(intensity::SameFactor(intensity::SavedFactor(root), 2.5));

    CHECK(intensity::SaveNeo(root, false));
    CHECK(!intensity::SavedNeo(root));
    CHECK(Read(prefs).find("MyLivery") != std::string::npos);
}

TEST("intensity: a NEO flag that is not a real boolean reads as false") {
    // The plugin writes a JSON true/false for exactly this reason. A numeric 1
    // here must land on "change nothing" -- the flag decides what ships in the
    // user's OBJ, and the two halves have to agree on it.
    TempDir tmp;
    const fs::path prefs =
        tmp.path() / "Output" / "preferences" / photon::kProfilesJsonName;
    Write(prefs, "{\n  \"$cockpit\": {\"neo\": 1}\n}\n");
    CHECK(!intensity::SavedNeo(fsutil::PathToUtf8(tmp.path())));

    Write(prefs, "{\n  \"$cockpit\": {\"neo\": true}\n}\n");
    CHECK(intensity::SavedNeo(fsutil::PathToUtf8(tmp.path())));
}

TEST("intensity: SaveNeo refuses a corrupt prefs file rather than rewriting it") {
    // It may be the only copy of the user's per-livery profiles.
    TempDir tmp;
    const fs::path prefs =
        tmp.path() / "Output" / "preferences" / photon::kProfilesJsonName;
    Write(prefs, "{ this is not json");
    CHECK(!intensity::SaveNeo(fsutil::PathToUtf8(tmp.path()), true));
    CHECK(Read(prefs) == std::string("{ this is not json"));
}

TEST("intensity: SaveNeo(false) with no prefs file writes nothing") {
    // Absence already means false; inventing a preferences file to record a
    // default is state the user never set.
    TempDir tmp;
    const fs::path prefs =
        tmp.path() / "Output" / "preferences" / photon::kProfilesJsonName;
    CHECK(intensity::SaveNeo(fsutil::PathToUtf8(tmp.path()), false));
    CHECK(!fs::exists(prefs));
}
