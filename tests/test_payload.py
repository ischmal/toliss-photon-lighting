"""Tests for installer/payload.py — the BUNDLE-ONLY artifact locator.

⚠ THE DEV-TREE FALLBACK IS GONE AND MUST NOT COME BACK. `python install.py` used
to run straight from a repo checkout with no release built, generating each OBJ
from the DSL via `build_objs` and taking the plugin from the CMake build output.
That made the DSL toolchain an INSTALL-TIME dependency — which a compiled
installer cannot honor and which forced every release to ship `payload/dsl/`
(docs/installer_cpp_plan.md §2b). The dev path is now an explicit staging step:

    python build/make_release.py --payload-only

so the tests below pin the *absence* of the fallback: no bundle is an error with
a message that names that command, not a silent second code path.

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

from installer import payload, realwings


class NoDevModeTests(unittest.TestCase):
    """Pointing PAYLOAD_DIR at nothing used to select dev mode. Now it is simply
    unavailable — in a real repo checkout, which is precisely the case that used
    to take the fallback."""

    def setUp(self):
        self._saved = payload.PAYLOAD_DIR
        payload.PAYLOAD_DIR = REPO / "no-such-payload-dir"
        self.addCleanup(setattr, payload, "PAYLOAD_DIR", self._saved)

    def test_a_repo_checkout_without_a_payload_is_unavailable(self):
        self.assertFalse(payload._bundled())
        self.assertEqual(payload.mode(), "unavailable")
        self.assertFalse(payload.available())

    def test_the_error_names_the_staging_command(self):
        """The dev who used to be able to just run install.py has to be told what
        replaced that, in the message they will actually see."""
        for call in (lambda: payload.obj_text("stock", "a320"),
                     payload.interior_obj_text,
                     payload.screens_obj_text):
            with self.assertRaises(payload.PayloadError) as ctx:
                call()
            self.assertIn("--payload-only", str(ctx.exception))

    def test_there_is_no_dsl_dir_helper_left(self):
        """`payload.dsl_dir()` was the sys.path hook every install-time patcher
        import went through. Its removal is the load-bearing half of the
        decoupling, so its absence is asserted rather than assumed."""
        self.assertFalse(hasattr(payload, "dsl_dir"))
        self.assertFalse(hasattr(payload, "_dev_repo"))

    def test_the_unsupported_wing_gate_still_fires_without_a_bundle(self):
        """⚠ ORDER MATTERS. The WINGS_FOR check must precede the bundle check, or
        `a339 + durantula` reports "this build is incomplete" — sending the user
        to re-download over a combination that does not exist and never will."""
        for wing in ("durantula", "realwings"):
            with self.assertRaises(payload.PayloadError) as ctx:
                payload.obj_text(wing, "a339")
            self.assertIn("wing-mod variant", str(ctx.exception))

    def test_plugin_root_errors_rather_than_reaching_into_the_repo(self):
        with self.assertRaises(payload.PayloadError):
            payload.plugin_src_root()


class RealWingsPatchDataTests(unittest.TestCase):
    """`realwings_patch.json` is what replaced `payload/dsl/`. A bundle without it
    must degrade to "no wingtip patch", never to a crash or a half-applied mod."""

    def setUp(self):
        self._saved = payload.PAYLOAD_DIR
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_test_rwdata_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.addCleanup(setattr, payload, "PAYLOAD_DIR", self._saved)
        payload.PAYLOAD_DIR = self.tmp

    def test_missing_data_is_reported_as_unavailable_not_raised(self):
        self.assertFalse(payload.realwings_available())
        with self.assertRaises(realwings.RealWingsError):
            payload.realwings_patch_data()

    def test_corrupt_data_is_unavailable_too(self):
        (self.tmp / realwings.PATCH_JSON).write_text("{not json", encoding="utf-8")
        self.assertFalse(payload.realwings_available())

    def test_a_future_schema_is_refused_rather_than_half_applied(self):
        """An installer meeting a newer bundle must say so. Reading fields it
        does not understand would emit LIGHT_PARAM lines built from defaults."""
        (self.tmp / realwings.PATCH_JSON).write_text(
            '{"schema": 999, "classes": {}, "airframes": {}, '
            '"sourceAirframe": "a320"}', encoding="utf-8")
        self.assertFalse(payload.realwings_available())


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
    def test_unavailable_when_there_is_no_bundle(self):
        saved_dir = payload.PAYLOAD_DIR
        payload.PAYLOAD_DIR = REPO / "no-such-payload-dir"
        try:
            self.assertFalse(payload.available())
            self.assertEqual(payload.mode(), "unavailable")
            with self.assertRaises(payload.PayloadError):
                payload.obj_text("stock", "a320")
        finally:
            payload.PAYLOAD_DIR = saved_dir


if __name__ == "__main__":
    unittest.main()
