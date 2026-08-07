"""Interior ("Gus Mod") install / uninstall through installer/actions.py.

The acceptance criterion for docs/interior_plan.md Phase V: install then
uninstall must restore the aircraft byte-for-byte, `.acf` included. That is
harder than it sounds because three of the five things the interior install
touches CANNOT be undone by restoring a backup:

  * two textures have no stock counterpart, so they must be DELETED;
  * livery `.dds` overrides are removed from a different folder entirely;
  * the `.acf` must be reverted by writing stock values back, not by restoring a
    snapshot, so other mods' edits to the same file survive.

A real release payload is built once for the module, as test_actions.py does, so
this also checks make_release.py still stages the interior OBJ and textures.
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
sys.path.insert(0, str(REPO / "build"))

from installer import actions, detect, payload  # noqa: E402
from installer.constants import (  # noqa: E402
    INTERIOR_OBJ, INTERIOR_TEXTURES, INTERIOR_TEXTURES_ADDED, VERSION,
)

RELEASE_DIR: Path | None = None
FAKE_PLUGIN_DIR: Path | None = None

STOCK_ACF = """\
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
# XP11 renders the same values differently — the format trap, exercised here at
# the install level too, not just in test_patch_acf.py.
STOCK_ACF_XP11 = (STOCK_ACF
                  .replace("0.000000000", "0.0").replace("1.000000000", "1.0")
                  .replace("17.000000000", "17.0").replace("14.000000000", "14.0")
                  .replace("-20.000000000", "-20.0").replace("20.000000000", "20.0")
                  .replace("-70.000000000", "-70.0").replace("-66.000000000", "-66.0")
                  .replace("-53.000000000", "-53.0"))


class _NullLog:
    def write(self, msg, level="INFO"):
        pass


def setUpModule():
    global RELEASE_DIR, FAKE_PLUGIN_DIR
    RELEASE_DIR = Path(tempfile.mkdtemp(prefix="photon_release_int_"))
    FAKE_PLUGIN_DIR = Path(tempfile.mkdtemp(prefix="photon_plugin_int_"))
    for arch in ("win_x64", "mac_x64", "lin_x64"):
        p = FAKE_PLUGIN_DIR / arch / "ToLissPhoton.xpl"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"fake " + arch.encode())
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
    """A fake ToLiss A320 tree complete enough for the interior install: the
    exterior OBJ, a stock lights_inn.obj, stock copies of the nine textures the
    mod replaces, four .acf files (two XP12, two XP11), and a livery carrying a
    .dds that would shadow one of our PNGs."""

    def __init__(self, tmp: Path):
        self.xplane_root = tmp / "X-Plane 12"
        self.folder = self.xplane_root / "Aircraft" / "ToLissA320_V1p3p2"
        self.objects = self.folder / "objects"
        self.objects.mkdir(parents=True)
        self.obj_path = self.objects / "lights_out320_XP12.obj"
        self.obj_path.write_text(
            "I\n800\nOBJ\n\nGLOBAL_cockpit_lit\nPOINT_COUNTS\t0\t0\t0\t0\n"
            "# ORIGINAL STOCK CONTENT\n", encoding="utf-8")
        self.inn_path = self.objects / INTERIOR_OBJ
        self.inn_path.write_text(
            "I\n800\nOBJ\n\nTEXTURE\t\nPOINT_COUNTS\t0 0 1 0\n"
            "# ORIGINAL STOCK INTERIOR\n", encoding="utf-8")

        # stock textures — only the nine that have a stock counterpart
        self.stock_textures = {}
        for name in INTERIOR_TEXTURES:
            if name in INTERIOR_TEXTURES_ADDED:
                continue
            data = f"STOCK {name}".encode()
            (self.objects / name).write_bytes(data)
            self.stock_textures[name] = data

        # four .acf, in both numeric formats
        self.acfs = {}
        for name, text in (("a320.acf", STOCK_ACF),
                           ("a320_StdDef.acf", STOCK_ACF),
                           ("a320_XP11.acf", STOCK_ACF_XP11),
                           ("a320_XP11_StdDef.acf", STOCK_ACF_XP11)):
            (self.folder / name).write_text(text, encoding="utf-8")
            self.acfs[name] = text
        # a .bak another mod left behind — must never be patched
        (self.folder / "a320.acf.durantula.bak").write_text(STOCK_ACF, encoding="utf-8")

        # a livery whose .dds shadows our chairs_LIT.png
        self.livery_dds = (self.folder / "liveries" / "JV - Frontier N369FR"
                           / "objects" / "chairs_LIT.dds")
        self.livery_dds.parent.mkdir(parents=True)
        self.livery_dds.write_bytes(b"LIVERY DDS OVERRIDE")

        self.ac = dict(folder=self.folder.name, airframe="a320", ac_ver="1.4.1",
                       name="ToLiss A320", photon=None, wing=None,
                       path=str(self.folder))

    def snapshot(self) -> dict[str, bytes]:
        """Every file in the aircraft folder, for a byte-for-byte comparison."""
        return {p.relative_to(self.folder).as_posix(): p.read_bytes()
                for p in sorted(self.folder.rglob("*")) if p.is_file()}


class InteriorInstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_int_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.fa = FakeAircraft(self.tmp)
        self.log = _NullLog()

    def _install(self, interior=True):
        return actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log,
                               interior=interior)

    # ── opt-in ────────────────────────────────────────────────────────────────
    def test_exterior_only_install_leaves_the_interior_completely_alone(self):
        """§5.1 — a user who wants only the exterior mod must be able to decline,
        and declining must touch nothing interior at all."""
        before = self.fa.snapshot()
        self._install(interior=False)
        after = self.fa.snapshot()
        self.assertEqual(after[f"objects/{INTERIOR_OBJ}"],
                         before[f"objects/{INTERIOR_OBJ}"])
        for name in self.fa.stock_textures:
            self.assertEqual(after[f"objects/{name}"], before[f"objects/{name}"])
        for name, text in self.fa.acfs.items():
            self.assertEqual((self.fa.folder / name).read_text(encoding="utf-8"), text)
        self.assertTrue(self.fa.livery_dds.is_file())
        manifest = actions.read_manifest(self.fa.objects)
        self.assertFalse((manifest.get("interior") or {}).get("installed"))

    # ── the five parts ────────────────────────────────────────────────────────
    def test_installs_the_interior_obj_with_a_marker(self):
        self._install()
        text = self.fa.inn_path.read_text(encoding="utf-8")
        self.assertIn(f"ToLissPhoton version: {VERSION} wing: interior", text)
        self.assertIn("ToLissPhoton/interior/dome", text)
        self.assertIn("GUS", text)

    def test_installs_every_texture(self):
        self._install()
        for name in INTERIOR_TEXTURES:
            with self.subTest(texture=name):
                p = self.objects_file(name)
                self.assertTrue(p.is_file())
                self.assertNotEqual(p.read_bytes(), self.fa.stock_textures.get(name))

    def objects_file(self, name):
        return self.fa.objects / name

    def test_patches_all_four_acf_in_both_formats(self):
        from installer import patch_acf as P
        self._install()
        for name in self.fa.acfs:
            with self.subTest(acf=name):
                text = (self.fa.folder / name).read_text(encoding="utf-8")
                self.assertTrue(P.is_patched(text))
                self.assertEqual(P.verify_geometry(text), [])

    def test_never_patches_a_bak_file(self):
        """Durantula keeps an a320.acf.durantula.bak of this very file; patching
        it would corrupt Durantula's own uninstall."""
        from installer import patch_acf as P
        self._install()
        bak = (self.fa.folder / "a320.acf.durantula.bak").read_text(encoding="utf-8")
        self.assertEqual(bak, STOCK_ACF)
        self.assertFalse(P.is_patched(bak))

    def test_removes_livery_dds_that_would_shadow_our_texture(self):
        """§1.5 — X-Plane substitutes .dds for the .png an OBJ names, so a
        livery's chairs_LIT.dds beats our chairs_LIT.png and the mod is invisible
        in that livery. Dropping our PNG in the livery folder does nothing."""
        self._install()
        self.assertFalse(self.fa.livery_dds.is_file())
        manifest = actions.read_manifest(self.fa.objects)
        self.assertEqual(manifest["interior"]["liveries_patched"],
                         ["liveries/JV - Frontier N369FR/objects/chairs_LIT.dds"])

    def test_manifest_records_both_axes_independently(self):
        self._install()
        manifest = actions.read_manifest(self.fa.objects)
        self.assertEqual(manifest["version"], VERSION)      # exterior axis
        self.assertEqual(manifest["wing"], "stock")
        interior = manifest["interior"]                      # interior axis
        self.assertTrue(interior["installed"])
        self.assertEqual(sorted(interior["added"]), sorted(INTERIOR_TEXTURES_ADDED))
        self.assertEqual(len(interior["acf_patched"]), 4)

    def test_added_textures_are_tracked_separately_from_backups(self):
        """pedals_details_{1,2}.png have no stock original, so they can't be
        'restored' — they are recorded in `added` to be DELETED on uninstall."""
        self._install()
        manifest = actions.read_manifest(self.fa.objects)
        backed_up = manifest["backed_up"]
        for name in INTERIOR_TEXTURES_ADDED:
            self.assertNotIn(name, backed_up)
            self.assertIn(name, manifest["interior"]["added"])

    # ── the round trip ────────────────────────────────────────────────────────
    def test_install_then_uninstall_restores_the_aircraft_byte_for_byte(self):
        """THE Phase VI acceptance criterion, .acf and liveries included."""
        before = self.fa.snapshot()
        self._install()
        self.assertNotEqual(self.fa.snapshot(), before)
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log)
        self.assertEqual(self.fa.snapshot(), before)

    def test_uninstall_deletes_the_added_textures(self):
        self._install()
        for name in INTERIOR_TEXTURES_ADDED:
            self.assertTrue((self.fa.objects / name).is_file())
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log)
        for name in INTERIOR_TEXTURES_ADDED:
            with self.subTest(texture=name):
                self.assertFalse((self.fa.objects / name).exists(),
                                 "a file with no stock original must be deleted, "
                                 "not left orphaned")

    def test_uninstall_restores_the_livery_override(self):
        self._install()
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log)
        self.assertTrue(self.fa.livery_dds.is_file())
        self.assertEqual(self.fa.livery_dds.read_bytes(), b"LIVERY DDS OVERRIDE")

    def test_uninstall_reverts_the_acf_by_writing_stock_values_not_a_snapshot(self):
        """Reversal must compose with other mods' edits to the same file — so an
        unrelated line added AFTER our install must survive the uninstall."""
        self._install()
        target = self.fa.folder / "a320.acf"
        text = target.read_text(encoding="utf-8")
        target.write_text(text + "P _obja/11/_v10_att_z_acf_prt_ref 20.750000000\n",
                          encoding="utf-8")
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log)
        after = target.read_text(encoding="utf-8")
        self.assertIn("P acf/_spot_the/0 -70.000000000", after)
        self.assertIn("P acf/_spot_name_3d/2 ckpt/lights/map", after)
        self.assertIn("P _obja/11/_v10_att_z_acf_prt_ref 20.750000000", after,
                      "another mod's edit must survive our uninstall")

    def test_reinstall_keeps_the_true_original_backups(self):
        original = self.fa.inn_path.read_text(encoding="utf-8")
        self._install()
        self._install()
        backup = self.fa.objects / "Photon Backup Files" / INTERIOR_OBJ
        self.assertEqual(backup.read_text(encoding="utf-8"), original)

    def test_reinstall_then_uninstall_still_round_trips(self):
        before = self.fa.snapshot()
        self._install()
        self._install()
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log)
        self.assertEqual(self.fa.snapshot(), before)

    # ── status reporting ──────────────────────────────────────────────────────
    def test_interior_status_reports_installed(self):
        self.assertEqual(detect.interior_status(self.fa.objects), (None, False, False))
        self._install()
        ver, installed, stale = detect.interior_status(self.fa.objects)
        self.assertEqual(ver, VERSION)
        self.assertTrue(installed)
        self.assertFalse(stale)

    def test_interior_status_reports_stale_when_an_update_reverts_the_obj(self):
        """A SkunkCrafts update can revert lights_inn.obj while leaving our
        manifest in Photon Backup Files/ untouched."""
        self._install()
        self.fa.inn_path.write_text("I\n800\nOBJ\n\n# reverted by an update\n",
                                    encoding="utf-8")
        ver, installed, stale = detect.interior_status(self.fa.objects)
        self.assertTrue(installed)
        self.assertTrue(stale)

    # ── airframe scope ────────────────────────────────────────────────────────
    def test_a339_never_gets_the_interior_mod(self):
        """Decision #14 — out of scope, and asking for it must be a no-op rather
        than shipping A320 cockpit lights into an A330-900."""
        ac = dict(self.fa.ac, airframe="a339")
        steps = actions.install(ac, "stock", self.fa.xplane_root, self.log,
                                interior=True)
        self.assertTrue(any("not available" in s for s in steps))
        manifest = actions.read_manifest(self.fa.objects)
        self.assertFalse((manifest.get("interior") or {}).get("installed"))


if __name__ == "__main__":
    unittest.main()
