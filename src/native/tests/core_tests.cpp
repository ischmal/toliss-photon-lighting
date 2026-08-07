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
#include "core/constants.h"
#include "core/fsutil.h"
#include "core/manifest.h"
#include "core/marker.h"
#include "core/patch_acf.h"
#include "core/patch_acf_screens.h"
#include "core/patch_glow.h"
#include "core/patch_realwings.h"
#include "core/version.h"

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

// ── .acf fixtures ────────────────────────────────────────────────────────────
// The two float styles, same values, as the two file generations really write
// them. Every `.acf` test runs against BOTH: the format trap's whole nature is
// that a patch works on one and silently no-ops on the other.
const char* kAcfXp12 =
    "I\r\n"
    "1 version\r\n"
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
    "1 version\r\n"
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
    return std::string(
        "I\r\n"
        "ACF\r\n"
        "P _obja/0/_v10_att_file_stl fuselage.obj\r\n"
        "P _obja/0/_v10_att_y_acf_prt_ref ") + y + "\r\n"
        "P _obja/1/_v10_att_file_stl lights_inn.obj\r\n"
        "P _obja/1/_v10_att_y_acf_prt_ref " + y + "\r\n"
        "P _obja/count 2\r\n";
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
    // XP12 files and silently no-ops on the two XP11 ones. Every airframe ships
    // all four, so a one-style test would have passed while half the install did
    // nothing.
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

    const auto files = patch_acf::AcfFiles(fsutil::PathToUtf8(tmp.path()));
    CHECK_EQ(files.size(), static_cast<std::size_t>(2));
    for (const std::string& f : files) CHECK(f.find(".bak") == std::string::npos);

    const auto res = patch_acf::Run(fsutil::PathToUtf8(tmp.path()), false, false);
    CHECK_EQ(res.touched.size(), static_cast<std::size_t>(2));
    CHECK(patch_acf::IsPatched(Read(tmp / "a320.acf")));
    CHECK(patch_acf::IsPatched(Read(tmp / "a320_XP11.acf")));
    CHECK_EQ(Read(tmp / "a320.acf.durantula.bak"), std::string(kAcfXp12));
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
    CHECK(!manifest::Read(fsutil::PathToUtf8(tmp.path())).present);
    Write(tmp.path() / kBackupDirName / kManifestName, "{not json at all");
    CHECK(!manifest::Read(fsutil::PathToUtf8(tmp.path())).present);
    Write(tmp.path() / kBackupDirName / kManifestName, "[1,2,3]");
    CHECK(!manifest::Read(fsutil::PathToUtf8(tmp.path())).present);
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
    manifest::Manifest m;
    m.present = true;
    m.version = kPhotonVersion;
    m.wing = "durantula";
    m.backedUp = {"a.obj", "b.obj"};
    m.screens.installed = true;
    m.screens.added = {kScreensObj};
    std::string err;
    CHECK(manifest::Write(fsutil::PathToUtf8(tmp.path()), m, err));
    const manifest::Manifest back = manifest::Read(fsutil::PathToUtf8(tmp.path()));
    CHECK(back.present);
    CHECK_EQ(back.wing, std::string("durantula"));
    CHECK_EQ(back.backedUp.size(), static_cast<std::size_t>(2));
    CHECK(back.screens.installed);
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

TEST("constants: a339 offers stock only and has no interior or screens") {
    // ⚠ Without this gate, the DSL's mount lookup falls back to whatever mount
    // exists and ships a stock OBJ mislabeled as a mod build.
    CHECK(WingIsOfferedFor("a339", "stock"));
    CHECK(!WingIsOfferedFor("a339", "durantula"));
    CHECK(!WingIsOfferedFor("a339", "realwings"));
    CHECK(!AirframeHasInterior("a339"));
    CHECK(!AirframeHasScreens("a339"));
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

int main(int argc, char** argv) { return photontest::RunAll(argc, argv); }
