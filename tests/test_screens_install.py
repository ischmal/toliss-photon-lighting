"""Display glow (`lights_screens.obj`) install / uninstall through installer/actions.py.

The screen glow is the only thing Photon installs that has NO stock counterpart
and is NOT referenced by a stock ToLiss aircraft. Both halves of that are load
bearing and both fail quietly:

  * the OBJ must be recorded in `screens.added[]` and DELETED on uninstall — a
    backup restore would only log "nothing to back up" and leave it orphaned in
    the aircraft folder forever;
  * the `.acf` attachment is what makes it draw AT ALL, so an install that wrote
    the file but failed to attach it is not a dim glow, it is no glow — and the
    reverse must put the `_obja` table back exactly as it was, because that table
    is Durantula's namespace too.

A real release payload is built once for the module, as test_actions.py does, so
this also checks make_release.py stages the screen-glow OBJ and that
patch_acf_screens.py rides along in the bundled toolchain.
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

from installer import actions, payload  # noqa: E402
from installer.constants import SCREENS_OBJ, VERSION  # noqa: E402

RELEASE_DIR: Path | None = None
FAKE_PLUGIN_DIR: Path | None = None

# A .acf with an attachment table that HAS a lights_inn.obj row — the frame the
# patcher copies. XP12 float style; the XP11 style is covered in test_screens.py.
ACF_ROWS = [
    ("fuselage.obj", 0),
    ("wingL.obj", 1),
    ("lights_inn.obj", 2),
]


def make_acf(eol="\r\n") -> str:
    lines = ["I", "1 version", "ACF"]
    for name, i in ACF_ROWS:
        lines += [
            f"P _obja/{i}/_obj_flags 5",
            f"P _obja/{i}/_v10_att_file_stl {name}",
            f"P _obja/{i}/_v10_att_x_acf_prt_ref 0.000000000",
            f"P _obja/{i}/_v10_att_y_acf_prt_ref 0.400000006",
            f"P _obja/{i}/_v10_att_z_acf_prt_ref 20.750000000",
            f"P _obja/{i}/_v10_is_internal 6",
        ]
    lines += [f"P _obja/count {len(ACF_ROWS)}", "P acf/_tailnum ABCDE"]
    return eol.join(lines) + eol


class _NullLog:
    def write(self, msg, level="INFO"):
        pass


def setUpModule():
    global RELEASE_DIR, FAKE_PLUGIN_DIR
    RELEASE_DIR = Path(tempfile.mkdtemp(prefix="photon_release_scr_"))
    FAKE_PLUGIN_DIR = Path(tempfile.mkdtemp(prefix="photon_plugin_scr_"))
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
    """An A3xx tree with an exterior OBJ and one .acf carrying an attachment
    table. Read the .acf back with newline="" everywhere below — these are CRLF
    on disk and universal-newline reads would hide a terminator change."""

    def __init__(self, tmp: Path, airframe="a320",
                 fname="lights_out320_XP12.obj", folder="ToLissA320_V1p3p2"):
        self.xplane_root = tmp / "X-Plane 12"
        self.folder = self.xplane_root / "Aircraft" / folder
        self.objects = self.folder / "objects"
        self.objects.mkdir(parents=True)
        (self.objects / fname).write_text(
            "I\n800\nOBJ\n\nGLOBAL_cockpit_lit\nPOINT_COUNTS\t0\t0\t0\t0\n"
            "# ORIGINAL STOCK CONTENT\n", encoding="utf-8")
        self.acf = self.folder / "a320.acf"
        self.acf_text = make_acf()
        with self.acf.open("w", encoding="utf-8", newline="") as f:
            f.write(self.acf_text)
        self.ac = dict(folder=self.folder.name, airframe=airframe, ac_ver="1.4.1",
                       name="ToLiss A320", photon=None, wing=None,
                       path=str(self.folder))

    def read_acf(self) -> str:
        with self.acf.open("r", encoding="utf-8", newline="") as f:
            return f.read()


class ScreensInstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_scr_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.fa = FakeAircraft(self.tmp)
        self.log = _NullLog()

    def _install(self, **kw):
        return actions.install(self.fa.ac, "stock", self.fa.xplane_root, self.log, **kw)

    def test_install_writes_the_obj_and_attaches_it(self):
        steps = self._install()
        obj = self.fa.objects / SCREENS_OBJ
        self.assertTrue(obj.is_file(), "screen-glow OBJ not installed")
        self.assertIn(f"ToLissPhoton version: {VERSION} wing: screens",
                      obj.read_text(encoding="utf-8"))
        self.assertIn(SCREENS_OBJ, self.fa.read_acf())
        self.assertTrue(any("display glow" in s for s in steps), steps)
        self.assertTrue(any("Attached" in s for s in steps), steps)

    def test_the_obj_is_not_a_debug_build(self):
        """A `--debug` screens OBJ binds ToLissPhoton/debug/light/* and ignores
        every DU knob. It is a tuning artifact and must never be what ships — and
        a dev's own X-Plane may well have one installed, which is exactly how this
        could be mistaken for working."""
        self._install()
        text = (self.fa.objects / SCREENS_OBJ).read_text(encoding="utf-8")
        self.assertNotIn("ToLissPhoton/debug/light", text)
        self.assertIn("ToLissPhoton/screens/spill/", text)

    def test_the_manifest_records_the_obj_as_added_not_backed_up(self):
        self._install()
        manifest = actions.read_manifest(self.fa.objects)
        self.assertIn(SCREENS_OBJ, manifest["screens"]["added"])
        # ⚠ NOT in backed_up: there was no stock file to back up, and a restore
        # loop that finds no backup leaves the file in place.
        self.assertNotIn(SCREENS_OBJ, manifest.get("backed_up", []))
        self.assertEqual(manifest["screens"]["acf_patched"], ["a320.acf"])

    def test_reinstall_is_idempotent_in_the_acf(self):
        self._install()
        once = self.fa.read_acf()
        self._install()
        self.assertEqual(self.fa.read_acf(), once)

    def test_uninstall_detaches_and_deletes(self):
        self._install()
        actions.uninstall(self.fa.ac, self.fa.xplane_root, self.log)
        self.assertFalse((self.fa.objects / SCREENS_OBJ).exists(),
                         "the added OBJ was left behind")
        self.assertEqual(self.fa.read_acf(), self.fa.acf_text,
                         "the .acf did not come back byte-for-byte")

    def test_dry_run_writes_nothing(self):
        self._install(dry_run=True)
        self.assertFalse((self.fa.objects / SCREENS_OBJ).exists())
        self.assertEqual(self.fa.read_acf(), self.fa.acf_text)

    def test_an_acf_with_no_inn_row_is_refused_not_guessed(self):
        """The attachment row carries the OBJ origin in feet against a per-airframe
        datum, and lights_inn.obj's row is the only correct source for it. With no
        such row the patcher must refuse — a guessed datum mis-places every screen
        light on two airframes out of three, invisibly."""
        text = self.fa.acf_text.replace("lights_inn.obj", "gear.obj")
        with self.fa.acf.open("w", encoding="utf-8", newline="") as f:
            f.write(text)
        steps = self._install()
        self.assertNotIn(SCREENS_OBJ, self.fa.read_acf())
        self.assertTrue(any("Could not attach" in s for s in steps), steps)
        # ...and the rest of the install still happened.
        self.assertTrue(any("lights_out320_XP12.obj" in s for s in steps), steps)

    def test_an_acf_with_no_attachment_table_is_survivable(self):
        with self.fa.acf.open("w", encoding="utf-8", newline="") as f:
            f.write("I\r\n1 version\r\nACF\r\nP acf/_tailnum ABCDE\r\n")
        steps = self._install()
        self.assertTrue(any("Could not attach" in s for s in steps), steps)


class ScreensAirframeGateTests(unittest.TestCase):
    """The A330-900 is a different flight deck, so these six positions are simply
    not its — the same reason it is out of the interior mod. Same shape of gate,
    and it has to be checked at the ACTION level: resolve_mount()-style fallbacks
    are what let an unsupported airframe quietly receive an A3xx artifact."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_scr339_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.log = _NullLog()

    def test_a339_gets_no_screens_obj_and_no_acf_edit(self):
        fa = FakeAircraft(self.tmp, airframe="a339",
                          fname="ExternalLights_XP12.obj",
                          folder="ToLissA339_V1p0p4")
        actions.install(fa.ac, "stock", fa.xplane_root, self.log)
        self.assertFalse((fa.objects / SCREENS_OBJ).exists())
        self.assertEqual(fa.read_acf(), fa.acf_text)
        self.assertIsNone(actions.read_manifest(fa.objects).get("screens"))


class ScreensPayloadTests(unittest.TestCase):
    def test_the_bundle_stages_the_screens_obj(self):
        self.assertTrue(payload.screens_available())
        self.assertTrue(payload.screens_obj_path().is_file())

    def test_the_attachment_patcher_ships_inside_the_installer(self):
        """⚠ THE OBJ ALONE DOES NOTHING — ToLiss neither ships nor references it,
        so a release carrying the OBJ but not the patcher installs a file nothing
        ever draws. The patcher used to ride along in the bundle's `payload/dsl/`
        copy of the toolchain and be reached by a sys.path insert; it is now part
        of the installer package itself, which is what makes it un-droppable."""
        self.assertFalse((payload.PAYLOAD_DIR / "dsl").exists(),
                         "the bundle still ships payload/dsl/")
        self.assertEqual(actions.patch_acf_screens.__name__,
                         "installer.patch_acf_screens")


if __name__ == "__main__":
    unittest.main()
