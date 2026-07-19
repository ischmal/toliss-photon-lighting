"""Unit tests for installer/detect.py — pure filesystem probing against fake
trees, no terminal involved. Run: python -m unittest discover -s tests -v
(no pytest in this repo; stdlib unittest only, matching the installer's own
zero-third-party-dependency policy)."""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from installer import detect


class ValidRootTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_root_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_rejects_missing_dir(self):
        self.assertFalse(detect._valid_root(self.tmp / "nope"))

    def test_rejects_dir_missing_aircraft(self):
        (self.tmp / "Resources").mkdir()
        self.assertFalse(detect._valid_root(self.tmp))

    def test_accepts_valid_root(self):
        (self.tmp / "Aircraft").mkdir()
        (self.tmp / "Resources").mkdir()
        self.assertTrue(detect._valid_root(self.tmp))


class SkunkCfgTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_cfg_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_parses_pipe_delimited_cfg(self):
        (self.tmp / "skunkcrafts_updater.cfg").write_text(
            "name|ToLiss A319\nversion|v1.12\nother|ignored\n", encoding="utf-8")
        cfg = detect._read_skunk_cfg(self.tmp)
        self.assertEqual(cfg["name"], "ToLiss A319")
        self.assertEqual(cfg["version"], "1.12")  # leading v stripped

    def test_missing_cfg_returns_empty(self):
        self.assertEqual(detect._read_skunk_cfg(self.tmp), {})


class AircraftScanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_scan_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.xp = self.tmp / "X-Plane 12"
        self.ac = self.xp / "Aircraft" / "ToLissA320_V1p3p2"
        self.objects = self.ac / "objects"
        self.objects.mkdir(parents=True)
        (self.ac / "skunkcrafts_updater.cfg").write_text(
            "version|v1.4.1\nname|ToLiss A320\n", encoding="utf-8")

    def test_detects_not_installed(self):
        (self.objects / "lights_out320_XP12.obj").write_text("stock", encoding="utf-8")
        found = detect.detect_aircraft(self.xp)
        self.assertEqual(len(found), 1)
        self.assertIsNone(found[0]["photon"])
        self.assertEqual(found[0]["ac_ver"], "1.4.1")
        self.assertEqual(found[0]["name"], "ToLiss A320")

    def test_detects_installed_via_marker_fallback(self):
        (self.objects / "lights_out320_XP12.obj").write_text(
            "I\n800\nOBJ\n\n# ToLissPhoton version: 0.3 wing: durantula\n",
            encoding="utf-8")
        found = detect.detect_aircraft(self.xp)
        self.assertEqual(found[0]["photon"], "0.3")
        self.assertEqual(found[0]["wing"], "durantula")

    def test_manifest_takes_priority_over_marker(self):
        (self.objects / "lights_out320_XP12.obj").write_text(
            "# ToLissPhoton version: 0.3 wing: durantula\n", encoding="utf-8")
        backup_dir = self.objects / "Photon Backup Files"
        backup_dir.mkdir()
        (backup_dir / "photon_manifest.json").write_text(
            json.dumps({"version": "0.4", "wing": "stock", "backed_up": []}),
            encoding="utf-8")
        found = detect.detect_aircraft(self.xp)
        self.assertEqual(found[0]["photon"], "0.4")
        self.assertEqual(found[0]["wing"], "stock")
        self.assertFalse(found[0]["stale"])

    def test_stale_when_manifest_present_but_obj_reverted(self):
        # An aircraft update reverted the OBJ to stock (no marker) while the
        # manifest survived in Photon Backup Files/. Should read as stale.
        (self.objects / "lights_out320_XP12.obj").write_text(
            "I\n800\nOBJ\n\nstock geometry, no marker\n", encoding="utf-8")
        backup_dir = self.objects / "Photon Backup Files"
        backup_dir.mkdir()
        (backup_dir / "photon_manifest.json").write_text(
            json.dumps({"version": "0.4", "wing": "stock", "backed_up": []}),
            encoding="utf-8")
        found = detect.detect_aircraft(self.xp)
        self.assertEqual(found[0]["photon"], "0.4")
        self.assertTrue(found[0]["stale"])

    def test_not_stale_when_obj_marker_present(self):
        (self.objects / "lights_out320_XP12.obj").write_text(
            "I\n800\nOBJ\n\n# ToLissPhoton version: 0.4 wing: stock\n",
            encoding="utf-8")
        backup_dir = self.objects / "Photon Backup Files"
        backup_dir.mkdir()
        (backup_dir / "photon_manifest.json").write_text(
            json.dumps({"version": "0.4", "wing": "stock", "backed_up": []}),
            encoding="utf-8")
        found = detect.detect_aircraft(self.xp)
        self.assertFalse(found[0]["stale"])


class OtherAirframeCheckTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_other_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.xp = self.tmp / "X-Plane 12"

    def _make(self, folder_name, marker=None):
        ac = self.xp / "Aircraft" / folder_name
        objects = ac / "objects"
        objects.mkdir(parents=True)
        text = "I\n800\nOBJ\n\n"
        if marker:
            text += f"# ToLissPhoton version: {marker} wing: stock\n"
        (objects / "lights_out320_XP12.obj").write_text(text, encoding="utf-8")

    def test_true_when_another_airframe_has_photon(self):
        self._make("ToLissA320_V1p3p2", marker=None)
        self._make("ToLissA321_V1p7p1", marker="0.4")
        self.assertTrue(detect.any_other_airframe_has_photon(
            self.xp, "ToLissA320_V1p3p2"))

    def test_false_when_no_other_airframe_has_photon(self):
        self._make("ToLissA320_V1p3p2", marker="0.4")
        self._make("ToLissA321_V1p7p1", marker=None)
        self.assertFalse(detect.any_other_airframe_has_photon(
            self.xp, "ToLissA320_V1p3p2"))


if __name__ == "__main__":
    unittest.main()
