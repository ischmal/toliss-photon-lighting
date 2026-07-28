"""End-to-end smoke test: drive install.py's real state machine (launch ->
aircraft -> action -> perform -> complete) with a scripted key sequence against
a fake X-Plane tree, in --dry-run so nothing is written. Confirms the screens
are wired together correctly — detect.py feeding screen_aircraft, actions.py
feeding screen_perform, etc. — as one process, not just each module in
isolation.

msvcrt.getwch() reads the real console, not redirected stdin, so this drives
the flow in-process via a monkeypatched tui.read_key rather than a subprocess
with piped input (which would hang on Windows).

Run: python -m unittest discover -s tests -v
"""
from __future__ import annotations

import io
import re
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
from installer import actions, detect, payload, tui
from installer.constants import VERSION

RELEASE_DIR: Path | None = None
FAKE_PLUGIN_DIR: Path | None = None


def setUpModule():
    global RELEASE_DIR, FAKE_PLUGIN_DIR
    RELEASE_DIR = Path(tempfile.mkdtemp(prefix="photon_release_e2e_"))
    # A fake native fat-plugin folder so make_release doesn't need the C++ build.
    FAKE_PLUGIN_DIR = Path(tempfile.mkdtemp(prefix="photon_plugin_e2e_"))
    for arch in ("win_x64", "mac_x64", "lin_x64"):
        p = FAKE_PLUGIN_DIR / arch / "ToLissPhoton.xpl"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(f"fake {arch} ToLissPhoton.xpl\n".encode())
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


class _NullLog:
    def write(self, msg, level="INFO"):
        pass


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
        (self.xp / "Resources" / "plugins").mkdir(parents=True)
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
            tui.KEY_ENTER,  # interior opt-in -> first entry (exterior only)
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
            tui.KEY_ENTER,  # interior opt-in -> first entry (exterior only)
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


class A339ActionScreenTests(unittest.TestCase):
    """a339 (A330-900) has no wing mods — screen_action must build its option
    list from WINGS_FOR[airframe], not the global WINGS, so Durantula/RealWings
    never appear for it."""

    def test_action_screen_offers_only_stock_and_uninstall(self):
        ac = {"folder": "ToLissA339_V1p0p4", "airframe": "a339", "ac_ver": "1.0.4",
              "name": "ToLiss A330-900", "photon": "0.4", "wing": "stock", "stale": False}
        old_read_key, old_log = tui.read_key, install.LOG
        # If Durantula/RealWings leaked in, option 2 would be "Durantula" and
        # this key sequence would select the wrong action.
        tui.read_key = ScriptedKeys(["2"])
        install.LOG = _NullLog()
        try:
            with redirect_stdout(io.StringIO()):
                action = install.screen_action(ac)
        finally:
            tui.read_key = old_read_key
            install.LOG = old_log
        self.assertEqual(action, "uninstall")


class CloseXPlaneWaitTests(unittest.TestCase):
    """screen_check_not_running now holds until X-Plane is closed and then
    proceeds on its own, rather than erroring out mid-install. Regression guard
    for the reported 'installer just failed: file in use' bug: auto-poll while
    the sim is up, ESC backs out a step, ENTER overrides."""

    def setUp(self):
        self._old = dict(running=detect.xplane_running,
                         current=actions.plugin_is_current,
                         rkt=tui.read_key_timeout, log=install.LOG)
        install.LOG = _NullLog()
        # Defaults: the plugin differs (so the wait is genuinely needed) and no
        # key is pressed (timeout) — each test overrides what it exercises.
        actions.plugin_is_current = lambda root: False
        tui.read_key_timeout = lambda timeout: None
        self.addCleanup(self._restore)

    def _restore(self):
        detect.xplane_running = self._old["running"]
        actions.plugin_is_current = self._old["current"]
        tui.read_key_timeout = self._old["rkt"]
        install.LOG = self._old["log"]

    def _running_then_closed(self, values):
        seq = list(values)
        detect.xplane_running = lambda: seq.pop(0) if seq else False

    def _run(self, action="stock"):
        with redirect_stdout(io.StringIO()):
            return install.screen_check_not_running({}, action, Path("/xp"))

    def test_returns_automatically_once_xplane_closes(self):
        self._running_then_closed([True, True, False])
        polls = {"n": 0}

        def _rkt(timeout):
            polls["n"] += 1
            return None  # nothing pressed — the loop rechecks the process

        tui.read_key_timeout = _rkt
        self._run()                       # returns without raising
        self.assertGreaterEqual(polls["n"], 1)  # it polled while running

    def test_esc_backs_out_a_step(self):
        detect.xplane_running = lambda: True
        tui.read_key_timeout = lambda timeout: tui.KEY_ESC
        with self.assertRaises(tui.Back):
            self._run()

    def test_enter_continues_anyway(self):
        detect.xplane_running = lambda: True
        tui.read_key_timeout = lambda timeout: tui.KEY_ENTER
        self._run()  # returns (no raise) despite X-Plane still "running"

    def test_no_wait_when_plugin_already_current(self):
        detect.xplane_running = lambda: True
        actions.plugin_is_current = lambda root: True
        polls = {"n": 0}
        tui.read_key_timeout = lambda timeout: polls.__setitem__("n", polls["n"] + 1)
        self._run()
        self.assertEqual(polls["n"], 0)  # OBJ-only reinstall never enters the wait

    def test_uninstall_waits_even_if_plugin_current(self):
        # uninstall may remove the plugin, so it must still wait for the sim.
        self._running_then_closed([True, False])
        actions.plugin_is_current = lambda root: True
        polls = {"n": 0}

        def _rkt(timeout):
            polls["n"] += 1
            return None

        tui.read_key_timeout = _rkt
        self._run(action="uninstall")
        self.assertGreaterEqual(polls["n"], 1)


def _plain(s: str) -> str:
    return re.sub(r"\033\[[0-9;]*m", "", s)


class CompleteSummaryTests(unittest.TestCase):
    def test_install_summary(self):
        s = _plain(install._complete_summary({"name": "ToLiss A321"}, "durantula", True))
        self.assertEqual(s, f"ToLiss Photon v{VERSION} (Durantula) installed in ToLiss A321.")

    def test_stock_install_summary(self):
        s = _plain(install._complete_summary({"name": "ToLiss A320"}, "stock", True))
        self.assertEqual(s, f"ToLiss Photon v{VERSION} (Default) installed in ToLiss A320.")

    def test_uninstall_summary(self):
        s = _plain(install._complete_summary({"name": "ToLiss A319"}, "uninstall", True))
        self.assertIn("removed from ToLiss A319", s)

    def test_failure_summary(self):
        s = _plain(install._complete_summary({"name": "x"}, "stock", False))
        self.assertIn("did not complete", s)


class ActionLabelTests(unittest.TestCase):
    def test_parenthetical_is_dark_gray(self):
        ac = {"photon": "0.3", "wing": "durantula", "stale": False}
        label = install._action_label(ac, "durantula")
        self.assertIn(tui.C.BR_BLACK, label)                  # the note is dark gray
        self.assertIn(f"v0.3 → v{VERSION}", _plain(label))    # an upgrade reinstall

    def test_switch_wing_label(self):
        ac = {"photon": "0.4", "wing": "durantula", "stale": False}
        self.assertIn("switch from Durantula", _plain(install._action_label(ac, "stock")))

    def test_fresh_install_has_no_parenthetical(self):
        ac = {"photon": None, "wing": None, "stale": False}
        self.assertNotIn("(", install._action_label(ac, "stock"))


if __name__ == "__main__":
    unittest.main()
