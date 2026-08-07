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

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from installer import actions, payload
from installer.constants import VERSION

RELEASE_DIR: Path | None = None
FAKE_PLUGIN_DIR: Path | None = None

# Distinct bytes per arch so tests can assert each .xpl lands verbatim. Stand-ins
# for the real compiled binaries — the installer tests care about placement and
# byte-fidelity, not that these are valid X-Plane plugins, so no C++ toolchain is
# needed to run the suite.
FAKE_XPL = {
    "win_x64": b"MZ  fake win ToLissPhoton.xpl\n",
    "mac_x64": b"\xcf\xfa\xed\xfe  fake mac ToLissPhoton.xpl\n",
    "lin_x64": b"\x7fELF  fake lin ToLissPhoton.xpl\n",
}


class _NullLog:
    def write(self, msg, level="INFO"):
        pass


def _make_fake_plugin(dirpath: Path):
    """A fake native fat-plugin folder — <arch>/ToLissPhoton.xpl for all three
    platforms — so the multi-arch install path is exercised without the C++ build."""
    for arch, data in FAKE_XPL.items():
        p = dirpath / arch / "ToLissPhoton.xpl"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


def setUpModule():
    global RELEASE_DIR, FAKE_PLUGIN_DIR
    RELEASE_DIR = Path(tempfile.mkdtemp(prefix="photon_release_"))
    FAKE_PLUGIN_DIR = Path(tempfile.mkdtemp(prefix="photon_plugin_"))
    _make_fake_plugin(FAKE_PLUGIN_DIR)
    subprocess.run(
        [sys.executable, str(REPO / "build" / "make_release.py"),
         "--out", str(RELEASE_DIR), "--plugin-dir", str(FAKE_PLUGIN_DIR)],
        check=True, cwd=REPO, capture_output=True, text=True,
    )
    payload.PAYLOAD_DIR = RELEASE_DIR / "payload"


def tearDownModule():
    for d in (RELEASE_DIR, FAKE_PLUGIN_DIR):
        if d is not None:
            shutil.rmtree(d, ignore_errors=True)


class FakeAircraft:
    """A minimal fake <X-Plane>/Aircraft/<folder>/objects/ tree with an
    original (non-Photon) lights_out OBJ already in place, as if freshly
    installed from ToLiss."""

    def __init__(self, tmp: Path, airframe="a320", fname="lights_out320_XP12.obj",
                folder="ToLissA320_V1p3p2"):
        self.xplane_root = tmp / "X-Plane 12"
        self.folder = self.xplane_root / "Aircraft" / folder
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
        self.assertIn(f"ToLissPhoton version: {VERSION} wing: stock", installed)

        backup = self.fa.objects / "Photon Backup Files" / self.fa.obj_path.name
        self.assertEqual(backup.read_text(encoding="utf-8"), original)

    def test_manifest_roundtrip(self):
        self.assertIsNone(actions.read_manifest(self.fa.objects))
        actions.install(self.fa.ac, "durantula", self.fa.xplane_root, self.log)
        manifest = actions.read_manifest(self.fa.objects)
        self.assertEqual(manifest["version"], VERSION)
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

    def test_native_plugin_installed_for_all_arches(self):
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        plugin_root = (self.fa.xplane_root / "Resources" / "plugins" / "ToLissPhoton")
        for arch, data in FAKE_XPL.items():
            xpl = plugin_root / arch / "ToLissPhoton.xpl"
            self.assertTrue(xpl.is_file(), f"missing {arch} .xpl")
            self.assertEqual(xpl.read_bytes(), data)


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


