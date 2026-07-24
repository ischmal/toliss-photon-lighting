#!/usr/bin/env python3
"""Freeze the installer into a standalone native executable via PyInstaller.

Produces a single self-contained binary that end users double-click — no Python
required on their machine. Bundles the pre-built release ``payload/`` as data;
``installer/payload.py`` finds it inside PyInstaller's extraction dir with **no
code change** (its ``ROOT = Path(__file__).parent.parent`` lands on the _MEI…
dir, so ``PAYLOAD_DIR`` = ``<_MEI…>/payload`` — verified frozen, including the
dynamic ``import build_objs`` the RealWings live-patch needs).

PyInstaller cannot cross-compile: a Windows .exe builds only on Windows, macOS
only on macOS, Linux only on Linux. Run this once per target OS, or let the
GitHub Actions matrix in .github/workflows/release.yml build all three.

Usage:
    python build/make_exe.py [--out release/exe] [--skip-release]

--skip-release reuses an existing release/payload instead of rebuilding it
(the CI job builds the payload as its own step, then passes this flag).
"""
from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from installer.constants import VERSION  # noqa: E402

# platform.system() -> release-asset OS tag
PLATFORM_TAG = {"Windows": "windows", "Darwin": "macos", "Linux": "linux"}

# OS-appropriate "this app isn't code-signed" note for the loose-snapshot README.
_UNSIGNED_NOTE = {
    "Windows": ('WINDOWS SMARTSCREEN\n'
                '  The installer is not code-signed, so Windows may show\n'
                '  "Windows protected your PC". Click  More info  ->  Run anyway.'),
    "Darwin": ('macOS GATEKEEPER\n'
               '  The installer is not notarized. If macOS blocks it, right-click\n'
               '  the app -> Open, or clear the quarantine flag in System Settings.'),
    "Linux": ('LINUX\n'
              '  If the file is not executable, run:  chmod +x <installer>'),
}


def _loose_readme(exe_name: str, arches: str) -> str:
    return f"""ToLiss Photon Lighting - Installer  (v{VERSION})
====================================================

WHAT THIS IS
  A lighting modification for ToLiss Airbus aircraft (A319/A320/A321) in
  X-Plane 12. It reshapes the beacon/strobe flash and swaps exterior light
  colors between an LED and the original look.

  This is the "loose" snapshot: the installer and the files it installs are
  shipped SEPARATELY. The installer program reads the plainly-named  data/
  folder that sits next to it. No Python or other prerequisite is required.

HOW TO RUN
  1. Extract this WHOLE folder out of the zip (keep the .exe and the data/
     folder together - the installer reads data/ from right beside itself).
  2. Double-click  {exe_name}
  3. Follow the on-screen steps (arrow keys / ENTER / ESC to go back). It
     auto-detects X-Plane 12; pick the aircraft and wing variant and install.

  Close X-Plane before installing.

{_UNSIGNED_NOTE.get(platform.system(), '')}

WHAT'S IN data/
  objs/     the pre-built light files (3 wing variants x 3 airframes)
  plugin/   the native X-Plane plugin  (this build: {arches})
  dsl/      the small toolchain the RealWings live-patch needs at install time

REQUIREMENTS
  - X-Plane 12 with a ToLiss A319/A320/A321 already installed.

UNINSTALL
  Re-run the installer and choose the uninstall action for the aircraft;
  it restores the original files byte-for-byte.

This is a test build. Please report anything that looks off.
"""


