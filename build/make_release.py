#!/usr/bin/env python3
"""
Build the ToLiss Photon installer release bundle.

Pre-builds the 9 OBJs (3 wing variants x 3 airframes) and stages the plugin,
so the end-user installer never needs the DSL toolchain to *generate*
lighting. The RealWings live patch is the one exception — it rewrites the
user's already-installed RealWings mod files in place, at install time, so it
can't be pre-baked the same way; a small copy of the DSL toolchain
(build_objs.py, photon_dsl.py, patch_realwings.py + the two .phdsl files) is
bundled under payload/dsl/ for that one purpose.

Usage:
    python build/make_release.py [--out DIR] [--zip]

Output (default --out release/, gitignored like dist/):
    release/install.py
    release/installer/...
    release/payload/objs/<stock|durantula|realwings>/<obj filename>
    release/payload/plugin/PI_ToLissPhoton.py
    release/payload/dsl/...
"""
from __future__ import annotations

import argparse
import shutil
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "build"))
sys.path.insert(0, str(REPO))
import build_objs as B  # noqa: E402
from installer.constants import VERSION, WINGS  # noqa: E402

INSTALLER_ENTRY = REPO / "install.py"
INSTALLER_PKG = REPO / "installer"
DSL_TOOLCHAIN_FILES = ["build_objs.py", "photon_dsl.py", "patch_realwings.py"]
DSL_CONFIG_FILES = ["lights.style.phdsl", "lights.layout.phdsl"]


def build_objs_payload(payload: Path):
    for wing in WINGS:
        cfg = B.load_config(wing=wing)
        for airframe, (_folder, fname) in B.OBJ_PATH.items():
            text = B.Emitter(cfg, airframe, wing).emit()
            dest = payload / "objs" / wing / fname
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(text.encode("utf-8"))
            print(f"  wrote payload/{dest.relative_to(payload)} ({len(text)} bytes)")


def stage_plugin(payload: Path):
    dest = payload / "plugin" / "PI_ToLissPhoton.py"
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(B.PLUGIN_SRC, dest)
    print(f"  staged payload/{dest.relative_to(payload)}")


def stage_dsl_toolchain(payload: Path):
    """Mirrors the real repo's build/ + src/lights/ relative layout under
    payload/dsl/, because build_objs.py locates its DSL config two directories
    up from its own file (REPO = Path(__file__).parent.parent) — flattening
    this into one directory would break that lookup."""
    dsl = payload / "dsl"
    build_dest = dsl / "build"
    build_dest.mkdir(parents=True, exist_ok=True)
    for name in DSL_TOOLCHAIN_FILES:
        shutil.copyfile(REPO / "build" / name, build_dest / name)
    lights_dest = dsl / "src" / "lights"
    lights_dest.mkdir(parents=True, exist_ok=True)
    for name in DSL_CONFIG_FILES:
        shutil.copyfile(REPO / "src" / "lights" / name, lights_dest / name)
    print(f"  staged payload/dsl/ (RealWings live-patch toolchain)")


def stage_installer(out: Path):
    shutil.copyfile(INSTALLER_ENTRY, out / "install.py")
    dest_pkg = out / "installer"
    if dest_pkg.exists():
        shutil.rmtree(dest_pkg)
    shutil.copytree(INSTALLER_PKG, dest_pkg, ignore=shutil.ignore_patterns("__pycache__"))
    print("staged install.py + installer/")


def make_zip(out: Path) -> Path:
    zip_path = out.parent / f"ToLissPhoton-installer-v{VERSION}.zip"
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for f in sorted(out.rglob("*")):
            if f.is_file():
                zf.write(f, f.relative_to(out.parent))
    return zip_path


def main():
    ap = argparse.ArgumentParser(description="Build the ToLiss Photon installer release bundle")
    ap.add_argument("--out", default="release", help="output dir (default: release/)")
    ap.add_argument("--zip", action="store_true", help="also zip the release folder")
    args = ap.parse_args()

    out = Path(args.out)
    if not out.is_absolute():
        out = REPO / out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    payload = out / "payload"
    print(f"building release v{VERSION} -> {out}")
    build_objs_payload(payload)
    stage_plugin(payload)
    stage_dsl_toolchain(payload)
    stage_installer(out)

    if args.zip:
        zip_path = make_zip(out)
        print(f"zipped -> {zip_path}")

    print(f"\nrelease bundle ready: {out}")


if __name__ == "__main__":
    main()
