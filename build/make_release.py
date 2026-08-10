#!/usr/bin/env python3
"""
Build the ToLiss Photon installer release bundle.

Pre-builds the 9 OBJs (3 wing variants x 3 airframes) and stages the compiled
native plugin, so the end-user installer needs neither the DSL toolchain nor a C++
toolchain.

⚠ THERE IS ONE INSTALLER AND IT IS COMPILED: `photon-installer`. The Python one is
GONE — `install.py`, `installer/`, `build/make_exe.py`, the PyInstaller dependency,
the Python-Universal bundle and the parity harness were all deleted on 2026-08-09
(docs/installer_cpp_plan.md §9). The line to hold: a bundle carries exactly one
executable, so no user is ever told which of two to run.

⚠ THIS SCRIPT IS STILL PYTHON, AND THAT IS NOT A LEFTOVER. It runs at BUILD time,
where the DSL toolchain lives; what the port removed was Python at INSTALL time. Its
shared constants come from `build/constants.py` (mirrored from `core/constants.cpp`
and pinned to it by tests/test_version.py), and its version from `core/version.h`
via `build/version.py` — never from a copy.

⚠ THE BUNDLE CARRIES NO DSL TOOLCHAIN AT ALL. It used to ship `payload/dsl/` —
build_objs.py, photon_dsl.py and the two `.phdsl` files — for the one install-time
step that could not be pre-baked: the RealWings live patch, which rewrites the
user's already-installed mod files in place and so has to resolve each replacement
light's parameters against the DSL. Those parameters are now RESOLVED AT BUNDLE TIME
into `payload/realwings_patch.json` (see `stage_realwings_patch` and
build/patch_realwings.py) and applied by `core/patch_realwings.cpp`. The skin-glow
redirect and the two `.acf` patchers never needed the DSL and are C++ outright. Net
effect: install time has zero DSL dependency, which is the precondition for the C++
installer (docs/installer_cpp_plan.md §2).

The plugin is the native `.xpl` (no XPPython3/Python at runtime). `--plugin-dir`
points at a `ToLissPhoton/` folder holding one `<arch>/ToLissPhoton.xpl` per
platform, and X-Plane loads the subfolder matching the host OS. The FORMAT still
allows all three side by side, but nothing ships that way any more: the bundle that
did — Python-Universal, the only one whose installer ran on every OS — is gone, and
each per-platform download now carries the one arch its own runner compiled. See
.github/workflows/release.yml and src/native/README.md.

Usage:
    python build/make_release.py [--out DIR] [--plugin-dir DIR] [--bundle]
    python build/make_release.py --payload-only [--out DIR]

Output (default --out release/, gitignored like dist/) — the STAGING layout, which
is what the binary reads when run from a checkout, not a download:
    release/photon-installer[.exe]                    the compiled installer
    release/payload/objs/<stock|durantula|realwings>/<obj filename>
    release/payload/objs/interior/lights_inn.obj      (wing- & airframe-independent)
    release/payload/objs/screens/lights_screens.obj   (display glow, A3xx)
    release/payload/textures/interior/*.png           (Gus's set, ~57 MiB)
    release/payload/plugin/ToLissPhoton/<arch>/ToLissPhoton.xpl
    release/payload/plugindata/{panelfx.txt,overlays/*.png}   (into the plugin folder)
    release/payload/realwings_patch.json              (resolved from the DSL)

`--bundle` additionally packs the DISTRIBUTABLE per-platform bundle, for the OS
this runs on (the C++ toolchain cannot cross-compile, so one machine produces one
platform's download; CI runs the same command on each of the three runners):

    release/ToLissPhoton-Installer-v<VER>-<Windows|macOS|Linux>[.zip|.tar.gz]
      └ ToLissPhoton-Installer-v<VER>-<OS>/
          photon-installer[.exe]   the installer
          data/                    objs/ + textures/ + plugin/<arch> + plugindata/
                                   + realwings_patch.json
          README.txt

⚠ THE BUNDLE CALLS THE PAYLOAD FOLDER `data/`, not `payload/`. The binary accepts
both (`core/payload.cpp` prefers `payload/`, then `data/`) precisely so a repo
checkout with a staged `payload/` and a shipped bundle can both work; `data/` is
the name the download has always used and the one its README documents.
Compression is per-OS: Windows/macOS `.zip` (Explorer/Finder open it natively),
Linux `.tar.gz` (convention, and it preserves the exec bit natively). The exec bit
is written into the `.zip` external attrs too, so a macOS extraction is runnable
without a manual `chmod`.

`--payload-only` stages just `payload/` (no installer copy, no README, no archive,
and no plugin unless one is built) into `--out`, defaulting to the REPO ROOT so it
lands beside the repo root. That is the dev path: point the freshly built
`photon-installer` at it with `--payload-dir`, or copy the binary next to it. The
Python installer used to build OBJs on the fly from the DSL when run out of a
checkout, which made the toolchain an install-time dependency in the one place it
had to not be; staging is what replaced that.
"""
from __future__ import annotations

