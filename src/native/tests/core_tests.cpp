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

#include <filesystem>
#include <string>
#include <vector>

#include "core/acf.h"
#include "core/backup.h"
#include "core/constants.h"
#include "core/fsutil.h"
#include "core/manifest.h"
#include "core/marker.h"
#include "core/patch_acf.h"
#include "core/patch_acf_screens.h"
#include "core/patch_glow.h"
#include "core/patch_realwings.h"
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

int main(int argc, char** argv) { return photontest::RunAll(argc, argv); }
