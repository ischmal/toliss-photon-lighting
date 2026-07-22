"""Tests for installer/payload.py's two modes. The dev-tree fallback is what
lets `python install.py` run straight from the repo (no release built): OBJs
are generated from the DSL and the native plugin comes from the CMake build
output under src/native/build/. Forcing dev mode is just pointing PAYLOAD_DIR at
a path that doesn't exist, so _bundled() is False and _dev_repo() (the repo we're
running in) takes over.

Run: python -m unittest discover -s tests -v
"""
from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from installer import payload


class DevModePayloadTests(unittest.TestCase):
    def setUp(self):
        self._saved = payload.PAYLOAD_DIR
        payload.PAYLOAD_DIR = REPO / "no-such-payload-dir"  # force dev fallback
        self.addCleanup(setattr, payload, "PAYLOAD_DIR", self._saved)
        # Other test modules (test_actions) import build_objs/patch_realwings
        # from a release temp dir that is then deleted; drop those cached copies
        # so dev-mode re-imports them from the repo. (A real installer run only
        # ever uses one mode, so this collision can't happen outside the suite.)
        for m in ("build_objs", "photon_dsl", "patch_realwings"):
            sys.modules.pop(m, None)
        sys.path.insert(0, str(REPO / "build"))

    def test_mode_is_dev_when_no_bundle(self):
        self.assertFalse(payload._bundled())
        self.assertEqual(payload.mode(), "dev")
        self.assertTrue(payload.available())

    def test_obj_text_is_generated_from_dsl(self):
        for wing in ("stock", "durantula", "realwings"):
            for airframe in ("a319", "a320", "a321"):
                text = payload.obj_text(wing, airframe)
                self.assertIn("ANIM_begin", text)
                self.assertIn("ToLissPhoton/exterior/", text)

    def test_a339_stock_obj_text_is_generated_from_dsl(self):
        # a339 (A330-900) has no wing mods — stock is the only variant.
        text = payload.obj_text("stock", "a339")
        self.assertIn("ANIM_begin", text)
        self.assertIn("ToLissPhoton/exterior/", text)

    def test_a339_unsupported_wing_raises_payload_error(self):
        """Dev mode calls the Emitter directly, bypassing the bundled-mode
        missing-file check — this locks in that obj_text() itself rejects a
        wing variant an airframe doesn't support (WINGS_FOR) instead of
        resolve_mount() silently falling back to stock content under the
        wrong label."""
        for wing in ("durantula", "realwings"):
            with self.assertRaises(payload.PayloadError):
                payload.obj_text(wing, "a339")

    def test_plugin_comes_from_native_build(self):
        # Dev mode resolves the native fat-plugin folder at the CMake build output.
        # It only exists once the .xpl is built; assert the path either way so the
        # test is hermetic regardless of whether the native plugin is compiled here.
        expected = REPO / "src" / "native" / "build" / "ToLissPhoton"
        if expected.is_dir():
            self.assertEqual(payload.plugin_src_root(), expected)
            self.assertTrue(payload.plugin_files())
            self.assertTrue(payload.plugin_arches())
        else:
            with self.assertRaises(payload.PayloadError):
                payload.plugin_src_root()

    def test_dsl_dir_points_at_repo_build(self):
        self.assertEqual(payload.dsl_dir(), REPO / "build")


class LooseFrozenPayloadTests(unittest.TestCase):
    """The loose-snapshot path (build/make_exe.py --loose): a frozen exe prefers a
    sibling data/ folder, so the installer and its files ship separately."""

    def _set_frozen(self, exe_path: Path):
        self._saved_frozen = getattr(sys, "frozen", "<unset>")
        self._saved_exe = sys.executable
        sys.frozen = True  # type: ignore[attr-defined]
        sys.executable = str(exe_path)

        def restore():
            if self._saved_frozen == "<unset>":
                delattr(sys, "frozen")
            else:
                sys.frozen = self._saved_frozen  # type: ignore[attr-defined]
            sys.executable = self._saved_exe

        self.addCleanup(restore)

    def test_frozen_prefers_sibling_data_dir(self):
        d = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        (d / "data").mkdir()
        self._set_frozen(d / "installer.exe")
        self.assertEqual(payload._default_payload_dir(), d / "data")

    def test_frozen_without_sibling_data_falls_back(self):
        d = Path(tempfile.mkdtemp())  # no data/ next to the exe
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        self._set_frozen(d / "installer.exe")
        self.assertEqual(payload._default_payload_dir(), payload.ROOT / "payload")

    def test_source_run_never_takes_loose_branch(self):
        # Not frozen: a data/ dir next to the interpreter must be ignored.
        self.assertFalse(getattr(sys, "frozen", False))
        self.assertEqual(payload._default_payload_dir(), payload.ROOT / "payload")


class UnavailablePayloadTests(unittest.TestCase):
    def test_unavailable_when_neither_bundle_nor_repo(self):
        saved_dir, saved_root = payload.PAYLOAD_DIR, payload.ROOT
        payload.PAYLOAD_DIR = REPO / "no-such-payload-dir"
        payload.ROOT = REPO / "no-such-root"  # not a repo checkout either
        try:
            self.assertFalse(payload.available())
            self.assertEqual(payload.mode(), "unavailable")
            with self.assertRaises(payload.PayloadError):
                payload.obj_text("stock", "a320")
        finally:
            payload.PAYLOAD_DIR, payload.ROOT = saved_dir, saved_root


if __name__ == "__main__":
    unittest.main()
