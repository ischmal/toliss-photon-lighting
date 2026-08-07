"""⚠ THE PARITY HARNESS — the single strongest correctness net for the C++ port.

Two installers exist during the transition (docs/installer_cpp_plan.md §9). This
runs BOTH against two copies of the same fake X-Plane tree, with the same payload
and the same actions, then diffs the resulting trees BYTE FOR BYTE and the
manifests KEY FOR KEY.

Why a tree diff and not more unit tests: `installer/actions.py`'s value is in
accumulated edge-case handling — the backup-once rule, the plugin-write skip, the
detach-before-delete order, the `.acf` format trap, the RealWings refresh-from-.bak
— and a missed branch in the port is exactly the kind of thing that reads fine in
the code and produces a different tree on disk. A byte diff cannot be argued with.

⚠ DELETE THIS MODULE AT CUTOVER (§9 step 3), together with `install.py` and
`installer/`. It is scaffolding for the period when both exist; keeping it after
the Python installer goes would leave a test that can never run.

The C++ binary is found via `PHOTON_INSTALLER_EXE`, falling back to the CMake
output. ⚠ When it is absent the tests SKIP WITH A LOUD MESSAGE rather than failing:
the suite must stay runnable on a machine with no C++ toolchain, which is the same
reason it stubs the plugin binary instead of building one.

Run: python -m unittest discover -s tests -v
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from installer import actions, payload  # noqa: E402

RELEASE_DIR: Path | None = None
FAKE_PLUGIN_DIR: Path | None = None

FAKE_XPL = {
    "win_x64": b"MZ  fake win ToLissPhoton.xpl\n",
    "mac_x64": b"\xcf\xfa\xed\xfe  fake mac ToLissPhoton.xpl\n",
    "lin_x64": b"\x7fELF  fake lin ToLissPhoton.xpl\n",
}

# Where CMake puts it, per generator. Multi-config (MSVC) nests by config.
_EXE_CANDIDATES = [
    Path("src/native/build-core/Release/photon-installer.exe"),
    Path("src/native/build-core/photon-installer.exe"),
    Path("src/native/build-core/photon-installer"),
    Path("src/native/build/Release/photon-installer.exe"),
    Path("src/native/build/photon-installer"),
]


def installer_exe() -> Path | None:
    env = os.environ.get("PHOTON_INSTALLER_EXE")
    if env:
        p = Path(env)
        return p if p.is_file() else None
    for rel in _EXE_CANDIDATES:
        p = REPO / rel
        if p.is_file():
            return p
    return None


EXE = None  # bound in setUpModule


class _NullLog:
    def write(self, msg, level="INFO"):
        pass


def _make_fake_plugin(dirpath: Path):
    for arch, data in FAKE_XPL.items():
        p = dirpath / arch / "ToLissPhoton.xpl"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


def setUpModule():
    global RELEASE_DIR, FAKE_PLUGIN_DIR, EXE
    EXE = installer_exe()
    RELEASE_DIR = Path(tempfile.mkdtemp(prefix="photon_parity_release_"))
    FAKE_PLUGIN_DIR = Path(tempfile.mkdtemp(prefix="photon_parity_plugin_"))
    _make_fake_plugin(FAKE_PLUGIN_DIR)
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


# ─── the fake tree ────────────────────────────────────────────────────────────
# A stock lights_out, four .acf files in BOTH float styles (⚠ the XP11/XP12 trap
# is the port's most dangerous single difference, so parity has to cover both),
# a RealWings mod folder with baked wingtip lights, and a livery whose .dds
# shadows one of our interior textures.

_STOCK_OBJ = ("I\n800\nOBJ\n\nGLOBAL_cockpit_lit\nPOINT_COUNTS\t0\t0\t0\t0\n"
              "# ORIGINAL STOCK CONTENT\n")

_SPOT_BLOCK_XP12 = (
    "P acf/_spot1_3d_xyz/1 1.000000000\r\n"
    "P acf/_spot_angle/0 0.000000000\r\n"
    "P acf/_spot_size/0 20.000000000\r\n"
    "P acf/_spot_size/1 28.010000229\r\n"
    "P acf/_spot_size/2 38.009998322\r\n"
    "P acf/_spot_the/0 -70.000000000\r\n"
    "P acf/_spot_psi/0 -20.000000000\r\n"
    "P acf/_spot_name_3d/0 sim/cockpit2/electrical/panel_brightness_ratio[0]\r\n"
    "P acf/_spot_name_3d/1 sim/cockpit2/electrical/panel_brightness_ratio[3]\r\n"
    "P acf/_spot_name_3d/2 ckpt/lights/map\r\n"
)
_SPOT_BLOCK_XP11 = (
    "P acf/_spot1_3d_xyz/1 1.0\r\n"
    "P acf/_spot_angle/0 0.0\r\n"
    "P acf/_spot_size/0 20.0\r\n"
    "P acf/_spot_size/1 28.010000229\r\n"
    "P acf/_spot_size/2 38.009998322\r\n"
    "P acf/_spot_the/0 -70.0\r\n"
    "P acf/_spot_psi/0 -20.0\r\n"
    "P acf/_spot_name_3d/0 sim/cockpit2/electrical/panel_brightness_ratio[0]\r\n"
    "P acf/_spot_name_3d/1 sim/cockpit2/electrical/panel_brightness_ratio[3]\r\n"
    "P acf/_spot_name_3d/2 ckpt/lights/map\r\n"
)


def _acf(xp12: bool) -> str:
    y = "20.750000000" if xp12 else "20.75"
    return (
        "I\r\n1 version\r\nACF\r\n"
        + (_SPOT_BLOCK_XP12 if xp12 else _SPOT_BLOCK_XP11)
        + f"P _obja/0/_v10_att_file_stl fuselage.obj\r\n"
        f"P _obja/0/_v10_att_y_acf_prt_ref {y}\r\n"
        f"P _obja/1/_v10_att_file_stl lights_inn.obj\r\n"
        f"P _obja/1/_v10_att_y_acf_prt_ref {y}\r\n"
        "P _obja/count 2\r\n"
    )


_REALWINGS_OBJ = (
    "I\n800\nOBJ\n\n"
    "LIGHT_PARAM airplane_nav_left_size -5.1 0.5 -1.2 1 0 0 4 1000cd 0 0 -1 0.5\n"
    "LIGHT_PARAM airplane_strobe_size -5.2 0.5 -1.3 1 1 1 4 200000cd 0 0 -1 0.5\n"
)


def build_tree(root: Path, *, airframe="a320", obj="lights_out320_XP12.obj",
               folder="ToLissA320_V1p3p2") -> dict:
    """A fake <X-Plane>/Aircraft/<folder>/ tree, as ToLiss ships it."""
    xplane = root / "X-Plane 12"
    ac = xplane / "Aircraft" / folder
    objects = ac / "objects"
    objects.mkdir(parents=True)
    (xplane / "Resources").mkdir(parents=True, exist_ok=True)

    (objects / obj).write_text(_STOCK_OBJ, encoding="utf-8")
    (objects / "lights_inn.obj").write_text(
        "I\n800\nOBJ\n\nPOINT_COUNTS 0 0 1 0\n# STOCK INTERIOR\n", encoding="utf-8")

    # ⚠ Both float styles, because ToLiss ships all four .acf per airframe and a
    # literal string patch works on two of them and silently no-ops on the others.
    for name, xp12 in ((f"{airframe}.acf", True), (f"{airframe}_StdDef.acf", True),
                       (f"{airframe}_XP11.acf", False),
                       (f"{airframe}_XP11_StdDef.acf", False)):
        with (ac / name).open("w", encoding="utf-8", newline="") as f:
            f.write(_acf(xp12))

    # ⚠ A .bak that must be left alone — Durantula keeps one of this very file, and
    # patching it would corrupt that mod's uninstall.
    with (ac / f"{airframe}.acf.durantula.bak").open(
            "w", encoding="utf-8", newline="") as f:
        f.write(_acf(True))

    rw = objects / "RealWings320"
    rw.mkdir()
    (rw / "MainNEO.obj").write_text(_REALWINGS_OBJ, encoding="utf-8")
    (rw / "Main.obj").write_text(_REALWINGS_OBJ, encoding="utf-8")

    livery = ac / "liveries" / "Air France" / "objects"
    livery.mkdir(parents=True)
    (livery / "chairs_LIT.dds").write_bytes(b"DDS  fake livery override\n")
    (livery / "unrelated.dds").write_bytes(b"DDS  not ours, never touch\n")

    return dict(xplane_root=xplane, aircraft=ac, objects=objects, airframe=airframe,
                obj_name=obj)


def snapshot(root: Path) -> dict[str, bytes]:
    """Every file under `root` as {posix relative path: bytes}."""
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            out[p.relative_to(root).as_posix()] = p.read_bytes()
    return out


# Fields that legitimately differ between two runs of the SAME installer, so
# comparing them would only produce flaky failures.
_VOLATILE_MANIFEST_KEYS = {"installed_at"}


def normalized_manifest(objects: Path) -> dict:
    p = objects / "Photon Backup Files" / "photon_manifest.json"
    if not p.is_file():
        return {}
    data = json.loads(p.read_text(encoding="utf-8"))
    for k in _VOLATILE_MANIFEST_KEYS:
        data.pop(k, None)
    # `realwings_dir` is an absolute path into the tree, which differs by design
    # between the two copies; compare only its tail.
    if "realwings_dir" in data:
        data["realwings_dir"] = Path(data["realwings_dir"]).name
    return data


class ParityTestCase(unittest.TestCase):
    """Base: builds two identical trees, runs one installer on each, diffs."""

    def setUp(self):
        if EXE is None:
            self.skipTest(
                "\n"
                "  ================================================================\n"
                "  PARITY HARNESS SKIPPED — photon-installer was not found.\n"
                "  This is the strongest correctness net for the C++ installer port\n"
                "  and it did NOT run. Build it, or set PHOTON_INSTALLER_EXE:\n"
                "     cmake -S src/native -B src/native/build-core -DPHOTON_CORE_ONLY=ON\n"
                "     cmake --build src/native/build-core --config Release\n"
                "  ================================================================")
        self.tmp = Path(tempfile.mkdtemp(prefix="photon_parity_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.py = build_tree(self.tmp / "py")
        self.cpp = build_tree(self.tmp / "cpp")
        self.log = _NullLog()

    # ── drivers ──────────────────────────────────────────────────────────────
    def run_python_install(self, tree, wing="stock", interior=False, dry_run=False):
        ac = dict(folder=tree["aircraft"].name, airframe=tree["airframe"],
                  ac_ver="1.4.1", name="ToLiss A320", photon=None, wing=None,
                  path=str(tree["aircraft"]))
        return actions.install(ac, wing, tree["xplane_root"], self.log,
                               dry_run=dry_run, interior=interior)

    def run_python_uninstall(self, tree, remove_plugin=False, dry_run=False):
        ac = dict(folder=tree["aircraft"].name, airframe=tree["airframe"],
                  path=str(tree["aircraft"]))
        return actions.uninstall(ac, tree["xplane_root"], self.log,
                                 dry_run=dry_run, remove_plugin=remove_plugin)

    def run_cpp(self, argv):
        """Drive the compiled installer, with the payload staged beside it.

        ⚠ `payload/` is resolved from the EXECUTABLE'S OWN directory, never CWD —
        that is what lets a user run it from Downloads or a USB stick — so the
        harness stages a link/copy of the real release payload next to a copy of
        the binary rather than passing a flag that does not exist."""
        proc = subprocess.run([str(self.staged_exe()), *argv, "--json"],
                              capture_output=True, text=True, encoding="utf-8")
        try:
            result = json.loads(proc.stdout)
        except json.JSONDecodeError:
            self.fail(f"installer produced no JSON (exit {proc.returncode}):\n"
                      f"stdout: {proc.stdout[:2000]}\nstderr: {proc.stderr[:2000]}")
        return result, proc

    def staged_exe(self) -> Path:
        """A copy of the binary with `payload/` beside it, made once per test."""
        staged = getattr(self, "_staged_exe", None)
        if staged is not None:
            return staged
        d = self.tmp / "bin"
        d.mkdir(parents=True, exist_ok=True)
        staged = d / EXE.name
        shutil.copyfile(EXE, staged)
        staged.chmod(0o755)
        shutil.copytree(RELEASE_DIR / "payload", d / "payload")
        self._staged_exe = staged
        return staged

    def run_cpp_install(self, tree, wing="stock", interior=False, dry_run=False):
        argv = ["install", "--aircraft", str(tree["aircraft"]),
                "--wing", wing,
                "--xplane-root", str(tree["xplane_root"]), "--force"]
        if interior:
            argv.append("--interior")
        if dry_run:
            argv.append("--dry-run")
        result, proc = self.run_cpp(argv)
        self.assertTrue(result.get("ok"),
                        f"C++ install failed: {result.get('error')}\n"
                        f"{chr(10).join(result.get('log', []))}")
        return result

    def run_cpp_uninstall(self, tree, remove_plugin=False, dry_run=False):
        argv = ["uninstall", "--aircraft", str(tree["aircraft"]),
                "--xplane-root", str(tree["xplane_root"]), "--force"]
        if remove_plugin:
            argv.append("--remove-plugin")
        if dry_run:
            argv.append("--dry-run")
        result, _ = self.run_cpp(argv)
        return result

    # ── the comparison ───────────────────────────────────────────────────────
    def assert_trees_match(self, what: str):
        a = snapshot(self.py["xplane_root"])
        b = snapshot(self.cpp["xplane_root"])
        only_py = sorted(set(a) - set(b))
        only_cpp = sorted(set(b) - set(a))
        self.assertEqual(only_py, [], f"{what}: files only the Python installer made")
        self.assertEqual(only_cpp, [], f"{what}: files only the C++ installer made")

        differing = []
        for rel in sorted(a):
            if rel.endswith("photon_manifest.json"):
                continue         # compared structurally below (key order, timestamps)
            if a[rel] != b[rel]:
                differing.append(rel)
        if differing:
            rel = differing[0]
            self.fail(
                f"{what}: {len(differing)} file(s) differ between the installers.\n"
                f"first: {rel}\n"
                f"--- python ---\n{a[rel][:1200]!r}\n"
                f"--- c++ ---\n{b[rel][:1200]!r}\n"
                f"all: {differing}")

        self.assertEqual(normalized_manifest(self.py["objects"]),
                         normalized_manifest(self.cpp["objects"]),
                         f"{what}: manifests differ")


class StockInstallParityTests(ParityTestCase):
    def test_a_plain_stock_install_produces_the_same_tree(self):
        self.run_python_install(self.py)
        self.run_cpp_install(self.cpp)
        self.assert_trees_match("stock install")

    def test_the_marker_and_backup_match(self):
        self.run_python_install(self.py)
        self.run_cpp_install(self.cpp)
        name = self.py["obj_name"]
        self.assertEqual((self.py["objects"] / name).read_bytes(),
                         (self.cpp["objects"] / name).read_bytes())
        self.assertEqual(
            (self.py["objects"] / "Photon Backup Files" / name).read_bytes(),
            (self.cpp["objects"] / "Photon Backup Files" / name).read_bytes())

    def test_reinstall_is_identical_too(self):
        """⚠ The second run is where the divergent branches live: backup-once,
        the plugin-write skip, and the manifest merge."""
        for _ in range(2):
            self.run_python_install(self.py)
            self.run_cpp_install(self.cpp)
        self.assert_trees_match("reinstall")

    def test_dry_run_changes_nothing_on_either_side(self):
        before_py = snapshot(self.py["xplane_root"])
        before_cpp = snapshot(self.cpp["xplane_root"])
        self.run_python_install(self.py, dry_run=True)
        self.run_cpp_install(self.cpp, dry_run=True)
        self.assertEqual(snapshot(self.py["xplane_root"]), before_py)
        self.assertEqual(snapshot(self.cpp["xplane_root"]), before_cpp)


class InteriorParityTests(ParityTestCase):
    def test_interior_install_matches_including_both_acf_styles(self):
        """⚠ THE FORMAT TRAP, end to end. The tree carries two XP12 and two XP11
        `.acf` files; a renderer that disagreed with Python's `repr` on either pair
        shows up here as a byte diff and nowhere else."""
        self.run_python_install(self.py, interior=True)
        self.run_cpp_install(self.cpp, interior=True)
        self.assert_trees_match("interior install")

    def test_the_untouched_durantula_bak_is_identical_and_unpatched(self):
        self.run_python_install(self.py, interior=True)
        self.run_cpp_install(self.cpp, interior=True)
        for tree in (self.py, self.cpp):
            bak = tree["aircraft"] / f"{tree['airframe']}.acf.durantula.bak"
            self.assertIn("panel_brightness_ratio[0]",
                          bak.read_text(encoding="utf-8"))

    def test_interior_uninstall_returns_both_trees_to_stock(self):
        self.run_python_install(self.py, interior=True)
        self.run_cpp_install(self.cpp, interior=True)
        self.run_python_uninstall(self.py)
        self.run_cpp_uninstall(self.cpp)
        self.assert_trees_match("interior uninstall")


class RealWingsParityTests(ParityTestCase):
    def test_realwings_install_matches_byte_for_byte(self):
        """The patched wingtip lines carry resolved colours, intensities, cones and
        positions. Any disagreement between the DSL-derived JSON and either
        applier lands here."""
        self.run_python_install(self.py, wing="realwings")
        self.run_cpp_install(self.cpp, wing="realwings")
        self.assert_trees_match("realwings install")

    def test_switching_away_from_realwings_reverses_identically(self):
        self.run_python_install(self.py, wing="realwings")
        self.run_cpp_install(self.cpp, wing="realwings")
        self.run_python_install(self.py, wing="stock")
        self.run_cpp_install(self.cpp, wing="stock")
        self.assert_trees_match("realwings -> stock")

    def test_realwings_uninstall_restores_the_mod(self):
        self.run_python_install(self.py, wing="realwings")
        self.run_cpp_install(self.cpp, wing="realwings")
        self.run_python_uninstall(self.py)
        self.run_cpp_uninstall(self.cpp)
        self.assert_trees_match("realwings uninstall")


class UninstallParityTests(ParityTestCase):
    def test_uninstall_leaves_identical_trees(self):
        self.run_python_install(self.py)
        self.run_cpp_install(self.cpp)
        self.run_python_uninstall(self.py)
        self.run_cpp_uninstall(self.cpp)
        self.assert_trees_match("uninstall")

    def test_uninstall_with_remove_plugin_matches(self):
        self.run_python_install(self.py)
        self.run_cpp_install(self.cpp)
        self.run_python_uninstall(self.py, remove_plugin=True)
        self.run_cpp_uninstall(self.cpp, remove_plugin=True)
        self.assert_trees_match("uninstall --remove-plugin")

    def test_uninstall_without_an_install_fails_on_both(self):
        with self.assertRaises(actions.ActionError):
            self.run_python_uninstall(self.py)
        result = self.run_cpp_uninstall(self.cpp)
        self.assertFalse(result.get("ok"))
        self.assertIn("no Photon installation", result.get("error", ""))


class CliContractTests(ParityTestCase):
    """The CLI's own contracts — no prompts, honest exit codes, and the gates that
    have to fire before anything is written."""

    def test_detect_finds_the_aircraft_the_python_scanner_finds(self):
        from installer import detect as pydetect

        result, _ = self.run_cpp(["detect", "--xplane-root",
                                  str(self.cpp["xplane_root"])])
        self.assertTrue(result["ok"])
        cpp_folders = sorted(a["folder"] for a in result["aircraft"])
        py_folders = sorted(a["folder"] for a in
                            pydetect.detect_aircraft(self.py["xplane_root"]))
        self.assertEqual(cpp_folders, py_folders)
        self.assertEqual([a["airframe"] for a in result["aircraft"]], ["a320"])

    def test_status_reports_both_axes_independently(self):
        """⚠ The interior is a SECOND, INDEPENDENT AXIS. Folding it into the
        exterior status would make an aircraft with the interior installed and the
        exterior reverted read as 'not installed at all'."""
        self.run_cpp_install(self.cpp, interior=True)
        result, _ = self.run_cpp(["status", "--aircraft", str(self.cpp["aircraft"])])
        self.assertTrue(result["exterior"]["installed"])
        self.assertTrue(result["interior"]["installed"])

        # revert only the exterior OBJ, as an aircraft update would
        (self.cpp["objects"] / self.cpp["obj_name"]).write_text(
            _STOCK_OBJ, encoding="utf-8")
        result, _ = self.run_cpp(["status", "--aircraft", str(self.cpp["aircraft"])])
        self.assertTrue(result["exterior"]["stale"], "a reverted OBJ must read stale")
        self.assertTrue(result["interior"]["installed"],
                        "the interior must still report itself")

    def test_an_unsupported_wing_is_refused_by_name_not_by_missing_file(self):
        """⚠ The error has to name the real reason. 'This build is incomplete'
        would send the user to re-download over a combination that does not exist
        and never will."""
        result, proc = self.run_cpp(
            ["install", "--aircraft", str(self.cpp["aircraft"]),
             "--wing", "durantula", "--xplane-root", str(self.cpp["xplane_root"]),
             "--force"])
        # a320 DOES support durantula, so build a fake a339 to test the gate
        self.assertTrue(result["ok"], result.get("error"))

        a339 = build_tree(self.tmp / "a339", airframe="a339",
                          obj="ExternalLights_XP12.obj", folder="ToLissA339_V1p0p4")
        result, proc = self.run_cpp(
            ["install", "--aircraft", str(a339["aircraft"]),
             "--wing", "durantula", "--xplane-root", str(a339["xplane_root"]),
             "--force"])
        self.assertFalse(result["ok"])
        self.assertIn("wing-mod variant", result["error"])
        self.assertEqual(proc.returncode, 1)

    def test_missing_required_input_is_an_error_never_a_prompt(self):
        result, proc = self.run_cpp(["install"])
        self.assertFalse(result["ok"])
        self.assertIn("--aircraft", result["error"])
        self.assertEqual(proc.returncode, 1)


if __name__ == "__main__":
    unittest.main()