def build_loose(name: str, payload: Path, do_zip: bool):
    """Produce a *loose* snapshot: the installer and its files shipped SEPARATELY.

    Freeze the exe WITHOUT embedding the payload, then stage it next to a
    plainly-named ``data/`` folder (a copy of ``payload/``) + a README. At
    runtime ``installer/payload.py`` reads that sibling ``data/`` (see
    ``_default_payload_dir``). Result: release/snapshot/<name>/ (and, with --zip,
    release/<name>.zip that extracts to one tidy <name>/ folder)."""
    workdir = REPO / "release" / "_pyinstaller"
    dist = workdir / "loose_dist"
    cmd = [
        sys.executable, "-m", "PyInstaller",
        "--onefile", "--console", "--clean", "--noconfirm",
        "--name", name,
        "--paths", str(REPO),            # so `installer` imports
        # NB: no --add-data — the payload ships beside the exe as data/, not inside.
        "--distpath", str(dist),
        "--workpath", str(workdir / "build"),
        "--specpath", str(workdir),
        str(REPO / "install.py"),
    ]
    print("== freezing (loose: payload ships separately as data/) ==")
    print("  " + " ".join(cmd))
    subprocess.run(cmd, check=True)

    exe_name = name + (".exe" if platform.system() == "Windows" else "")
    snap = REPO / "release" / "snapshot" / name
    if snap.exists():
        shutil.rmtree(snap)
    snap.mkdir(parents=True)
    shutil.copy2(dist / exe_name, snap / exe_name)
    shutil.copytree(payload, snap / "data")
    arches = ", ".join(sorted(d.name for d in (payload / "plugin").rglob("*")
                              if d.is_dir() and d.name.endswith("_x64"))) or "(none)"
    (snap / "README.txt").write_text(_loose_readme(exe_name, arches), encoding="utf-8")

    print(f"\nloose snapshot ready: {snap}")
    print(f"  {exe_name}: {(snap / exe_name).stat().st_size / 1e6:.1f} MB"
          f"  +  data/ ({arches})")
    if do_zip:
        zip_path = REPO / "release" / f"{name}.zip"
        if zip_path.exists():
            zip_path.unlink()
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for f in sorted(snap.rglob("*")):
                if f.is_file():
                    zf.write(f, Path(name) / f.relative_to(snap))
        print(f"zipped -> {zip_path} ({zip_path.stat().st_size / 1e6:.1f} MB)")


def main():
    ap = argparse.ArgumentParser(description="Freeze the ToLiss Photon installer")
    ap.add_argument("--out", default="release/exe", help="output dir (default: release/exe)")
    ap.add_argument("--skip-release", action="store_true",
                    help="reuse an existing release/payload instead of rebuilding it")
    ap.add_argument("--loose", action="store_true",
                    help="ship the installer and its files SEPARATELY: freeze the exe "
                         "without embedding the payload and stage it beside a data/ "
                         "folder (+ README) as a ready-to-hand-off snapshot")
    ap.add_argument("--zip", action="store_true",
                    help="with --loose, also zip the snapshot to release/<name>.zip")
    ap.add_argument("--exe-name",
                    help="override the base name of the produced binary (default: "
                         "ToLissPhoton-Installer-v<VER>-<os>). CI uses this to name "
                         "the per-OS loose binaries installer_windows / _macos / _linux "
                         "for the combined all-platform zip (build/make_combined.py).")
    args = ap.parse_args()

    tag = PLATFORM_TAG.get(platform.system(), platform.system().lower())
    name = args.exe_name or f"ToLissPhoton-Installer-v{VERSION}-{tag}"
    payload = REPO / "release" / "payload"

    # Build the payload first so the frozen binary always matches the current
    # DSL sources. (make_release wipes+rebuilds release/; the exe lands in
    # release/exe afterwards, so it survives.) make_release stages the native
    # plugin from its default CMake build output (src/native/build/ToLissPhoton),
    # so that must already be built — in CI, build the payload as a separate step
    # (passing --plugin-dir with all arches merged) and run this with --skip-release.
    if not args.skip_release:
        print("== building release payload ==")
        subprocess.run([sys.executable, str(REPO / "build" / "make_release.py")], check=True)
    if not payload.is_dir():
        sys.exit(f"no payload at {payload} — run without --skip-release to build it")

    if args.loose:
        build_loose(name, payload, do_zip=args.zip)
        return

    out = Path(args.out)
    if not out.is_absolute():
        out = REPO / out
    out.mkdir(parents=True, exist_ok=True)
    workdir = REPO / "release" / "_pyinstaller"

    cmd = [
        sys.executable, "-m", "PyInstaller",
        "--onefile",            # single self-contained binary
        "--console",            # the installer IS a terminal TUI — keep the console
        "--clean", "--noconfirm",
        "--name", name,
        "--paths", str(REPO),                             # so `installer` imports
        "--add-data", f"{payload}{os.pathsep}payload",    # ship the pre-built payload
        "--distpath", str(out),
        "--workpath", str(workdir / "build"),
        "--specpath", str(workdir),
        str(REPO / "install.py"),
    ]
    print("== freezing ==")
    print("  " + " ".join(cmd))
    subprocess.run(cmd, check=True)

    produced = out / (name + (".exe" if platform.system() == "Windows" else ""))
    print(f"\nexecutable ready: {produced}")
    if produced.exists():
        print(f"  size: {produced.stat().st_size / 1e6:.1f} MB")
    else:
        sys.exit(f"expected output not found: {produced}")


if __name__ == "__main__":
    main()
