#!/usr/bin/env python3
"""Freeze the installer into a standalone, per-platform installer bundle.

Produces ONE platform bundle for the OS this runs on (PyInstaller cannot
cross-compile): a folder holding the frozen installer binary, a ``data/`` folder
holding the files it installs, and a README, packed into a single archive that
extracts to one tidy folder:

    ToLissPhoton-Installer-v<VER>-<Windows|macOS|Linux>[.zip|.tar.gz]
      └ ToLissPhoton-Installer-v<VER>-<OS>/
          ToLissPhoton-Installer-v<VER>-<OS>[.exe]   the installer
          data/                objs/ + textures/ + plugin/<arch> + plugindata/
                               + realwings_patch.json
          README.txt

The binary is *loose* (no embedded payload); at runtime ``installer/payload.py``
reads the sibling ``data/`` (see ``_default_payload_dir``). ``data/`` carries only
THIS platform's plugin arch — the payload is built from whatever plugin
``make_release`` staged, which on each OS runner is just that arch. The
Python-Universal bundle (all arches) is a separate artifact — see
``build/make_release.py`` and ``.github/workflows/release.yml``.

Compression per platform: Windows/macOS -> .zip (Finder/Explorer native),
Linux -> .tar.gz (convention; preserves the exec bit natively). The exec bit is
also written into the .zip external attrs so macOS extracts a runnable binary.

Usage:
    python build/make_exe.py [--skip-release] [--format auto|zip|tgz] [--name NAME]

--skip-release  reuse an existing release/payload instead of rebuilding it (CI
                builds the payload as its own step, then passes this flag).
--name          override the bundle base name (default
                ToLissPhoton-Installer-v<VER>-<OS>).
"""
from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / "build"))
from installer.constants import VERSION  # noqa: E402
import readme as _readme  # noqa: E402  (build/readme.py)

# platform.system() -> pretty OS name used in the bundle/exe filenames + README.
OS_NAME = {"Windows": "Windows", "Darwin": "macOS", "Linux": "Linux"}


def _default_format() -> str:
    return "tgz" if platform.system() == "Linux" else "zip"


def _archive(folder: Path, base_name: str, fmt: str, out_dir: Path, exe_name: str) -> Path:
    """Pack `folder` into out_dir/<base_name>.<ext>, everything nested under a
    single top-level <base_name>/ dir. The frozen binary is marked executable
    (0o755) in the archive metadata so Unix extractions are runnable without a
    manual chmod; Windows ignores it."""
    if fmt == "zip":
        dest = out_dir / f"{base_name}.zip"
        if dest.exists():
            dest.unlink()
        with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
            for f in sorted(folder.rglob("*")):
                if not f.is_file():
                    continue
                arc = str(Path(base_name) / f.relative_to(folder))
                zi = zipfile.ZipInfo(arc, date_time=time.localtime(f.stat().st_mtime)[:6])
                zi.compress_type = zipfile.ZIP_DEFLATED
                mode = 0o755 if f.name == exe_name else 0o644
                zi.external_attr = (mode & 0xFFFF) << 16
                zf.writestr(zi, f.read_bytes())
        return dest
    # tar.gz — tarfile preserves the on-disk mode; force 0o755 on the binary.
    dest = out_dir / f"{base_name}.tar.gz"
    if dest.exists():
        dest.unlink()

    def _filter(ti: tarfile.TarInfo) -> tarfile.TarInfo:
        ti.mode = 0o755 if Path(ti.name).name == exe_name else (
            0o755 if ti.isdir() else 0o644)
        return ti

    with tarfile.open(dest, "w:gz") as tf:
        tf.add(folder, arcname=base_name, filter=_filter)
    return dest


def main():
    ap = argparse.ArgumentParser(description="Build the per-platform installer bundle")
    ap.add_argument("--skip-release", action="store_true",
                    help="reuse an existing release/payload instead of rebuilding it")
    ap.add_argument("--format", choices=["auto", "zip", "tgz"], default="auto",
                    help="archive format (default: auto = tgz on Linux, zip elsewhere)")
    ap.add_argument("--name", help="override the bundle base name "
                                   "(default ToLissPhoton-Installer-v<VER>-<OS>)")
    ap.add_argument("--out", default="release", help="output dir for the archive (default: release/)")
    args = ap.parse_args()

    os_name = OS_NAME.get(platform.system(), platform.system())
    base_name = args.name or f"ToLissPhoton-Installer-v{VERSION}-{os_name}"
    exe_name = base_name + (".exe" if platform.system() == "Windows" else "")
    fmt = _default_format() if args.format == "auto" else args.format
    payload = REPO / "release" / "payload"

    # Build the payload first so the binary always matches current sources.
    # (make_release wipes+rebuilds release/; the bundle lands under release/ after.)
    if not args.skip_release:
        print("== building release payload ==")
        subprocess.run([sys.executable, str(REPO / "build" / "make_release.py")], check=True)
    if not payload.is_dir():
        sys.exit(f"no payload at {payload} - run without --skip-release to build it")

    out = Path(args.out)
    if not out.is_absolute():
        out = REPO / out
    out.mkdir(parents=True, exist_ok=True)
    workdir = REPO / "release" / "_pyinstaller"
    dist = workdir / "loose_dist"

    cmd = [
        sys.executable, "-m", "PyInstaller",
        "--onefile", "--console", "--clean", "--noconfirm",
        "--name", base_name,
        "--paths", str(REPO),          # so `installer` imports
        # NB: no --add-data — payload ships beside the exe as data/, not inside.
        "--distpath", str(dist),
        "--workpath", str(workdir / "build"),
        "--specpath", str(workdir),
        str(REPO / "install.py"),
    ]
    print(f"== freezing {base_name} ({os_name}, loose) ==")
    print("  " + " ".join(cmd))
    subprocess.run(cmd, check=True)

    # Stage the bundle folder: <base_name>/{exe, data/, README.txt}
    stage = out / base_name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    binary = stage / exe_name
    shutil.copy2(dist / exe_name, binary)
    if platform.system() != "Windows":
        binary.chmod(0o755)
    shutil.copytree(payload, stage / "data")

    arch = next((d.name for d in (payload / "plugin").rglob("*")
                 if d.is_dir() and d.name.endswith("_x64")), None)
    (stage / "README.txt").write_text(  # os_name is the README kind: Windows/macOS/Linux
        _readme.render(os_name, exe_name=exe_name, arch=arch), encoding="utf-8")

    archive = _archive(stage, base_name, fmt, out, exe_name)
    print(f"\nplatform bundle ready: {archive}")
    print(f"  {exe_name}: {binary.stat().st_size / 1e6:.1f} MB  +  data/ ({arch or 'no arch'})")
    print(f"  archive: {archive.name} ({archive.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
