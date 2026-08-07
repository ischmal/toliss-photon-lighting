"""Regression tests for build/patch_realwings.py — the RealWings wingtip patcher,
in particular the DSL @realwings/@fence(CEO)/@sharklet(NEO) offset path.

The invariants locked in here:
  * a zero/absent offset leaves RealWings' baked coordinates BYTE-FOR-BYTE
    (the verbatim-position guarantee),
  * the @fence correction moves only the CEO OBJ (Main.obj), never the NEO one
    (MainNEO.obj), with X mirrored for the starboard side,
  * an undeclared wingtip variant gets NO correction (it must not inherit the
    other variant's),
  * re-patching an already-patched OBJ REFRESHES it from the pristine .bak (so
    tuned DSL values propagate) and never clobbers that .bak,
  * the emitter rejects a wingtip-keyed offset on a fixture it actually emits.

Import note: these tests import the repo's own build/ modules. test_actions (which
runs earlier) imports *bundled* copies of build_objs/patch_realwings from a temp
release dir that its teardown deletes, so the imports happen in setUpModule (after
that module finished), clear any stale cached copies first, and tear down their own
cache so test_payload's bundled-mode checks start clean.
"""
import contextlib
import io
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"

B = None  # build_objs, bound in setUpModule
P = None  # patch_realwings, bound in setUpModule

_MODULES = ("build_objs", "patch_realwings", "photon_dsl")


def setUpModule():
    global B, P
    for m in _MODULES:
        sys.modules.pop(m, None)
    sys.path.insert(0, str(BUILD))
    import build_objs
    import patch_realwings
    B, P = build_objs, patch_realwings


def tearDownModule():
    for m in _MODULES:
        sys.modules.pop(m, None)
    try:
        sys.path.remove(str(BUILD))
    except ValueError:
        pass


# A real baked RealWings port-nav position (A320 CEO Main.obj) — 7-8 dp strings,
# so byte-for-byte preservation is distinguishable from a reformatted round-trip.
BAKED = ("-3.0584354", "0.19549444", "0.64423001")

OBJ_TEMPLATE = (
    "A\n800\nOBJ\n\n"
    "TRIS 0 3\n"
    "\t\t\tLIGHT_PARAM\tairplane_nav_left_size "
    "-3.0584354 0.19549444 0.64423001 0.1 0\n"
    "TRIS 3 6\n"
)

FENCE_ONLY = {"fence": [0.1, -0.15, 0], "sharklet": [0, 0, 0]}


class ResolveOffsetTests(unittest.TestCase):
    def test_scalar_is_returned_as_is(self):
        self.assertEqual(P._resolve_offset([1, 2, 3], "fence", "port"), [1, 2, 3])

    def test_wingtip_map_selects_variant(self):
        self.assertEqual(P._resolve_offset(FENCE_ONLY, "fence", "port"),
                         [0.1, -0.15, 0])
        self.assertEqual(P._resolve_offset(FENCE_ONLY, "sharklet", "port"),
                         [0, 0, 0])

    def test_missing_wingtip_key_means_no_correction(self):
        # a partial map (config lacking the unconditioned base): the undeclared
        # variant must NOT inherit the other variant's correction
        self.assertIsNone(P._resolve_offset({"fence": [1, 0, 0]},
                                            "sharklet", "port"))

    def test_wingtip_then_side_nesting(self):
        off = {"fence": {"port": [1, 0, 0], "starboard": [2, 0, 0]},
               "sharklet": [0, 0, 0]}
        self.assertEqual(P._resolve_offset(off, "fence", "starboard"), [2, 0, 0])
        self.assertEqual(P._resolve_offset(off, "fence", "port"), [1, 0, 0])


