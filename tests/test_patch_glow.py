"""Tests for build/patch_glow.py — the skin-glow redirect patcher.

patch_glow repoints ToLiss's `ATTR_light_level 0 1
AirbusFBW/ExternalLightBrightnesses[N] <nits>` LIT-texture regions at Photon's own
`ToLissPhoton/exterior/glow[N]` dataref (which the native plugin owns and drives),
for exactly the indices Photon's engine drives. These lock in the invariants that
keep it safe: only the redirected indices change, the trailing index + nits are
preserved, it's idempotent, and --reverse restores ToLiss's line byte-for-byte.

patch_glow is stdlib-only (no DSL/build_objs), so it imports at module level.
"""
import sys
import unittest
from pathlib import Path

BUILD = Path(__file__).resolve().parent.parent / "build"
sys.path.insert(0, str(BUILD))
from installer import patch_glow as G  # noqa: E402  (path insert must precede this)

OLD = G.OLD_DATAREF
NEW = G.NEW_DATAREF

# A realistic ToLiss mesh-OBJ excerpt: several ExternalLightBrightnesses regions,
# only some of which A339 redirects (12,13,14,15,16). [11],[17],[19] and the
# landing/taxi indices [0]-[5] must be left reading ToLiss's array untouched.
SAMPLE = "\n".join([
    "POINT_COUNTS 10 0 0 30",
    "\tATTR_light_level\t0\t1\t" + OLD + "[0]\t5000",
    "\tATTR_light_level\t0\t1\t" + OLD + "[11]\t5000",
    "\tATTR_light_level\t0\t1\t" + OLD + "[12]\t5000",
    "\tATTR_light_level\t0\t1\t" + OLD + "[13]\t5000",
    "\tATTR_light_level\t0\t1\t" + OLD + "[14]\t5000",
    "\tATTR_light_level\t0\t1\t" + OLD + "[15]\t3000",
    "\tATTR_light_level\t0\t1\t" + OLD + "[16]\t3000",
    "\tATTR_light_level\t0\t1\t" + OLD + "[17]\t5000",
    "\tATTR_light_level\t0\t1\t" + OLD + "[19]\t5000",
    "TRIS 0 30",
    "",
])


class IndicesTests(unittest.TestCase):
    def test_a339_indices(self):
        self.assertEqual(G.indices_for("a339"), [12, 13, 14, 15, 16])

    def test_unknown_airframe_has_no_indices(self):
        self.assertEqual(G.indices_for("a320"), [])
        self.assertEqual(G.indices_for("nope"), [])


