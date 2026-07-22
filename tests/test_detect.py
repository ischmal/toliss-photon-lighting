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


class A339ScanTests(unittest.TestCase):
    """a339 (A330-900) uses a different OBJ filename (ExternalLights_XP12.obj,
    not lights_out*) — this locks in that detect_aircraft/_obj_marker work off
    the AIRFRAMES table rather than any lights_out-shaped assumption."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_a339_scan_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.xp = self.tmp / "X-Plane 12"
        self.ac = self.xp / "Aircraft" / "ToLissA339_V1p0p4"
        self.objects = self.ac / "objects"
        self.objects.mkdir(parents=True)
        (self.ac / "skunkcrafts_updater.cfg").write_text(
            "version|v1.0.4\nname|ToLiss A330-900\n", encoding="utf-8")

    def test_detects_not_installed(self):
        (self.objects / "ExternalLights_XP12.obj").write_text("stock", encoding="utf-8")
        found = detect.detect_aircraft(self.xp)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["airframe"], "a339")
        self.assertIsNone(found[0]["photon"])
        self.assertEqual(found[0]["ac_ver"], "1.0.4")

    def test_detects_installed_via_marker(self):
        (self.objects / "ExternalLights_XP12.obj").write_text(
            "I\n800\nOBJ\n\n# ToLissPhoton version: 0.4 wing: stock\n",
            encoding="utf-8")
        found = detect.detect_aircraft(self.xp)
        self.assertEqual(found[0]["photon"], "0.4")
        self.assertEqual(found[0]["wing"], "stock")
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

    def _path(self, folder_name):
        return str(self.xp / "Aircraft" / folder_name)

    def test_true_when_another_airframe_has_photon(self):
        self._make("ToLissA320_V1p3p2", marker=None)
        self._make("ToLissA321_V1p7p1", marker="0.4")
        self.assertTrue(detect.any_other_airframe_has_photon(
            self.xp, self._path("ToLissA320_V1p3p2")))

    def test_false_when_no_other_airframe_has_photon(self):
        self._make("ToLissA320_V1p3p2", marker="0.4")
        self._make("ToLissA321_V1p7p1", marker=None)
        self.assertFalse(detect.any_other_airframe_has_photon(
            self.xp, self._path("ToLissA320_V1p3p2")))


class NestedAndRenamedScanTests(unittest.TestCase):
    """The scan is content-based (skunkcrafts_updater.cfg / OBJ filename), not
    folder-name/location based, so an install works when it's renamed or dropped
    in a sub-hangar (user feedback: 'installer doesn't recognise the folder if
    named anything other than ToLissA3xx_Vypy, and not if it isn't directly under
    X-Plane 12/Aircraft')."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_nested_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.xp = self.tmp / "X-Plane 12"

    def _make(self, rel, *, cfg=None, fname="lights_out320_XP12.obj", marker=None,
              acf=True):
        ac = self.xp / "Aircraft" / rel
        objects = ac / "objects"
        objects.mkdir(parents=True)
        if acf:  # every real aircraft folder carries a .acf at its root
            (ac / (ac.name.replace(" ", "_") + ".acf")).write_text("ACF", encoding="utf-8")
        if cfg is not None:
            (ac / "skunkcrafts_updater.cfg").write_text(cfg, encoding="utf-8")
        text = "I\n800\nOBJ\n\n"
        if marker:
            text += f"# ToLissPhoton version: {marker} wing: stock\n"
        (objects / fname).write_text(text, encoding="utf-8")
        return ac

    def test_detects_aircraft_in_sub_hangar(self):
        self._make("My Hangar/ToLissA339_V1p0p4",
                   cfg="version|v1.1\nname|ToLiss A330-900\n"
                       "module|http://updates.toliss.com/aircraft-repositories/A330-900/\n",
                   fname="ExternalLights_XP12.obj")
        found = detect.detect_aircraft(self.xp)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["airframe"], "a339")
        # label carries the sub-hangar path so it's legible / unambiguous
        self.assertEqual(found[0]["folder"], str(Path("My Hangar") / "ToLissA339_V1p0p4"))
        self.assertEqual(found[0]["path"], str(self.xp / "Aircraft" / "My Hangar" / "ToLissA339_V1p0p4"))

    def test_detects_renamed_folder_via_cfg_module(self):
        # folder name matches no glob; identified purely from the cfg module URL
        self._make("A320 (my tweaks)",
                   cfg="version|v1.4\nname|Whatever Custom Name\n"
                       "module|http://updates.toliss.com/aircraft-repositories/A320/\n")
        found = detect.detect_aircraft(self.xp)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["airframe"], "a320")

    def test_detects_renamed_folder_via_cfg_name(self):
        # no module line; the display name carries the airframe token
        self._make("weird-name",
                   cfg="version|v1.9\nname|ToLiss A321\n",
                   fname="lights_out321_XP12.obj")
        found = detect.detect_aircraft(self.xp)
        self.assertEqual(found[0]["airframe"], "a321")

    def test_detects_renamed_folder_via_obj_when_no_cfg(self):
        # no cfg at all, non-standard name — identified by the intrinsic OBJ name
        self._make("Frankenplane", cfg=None, fname="ExternalLights_XP12.obj")
        found = detect.detect_aircraft(self.xp)
        self.assertEqual(found[0]["airframe"], "a339")

    def test_skips_non_toliss_skunkcrafts_aircraft(self):
        # some other developer's SkunkCrafts aircraft: has the cfg, but nothing
        # marks it as a ToLiss A3xx -> not one of ours, skip it
        ac = self.xp / "Aircraft" / "SomeOtherJet"
        (ac / "objects").mkdir(parents=True)
        (ac / "skunkcrafts_updater.cfg").write_text(
            "version|v2.0\nname|Acme 737\n"
            "module|http://example.com/aircraft-repositories/B737/\n", encoding="utf-8")
        self.assertEqual(detect.detect_aircraft(self.xp), [])

    def test_does_not_redetect_aircraft_subfolders(self):
        # exactly one hit even though objects/ (and its Photon Backup Files) sit
        # under the detected aircraft — the walk prunes into it
        self._make("ToLissA320_V1p3p2",
                   cfg="version|v1.3.2\nname|ToLiss A320\n")
        found = detect.detect_aircraft(self.xp)
        self.assertEqual(len(found), 1)


if __name__ == "__main__":
    unittest.main()
