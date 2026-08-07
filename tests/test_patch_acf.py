"""build/patch_acf.py — the ToLiss .acf cockpit-spot patch.

The headline risk this file guards is the XP12/XP11 NUMERIC FORMAT TRAP
(docs/interior_plan.md §3.5): the same values are written `-70.000000000` in an
XP12 .acf and `-70.0` in an XP11 one, so a naive literal patcher works on the two
XP12 files and SILENTLY NO-OPS on the two XP11 ones. Both formats are therefore
exercised as fixtures, on every test.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
if str(BUILD) not in sys.path:
    sys.path.insert(0, str(BUILD))

from installer import patch_acf as P  # noqa: E402


# Trimmed but faithful spot blocks, copied from the real ToLiss A320 files. The
# only difference between them is float rendering — that is the whole point.
XP12 = """\
P acf/_has_spot_lite 0
P acf/_spot1_3d_xyz/0 0.000000000
P acf/_spot1_3d_xyz/1 1.000000000
P acf/_spot1_3d_xyz/2 9.489999771
P acf/_spot_angle/0 0.000000000
P acf/_spot_angle/1 17.000000000
P acf/_spot_angle/2 14.000000000
P acf/_spot_name_3d/0 sim/cockpit2/electrical/panel_brightness_ratio[0]
P acf/_spot_name_3d/1 sim/cockpit2/electrical/panel_brightness_ratio[3]
P acf/_spot_name_3d/2 ckpt/lights/map
P acf/_spot_psi/0 -20.000000000
P acf/_spot_psi/1 0.000000000
P acf/_spot_psi/2 14.000000000
P acf/_spot_size/0 20.000000000
P acf/_spot_size/1 28.010000229
P acf/_spot_size/2 38.009998322
P acf/_spot_the/0 -70.000000000
P acf/_spot_the/1 -66.000000000
P acf/_spot_the/2 -53.000000000
"""

XP11 = """\
P acf/_has_spot_lite 0
P acf/_spot1_3d_xyz/0 0.0
P acf/_spot1_3d_xyz/1 1.0
P acf/_spot1_3d_xyz/2 9.489999771
P acf/_spot_angle/0 0.0
P acf/_spot_angle/1 17.0
P acf/_spot_angle/2 14.0
P acf/_spot_name_3d/0 sim/cockpit2/electrical/panel_brightness_ratio[0]
P acf/_spot_name_3d/1 sim/cockpit2/electrical/panel_brightness_ratio[3]
P acf/_spot_name_3d/2 ckpt/lights/map
P acf/_spot_psi/0 -20.0
P acf/_spot_psi/1 0.0
P acf/_spot_psi/2 14.0
P acf/_spot_size/0 20.0
P acf/_spot_size/1 28.010000229
P acf/_spot_size/2 38.009998322
P acf/_spot_the/0 -70.0
P acf/_spot_the/1 -66.0
P acf/_spot_the/2 -53.0
"""

BOTH = {"xp12": XP12, "xp11": XP11}


class StyleDetectionTests(unittest.TestCase):
    def test_detects_each_format(self):
        for want, text in BOTH.items():
            with self.subTest(style=want):
                self.assertEqual(P.detect_style(text), want)

    def test_render_matches_the_files_own_style(self):
        self.assertEqual(P.render(-60.0, "xp12"), "-60.000000000")
        self.assertEqual(P.render(-60.0, "xp11"), "-60.0")
        self.assertEqual(P.render(50.0, "xp12"), "50.000000000")
        self.assertEqual(P.render(50.0, "xp11"), "50.0")

    def test_xp11_render_always_keeps_a_decimal_point(self):
        """A bare `8` where the file says `8.0` would be a gratuitous format
        change in a file other mods also parse."""
        for v in (4.0, 8.0, 9.0, 10.0, 180.0, -60.0):
            self.assertIn(".", P.render(v, "xp11"))


class PatchTests(unittest.TestCase):
    def test_patch_applies_in_both_formats(self):
        """THE FORMAT TRAP. A literal string patcher passes the xp12 case and
        silently no-ops on xp11; this asserts both actually changed."""
        for style, text in BOTH.items():
            with self.subTest(style=style):
                out, changed = P.patch_text(text)
                self.assertEqual(changed, 10, "expected 7 numeric + 3 name lines")
                self.assertTrue(P.is_patched(out))
                self.assertEqual(P.verify_geometry(out), [])
                props = P.read_props(out)
                # rendered in this file's OWN style, not the other one's
                self.assertEqual(props["acf/_spot_the/0"], P.render(-60.0, style))
                self.assertEqual(props["acf/_spot_angle/0"], P.render(50.0, style))
                self.assertEqual(props["acf/_spot1_3d_xyz/1"], P.render(4.0, style))
                self.assertEqual(props["acf/_spot_size/0"], P.render(8.0, style))
                self.assertEqual(props["acf/_spot_size/1"], P.render(9.0, style))
                self.assertEqual(props["acf/_spot_size/2"], P.render(10.0, style))
                self.assertEqual(props["acf/_spot_psi/0"], P.render(180.0, style))

    def test_all_three_spots_are_disabled(self):
        for style, text in BOTH.items():
            with self.subTest(style=style):
                out, _ = P.patch_text(text)
                props = P.read_props(out)
                for i in range(3):
                    self.assertEqual(props[f"acf/_spot_name_3d/{i}"], "none")

    def test_untouched_properties_survive(self):
        """We must only ever write acf/_spot*; everything else in the file — and
        in the real thing that includes Durantula's 312 rewritten _obja lines —
        stays byte-identical."""
        for style, text in BOTH.items():
            with self.subTest(style=style):
                out, _ = P.patch_text(text)
                props_in, props_out = P.read_props(text), P.read_props(out)
                for keep in ("acf/_has_spot_lite", "acf/_spot1_3d_xyz/0",
                             "acf/_spot1_3d_xyz/2", "acf/_spot_angle/1",
                             "acf/_spot_angle/2", "acf/_spot_the/1",
                             "acf/_spot_the/2", "acf/_spot_psi/1", "acf/_spot_psi/2"):
                    self.assertEqual(props_out[keep], props_in[keep], keep)

    def test_patch_is_idempotent(self):
        for style, text in BOTH.items():
            with self.subTest(style=style):
                once, _ = P.patch_text(text)
                twice, changed = P.patch_text(once)
                self.assertEqual(twice, once)
                self.assertEqual(changed, 0)

    def test_round_trip_restores_the_file_byte_for_byte(self):
        for style, text in BOTH.items():
            with self.subTest(style=style):
                patched, _ = P.patch_text(text)
                back, _ = P.unpatch_text(patched)
                self.assertEqual(back, text)

    def test_line_endings_are_preserved(self):
        """.acf files are CRLF on disk; splitlines(True) keeps each terminator."""
        crlf = XP12.replace("\n", "\r\n")
        out, _ = P.patch_text(crlf)
        self.assertNotIn("\r\r", out)
        self.assertEqual(out.count("\r\n"), crlf.count("\r\n"))


class DetectionTests(unittest.TestCase):
    def test_stock_file_is_not_reported_as_patched(self):
        for style, text in BOTH.items():
            with self.subTest(style=style):
                self.assertFalse(P.is_patched(text))

    def test_sentinel_is_the_spot_names_not_a_hash(self):
        """Decision #12: detection reads whether OUR lines are present, never a
        file hash — the .acf legitimately carries other mods' edits. Appending an
        unrelated line must not disturb the verdict."""
        patched, _ = P.patch_text(XP12)
        meddled = patched + "P _obja/11/_v10_att_z_acf_prt_ref 20.750000000\n"
        self.assertTrue(P.is_patched(meddled))
        self.assertEqual(P.verify_geometry(meddled), [])

    def test_partial_revert_is_detected_by_geometry_corroboration(self):
        """The sentinel says 'our patch is live' but a mod could have reverted a
        geometry value underneath it. verify_geometry is what notices."""
        patched, _ = P.patch_text(XP12)
        broken = patched.replace("P acf/_spot_the/0 -60.000000000",
                                 "P acf/_spot_the/0 -70.000000000")
        self.assertTrue(P.is_patched(broken))          # sentinel still set
        self.assertIn("acf/_spot_the/0", P.verify_geometry(broken))

    def test_geometry_compare_is_numeric_not_textual(self):
        """-60 written any of several equivalent ways must still read as patched
        — the comparison is numeric with a tolerance, never a string match."""
        patched, _ = P.patch_text(XP12)
        for spelling in ("-60", "-60.0", "-60.00000", "-6.0e1"):
            variant = patched.replace("P acf/_spot_the/0 -60.000000000",
                                      f"P acf/_spot_the/0 {spelling}")
            with self.subTest(spelling=spelling):
                self.assertEqual(P.verify_geometry(variant), [])


class FileDiscoveryTests(unittest.TestCase):
    def _tree(self):
        import tempfile, shutil
        d = Path(tempfile.mkdtemp(prefix="photon_test_acf_"))
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        return d

    def test_finds_all_four_variants_by_content(self):
        """All four .acf per airframe carry the spot block and all four must be
        patched: Photon's OBJ work is XP12-only, but lights_inn.obj is shared by
        every variant, so an XP11 user would otherwise get patched OBJ lights
        alongside unpatched sim spots."""
        d = self._tree()
        for name, text in (("a320.acf", XP12), ("a320_StdDef.acf", XP12),
                           ("a320_XP11.acf", XP11), ("a320_XP11_StdDef.acf", XP11)):
            (d / name).write_text(text, encoding="utf-8")
        found = sorted(p.name for p in P.acf_files(d))
        self.assertEqual(found, ["a320.acf", "a320_StdDef.acf",
                                 "a320_XP11.acf", "a320_XP11_StdDef.acf"])

    def test_ignores_bak_files(self):
        """Durantula keeps an a320.acf.durantula.bak of this very file. Patching
        a backup would corrupt ITS uninstall."""
        d = self._tree()
        (d / "a320.acf").write_text(XP12, encoding="utf-8")
        (d / "a320.acf.durantula.bak").write_text(XP12, encoding="utf-8")
        (d / "a320.acf.bak").write_text(XP12, encoding="utf-8")
        self.assertEqual([p.name for p in P.acf_files(d)], ["a320.acf"])

    def test_finds_files_regardless_of_folder_or_file_naming(self):
        """Detection is by CONTENT, matching installer/detect.py's principle —
        the user may have renamed the aircraft folder or the .acf."""
        d = self._tree()
        (d / "totally-renamed.acf").write_text(XP12, encoding="utf-8")
        (d / "unrelated.acf").write_text("P acf/_wing_area 1220.0\n", encoding="utf-8")
        self.assertEqual([p.name for p in P.acf_files(d)], ["totally-renamed.acf"])

    def test_run_preserves_crlf_on_disk(self):
        """REGRESSION: real .acf files are CRLF. Reading them with Python's
        default universal-newline translation and writing back with newline=""
        silently rewrote EVERY line ending in the file — a gratuitous whole-file
        diff in a file other mods also read and back up. Both sides must use
        newline="". This is a file-I/O bug, so it only shows through run()."""
        import tempfile, shutil
        d = Path(tempfile.mkdtemp(prefix="photon_test_acf_crlf_"))
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        target = d / "a320.acf"
        raw = XP12.replace("\n", "\r\n").encode()
        target.write_bytes(raw)

        P.run(d)
        after = target.read_bytes()
        self.assertEqual(after.count(b"\r\n"), raw.count(b"\r\n"))
        self.assertNotIn(b"\r\r", after)
        self.assertEqual(after.count(b"\n"), after.count(b"\r\n"),
                         "no bare LF may be introduced")

        P.run(d, reverse=True)
        self.assertEqual(target.read_bytes(), raw, "byte-for-byte round trip")

    def test_run_preserves_lf_on_disk(self):
        """The converse: an LF file must not gain CRLF either."""
        import tempfile, shutil
        d = Path(tempfile.mkdtemp(prefix="photon_test_acf_lf_"))
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        target = d / "a320.acf"
        target.write_bytes(XP12.encode())
        P.run(d)
        self.assertNotIn(b"\r", target.read_bytes())
        P.run(d, reverse=True)
        self.assertEqual(target.read_bytes(), XP12.encode())

    def test_missing_spot_block_is_a_clear_error(self):
        with self.assertRaises(P.AcfError) as ctx:
            P._set_props("P acf/_wing_area 1220.0\n", {"acf/_spot_the/0": "-60.0"})
        self.assertIn("_spot_the/0", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