class A339InstallTests(unittest.TestCase):
    """a339 (A330-900) has no wing mods — WINGS_FOR only ever offers "stock" in
    the installer UI — but install()/uninstall() themselves are airframe-generic,
    so a plain stock round trip should work exactly like the A3xx ones above."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_a339_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.fa = FakeAircraft(self.tmp, airframe="a339", fname="ExternalLights_XP12.obj",
                               folder="ToLissA339_V1p0p4")
        self.log = _NullLog()

    def test_install_writes_marker_and_backs_up_original(self):
        original = self.fa.obj_path.read_text(encoding="utf-8")
        steps = actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertTrue(any("Backed up" in s for s in steps))
        installed = self.fa.obj_path.read_text(encoding="utf-8")
        self.assertIn(f"ToLissPhoton version: {VERSION} wing: stock", installed)
        backup = self.fa.objects / "Photon Backup Files" / self.fa.obj_path.name
        self.assertEqual(backup.read_text(encoding="utf-8"), original)

    def test_uninstall_restores_byte_for_byte(self):
        original = self.fa.obj_path.read_text(encoding="utf-8")
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log)
        self.assertEqual(self.fa.obj_path.read_text(encoding="utf-8"), original)
        self.assertFalse((self.fa.objects / "Photon Backup Files").exists())

    # A realistic mesh-OBJ excerpt: two redirected glow regions (12,13) plus one
    # Photon does NOT drive (0) that must be left reading ToLiss's array.
    _OLD = "AirbusFBW/ExternalLightBrightnesses"
    _NEW = "ToLissPhoton/exterior/glow"
    _MESH = ("I\n800\nOBJ\n\nPOINT_COUNTS 1 0 0 3\n"
             "\tATTR_light_level\t0\t1\t" + _OLD + "[0]\t5000\n"
             "\tATTR_light_level\t0\t1\t" + _OLD + "[12]\t5000\n"
             "\tATTR_light_level\t0\t1\t" + _OLD + "[13]\t5000\n")

    def _write_mesh(self):
        p = self.fa.objects / "WingL.obj"
        p.write_text(self._MESH, encoding="utf-8")
        return p

    def test_install_redirects_skin_glow_and_uninstall_restores(self):
        mesh = self._write_mesh()
        original = mesh.read_text(encoding="utf-8")

        steps = actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertTrue(any("skin-glow" in s for s in steps))

        patched = mesh.read_text(encoding="utf-8")
        # driven indices repointed at our dataref; the undriven one untouched
        self.assertIn(f"{self._NEW}[12]", patched)
        self.assertIn(f"{self._NEW}[13]", patched)
        self.assertIn(f"{self._OLD}[0]", patched)
        self.assertNotIn(f"{self._NEW}[0]", patched)

        # the mesh OBJ is backed up (in the manifest + on disk) as the true original
        manifest = actions.read_manifest(self.fa.objects)
        self.assertIn("WingL.obj", manifest["backed_up"])
        backup = self.fa.objects / "Photon Backup Files" / "WingL.obj"
        self.assertEqual(backup.read_text(encoding="utf-8"), original)

        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log)
        self.assertEqual(mesh.read_text(encoding="utf-8"), original)

    def test_reinstall_keeps_true_mesh_backup(self):
        mesh = self._write_mesh()
        original = mesh.read_text(encoding="utf-8")
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        # reinstall over the already-redirected mesh: patch is idempotent, backup
        # must stay the pristine ToLiss original (not the redirected file)
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        backup = self.fa.objects / "Photon Backup Files" / "WingL.obj"
        self.assertEqual(backup.read_text(encoding="utf-8"), original)

    def test_durantula_wing_unavailable_raises_payload_error(self):
        """a339 has no wing mods — the installer UI never offers this
        combination (WINGS_FOR), but calling install() directly with an
        unsupported wing must still fail loudly rather than silently shipping
        a stock OBJ under the durantula label. Locks in make_release.py's
        SUPPORTED_WINGS gate: only objs/stock/ExternalLights_XP12.obj was
        ever built into the payload for this airframe."""
        with self.assertRaises(payload.PayloadError):
            actions.install(self.fa.ac, "durantula", self.fa.xplane_root, self.log)


class PluginCurrentTests(unittest.TestCase):
    """plugin_is_current gates whether the X-Plane-running warning is skipped:
    if the installed plugin already matches, an install only rewrites OBJs
    (safe while loaded)."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_plugincur_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.fa = FakeAircraft(self.tmp)
        self.log = _NullLog()
        self.plugin_root = (self.fa.xplane_root / "Resources" / "plugins" / "ToLissPhoton")

    def test_false_when_plugin_absent(self):
        self.assertFalse(actions.plugin_is_current(self.fa.xplane_root))

    def test_true_after_install(self):
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertTrue(actions.plugin_is_current(self.fa.xplane_root))

    def test_false_when_installed_plugin_differs(self):
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        # one arch's .xpl replaced by an older build => no longer current
        (self.plugin_root / "win_x64" / "ToLissPhoton.xpl").write_bytes(b"# older build\n")
        self.assertFalse(actions.plugin_is_current(self.fa.xplane_root))


