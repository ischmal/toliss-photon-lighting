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

Output (default --out release/, gitignored like dist/) — the Python-Universal
bundle (`python install.py`, needs only Python 3.8+, runs on any OS):
    release/install.py
    release/installer/...
    release/README.txt
    release/payload/objs/<stock|durantula|realwings>/<obj filename>
    release/payload/objs/interior/lights_inn.obj      (wing- & airframe-independent)
    release/payload/textures/interior/*.png           (Gus's set, ~57 MiB)
    release/payload/plugin/ToLissPhoton/<arch>/ToLissPhoton.xpl   (all arches in CI)
    release/payload/dsl/...
With --zip: release/ToLissPhoton-Installer-v<VER>-Python-Universal.zip
(extracts to one ToLissPhoton-Installer-v<VER>-Python-Universal/ folder).
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
import readme as _readme  # noqa: E402  (build/readme.py)
from installer.constants import (  # noqa: E402
    PLUGIN_FOLDER, PLUGIN_USER_DIRS, VERSION, WINGS, XPL_NAME,
)

INSTALLER_ENTRY = REPO / "install.py"
INSTALLER_PKG = REPO / "installer"
# The loose-.py bundle is the "Python-Universal" release download: readable
# Python that runs on any OS (its plugin/ is the fat folder with every arch).
BUNDLE_NAME = f"ToLissPhoton-Installer-v{VERSION}-Python-Universal"
DSL_TOOLCHAIN_FILES = ["build_objs.py", "photon_dsl.py", "patch_realwings.py",
                       "patch_glow.py", "patch_acf.py"]
DSL_CONFIG_FILES = ["lights.style.phdsl", "lights.layout.phdsl"]
# Default location of the compiled native fat-plugin folder (CMake build output).
DEFAULT_PLUGIN_DIR = REPO / "src" / "native" / "build" / PLUGIN_FOLDER


def build_objs_payload(payload: Path):
    """Builds every (airframe, wing) combo that airframe actually supports —
    B.SUPPORTED_WINGS, not a blind WINGS x OBJ_TARGETS cross product. Without
    that guard, an airframe with no wing mods (e.g. a339) would still build a
    "durantula"/"realwings" OBJ: resolve_mount()'s fallback to whatever mount
    variant exists would silently emit a byte-identical copy of the stock OBJ
    under those names instead of erroring."""
    for wing in WINGS:
        cfg = B.load_config(wing=wing)
        for airframe, targets in B.OBJ_TARGETS.items():
            if wing not in B.SUPPORTED_WINGS[airframe]:
                continue
            fname = targets.get("exterior")
            if fname is None:
                continue
            text = B.Emitter(cfg, airframe, wing, target="exterior").emit()
            dest = payload / "objs" / wing / fname
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(text.encode("utf-8"))
            print(f"  wrote payload/{dest.relative_to(payload)} ({len(text)} bytes)")


def build_interior_payload(payload: Path):
    """The interior OBJ is built ONCE, outside the wing loop, to
    payload/objs/interior/lights_inn.obj.

    It is both wing-independent (wing mods don't touch the cockpit) and
    airframe-independent (stock lights_inn.obj is byte-identical across
    A319/A320/A321, and Gus's variants are too), so the per-wing layout the
    exterior uses does not apply. Writing it per-airframe under
    payload/objs/<wing>/ would put all three airframes on the SAME path and
    silently collapse them into one file anyway — this makes that explicit.

    Built from the a320 fixtures, which a319/a321 inherit verbatim; a339 has no
    interior target at all (interior_plan.md decision #14)."""
    cfg = B.load_config(wing="stock")
    fname = B.OBJ_TARGETS["a320"]["interior"]
    text = B.Emitter(cfg, "a320", "stock", target="interior").emit()
    dest = payload / "objs" / "interior" / fname
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(text.encode("utf-8"))
    print(f"  wrote payload/{dest.relative_to(payload)} ({len(text)} bytes)")


def stage_interior_textures(payload: Path):
    """Copy Gus's canonical texture set into the bundle (decision #7 — textures
    ship bundled). ~57 MB, shared by all three airframes, staged once."""
    src = REPO / "reference" / "gus" / "textures"
    if not src.is_dir():
        raise SystemExit(
            f"interior texture set not found: {src}\n"
            f"Phase 0 vendoring is a hard prerequisite — the textures cannot be "
            f"reproduced from any document. See reference/gus/README.md.")
    dest = payload / "textures" / "interior"
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    total = 0
    for f in sorted(src.glob("*.png")):
        shutil.copyfile(f, dest / f.name)
        total += f.stat().st_size
    print(f"  staged payload/textures/interior/ "
          f"({len(list(dest.glob('*.png')))} files, {total / 1048576:.1f} MiB)")


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
    # PLUGIN_USER_DIRS (`overlays/`) is skipped, not copied. --plugin-dir may
    # legitimately point at a DEPLOYED plugin folder — that is how a merged
    # multi-arch bundle gets assembled — and such a folder on a dev machine holds
    # the Panel FX overlay images deploy.ps1 seeded there. The compositor is
    # PHOTON_DEV-only, so a release .xpl cannot even read them; bundling them
    # would ship inert files and hand every user a folder the plugin ignores.
    # docs/fcu_tint_plan.md §4: nothing in the release bundle until it ships.
    shutil.copytree(plugin_dir, dest_root,
                    ignore=shutil.ignore_patterns(*PLUGIN_USER_DIRS))
    skipped = [d.name for d in sorted(plugin_dir.iterdir())
               if d.is_dir() and d.name in PLUGIN_USER_DIRS]
    print(f"  staged payload/plugin/{PLUGIN_FOLDER}/ (arches: {', '.join(arches)})")
    for name in skipped:
        print(f"    skipped {name}/ — user/dev content, not a shipped artifact")


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


def stage_readme(out: Path):
    (out / "README.txt").write_text(_readme.render("Python"), encoding="utf-8")
    print("staged README.txt (Python-Universal)")


def make_zip(out: Path) -> Path:
    """Zip the bundle into out/ itself (gitignored, like the other release
    archives), extracting to one tidy BUNDLE_NAME/ folder. The zip is written
    into `out`, so it's skipped from its own contents (`f != zip_path`)."""
    zip_path = out / f"{BUNDLE_NAME}.zip"
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for f in sorted(out.rglob("*")):
            if f.is_file() and f != zip_path:
                zf.write(f, Path(BUNDLE_NAME) / f.relative_to(out))
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
    build_interior_payload(payload)
    stage_interior_textures(payload)
    stage_plugin(payload, plugin_dir)
    stage_dsl_toolchain(payload)
    stage_installer(out)
    stage_readme(out)

    if args.zip:
        zip_path = make_zip(out)
        print(f"zipped -> {zip_path}")

    print(f"\nrelease bundle ready: {out}")


if __name__ == "__main__":
    main()