class PatchFileTests(unittest.TestCase):
    def _write(self, tmp: Path, text: str = SAMPLE) -> Path:
        p = tmp / "WingL.obj"
        p.write_text(text, encoding="utf-8")
        return p

    def setUp(self):
        import tempfile
        self.tmp = Path(tempfile.mkdtemp())

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_only_redirected_indices_change(self):
        p = self._write(self.tmp)
        n = G.patch_file(p, "a339")
        out = p.read_text(encoding="utf-8")
        self.assertEqual(n, 5)  # 12,13,14,15,16
        # redirected ones now read our dataref
        for i in (12, 13, 14, 15, 16):
            self.assertIn(f"{NEW}[{i}]", out)
            self.assertNotIn(f"{OLD}[{i}]", out)
        # everything else still reads ToLiss's array
        for i in (0, 11, 17, 19):
            self.assertIn(f"{OLD}[{i}]", out)
            self.assertNotIn(f"{NEW}[{i}]", out)

    def test_index_and_nits_preserved(self):
        p = self._write(self.tmp)
        G.patch_file(p, "a339")
        out = p.read_text(encoding="utf-8")
        # the whole line changes only the dataref token: index + trailing nits stay
        self.assertIn("\t0\t1\t" + NEW + "[15]\t3000", out)
        self.assertIn("\t0\t1\t" + NEW + "[12]\t5000", out)

    def test_short_index_not_matched_inside_long_one(self):
        # [1] must not corrupt [12]/[13]: the closing ] is part of the token
        text = "\n".join([
            "POINT_COUNTS 1 0 0 3",
            "\tATTR_light_level\t0\t1\t" + OLD + "[1]\t5000",
            "\tATTR_light_level\t0\t1\t" + OLD + "[12]\t5000",
            "",
        ])
        p = self.tmp / "x.obj"
        p.write_text(text, encoding="utf-8")
        G.patch_file(p, "a339")
        out = p.read_text(encoding="utf-8")
        self.assertIn(f"{OLD}[1]", out)      # index 1 untouched (not in redirect set)
        self.assertIn(f"{NEW}[12]", out)     # index 12 redirected
        self.assertNotIn(f"{NEW}[1]", out)

    def test_idempotent(self):
        p = self._write(self.tmp)
        G.patch_file(p, "a339")
        first = p.read_text(encoding="utf-8")
        n2 = G.patch_file(p, "a339")  # nothing left to swap
        self.assertEqual(n2, 0)
        self.assertEqual(p.read_text(encoding="utf-8"), first)

    def test_reverse_restores_byte_for_byte(self):
        p = self._write(self.tmp)
        G.patch_file(p, "a339")
        n = G.patch_file(p, "a339", reverse=True)
        self.assertEqual(n, 5)
        self.assertEqual(p.read_text(encoding="utf-8"), SAMPLE)

    def test_dry_run_changes_nothing(self):
        p = self._write(self.tmp)
        n = G.patch_file(p, "a339", dry=True)
        self.assertEqual(n, 5)  # reports what it would change
        self.assertEqual(p.read_text(encoding="utf-8"), SAMPLE)  # ...but doesn't

    def test_unknown_airframe_is_noop(self):
        p = self._write(self.tmp)
        n = G.patch_file(p, "a320")
        self.assertEqual(n, 0)
        self.assertEqual(p.read_text(encoding="utf-8"), SAMPLE)


class RunAndTargetFilesTests(unittest.TestCase):
    def setUp(self):
        import tempfile
        self.tmp = Path(tempfile.mkdtemp())
        # two OBJs with redirected regions, one without
        (self.tmp / "WingL.obj").write_text(SAMPLE, encoding="utf-8")
        (self.tmp / "ExteriorGlass.obj").write_text(
            "POINT_COUNTS 1 0 0 3\n\tATTR_light_level\t0\t1\t" + OLD + "[15]\t3000\n",
            encoding="utf-8")
        (self.tmp / "LandingGear.obj").write_text(
            "POINT_COUNTS 1 0 0 3\n\tATTR_light_level\t0\t1\t" + OLD + "[2]\t5000\n",
            encoding="utf-8")

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_target_files_finds_only_affected(self):
        files = {p.name for p in G.target_files(self.tmp, "a339")}
        self.assertEqual(files, {"WingL.obj", "ExteriorGlass.obj"})

    def test_target_files_empty_for_unknown_airframe(self):
        self.assertEqual(G.target_files(self.tmp, "a320"), [])

    def test_run_patches_all_and_reverse_undoes(self):
        n = G.run(self.tmp, "a339")
        self.assertEqual(n, 6)  # 5 in WingL + 1 in ExteriorGlass
        self.assertIn(f"{NEW}[15]", (self.tmp / "ExteriorGlass.obj").read_text())
        # landing gear OBJ (index 2, not redirected) untouched
        self.assertIn(f"{OLD}[2]", (self.tmp / "LandingGear.obj").read_text())
        self.assertNotIn(NEW, (self.tmp / "LandingGear.obj").read_text())
        # reverse restores; after a full round trip target_files is empty again
        G.run(self.tmp, "a339", reverse=True)
        self.assertNotIn(NEW, (self.tmp / "WingL.obj").read_text())
        self.assertEqual(G.target_files(self.tmp, "a339", reverse=True), [])

    def test_run_unknown_airframe_noop(self):
        self.assertEqual(G.run(self.tmp, "a320"), 0)


if __name__ == "__main__":
    unittest.main()