import argparse
import platform
import shutil
import sys
import tarfile
import time
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "build"))
sys.path.insert(0, str(REPO))
import build_objs as B  # noqa: E402
import patch_realwings as RWBUILD  # noqa: E402  (build/patch_realwings.py)
import readme as _readme  # noqa: E402  (build/readme.py)
from constants import (  # noqa: E402  (build/constants.py)
    PANELFX_FILE, PLUGIN_DATA_DIRNAME, PLUGIN_FOLDER, PLUGIN_USER_DIRS,
    WINGS, XPL_NAME,
)
from patch_realwings import PATCH_JSON  # noqa: E402  (build/patch_realwings.py)
# ⚠ THE VERSION COMES FROM core/version.h. That header is the one definition the
# plugin and the compiled installer both read, so a bundle's NAME cannot disagree
# with what the About tab reports (§8c).
from version import VERSION  # noqa: E402  (build/version.py)

# Default location of the compiled native fat-plugin folder (CMake build output).
DEFAULT_PLUGIN_DIR = REPO / "src" / "native" / "build" / PLUGIN_FOLDER

# platform.system() -> the pretty OS name used in the bundle filename + README.
OS_NAME = {"Windows": "Windows", "Darwin": "macOS", "Linux": "Linux"}


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

    Absent is only a warning HERE, where a contributor with no C++ toolchain may
    still want a staged payload to develop against. ⚠ `--bundle` turns the same
    condition into a hard error, because a bundle without the installer is a
    download with nothing to run — see `build_bundle`."""
    exe = find_installer_binary(explicit)
    if exe is None:
        print("  ! photon-installer not built — staged without it "
              "(build it: cmake --build src/native/build --config Release)")
        return None
    dest = out / exe.name
    shutil.copyfile(exe, dest)
    dest.chmod(dest.stat().st_mode | 0o111)   # keep it executable on macOS/Linux
    print(f"  staged {dest.name} ({exe.stat().st_size // 1024} KiB) from {exe}")
    return dest


def payload_arch(payload: Path) -> str | None:
    """The one plugin arch this payload carries, for the README's contents list."""
    return next((d.name for d in (payload / "plugin").rglob("*")
                 if d.is_dir() and d.name.endswith("_x64")), None)


def stage_readme(out: Path, os_name: str, exe_name: str, arch: str | None):
    (out / "README.txt").write_text(
        _readme.render(os_name, exe_name=exe_name, arch=arch), encoding="utf-8")
    print(f"staged README.txt ({os_name})")


def _archive(folder: Path, base_name: str, fmt: str, out_dir: Path,
             exe_names: set[str]) -> Path:
    """Pack `folder` into out_dir/<base_name>.<ext>, everything nested under a
    single top-level <base_name>/ dir. Every name in `exe_names` is marked
    executable (0o755) in the archive metadata so Unix extractions are runnable
    without a manual chmod; Windows ignores it.

    ⚠ THE EXEC BIT HAS TO BE WRITTEN, not inherited. A zip built without it hands
    macOS and Linux a binary that is present, correct and not runnable — which
    reads to the user as a corrupt download rather than as a permissions bit."""
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
                mode = 0o755 if f.name in exe_names else 0o644
                zi.external_attr = (mode & 0xFFFF) << 16
                zf.writestr(zi, f.read_bytes())
        return dest
    # tar.gz — tarfile preserves the on-disk mode; force 0o755 on the binaries.
    dest = out_dir / f"{base_name}.tar.gz"
    if dest.exists():
        dest.unlink()

    def _filter(ti: tarfile.TarInfo) -> tarfile.TarInfo:
        ti.mode = 0o755 if Path(ti.name).name in exe_names else (
            0o755 if ti.isdir() else 0o644)
        return ti

    with tarfile.open(dest, "w:gz") as tf:
        tf.add(folder, arcname=base_name, filter=_filter)
    return dest


