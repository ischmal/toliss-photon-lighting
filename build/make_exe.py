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
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from installer.constants import VERSION  # noqa: E402

# platform.system() -> release-asset OS tag
PLATFORM_TAG = {"Windows": "windows", "Darwin": "macos", "Linux": "linux"}


def main():
    ap = argparse.ArgumentParser(description="Freeze the ToLiss Photon installer")
    ap.add_argument("--out", default="release/exe", help="output dir (default: release/exe)")
    ap.add_argument("--skip-release", action="store_true",
                    help="reuse an existing release/payload instead of rebuilding it")
    args = ap.parse_args()

    tag = PLATFORM_TAG.get(platform.system(), platform.system().lower())
    name = f"ToLissPhoton-Installer-v{VERSION}-{tag}"
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
