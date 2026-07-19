"""Tests for installer/payload.py's two modes. The dev-tree fallback is what
lets `python install.py` run straight from the repo (no release built): OBJs
are generated from the DSL and the plugin comes from src/. Forcing dev mode is
just pointing PAYLOAD_DIR at a path that doesn't exist, so _bundled() is False
and _dev_repo() (the repo we're running in) takes over.

Run: python -m unittest discover -s tests -v
"""
from __future__ import annotations

import sys
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

    def test_plugin_comes_from_src(self):
        p = payload.plugin_path()
        self.assertEqual(p, REPO / "src" / "plugin" / "PI_ToLissPhoton.py")
        self.assertGreater(len(payload.plugin_bytes()), 0)

    def test_dsl_dir_points_at_repo_build(self):
        self.assertEqual(payload.dsl_dir(), REPO / "build")


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