def build_bundle(out: Path, payload: Path, exe: Path | None,
                 base_name: str, fmt: str) -> Path:
    """Assemble and pack the distributable per-platform bundle:
    <out>/<base_name>/{photon-installer[.exe], data/, README.txt}.

    ⚠ A MISSING BINARY IS FATAL HERE. Staging a payload without one is a
    reasonable dev state; shipping a download whose only executable is absent is
    not, and the failure would surface as a user unzipping 60 MiB with nothing to
    double-click."""
    if exe is None:
        raise SystemExit(
            "cannot build a bundle without photon-installer — build it first:\n"
            "  cmake --build src/native/build --config Release\n"
            "or pass --installer-exe at the binary.")
    stage = out / base_name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    binary = stage / exe.name
    shutil.copy2(exe, binary)
    binary.chmod(binary.stat().st_mode | 0o111)
    # ⚠ `data/`, not `payload/` — the name the download has always used and the
    # one its README documents. The binary accepts both (core/payload.cpp).
    shutil.copytree(payload, stage / "data")
    arch = payload_arch(payload)
    os_name = OS_NAME.get(platform.system(), platform.system())
    stage_readme(stage, os_name, exe.name, arch)
    archive = _archive(stage, base_name, fmt, out, {exe.name})
    print(f"  bundle: {base_name}/ ({exe.name} + data/, arch: {arch or 'none'})")
    return archive


def main():
    ap = argparse.ArgumentParser(description="Build the ToLiss Photon installer release bundle")
    ap.add_argument("--out", help="output dir (default: release/, or the repo "
                                  "root with --payload-only)")
    ap.add_argument("--plugin-dir", default=str(DEFAULT_PLUGIN_DIR),
                    help="native fat-plugin folder holding <arch>/%s "
                         "(default: the CMake build output under src/native/build/)" % XPL_NAME)
    ap.add_argument("--bundle", action="store_true",
                    help="also pack the distributable per-platform bundle "
                         "(<name>/{photon-installer, data/, README.txt} + archive)")
    ap.add_argument("--format", choices=["auto", "zip", "tgz"], default="auto",
                    help="archive format (default: auto = tgz on Linux, zip elsewhere)")
    ap.add_argument("--name", help="override the bundle base name "
                                   "(default ToLissPhoton-Installer-v<VER>-<OS>)")
    ap.add_argument("--payload-only", action="store_true",
                    help="stage only payload/ (the dev path: stage beside "
                         "the repo root, then point --payload-dir at it). Tolerates "
                         "an unbuilt plugin.")
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
              f"run it with: photon-installer --payload-dir {payload}")
        return

    # ⚠ NO README AT THE STAGING ROOT. It documents `data/` beside the binary, and
    # this level has `payload/` — a README describing a layout the reader is not
    # looking at is worse than none. It is written into the bundle instead.
    exe = stage_compiled_installer(
        out, Path(args.installer_exe) if args.installer_exe else None)

    if args.bundle:
        os_name = OS_NAME.get(platform.system(), platform.system())
        base_name = args.name or f"ToLissPhoton-Installer-v{VERSION}-{os_name}"
        fmt = ("tgz" if platform.system() == "Linux" else "zip") \
            if args.format == "auto" else args.format
        archive = build_bundle(out, payload, exe, base_name, fmt)
        print(f"\nplatform bundle ready: {archive} "
              f"({archive.stat().st_size / 1e6:.1f} MB)")
        return

    print(f"\nrelease staged: {out}")


if __name__ == "__main__":
    main()
