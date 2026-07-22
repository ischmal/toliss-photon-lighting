#!/usr/bin/env python3
"""
Build the ToLiss Photon installer release bundle.

Pre-builds the 9 OBJs (3 wing variants x 3 airframes) and stages the compiled
native plugin, so the end-user installer never needs the DSL toolchain to
*generate* lighting nor a C++ toolchain to build the plugin. The RealWings live
patch is one exception — it rewrites the user's already-installed RealWings mod
files in place, at install time, so it can't be pre-baked the same way; the
skin-glow redirect (patch_glow.py) is another, rewriting ToLiss's own mesh OBJs in
place. A small copy of the DSL toolchain (build_objs.py, photon_dsl.py,
patch_realwings.py, patch_glow.py + the two .phdsl files) is bundled under
payload/dsl/ for those purposes.

The plugin is the native `.xpl` (no XPPython3/Python at runtime). It is a fat
plugin: `--plugin-dir` points at a `ToLissPhoton/` folder holding one
`<arch>/ToLissPhoton.xpl` per platform. Locally only the arch you compiled is
present; a full multi-platform release is assembled in CI from the per-OS build
matrix (all three `<arch>/` dirs merged into one folder) — see
.github/workflows/release.yml and src/native/README.md.

Usage:
    python build/make_release.py [--out DIR] [--plugin-dir DIR] [--zip]

Output (default --out release/, gitignored like dist/):
    release/install.py
    release/installer/...
    release/payload/objs/<stock|durantula|realwings>/<obj filename>
    release/payload/plugin/ToLissPhoton/<arch>/ToLissPhoton.xpl
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
from installer.constants import PLUGIN_FOLDER, VERSION, WINGS, XPL_NAME  # noqa: E402

INSTALLER_ENTRY = REPO / "install.py"
INSTALLER_PKG = REPO / "installer"
DSL_TOOLCHAIN_FILES = ["build_objs.py", "photon_dsl.py", "patch_realwings.py",
                       "patch_glow.py"]
DSL_CONFIG_FILES = ["lights.style.phdsl", "lights.layout.phdsl"]
# Default location of the compiled native fat-plugin folder (CMake build output).
DEFAULT_PLUGIN_DIR = REPO / "src" / "native" / "build" / PLUGIN_FOLDER


def build_objs_payload(payload: Path):
    """Builds every (airframe, wing) combo that airframe actually supports —
    B.SUPPORTED_WINGS, not a blind WINGS x OBJ_PATH cross product. Without that
    guard, an airframe with no wing mods (e.g. a339) would still build a
    "durantula"/"realwings" OBJ: resolve_mount()'s fallback to whatever mount
    variant exists would silently emit a byte-identical copy of the stock OBJ
    under those names instead of erroring."""
    for wing in WINGS:
        cfg = B.load_config(wing=wing)
        for airframe, (_folder, fname) in B.OBJ_PATH.items():
            if wing not in B.SUPPORTED_WINGS[airframe]:
                continue
            text = B.Emitter(cfg, airframe, wing).emit()
            dest = payload / "objs" / wing / fname
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(text.encode("utf-8"))
            print(f"  wrote payload/{dest.relative_to(payload)} ({len(text)} bytes)")


def stage_plugin(payload: Path, plugin_dir: Path):
    """Copy the compiled native fat-plugin folder into the bundle. Each platform's
    build produces one <arch>/ToLissPhoton.xpl; a full release carries all three
    side by side (CI merges the per-OS build matrix). Locally only the arch you
    built is present — fine for a single-platform test bundle."""
    if not plugin_dir.is_dir():
        raise SystemExit(
            f"native plugin folder not found: {plugin_dir}\n"
            f"Build it first (see src/native/README.md), e.g.:\n"
            f"  cmake --build src/native/build --config Release\n"
            f"or pass --plugin-dir at a ToLissPhoton/ folder holding "
            f"<arch>/{XPL_NAME} for each platform.")
    arches = sorted(d.name for d in plugin_dir.iterdir()
                    if d.is_dir() and (d / XPL_NAME).is_file())
    if not arches:
        raise SystemExit(f"no {XPL_NAME} found under {plugin_dir}/*/ — "
                         f"build the native plugin first")
    dest_root = payload / "plugin" / PLUGIN_FOLDER
    if dest_root.exists():
        shutil.rmtree(dest_root)
    shutil.copytree(plugin_dir, dest_root)
    print(f"  staged payload/plugin/{PLUGIN_FOLDER}/ (arches: {', '.join(arches)})")


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
    ap.add_argument("--plugin-dir", default=str(DEFAULT_PLUGIN_DIR),
                    help="native fat-plugin folder holding <arch>/%s "
                         "(default: the CMake build output under src/native/build/)" % XPL_NAME)
    ap.add_argument("--zip", action="store_true", help="also zip the release folder")
    args = ap.parse_args()

    out = Path(args.out)
    if not out.is_absolute():
        out = REPO / out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    plugin_dir = Path(args.plugin_dir)
    if not plugin_dir.is_absolute():
        plugin_dir = REPO / plugin_dir

    payload = out / "payload"
    print(f"building release v{VERSION} -> {out}")
    build_objs_payload(payload)
    stage_plugin(payload, plugin_dir)
    stage_dsl_toolchain(payload)
    stage_installer(out)

    if args.zip:
        zip_path = make_zip(out)
        print(f"zipped -> {zip_path}")

    print(f"\nrelease bundle ready: {out}")


if __name__ == "__main__":
    main()