class ApplyOffsetTests(unittest.TestCase):
    def test_zero_or_absent_offset_is_byte_for_byte(self):
        for off in (None, [0, 0, 0],
                    {"fence": [0, 0, 0], "sharklet": [0, 0, 0]}):
            self.assertEqual(P.apply_offset(*BAKED, off, "port", "fence"), BAKED)

    def test_fence_correction_moves_xy_and_keeps_zero_axis_verbatim(self):
        x, y, z = P.apply_offset(*BAKED, FENCE_ONLY, "port", "fence")
        self.assertEqual((x, y, z), ("-2.9584354", "0.0454944", "0.64423001"))

    def test_sharklet_stays_verbatim(self):
        self.assertEqual(P.apply_offset(*BAKED, FENCE_ONLY, "port", "sharklet"),
                         BAKED)

    def test_starboard_mirrors_x_only(self):
        off = {"fence": [0.1, 0, 0], "sharklet": [0, 0, 0]}
        x, y, z = P.apply_offset("3.0504417", "0.19997063", "0.6460346",
                                 off, "starboard", "fence")
        self.assertEqual(x, "2.9504417")           # +0.1 mirrored to -0.1
        self.assertEqual((y, z), ("0.19997063", "0.6460346"))


class PatchFileTests(unittest.TestCase):
    """patch_file end-to-end on a temp OBJ, with a forced known offset so the
    tests stay valid however the real DSL numbers are tuned."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.cfg = B.load_config(wing="realwings")
        # resolve_airframe shares dicts with cfg for a no-extends airframe, so
        # this override reaches patch_file's own resolve_source calls
        fx = B.resolve_airframe(self.cfg, "a320")["fixtures"]["wing_nav"]
        fx["offset"] = dict(FENCE_ONLY)

    def _write(self, name):
        p = self.tmp / name
        p.write_text(OBJ_TEMPLATE, encoding="utf-8")
        return p

    @staticmethod
    def _patch(*args, **kwargs):
        """patch_file, with its progress prints kept out of the test output."""
        with contextlib.redirect_stdout(io.StringIO()):
            return P.patch_file(*args, **kwargs)

    def test_ceo_file_gets_fence_correction(self):
        p = self._write("Main.obj")
        self.assertEqual(self._patch(p, self.cfg, airframe="a320"), 1)
        text = p.read_text(encoding="utf-8")
        self.assertIn(P.MARKER, text)
        self.assertIn("ToLissPhoton/exterior/nav", text)
        # corrected coords in the emitted lines, z untouched
        self.assertIn("-2.9584354", text)
        self.assertIn("0.0454944", text)
        # the baked x survives only in the commented-out original
        self.assertEqual(text.count("-3.0584354"), 1)
        self.assertIn("#LIGHT_PARAM", text)
        # backup written, byte-identical to the original
        self.assertEqual((self.tmp / "Main.obj.bak").read_text(encoding="utf-8"),
                         OBJ_TEMPLATE)

    def test_neo_file_stays_verbatim_when_sharklet_is_zero(self):
        p = self._write("MainNEO.obj")
        self.assertEqual(self._patch(p, self.cfg, airframe="a320"), 1)
        text = p.read_text(encoding="utf-8")
        # comment + both photon branches carry the baked coords byte-for-byte
        self.assertEqual(text.count("-3.0584354"), 3)
        self.assertEqual(text.count("0.19549444"), 3)
        self.assertNotIn("-2.9584354", text)

    def test_repatch_refreshes_with_new_offset_and_keeps_bak_pristine(self):
        # the staleness fix: re-running the patch after the DSL was tuned must
        # re-derive from .bak so the new numbers land — not skip
        p = self._write("Main.obj")
        self._patch(p, self.cfg, airframe="a320")
        self.assertIn("-2.9584354", p.read_text(encoding="utf-8"))
        fx = B.resolve_airframe(self.cfg, "a320")["fixtures"]["wing_nav"]
        fx["offset"] = {"fence": [0.2, -0.15, 0], "sharklet": [0, 0, 0]}
        self.assertEqual(self._patch(p, self.cfg, airframe="a320"), 1)
        text = p.read_text(encoding="utf-8")
        self.assertIn("-2.8584354", text)       # new correction applied
        self.assertNotIn("-2.9584354", text)    # old correction gone
        # the true original survives the refresh untouched
        self.assertEqual((self.tmp / "Main.obj.bak").read_text(encoding="utf-8"),
                         OBJ_TEMPLATE)

    def test_repatch_is_idempotent_when_nothing_changed(self):
        p = self._write("Main.obj")
        self._patch(p, self.cfg, airframe="a320")
        first = p.read_text(encoding="utf-8")
        self.assertEqual(self._patch(p, self.cfg, airframe="a320"), 1)
        self.assertEqual(p.read_text(encoding="utf-8"), first)

    def test_patched_file_without_bak_is_skipped(self):
        # no pristine source to refresh from -> must not double-patch
        p = self._write("Main.obj")
        self._patch(p, self.cfg, airframe="a320")
        (self.tmp / "Main.obj.bak").unlink()
        patched = p.read_text(encoding="utf-8")
        self.assertEqual(self._patch(p, self.cfg, airframe="a320"), 0)
        self.assertEqual(p.read_text(encoding="utf-8"), patched)

    def test_dry_refresh_writes_nothing(self):
        p = self._write("Main.obj")
        self._patch(p, self.cfg, airframe="a320")
        patched = p.read_text(encoding="utf-8")
        self.assertEqual(self._patch(p, self.cfg, airframe="a320", dry=True), 1)
        self.assertEqual(p.read_text(encoding="utf-8"), patched)
        self.assertEqual((self.tmp / "Main.obj.bak").read_text(encoding="utf-8"),
                         OBJ_TEMPLATE)

    def test_dry_run_writes_nothing(self):
        p = self._write("Main.obj")
        self.assertEqual(self._patch(p, self.cfg, airframe="a320", dry=True), 1)
        self.assertEqual(p.read_text(encoding="utf-8"), OBJ_TEMPLATE)
        self.assertFalse((self.tmp / "Main.obj.bak").exists())

    def test_unknown_airframe_falls_back_to_a320_source(self):
        # e.g. cmd_patch_realwings --root without --airframe passes "?"
        p = self._write("Main.obj")
        self.assertEqual(self._patch(p, self.cfg, airframe="?"), 1)
        self.assertIn("-2.9584354", p.read_text(encoding="utf-8"))


class BundleTimeResolutionTests(unittest.TestCase):
    """⚠ THE SPLIT'S CORRECTNESS NET (docs/installer_cpp_plan.md §2a).

    The patcher used to resolve every emitted parameter out of the DSL at INSTALL
    time. It now resolves them at BUNDLE time into `realwings_patch.json` and the
    installer only applies them. The whole risk of that move is that the two ends
    disagree — a value that survives a Python dict but not a JSON round trip, or a
    dimension (side, wingtip, airframe) that the resolver keys one way and the
    applier looks up another. Either would show up as wingtip lights that are
    subtly wrong in a shipped bundle and perfect on the dev machine, which is the
    hardest possible place to notice it.

    So: resolve, serialize, reload, apply — and require the bytes to match the
    live-DSL path exactly."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_rwsplit_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.cfg = B.load_config(wing="realwings")
        # A non-zero, side- and wingtip-varying offset, so the placement
        # correction is actually exercised rather than passing by being null.
        fx = B.resolve_airframe(self.cfg, "a320")["fixtures"]["wing_nav"]
        fx["offset"] = dict(FENCE_ONLY)

    def _write(self, name):
        p = self.tmp / name
        p.write_text(OBJ_TEMPLATE, encoding="utf-8")
        return p

    def test_json_round_trip_patches_byte_for_byte(self):
        import json

        from installer import realwings as RW

        data = P.resolve_patch_data(self.cfg)
        reloaded = json.loads(json.dumps(data))

        for name in ("Main.obj", "MainNEO.obj"):
            live, bundled = self._write(f"live_{name}"), self._write(f"bun_{name}")
            with contextlib.redirect_stdout(io.StringIO()):
                RW.patch_file(live, data, airframe="a320")
                RW.patch_file(bundled, reloaded, airframe="a320")
            self.assertEqual(bundled.read_text(encoding="utf-8"),
                             live.read_text(encoding="utf-8"), name)
            # and it is a real patch, not two identically-unpatched files
            self.assertIn(P.MARKER, live.read_text(encoding="utf-8"))

    def test_every_dimension_is_resolved(self):
        """A record for every (airframe, class, wingtip, side). A gap would only
        surface when a user happened to own that combination."""
        data = P.resolve_patch_data(self.cfg)
        self.assertIn(P.SOURCE_AIRFRAME, data["airframes"])
        for af, per_class in data["airframes"].items():
            self.assertEqual(sorted(per_class), sorted(P.SOURCE), af)
            for cls, by_wingtip in per_class.items():
                self.assertEqual(sorted(by_wingtip), sorted(P.WINGTIPS))
                for wingtip, by_side in by_wingtip.items():
                    self.assertEqual(sorted(by_side), sorted(P.SIDES))
                    for side, rec in by_side.items():
                        where = f"{af}/{cls}/{wingtip}/{side}"
                        for field in ("category", "class", "index", "dir",
                                      "branches"):
                            self.assertIn(field, rec, where)
                        self.assertEqual(len(rec["branches"]), 2, where)

    def test_a339_is_absent_because_it_has_no_wing_mods(self):
        """Same gate as the OBJ payload's (SUPPORTED_WINGS). A339 records would be
        resolved off the A320 fixtures and mean nothing."""
        self.assertNotIn("a339", P.resolve_patch_data(self.cfg)["airframes"])

    def test_the_generated_file_says_it_is_generated(self):
        """⚠ The one thing standing between this artifact and someone tuning a
        light in it — which would be overwritten on the next build and, unlike a
        DSL edit, reach nothing else."""
        dest = self.tmp / "realwings_patch.json"
        P.write_patch_data(dest, self.cfg)
        text = dest.read_text(encoding="utf-8")
        self.assertIn("DO NOT HAND-EDIT", text)
        self.assertIn("lights.layout.phdsl", text)

    def test_an_unknown_airframe_falls_back_to_the_source_airframe(self):
        """`patch-realwings --root` without `--airframe` passes "?"; the applier
        must resolve it the way the DSL resolver did, not KeyError."""
        from installer import realwings as RW

        data = P.resolve_patch_data(self.cfg)
        rec = RW.record_for(data, "?", "airplane_nav_left_size", "fence", "port")
        self.assertEqual(
            rec, RW.record_for(data, P.SOURCE_AIRFRAME,
                               "airplane_nav_left_size", "fence", "port"))


