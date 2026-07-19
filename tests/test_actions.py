"""Unit tests for installer/actions.py — backup-once semantics, manifest
round-trip, byte-for-byte uninstall restore, and the RealWings live-patch
round trip, all against fake trees built in tempfile.mkdtemp(). A real
release payload is built once (via build/make_release.py) into a temp dir for
the whole module, since actions.install() needs real bundled OBJs/plugin/DSL
toolchain to copy — this is also an end-to-end check that make_release.py
itself still produces a working payload.

Run: python -m unittest discover -s tests -v
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from installer import actions, payload

RELEASE_DIR: Path | None = None


class _NullLog:
    def write(self, msg, level="INFO"):
        pass


def setUpModule():
    global RELEASE_DIR
    RELEASE_DIR = Path(tempfile.mkdtemp(prefix="photon_release_"))
    subprocess.run(
        [sys.executable, str(REPO / "build" / "make_release.py"),
         "--out", str(RELEASE_DIR)],
        check=True, cwd=REPO, capture_output=True, text=True,
    )
    payload.PAYLOAD_DIR = RELEASE_DIR / "payload"


def tearDownModule():
    if RELEASE_DIR is not None:
        shutil.rmtree(RELEASE_DIR, ignore_errors=True)


class FakeAircraft:
    """A minimal fake <X-Plane>/Aircraft/<folder>/objects/ tree with an
    original (non-Photon) lights_out OBJ already in place, as if freshly
    installed from ToLiss."""

    def __init__(self, tmp: Path, airframe="a320", fname="lights_out320_XP12.obj"):
        self.xplane_root = tmp / "X-Plane 12"
        self.folder = self.xplane_root / "Aircraft" / "ToLissA320_V1p3p2"
        self.objects = self.folder / "objects"
        self.objects.mkdir(parents=True)
        self.obj_path = self.objects / fname
        self.obj_path.write_text(
            "I\n800\nOBJ\n\nGLOBAL_cockpit_lit\nPOINT_COUNTS\t0\t0\t0\t0\n"
            "# ORIGINAL STOCK CONTENT\n", encoding="utf-8")
        self.ac = dict(folder=self.folder.name, airframe=airframe, ac_ver="1.4.1",
                       name="ToLiss A320", photon=None, wing=None, path=str(self.folder))


class InstallUninstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.fa = FakeAircraft(self.tmp)
        self.log = _NullLog()

    def test_install_writes_marker_and_backs_up_original(self):
        original = self.fa.obj_path.read_text(encoding="utf-8")
        steps = actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertTrue(any("Backed up" in s for s in steps))
        self.assertTrue(any("Installed" in s for s in steps))

        installed = self.fa.obj_path.read_text(encoding="utf-8")
        self.assertIn("ToLissPhoton version: 0.4 wing: stock", installed)

        backup = self.fa.objects / "Photon Backup Files" / self.fa.obj_path.name
        self.assertEqual(backup.read_text(encoding="utf-8"), original)

    def test_manifest_roundtrip(self):
        self.assertIsNone(actions.read_manifest(self.fa.objects))
        actions.install(self.fa.ac, "durantula", self.fa.xplane_root, self.log)
        manifest = actions.read_manifest(self.fa.objects)
        self.assertEqual(manifest["version"], "0.4")
        self.assertEqual(manifest["wing"], "durantula")
        self.assertIn(self.fa.obj_path.name, manifest["backed_up"])

    def test_reinstall_never_clobbers_original_backup(self):
        original = self.fa.obj_path.read_text(encoding="utf-8")
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        after_first = self.fa.obj_path.read_text(encoding="utf-8")
        self.assertNotEqual(original, after_first)

        # "reinstall" onto the now-Photon-ified file, switching wing variant
        actions.install(self.fa.ac, "durantula", self.fa.xplane_root, self.log)
        backup = self.fa.objects / "Photon Backup Files" / self.fa.obj_path.name
        self.assertEqual(backup.read_text(encoding="utf-8"), original,
                         "backup must stay the TRUE original, not the first Photon install")

    def test_uninstall_restores_byte_for_byte(self):
        original = self.fa.obj_path.read_text(encoding="utf-8")
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log)
        self.assertEqual(self.fa.obj_path.read_text(encoding="utf-8"), original)
        self.assertFalse((self.fa.objects / "Photon Backup Files").exists())

    def test_uninstall_without_install_raises(self):
        with self.assertRaises(actions.ActionError):
            actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log)

    def test_dry_run_makes_no_writes(self):
        original = self.fa.obj_path.read_text(encoding="utf-8")
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log, dry_run=True)
        self.assertEqual(self.fa.obj_path.read_text(encoding="utf-8"), original)
        self.assertFalse((self.fa.objects / "Photon Backup Files").exists())
        self.assertIsNone(actions.read_manifest(self.fa.objects))

    def test_plugin_staged_and_stale_pycache_cleared(self):
        plugin_dest = (self.fa.xplane_root / "Resources" / "plugins"
                       / "PythonPlugins" / "PI_ToLissPhoton.py")
        pycache = plugin_dest.parent / "__pycache__"
        pycache.mkdir(parents=True)
        (pycache / "stale.pyc").write_bytes(b"stale")

        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertTrue(plugin_dest.is_file())
        self.assertFalse(pycache.exists())


class RealWingsPatchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_rw_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.fa = FakeAircraft(self.tmp)
        self.rw_dir = self.fa.objects / "RealWings320"
        self.rw_dir.mkdir()
        self.mod_obj = self.rw_dir / "MainNEO.obj"
        self.mod_obj.write_text(
            "I\n800\nOBJ\n\n"
            "LIGHT_PARAM airplane_nav_left_size -5.1 0.5 -1.2 1 0 0 4 1000cd 0 0 -1 0.5\n"
            "LIGHT_PARAM airplane_strobe_size -5.2 0.5 -1.3 1 1 1 4 200000cd 0 0 -1 0.5\n",
            encoding="utf-8")
        self.log = _NullLog()

    def test_install_patches_and_uninstall_reverses(self):
        actions.install(self.fa.ac, "realwings", self.fa.xplane_root, self.log)
        patched = self.mod_obj.read_text(encoding="utf-8")
        self.assertIn("Photon RealWings", patched)
        self.assertTrue(self.mod_obj.with_suffix(".obj.bak").exists())

        manifest = actions.read_manifest(self.fa.objects)
        self.assertTrue(manifest["realwings_patched"])

        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log)
        self.assertNotIn("Photon RealWings", self.mod_obj.read_text(encoding="utf-8"))
        self.assertFalse(self.mod_obj.with_suffix(".obj.bak").exists())

    def test_missing_realwings_dir_falls_back_to_base_install(self):
        shutil.rmtree(self.rw_dir)
        steps = actions.install(self.fa.ac, "realwings", self.fa.xplane_root, self.log)
        self.assertTrue(any("continuing with" in s for s in steps))

    def test_switch_from_realwings_to_stock_reverses_mod_patch(self):
        """Regression: switching realwings -> stock must undo the in-place mod
        patch, or the baked wingtip lights double up with the stock OBJ's own
        and the patch leaks untracked past a later uninstall."""
        original_mod = self.mod_obj.read_text(encoding="utf-8")
        actions.install(self.fa.ac, "realwings", self.fa.xplane_root, self.log)
        self.assertIn("Photon RealWings", self.mod_obj.read_text(encoding="utf-8"))

        # switch to stock — must reverse the mod patch
        steps = actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertTrue(any("Reversed previous RealWings" in s for s in steps))
        self.assertEqual(self.mod_obj.read_text(encoding="utf-8"), original_mod)
        self.assertFalse(self.mod_obj.with_suffix(".obj.bak").exists())

        # manifest no longer claims a realwings patch, so uninstall is clean
        manifest = actions.read_manifest(self.fa.objects)
        self.assertFalse(manifest.get("realwings_patched"))

    def test_dry_run_uninstall_of_realwings_writes_nothing(self):
        """Regression: patch_realwings.run(reverse=True) once ignored `dry`, so
        a dry-run uninstall of a RealWings install actually restored+deleted the
        mod .bak files. The reverse must honor dry_run."""
        actions.install(self.fa.ac, "realwings", self.fa.xplane_root, self.log)
        patched_mod = self.mod_obj.read_text(encoding="utf-8")
        bak = self.mod_obj.with_suffix(".obj.bak")
        self.assertTrue(bak.exists())

        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log, dry_run=True)
        # nothing changed: mod still patched, .bak still there
        self.assertEqual(self.mod_obj.read_text(encoding="utf-8"), patched_mod)
        self.assertTrue(bak.exists())


if __name__ == "__main__":
    unittest.main()
