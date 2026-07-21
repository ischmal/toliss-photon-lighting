"""Regression tests for build/watch.py's auto-reload safety layer added
2026-07-20 (see the module's own docstring / build_objs.atomic_write_bytes).

Reported symptom: watch.py --write --wing realwings would work N times then
crash the sim. Root cause: sim/operation/reload_aircraft (fired by
send_reload) reads the installed OBJ / RealWings mod OBJ synchronously, and a
save landing soon enough after a previous one could rewrite that same file
mid-read. Two layers fix it: atomic_write_bytes (tested separately in
test_atomic_write.py) makes the write itself race-safe, and
should_defer_rebuild (tested here) keeps watch.py from even attempting a
rebuild while a reload it just fired is likely still resolving.
"""
import socket
import sys
import unittest
from pathlib import Path
from unittest import mock

BUILD = Path(__file__).resolve().parent.parent / "build"
sys.path.insert(0, str(BUILD))
import watch as W  # noqa: E402  (path insert must precede this)


class ShouldDeferRebuildTests(unittest.TestCase):
    def test_not_reloading_never_defers(self):
        # --write not passed, or --no-reload: nothing races a sim read, so
        # there is nothing to guard against
        self.assertFalse(W.should_defer_rebuild(False, 0.0, 1.0, 3.0))
        self.assertFalse(W.should_defer_rebuild(False, None, 1.0, 3.0))

    def test_no_reload_sent_yet_does_not_defer(self):
        self.assertFalse(W.should_defer_rebuild(True, None, 100.0, 3.0))

    def test_defers_within_the_guard_window(self):
        self.assertTrue(W.should_defer_rebuild(True, last_reload_at=10.0,
                                               now=11.0, guard_seconds=3.0))
        # right at the edge (elapsed < guard): still deferred
        self.assertTrue(W.should_defer_rebuild(True, last_reload_at=10.0,
                                               now=12.999, guard_seconds=3.0))

    def test_proceeds_once_the_guard_elapses(self):
        self.assertFalse(W.should_defer_rebuild(True, last_reload_at=10.0,
                                                now=13.0, guard_seconds=3.0))
        self.assertFalse(W.should_defer_rebuild(True, last_reload_at=10.0,
                                                now=50.0, guard_seconds=3.0))

    def test_zero_guard_never_defers(self):
        # --reload-guard-seconds 0: an explicit opt-out
        self.assertFalse(W.should_defer_rebuild(True, last_reload_at=10.0,
                                                now=10.0001, guard_seconds=0.0))


class SendReloadPacketTests(unittest.TestCase):
    """The UDP packet format matters: X-Plane's CMND datagram is
    b"CMND\\x00<command-name>\\x00" — get this wrong and the reload silently
    never fires (fire-and-forget, no ack), which is worse than an exception."""

    def test_packet_matches_xplane_cmnd_format(self):
        sent = {}

        class FakeSocket:
            def __init__(self, *a, **k):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def sendto(self, msg, addr):
                sent["msg"] = msg
                sent["addr"] = addr

        with mock.patch.object(W.socket, "socket", return_value=FakeSocket()):
            W.send_reload(port=49000)

        self.assertEqual(sent["addr"], ("127.0.0.1", 49000))
        self.assertTrue(sent["msg"].startswith(b"CMND\x00"))
        self.assertTrue(sent["msg"].endswith(b"\x00"))
        self.assertIn(W.RELOAD_COMMAND.encode("ascii"), sent["msg"])

    def test_socket_error_does_not_raise(self):
        # fire-and-forget: a closed sim / unreachable port must not blow up
        # the watch loop
        with mock.patch.object(W.socket, "socket",
                               side_effect=OSError("network unreachable")):
            W.send_reload(port=49000)  # must not raise


if __name__ == "__main__":
    unittest.main()