class EmitterGuardTests(unittest.TestCase):
    def test_wingtip_offset_on_emitted_fixture_is_rejected(self):
        # @fence/@sharklet offsets are patcher-only; on a fixture the emitter
        # actually emits they must be a clear error, not float("fence")
        cfg = B.load_config(wing="stock")
        fx = B.resolve_airframe(cfg, "a320")["fixtures"]["landing_left"]
        fx["offset"] = {"fence": [1, 0, 0], "sharklet": [0, 0, 0]}
        with self.assertRaises(SystemExit):
            B.Emitter(cfg, "a320", "stock").emit()


class A339NoRealWingsTests(unittest.TestCase):
    """a339 (A330-900) has no wing mods at all — REALWINGS_DIR has no entry for
    it — so anything that iterates "all airframes" for a RealWings operation
    must skip it with a message, not KeyError."""

    def test_find_realwings_dir_reports_no_wing_mods(self):
        rw, reason = B._find_realwings_dir("a339")
        self.assertIsNone(rw)
        self.assertIn("no wing mods", reason)

    def test_cmd_patch_realwings_skips_a339_without_crashing(self):
        class Args:
            airframe = None
            root = None
            dry_run = True
            reverse = False
        # Force _xplane_root() to a fake, empty tree so no real aircraft
        # (this dev machine may have real ones, RealWings or not) leaks into
        # the test — every airframe, including a339, must come back "not
        # found", and a339 specifically via its missing REALWINGS_DIR entry
        # rather than a KeyError.
        empty_root = Path(tempfile.mkdtemp(prefix="photon_test_norw_"))
        self.addCleanup(shutil.rmtree, empty_root, ignore_errors=True)
        (empty_root / "Aircraft").mkdir()
        saved = B._xplane_root
        B._xplane_root = lambda: empty_root
        self.addCleanup(setattr, B, "_xplane_root", saved)

        with contextlib.redirect_stdout(io.StringIO()) as out:
            with self.assertRaises(SystemExit) as ctx:
                B.cmd_patch_realwings(Args())
        self.assertNotIsInstance(ctx.exception.__cause__, KeyError)
        self.assertIn("a339: skipped", out.getvalue())


if __name__ == "__main__":
    unittest.main()