class PluginRewriteSkipTests(unittest.TestCase):
    """A reinstall must NOT rewrite an already-identical plugin `.xpl`. X-Plane
    keeps a loaded `.xpl` memory-mapped and locked, so re-replacing an unchanged
    one fails with 'file in use' on a running sim for no benefit. Regression:
    install() used to overwrite it unconditionally, so an OBJ-only reinstall
    crashed on a running sim even though the guard had (correctly) skipped the
    'please close X-Plane' wait — the reported 'installer just failed: file in
    use' bug."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_pluginskip_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.fa = FakeAircraft(self.tmp)
        self.log = _NullLog()
        self.xpl = (self.fa.xplane_root / "Resources" / "plugins" / "ToLissPhoton"
                    / "win_x64" / "ToLissPhoton.xpl")

    def test_fresh_install_writes_plugin(self):
        steps = actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertTrue(any("Installed ToLiss Photon plugin" in s for s in steps))

    def test_reinstall_does_not_rewrite_identical_plugin(self):
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        # Stamp a known-old mtime; if the 2nd install re-replaces the file, the
        # atomic os.replace hands it a fresh (now) mtime instead of preserving this.
        os.utime(self.xpl, (1_000_000_000, 1_000_000_000))
        before = self.xpl.stat().st_mtime_ns
        steps = actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertEqual(self.xpl.stat().st_mtime_ns, before)  # never touched
        self.assertTrue(any("already current" in s for s in steps))
        self.assertFalse(any("Installed ToLiss Photon plugin" in s for s in steps))

    def test_reinstall_rewrites_a_differing_plugin(self):
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.xpl.write_bytes(b"# stale older build\n")   # now differs from payload
        steps = actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertTrue(any("Installed ToLiss Photon plugin" in s for s in steps))
        self.assertEqual(self.xpl.read_bytes(), FAKE_XPL["win_x64"])  # corrected


class OverlayFolderTests(unittest.TestCase):
    """`Resources/plugins/ToLissPhoton/overlays/` (PLUGIN_USER_DIRS) is BOTH the
    Panel FX compositor's shipped starter images and the user's own drop folder.

    Since the compositor ships, install writes our images into it — but by NAME,
    which is what keeps the original contract intact in the same folder: a user's
    hand-made PNG has no backup and no stock counterpart, so install must not
    overwrite it and uninstall must not take it out with the plugin folder."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_overlay_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.fa = FakeAircraft(self.tmp)
        self.log = _NullLog()
        self.plugin_root = (self.fa.xplane_root / "Resources" / "plugins" / "ToLissPhoton")
        self.overlay = self.plugin_root / "overlays" / "my_own_sheen.png"

    def _seed_overlay(self):
        """What deploy.ps1 (or the user's paint program) leaves behind."""
        self.overlay.parent.mkdir(parents=True, exist_ok=True)
        self.overlay.write_bytes(b"\x89PNG\r\n\x1a\n fake user overlay\n")

    def test_install_writes_the_shipped_overlays_and_panelfx(self):
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        for rel, src in payload.plugin_data_files():
            dest = self.plugin_root / rel
            self.assertTrue(dest.is_file(), f"{rel} not installed")
            self.assertEqual(dest.read_bytes(), src.read_bytes(), rel)

    def test_install_leaves_an_existing_overlay_untouched(self):
        self._seed_overlay()
        before = self.overlay.read_bytes()
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertEqual(self.overlay.read_bytes(), before)

    def test_uninstall_removing_plugin_keeps_the_overlays_folder(self):
        self._seed_overlay()
        before = self.overlay.read_bytes()
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        steps = actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log,
                                  remove_plugin=True)
        # Photon's own files are gone...
        for arch in FAKE_XPL:
            self.assertFalse((self.plugin_root / arch).exists(), f"{arch} not removed")
        # ...the user's images are not, and the step list says so.
        self.assertEqual(self.overlay.read_bytes(), before)
        self.assertTrue(any("overlays" in s for s in steps),
                        f"uninstall did not report the kept folder: {steps}")

    def test_uninstall_removes_the_whole_folder_when_no_overlays(self):
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log, remove_plugin=True)
        self.assertFalse(self.plugin_root.exists())

    def test_uninstall_removes_the_folder_when_overlays_is_empty(self):
        # An empty leftover directory is not user content worth stranding.
        (self.plugin_root / "overlays").mkdir(parents=True)
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log, remove_plugin=True)
        self.assertFalse(self.plugin_root.exists())

    def test_dry_run_uninstall_removes_nothing(self):
        self._seed_overlay()
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log, dry_run=True,
                          remove_plugin=True)
        self.assertTrue(self.overlay.is_file())
        self.assertTrue((self.plugin_root / "win_x64" / "ToLissPhoton.xpl").is_file())


