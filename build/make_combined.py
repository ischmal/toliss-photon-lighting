#!/usr/bin/env python3
"""Assemble the combined, all-platform installer zip — the public deliverable.

One folder (zipped) holds the shared ``data/`` payload, a README, and the per-OS
installer executables side by side::

    ToLissPhoton-Lighting-v<VER>/
        data/                  the payload (light OBJs, native fat plugin, DSL)
        README.txt
        installer_windows.exe  (each present only if its binary was supplied)
        installer_macos
        installer_linux

Each installer is a *loose* PyInstaller binary (built per-OS by
``make_exe.py --loose --exe-name installer_<os>``, since neither the C++ plugin
nor PyInstaller can cross-compile) that reads the sibling ``data/`` at runtime.
``data/`` is OS-independent because the plugin inside it is a *fat* folder
carrying every arch's ``.xpl``, so one ``data/`` serves all three installers.

Because the binaries can only be built on their own OS, the full zip is
assembled in CI (.github/workflows/release.yml) after the per-OS matrix uploads
each one. Locally you can still run this with just your own OS's binary to
preview the layout and README before publishing.

Usage:
    python build/make_combined.py --exes DIR [--plugin-dir DIR]
                                  [--out release/combined] [--no-zip]

--exes DIR      folder of prebuilt installer binaries (installer_windows.exe, …)
--plugin-dir    passed through to make_release.py to build the payload/data
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from installer.constants import VERSION  # noqa: E402

DIST_NAME = f"ToLissPhoton-Lighting-v{VERSION}"
# OS -> the loose binary's filename inside the zip (order = README listing order).
INSTALLER_NAMES = {
    "Windows": "installer_windows.exe",
    "macOS": "installer_macos",
    "Linux": "installer_linux",
}


def _readme(present: list[str]) -> str:
    lines = [
        f"ToLiss Photon Lighting  —  Installer  (v{VERSION})",
        "=" * 52,
        "",
        "WHAT THIS IS",
        "  A lighting modification for ToLiss Airbus aircraft (A319/A320/A321,",
        "  and the A330-900) in X-Plane 12. It reshapes the beacon/strobe flash",
        "  and swaps the exterior light colors between an LED and the original",
        "  look. No Python or other prerequisite is required.",
        "",
        "HOW TO INSTALL",
        "  1. Extract this WHOLE folder from the zip. Keep the installer and the",
        "     data/ folder together — the installer reads data/ from beside itself.",
        "  2. Run the installer for your operating system:",
    ]
    for os_name, fname in INSTALLER_NAMES.items():
        mark = "" if fname in present else "   (not included in this build)"
        how = "double-click it" if os_name == "Windows" else f"in a terminal: ./{fname}"
        lines.append(f"       • {os_name:8}  {fname}{mark}")
        if fname in present:
            lines.append(f"                   → {how}")
    lines += [
        "  3. Follow the on-screen steps (arrow keys / ENTER / ESC to go back).",
        "     It auto-detects X-Plane 12; pick the aircraft and wing variant.",
        "",
        "  Close X-Plane before installing.",
        "",
        "IF YOUR OS BLOCKS THE INSTALLER (it is not code-signed)",
        "  • Windows: SmartScreen may say \"Windows protected your PC\" —",
        "             click  More info  →  Run anyway.",
        "  • macOS:   if Gatekeeper blocks it, right-click the app → Open, or",
        "             clear the quarantine flag in System Settings → Privacy.",
        "  • Linux:   if it will not run, mark it executable:  chmod +x installer_linux",
        "",
        "WHAT'S IN data/",
        "  objs/     the pre-built light files (wing variants × airframes)",
        "  plugin/   the native X-Plane plugin (a fat plugin — carries every OS)",
        "  dsl/      the small toolchain the RealWings live-patch needs at install",
        "",
        "REQUIREMENTS",
        "  X-Plane 12 with a ToLiss A319/A320/A321 or A330-900 already installed.",
        "",
        "UNINSTALL",
        "  Re-run the installer and choose the uninstall action for the aircraft;",
        "  it restores the original files byte-for-byte.",
        "",
    ]
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description="Assemble the combined all-platform installer zip")
    ap.add_argument("--exes", required=True,
                    help="folder of prebuilt installer binaries (installer_windows.exe, …)")
    ap.add_argument("--plugin-dir",
                    help="native fat-plugin folder, passed through to make_release.py")
    ap.add_argument("--out", default="release/combined", help="output dir (default: release/combined)")
    ap.add_argument("--no-zip", action="store_true", help="assemble the folder but do not zip it")
    args = ap.parse_args()

    # 1. Build the payload (make_release wipes+rebuilds release/, so this must run
    #    before we create anything under release/).
    rel_cmd = [sys.executable, str(REPO / "build" / "make_release.py")]
    if args.plugin_dir:
        rel_cmd += ["--plugin-dir", args.plugin_dir]
    print("== building payload (data/) ==")
    subprocess.run(rel_cmd, check=True)
    payload = REPO / "release" / "payload"
    if not payload.is_dir():
        sys.exit(f"payload not found at {payload} after make_release")

    out = Path(args.out)
    if not out.is_absolute():
        out = REPO / out
    combined = out / DIST_NAME
    if combined.exists():
        shutil.rmtree(combined)
    combined.mkdir(parents=True)

    # 2. data/ = the payload; then drop in whichever installer binaries exist.
    shutil.copytree(payload, combined / "data")
    exes_dir = Path(args.exes)
    if not exes_dir.is_absolute():
        exes_dir = REPO / exes_dir
    present: list[str] = []
    for fname in INSTALLER_NAMES.values():
        src = exes_dir / fname
        if src.is_file():
            dst = combined / fname
            shutil.copy2(src, dst)
            if not fname.endswith(".exe"):
                dst.chmod(0o755)  # keep the exec bit on the Unix binaries
            present.append(fname)
    if not present:
        sys.exit(f"no installer binaries found in {exes_dir} "
                 f"(looked for: {', '.join(INSTALLER_NAMES.values())})")
    print(f"  bundled installers: {', '.join(present)}")

    (combined / "README.txt").write_text(_readme(present), encoding="utf-8")
    print(f"\ncombined folder ready: {combined}")

    # 3. Zip so it extracts to a single tidy DIST_NAME/ folder. The Unix installer
    #    binaries are written with an explicit 0o755 mode in the zip's external
    #    attributes, so they extract already-executable (GitHub's artifact download
    #    strips the exec bit, and zipfile stores no perms by default). Windows
    #    ignores this; the .exe needs no bit.
    if not args.no_zip:
        zip_path = out / f"{DIST_NAME}.zip"
        if zip_path.exists():
            zip_path.unlink()
        unix_bins = {f for f in present if not f.endswith(".exe")}
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for f in sorted(combined.rglob("*")):
                if not f.is_file():
                    continue
                arcname = str(Path(DIST_NAME) / f.relative_to(combined))
                zi = zipfile.ZipInfo(arcname, date_time=time.localtime(f.stat().st_mtime)[:6])
                zi.compress_type = zipfile.ZIP_DEFLATED
                mode = 0o755 if f.name in unix_bins else 0o644
                zi.external_attr = (mode & 0xFFFF) << 16
                zf.writestr(zi, f.read_bytes())
        print(f"zipped -> {zip_path} ({zip_path.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
