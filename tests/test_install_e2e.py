"""End-to-end smoke test: drive install.py's real state machine (launch ->
requirements -> aircraft -> action -> perform -> complete) with a scripted key
sequence against a fake X-Plane tree, in --dry-run so nothing is written.
Confirms the screens are wired together correctly — detect.py feeding
screen_aircraft, actions.py feeding screen_perform, etc. — as one process,
not just each module in isolation.

msvcrt.getwch() reads the real console, not redirected stdin, so this drives
the flow in-process via a monkeypatched tui.read_key rather than a subprocess
with piped input (which would hang on Windows).

Run: python -m unittest discover -s tests -v
"""
from __future__ import annotations

import io
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import install
from installer import detect, payload, tui

RELEASE_DIR: Path | None = None


def setUpModule():
    global RELEASE_DIR
    RELEASE_DIR = Path(tempfile.mkdtemp(prefix="photon_release_e2e_"))
    subprocess.run(
        [sys.executable, str(REPO / "build" / "make_release.py"),
         "--out", str(RELEASE_DIR)],
        check=True, cwd=REPO, capture_output=True, text=True,
    )
    payload.PAYLOAD_DIR = RELEASE_DIR / "payload"


def tearDownModule():
    if RELEASE_DIR is not None:
        shutil.rmtree(RELEASE_DIR, ignore_errors=True)


class ScriptedKeys:
    def __init__(self, keys):
        self.keys = list(keys)

    def __call__(self):
        if not self.keys:
            raise AssertionError("scripted key sequence exhausted — flow asked for more input")
        return self.keys.pop(0)


class InstallFlowE2ETest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_e2e_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.xp = self.tmp / "X-Plane 12"
        (self.xp / "Resources" / "plugins" / "XPPython3").mkdir(parents=True)
        ac = self.xp / "Aircraft" / "ToLissA320_V1p3p2"
        objects = ac / "objects"
        objects.mkdir(parents=True)
        (ac / "skunkcrafts_updater.cfg").write_text(
            "version|v1.4.1\nname|ToLiss A320\n", encoding="utf-8")
        (objects / "lights_out320_XP12.obj").write_text(
            "I\n800\nOBJ\n\nGLOBAL_cockpit_lit\nPOINT_COUNTS\t0\t0\t0\t0\n"
            "# ORIGINAL STOCK CONTENT\n", encoding="utf-8")
        self.objects = objects
        self.obj_path = objects / "lights_out320_XP12.obj"

        # Deterministic regardless of whether X-Plane happens to be open on
        # the machine running these tests — the guard itself is exercised by
        # unit tests on detect.xplane_running() directly, not here.
        old = detect.xplane_running
        detect.xplane_running = lambda: False
        self.addCleanup(setattr, detect, "xplane_running", old)

    def _run(self, argv, keys):
        old_argv, old_read_key = sys.argv, tui.read_key
        sys.argv = argv
        tui.read_key = ScriptedKeys(keys)
        try:
            with redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    install.main()
        finally:
            sys.argv = old_argv
            tui.read_key = old_read_key
        return cm.exception.code

    def test_dry_run_install_then_exit_writes_nothing(self):
        original = self.obj_path.read_text(encoding="utf-8")
        keys = [
            tui.KEY_ENTER,  # launch -> continue
            tui.KEY_ENTER,  # aircraft select -> first (only) entry
            tui.KEY_ENTER,  # action select -> first entry (stock install)
            tui.KEY_ENTER,  # perform -> continue
            tui.KEY_ENTER,  # complete -> first entry (Exit installer)
        ]
        code = self._run(
            ["install.py", "--dry-run", "--xplane-root", str(self.xp)], keys)
        self.assertEqual(code, 0)
        # --dry-run: the flow ran end-to-end but touched nothing on disk
        self.assertEqual(self.obj_path.read_text(encoding="utf-8"), original)
        self.assertFalse((self.objects / "Photon Backup Files").exists())

    def test_esc_at_launch_exits_cleanly(self):
        code = self._run(
            ["install.py", "--dry-run", "--xplane-root", str(self.xp)],
            [tui.KEY_ESC])
        self.assertEqual(code, 0)

    def test_real_install_then_uninstall_via_the_full_flow(self):
        """No --dry-run: drives install through the real screens, then loops
        back ("Modify another aircraft") and drives a real uninstall — the
        same round trip test_actions.py checks at the module level, but here
        exercised through install.py's actual menu wiring end to end."""
        original = self.obj_path.read_text(encoding="utf-8")
        install_keys = [
            tui.KEY_ENTER,  # launch -> continue
            tui.KEY_ENTER,  # aircraft select -> first (only) entry
            tui.KEY_ENTER,  # action select -> first entry (stock install)
            tui.KEY_ENTER,  # perform -> continue
            "2",            # complete -> "Modify another aircraft"
            tui.KEY_ENTER,  # aircraft select -> first (only) entry, again
            "4",            # action select -> Uninstall
            tui.KEY_ENTER,  # plugin-disposition prompt -> "leave it installed"
            tui.KEY_ENTER,  # perform -> continue
            tui.KEY_ENTER,  # complete -> Exit installer
        ]
        code = self._run(["install.py", "--xplane-root", str(self.xp)], install_keys)
        self.assertEqual(code, 0)
        self.assertEqual(self.obj_path.read_text(encoding="utf-8"), original)
        self.assertFalse((self.objects / "Photon Backup Files").exists())


if __name__ == "__main__":
    unittest.main()
