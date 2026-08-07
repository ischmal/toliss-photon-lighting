#!/usr/bin/env python3
"""
Build the ToLiss Photon installer release bundle.

Pre-builds the 9 OBJs (3 wing variants x 3 airframes) and stages the compiled
native plugin, so the end-user installer needs neither the DSL toolchain nor a C++
toolchain.

⚠ THE BUNDLE NO LONGER CARRIES THE DSL TOOLCHAIN AT ALL. It used to ship
`payload/dsl/` — build_objs.py, photon_dsl.py and the two `.phdsl` files — for the
one install-time step that could not be pre-baked: the RealWings live patch, which
rewrites the user's already-installed mod files in place and so has to resolve each
replacement light's parameters against the DSL. Those parameters are now RESOLVED
AT BUNDLE TIME into `payload/realwings_patch.json` (see `stage_realwings_patch` and
build/patch_realwings.py) and applied by `installer/realwings.py`, which is
stdlib-only. The skin-glow redirect and the two `.acf` patchers never needed the
DSL and now live in the installer package outright. Net effect: install time has
zero DSL dependency, which is the precondition for the C++ installer
(docs/installer_cpp_plan.md §2).

The plugin is the native `.xpl` (no XPPython3/Python at runtime). It is a fat
plugin: `--plugin-dir` points at a `ToLissPhoton/` folder holding one
`<arch>/ToLissPhoton.xpl` per platform. Locally only the arch you compiled is
present; a full multi-platform release is assembled in CI from the per-OS build
matrix (all three `<arch>/` dirs merged into one folder) — see
.github/workflows/release.yml and src/native/README.md.

Usage:
    python build/make_release.py [--out DIR] [--plugin-dir DIR] [--zip]
    python build/make_release.py --payload-only [--out DIR]

Output (default --out release/, gitignored like dist/) — the Python-Universal
bundle (`python install.py`, needs only Python 3.8+, runs on any OS):
    release/install.py
    release/installer/...
    release/README.txt
    release/payload/objs/<stock|durantula|realwings>/<obj filename>
    release/payload/objs/interior/lights_inn.obj      (wing- & airframe-independent)
    release/payload/objs/screens/lights_screens.obj   (display glow, A3xx)
    release/payload/textures/interior/*.png           (Gus's set, ~57 MiB)
    release/payload/plugin/ToLissPhoton/<arch>/ToLissPhoton.xpl   (all arches in CI)
    release/payload/plugindata/{panelfx.txt,overlays/*.png}   (into the plugin folder)
    release/payload/realwings_patch.json              (resolved from the DSL)
With --zip: release/ToLissPhoton-Installer-v<VER>-Python-Universal.zip
(extracts to one ToLissPhoton-Installer-v<VER>-Python-Universal/ folder).

`--payload-only` stages just `payload/` (no installer copy, no README, no zip, and
no plugin unless one is built) into `--out`, defaulting to the REPO ROOT so it
lands next to `install.py`. That is the dev path: `install.py` used to build OBJs
on the fly from the DSL when run out of a checkout, which made the toolchain an
install-time dependency in the one place it had to not be. Stage, then run.
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
import patch_realwings as RWBUILD  # noqa: E402  (build/patch_realwings.py)
import readme as _readme  # noqa: E402  (build/readme.py)
from installer.constants import (  # noqa: E402
    PANELFX_FILE, PLUGIN_DATA_DIRNAME, PLUGIN_FOLDER, PLUGIN_USER_DIRS,
    WINGS, XPL_NAME,
)
from installer.realwings import PATCH_JSON  # noqa: E402
# ⚠ THE VERSION COMES FROM core/version.h, not from installer/constants.py. That
# header is the one definition the plugin and the compiled installer both read, so
# a bundle's NAME can no longer disagree with what the About tab reports (§8c).
from version import VERSION  # noqa: E402  (build/version.py)

INSTALLER_ENTRY = REPO / "install.py"
INSTALLER_PKG = REPO / "installer"
# The loose-.py bundle is the "Python-Universal" release download: readable
# Python that runs on any OS (its plugin/ is the fat folder with every arch).
BUNDLE_NAME = f"ToLissPhoton-Installer-v{VERSION}-Python-Universal"
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


def build_screens_payload(payload: Path):
    """The display-glow OBJ, built ONCE like the interior one and for the same
    reasons: no wing variants, and the A3xx flight decks are the same model so the
    six display positions are identical.

    ⚠ NOT covered by `build_objs.py build` — `screens` is deliberately absent from
    DEFAULT_TARGETS so a bare build cannot slip an experiment into `dist/`. That
    makes this call the only thing that puts it in a bundle, so it is emitted
    straight from the Emitter here rather than copied out of `dist/`."""
    cfg = B.load_config(wing="stock")
    fname = B.OBJ_TARGETS["a320"]["screens"]
    text = B.Emitter(cfg, "a320", "stock", target="screens").emit()
    dest = payload / "objs" / "screens" / fname
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(text.encode("utf-8"))
    print(f"  wrote payload/{dest.relative_to(payload)} ({len(text)} bytes)")


def stage_plugin_data(payload: Path):
    """The plugin's runtime data files — the Panel FX layer definition and the
    overlay images it names — staged under payload/plugindata/ mirroring the
    plugin folder's own layout, so the installer copies the tree in verbatim.

    ⚠ The overlays are enumerated, not listed. `panelfx.txt` names images by file
    name and a layer whose image is missing is SKIPPED at runtime rather than
    drawn as a solid, so a hard-coded list that fell one behind the look would
    ship a cockpit quietly missing a layer."""
    src_fx = REPO / "src" / "native" / PANELFX_FILE
    if not src_fx.is_file():
        raise SystemExit(
            f"Panel FX layer definition not found: {src_fx}\n"
            f"It is the authored look and cannot be regenerated. On a dev machine "
            f"it is symlinked from the plugin folder — see src/native/README.md.")
    dest_root = payload / PLUGIN_DATA_DIRNAME
    if dest_root.exists():
        shutil.rmtree(dest_root)
    dest_root.mkdir(parents=True)
    # Read + write rather than copyfile: on a dev machine the repo copy is the
    # TARGET of the plugin folder's symlink, and staging must capture its contents
    # either way.
    (dest_root / PANELFX_FILE).write_bytes(src_fx.read_bytes())
    n = 0
    for d in PLUGIN_USER_DIRS:
        src = REPO / "src" / "native" / d
        if not src.is_dir():
            continue
        for f in sorted(src.rglob("*")):
            if not f.is_file():
                continue
            out = dest_root / d / f.relative_to(src)
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(f, out)
            n += 1
    print(f"  staged payload/{PLUGIN_DATA_DIRNAME}/ ({PANELFX_FILE} + {n} overlay image(s))")


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


def stage_realwings_patch(payload: Path):
    """Resolve the RealWings wingtip replacements out of the DSL and write them to
    `payload/realwings_patch.json`.

    ⚠ THIS IS WHAT REPLACED `payload/dsl/`. The RealWings patch rewrites the user's
    already-installed mod OBJs in place, so unlike our own OBJs it cannot be
    pre-baked as a file — but its NUMBERS can, and that is the whole difference
    between an installer that needs the DSL parser and one that doesn't. Same
    shape as `build_objs_payload` above: resolve at bundle time, apply at install
    time.

    ⚠ The output is a GENERATED ARTIFACT and carries a `_generated` header saying
    so. Tuning still lives in `lights.layout.phdsl`; this file is downstream of it
    and is overwritten on every build."""
    dest = payload / PATCH_JSON
    data = RWBUILD.write_patch_data(dest)
    frames = ", ".join(sorted(data["airframes"]))
    print(f"  wrote payload/{PATCH_JSON} "
          f"({len(data['classes'])} baked light class(es); airframes: {frames})")


# Where CMake leaves the compiled installer, per generator. Multi-config
# generators (MSVC, Xcode) nest by configuration; single-config ones do not.
INSTALLER_EXE_NAMES = ("photon-installer.exe", "photon-installer")
INSTALLER_BUILD_DIRS = (
    REPO / "src" / "native" / "build",
    REPO / "src" / "native" / "build-core",
)


def find_installer_binary(explicit: Path | None = None) -> Path | None:
    """The compiled `photon-installer`, or None if it has not been built."""
    if explicit is not None:
        return explicit if explicit.is_file() else None
    for base in INSTALLER_BUILD_DIRS:
        for sub in ("", "Release", "RelWithDebInfo"):
            for name in INSTALLER_EXE_NAMES:
                p = base / sub / name if sub else base / name
                if p.is_file():
                    return p
    return None


def stage_compiled_installer(out: Path, explicit: Path | None = None) -> Path | None:
    """Stage the compiled `photon-installer` beside `payload/`.

    ⚠ IT GOES NEXT TO `payload/`, NOT INSIDE IT. The binary resolves its payload
    from its OWN directory rather than the working directory — that is what lets a
    user run it from Downloads, from a USB stick, or from inside
    Resources/plugins/ — so the two have to be siblings.

    Absent is not an error: this repo's own release path still builds the
    Python-Universal bundle, and one release ships BOTH installers side by side
    (docs/installer_cpp_plan.md §9) before the Python one is deleted. A machine that
    has not built the C++ side still produces a complete Python bundle."""
    exe = find_installer_binary(explicit)
    if exe is None:
        print("  ! photon-installer not built — bundle staged without it "
              "(build it: cmake --build src/native/build --config Release)")
        return None
    dest = out / exe.name
    shutil.copyfile(exe, dest)
    dest.chmod(dest.stat().st_mode | 0o111)   # keep it executable on macOS/Linux
    print(f"  staged {dest.name} ({exe.stat().st_size // 1024} KiB) from {exe}")
    return dest


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
    ap.add_argument("--out", help="output dir (default: release/, or the repo "
                                  "root with --payload-only)")
    ap.add_argument("--plugin-dir", default=str(DEFAULT_PLUGIN_DIR),
                    help="native fat-plugin folder holding <arch>/%s "
                         "(default: the CMake build output under src/native/build/)" % XPL_NAME)
    ap.add_argument("--zip", action="store_true", help="also zip the release folder")
    ap.add_argument("--payload-only", action="store_true",
                    help="stage only payload/ (the dev path: stage beside "
                         "install.py, then run it). Tolerates an unbuilt plugin.")
    ap.add_argument("--installer-exe",
                    help="the compiled photon-installer to stage (default: the "
                         "CMake output under src/native/build*/)")
    args = ap.parse_args()

    out = Path(args.out) if args.out else (REPO if args.payload_only
                                           else REPO / "release")
    if not out.is_absolute():
        out = REPO / out
    payload = out / "payload"

    # ⚠ --payload-only never wipes `out`: its default IS the repo root. Only the
    # payload/ subtree it owns is replaced.
    if args.payload_only:
        if payload.exists():
            shutil.rmtree(payload)
        payload.mkdir(parents=True)
    else:
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)

    plugin_dir = Path(args.plugin_dir)
    if not plugin_dir.is_absolute():
        plugin_dir = REPO / plugin_dir

    print(f"building {'payload' if args.payload_only else 'release'} "
          f"v{VERSION} -> {out}")
    build_objs_payload(payload)
    build_interior_payload(payload)
    build_screens_payload(payload)
    stage_realwings_patch(payload)
    stage_interior_textures(payload)
    # A dev staging a payload to try the TUI has usually not built the plugin, and
    # refusing over it would make the dev path harder than the one it replaced. A
    # real release still fails loudly — the installer has nothing to install
    # without it.
    if args.payload_only and not plugin_dir.is_dir():
        print(f"  ! no native plugin at {plugin_dir} — staged without it "
              f"(build it to test the plugin install)")
    else:
        stage_plugin(payload, plugin_dir)
    stage_plugin_data(payload)

    if args.payload_only:
        print(f"\npayload staged: {payload}\n"
              f"run the installer with: python install.py")
        return

    stage_installer(out)
    # ⚠ BOTH installers ship for one release (§9): the compiled one is primary and
    # the Python bundle is the fallback, and the release notes say so. This is the
    # only period in which both exist; the Python half goes one release later.
    stage_compiled_installer(
        out, Path(args.installer_exe) if args.installer_exe else None)
    stage_readme(out)

    if args.zip:
        zip_path = make_zip(out)
        print(f"zipped -> {zip_path}")

    print(f"\nrelease bundle ready: {out}")


if __name__ == "__main__":
    main()