class PanelFxDataTests(unittest.TestCase):
    """`panelfx.txt` in the plugin folder — the authored Panel FX look.

    ⚠ A DEV MACHINE SYMLINKS IT AT THE REPO COPY so that in-sim edits from a
    PHOTON_DEV build land in source control. A test install that replaced the link
    with a plain file would end that round trip silently: every later edit would
    be written to a file nobody reads, and the repo copy would look like it had
    simply stopped changing."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_fxdata_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.fa = FakeAircraft(self.tmp)
        self.log = _NullLog()
        self.plugin_root = self.fa.xplane_root / "Resources" / "plugins" / "ToLissPhoton"
        self.fx = self.plugin_root / "panelfx.txt"

    def _symlink_fx(self) -> Path:
        """Point panelfx.txt at a stand-in for the repo copy. Skips where the OS
        refuses — Windows needs Developer Mode or elevation for a file symlink,
        and the behavior under test is real either way."""
        target = self.tmp / "repo_panelfx.txt"
        target.write_text("# the dev's own copy\nenabled 1\n", encoding="utf-8")
        self.fx.parent.mkdir(parents=True, exist_ok=True)
        try:
            self.fx.symlink_to(target)
        except (OSError, NotImplementedError) as e:
            self.skipTest(f"cannot create a symlink here: {e}")
        return target

    def test_install_writes_panelfx_into_the_plugin_folder(self):
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertTrue(self.fx.is_file())
        self.assertIn("layer ", self.fx.read_text(encoding="utf-8"))

    def test_install_leaves_a_symlinked_panelfx_alone(self):
        target = self._symlink_fx()
        before = target.read_text(encoding="utf-8")
        steps = actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        self.assertTrue(self.fx.is_symlink(), "the symlink was replaced by a file")
        self.assertEqual(target.read_text(encoding="utf-8"), before,
                         "the install wrote through the link into the repo copy")
        self.assertTrue(any("symlink" in s for s in steps),
                        f"the install did not report the kept symlink: {steps}")

    def test_uninstall_leaves_a_symlinked_panelfx_alone(self):
        target = self._symlink_fx()
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log,
                          remove_plugin=True)
        self.assertTrue(target.is_file(), "uninstall deleted the repo copy")

    def test_uninstall_removes_our_own_panelfx(self):
        actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log)
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log,
                          remove_plugin=True)
        self.assertFalse(self.plugin_root.exists())


class PluginPayloadExclusionTests(unittest.TestCase):
    """The other direction: a source plugin folder that HOLDS overlays must not
    push them into an install or a release bundle. `--plugin-dir` pointed at a
    deployed plugin folder is a legitimate way to assemble a bundle, and on a dev
    machine that folder has deploy.ps1's starter images in it."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_plugexcl_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        # A plugin source folder as deploy.ps1 leaves it: arches + overlays/.
        self.src = self.tmp / "ToLissPhoton"
        _make_fake_plugin(self.src)
        (self.src / "overlays").mkdir()
        (self.src / "overlays" / "scanlines.png").write_bytes(b"\x89PNG fake\n")

    def test_plugin_files_skips_user_dirs(self):
        saved = payload.PAYLOAD_DIR
        payload.PAYLOAD_DIR = self.tmp / "payload"
        (payload.PAYLOAD_DIR / "plugin").mkdir(parents=True)
        shutil.copytree(self.src, payload.PAYLOAD_DIR / "plugin" / "ToLissPhoton")
        self.addCleanup(setattr, payload, "PAYLOAD_DIR", saved)

        rels = [rel for rel, _ in payload.plugin_files()]
        self.assertIn("win_x64/ToLissPhoton.xpl", rels)
        self.assertNotIn("overlays/scanlines.png", rels)
        self.assertFalse(any(r.startswith("overlays/") for r in rels), rels)

    def test_stage_plugin_skips_user_dirs(self):
        sys.path.insert(0, str(REPO / "build"))
        import make_release

        out = self.tmp / "bundle"
        (out / "plugin").mkdir(parents=True)
        make_release.stage_plugin(out, self.src)
        staged = out / "plugin" / "ToLissPhoton"
        self.assertTrue((staged / "win_x64" / "ToLissPhoton.xpl").is_file())
        self.assertFalse((staged / "overlays").exists())


if __name__ == "__main__":
    unittest.main()
