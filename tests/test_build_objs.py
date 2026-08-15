"""Unit tests for build/build_objs.py's airframe/wing-mod gating and the
`check --against --positions-only` authoring aid added for a339 (A330-900)
support — see docs/a339_plan.md.

The invariant locked in here: resolve_mount() falls back to whatever mount
variant exists when the requested one is missing, so an airframe with no wing
mods at all (a339) would otherwise silently build a "durantula"/"realwings"
OBJ that is just a byte-identical copy of stock under the wrong label.
SUPPORTED_WINGS is the guard that turns that into a clear error/skip instead.

Import note: mirrors test_patch_realwings.py's import-isolation pattern — other
test modules (test_actions) import *bundled* copies of build_objs from a temp
release dir that gets deleted, so this module clears any stale cached copy
before importing the repo's own build/build_objs.py, and cleans up after
itself so later test modules aren't affected either.
"""
from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"

B = None  # build_objs, bound in setUpModule

_MODULES = ("build_objs", "photon_dsl", "patch_realwings")


def setUpModule():
    global B
    for m in _MODULES:
        sys.modules.pop(m, None)
    sys.path.insert(0, str(BUILD))
    import build_objs
    B = build_objs


def tearDownModule():
    for m in _MODULES:
        sys.modules.pop(m, None)
    try:
        sys.path.remove(str(BUILD))
    except ValueError:
        pass


class _Args:
    """Minimal stand-in for argparse.Namespace — set only what each cmd_*
    function reads."""
    def __init__(self, **kw):
        self.__dict__.update(kw)


class SupportedWingsGateTests(unittest.TestCase):
    def test_explicit_unsupported_combo_errors_clearly(self):
        args = _Args(airframe="a339", wing="durantula", out=None, flat=True, write=False)
        with self.assertRaises(SystemExit) as ctx:
            B.cmd_build(args)
        msg = str(ctx.exception)
        self.assertIn("a339", msg)
        self.assertIn("durantula", msg)

    def test_supported_combo_is_unaffected(self):
        out_dir = Path(tempfile.mkdtemp(prefix="photon_test_a339_build_"))
        self.addCleanup(shutil.rmtree, out_dir, ignore_errors=True)
        args = _Args(airframe="a339", wing="stock", out=str(out_dir),
                    flat=True, write=False, target="exterior")
        B.cmd_build(args)  # must not raise
        self.assertTrue((out_dir / "ExternalLights_XP12.obj").is_file())

    def test_build_all_skips_airframes_that_dont_support_the_requested_wing(self):
        """a339 has no Durantula/RealWings mount — a plain `build --wing
        durantula` (no --airframe) must silently build only the airframes that
        actually support it, not emit a stock-content a339 OBJ mislabeled as
        durantula."""
        out_dir = Path(tempfile.mkdtemp(prefix="photon_test_buildall_"))
        self.addCleanup(shutil.rmtree, out_dir, ignore_errors=True)
        args = _Args(airframe=None, wing="durantula", out=str(out_dir),
                    flat=True, write=False, target="exterior")
        B.cmd_build(args)
        written = sorted(p.name for p in out_dir.iterdir())
        self.assertEqual(written, [
            "lights_out319_XP12.obj", "lights_out320_XP12.obj", "lights_out321_XP12.obj",
        ])
        self.assertNotIn("ExternalLights_XP12.obj", written)


class PositionsOnlyProjectionTests(unittest.TestCase):
    """project_positions() backs `check --against --positions-only`, the
    authoring aid used to verify a339's transcription against ToLiss's
    original file (see docs/a339_plan.md Step 1)."""

    def test_collapses_branch_duplication_and_drops_gate_rgb_intensity_cone(self):
        # Two records differing only in gate/rgb/intensity/cone — as Photon's
        # own halogen/xenon vs. LED emission of the SAME physical light would.
        records = B.Counter({
            (("g1",), "airplane_nav_bb", (1.0, 2.0, 3.0), (1.0, 0.0, 0.0),
             0, 50, (0.0, 0.0, 1.0), 0.1): 1,
            (("g2",), "airplane_nav_bb", (1.0, 2.0, 3.0), (0.0, 1.0, 0.0),
             0, 60, (0.0, 0.0, 1.0), 0.1): 1,
        })
        proj = B.project_positions(records)
        self.assertEqual(proj, {("airplane_nav_bb", (1.0, 2.0, 3.0), 0, (0.0, 0.0, 1.0))})

    def test_distinguishes_by_index(self):
        records = B.Counter({
            ((), "airplane_strobe_bb", (1.0, 0.0, 0.0), (1.0, 1.0, 1.0),
             0, 100, (1.0, 0.0, 0.0), 0.1): 1,
            ((), "airplane_strobe_bb", (1.0, 0.0, 0.0), (1.0, 1.0, 1.0),
             1, 100, (1.0, 0.0, 0.0), 0.1): 1,
        })
        proj = B.project_positions(records)
        self.assertEqual(len(proj), 2)


if __name__ == "__main__":
    unittest.main()
