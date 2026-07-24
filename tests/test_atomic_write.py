"""Regression tests for build_objs.atomic_write_bytes — the crash-lesson fix
from 2026-07-20 (see its docstring / build_objs.py's comment above it).

Symptom that motivated it: watch.py --write auto-reloads the aircraft right
after installing a rebuild; X-Plane's sim/operation/reload_aircraft call is
SYNCHRONOUS and reads the OBJ (or, for RealWings, the mod's Main.obj) off disk
while it runs. A fast-enough next save could start REWRITING that exact file
mid-read with a plain write, handing X-Plane a torn/half-written OBJ — not a
parse error, a native crash. atomic_write_bytes writes to a same-directory temp
file and os.replace()s it over the destination, so a concurrent reader always
sees the complete old file or the complete new one, never a mix — and retries
briefly on the transient Windows sharing-violation this exact scenario can
raise (the OS may deny a rename over a file another process has open for
reading without FILE_SHARE_DELETE).

Import note: see tests/test_patch_realwings.py's docstring for why build_objs
is imported in setUpModule rather than at module level (test_actions caches a
bundled copy from a temp dir its teardown deletes).
"""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"

B = None  # build_objs, bound in setUpModule


def setUpModule():
    global B
    sys.modules.pop("build_objs", None)
    sys.path.insert(0, str(BUILD))
    import build_objs
    B = build_objs


def tearDownModule():
    sys.modules.pop("build_objs", None)
    try:
        sys.path.remove(str(BUILD))
    except ValueError:
        pass


class AtomicWriteBytesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil_rmtree, self.tmp)

    def _leftover_tmp_files(self):
        return list(self.tmp.glob("*.tmp"))

    def test_creates_file_with_exact_content(self):
        dest = self.tmp / "lights_out320_XP12.obj"
        B.atomic_write_bytes(dest, b"hello world")
        self.assertEqual(dest.read_bytes(), b"hello world")
        self.assertEqual(self._leftover_tmp_files(), [])

    def test_overwrites_existing_file_and_leaves_no_tmp_litter(self):
        dest = self.tmp / "Main.obj"
        dest.write_bytes(b"old content, longer than the new one")
        B.atomic_write_bytes(dest, b"new")
        self.assertEqual(dest.read_bytes(), b"new")
        self.assertEqual(self._leftover_tmp_files(), [])

    def test_replaces_a_symlink_entry_rather_than_following_it(self):
        # the _write_live "stale symlink" case: dest is a symlink to a path
        # that may not even exist; the write must replace the LINK, not error
        # trying to write through a broken target
        dest = self.tmp / "dest.obj"
        broken_target = self.tmp / "nonexistent.obj"
        try:
            dest.symlink_to(broken_target)
        except OSError:
            self.skipTest("creating symlinks needs elevated privilege here")
        B.atomic_write_bytes(dest, b"real content")
        self.assertFalse(dest.is_symlink())
        self.assertEqual(dest.read_bytes(), b"real content")

    def test_creates_parent_directories(self):
        dest = self.tmp / "A320" / "objects" / "lights_out320_XP12.obj"
        B.atomic_write_bytes(dest, b"x")
        self.assertEqual(dest.read_bytes(), b"x")

    def test_retries_on_transient_replace_failure_then_succeeds(self):
        # simulates the documented Windows sharing-violation window: something
        # (X-Plane's reload_aircraft read) transiently holds dest so the first
        # couple of os.replace calls fail, then it releases and the 3rd lands
        dest = self.tmp / "Main.obj"
        dest.write_bytes(b"old")
        calls = {"n": 0}
        real_replace = B.os.replace

        def flaky_replace(src, dst):
            calls["n"] += 1
            if calls["n"] < 3:
                raise OSError("simulated transient sharing violation")
            return real_replace(src, dst)

        with mock.patch.object(B.os, "replace", side_effect=flaky_replace), \
             mock.patch.object(B.time, "sleep", return_value=None):  # don't slow the test
            B.atomic_write_bytes(dest, b"new", attempts=5, retry_delay=0.15)
        self.assertEqual(calls["n"], 3)
        self.assertEqual(dest.read_bytes(), b"new")
        self.assertEqual(self._leftover_tmp_files(), [])

    def test_gives_up_after_max_attempts_and_cleans_up_tmp(self):
        dest = self.tmp / "Main.obj"
        dest.write_bytes(b"old")
        with mock.patch.object(B.os, "replace",
                               side_effect=OSError("stuck")), \
             mock.patch.object(B.time, "sleep", return_value=None):
            with self.assertRaises(OSError):
                B.atomic_write_bytes(dest, b"new", attempts=3, retry_delay=0.01)
        # the original is untouched (replace never succeeded) and no .tmp litter
        self.assertEqual(dest.read_bytes(), b"old")
        self.assertEqual(self._leftover_tmp_files(), [])


def shutil_rmtree(path):
    import shutil
    shutil.rmtree(path, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
