#!/usr/bin/env python3
"""
Build the ToLiss Photon installer release bundle.

Pre-builds the 9 OBJs (3 wing variants x 3 airframes) and stages the compiled
native plugin, so the end-user installer needs neither the DSL toolchain nor a C++
toolchain.

⚠ THERE IS ONE INSTALLER, COMPILED, IN TWO BUILDS — and a bundle now carries BOTH
(2026-08-15). `photon-installer` is the Slint GUI, the thing users double-click;
`photon-installer-console` is the SAME installer compiled without the GUI
(PHOTON_GUI=OFF): the full-screen terminal wizard, same flow, same actions, same
payload. It ships because the GUI renders through OpenGL and on a machine where GL
is broken-but-present (Remote Desktop, old drivers, VMs) the GUI opens as a BLACK
WINDOW — at which point the user cannot be told anything by the GUI itself, and
the console binary is the one that always works. The README's install steps still
name exactly one thing to double-click; the console version appears only under
TROUBLESHOOTING, so nobody is handed a choice unless the first choice failed.
(The GUI binary also embeds the terminal UI behind `--tui` and a CPU renderer
behind `--software`, but a user staring at a black window discovers neither flag —
a second .exe with "console" in its name is the discoverable fallback.)

The Python installer is still GONE — `install.py`, `installer/`,
`build/make_exe.py`, the PyInstaller dependency, the Python-Universal bundle and
the parity harness were all deleted on 2026-08-09 (docs/installer_cpp_plan.md §9).

⚠ WHICH BINARY IS WHICH IS DECIDED BY CONTENT, NEVER BY FOLDER. Both builds emit a
file called `photon-installer[.exe]`, and which tree holds which FRONT-END differs
by machine: locally `build/` is deploy.ps1's tree (no GUI) while `build-gui/` has
the GUI; on CI `build/` IS the GUI tree. A location rule already shipped the wrong
front-end once (see release.yml's history). The GUI links Slint, whose backend
selector embeds the literal `SLINT_BACKEND`; the console build cannot contain it.
`installer_is_gui` reads the bytes.

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

⚠ Prefer `src/native/run-installer.ps1` over calling this by hand for a test
install: it builds the plugin first, restages when the payload has gone stale, and
points the binary at the tree it just wrote. THE SCRIPT IS THE DOCUMENTATION.

Output (default --out release/, gitignored like dist/) — the STAGING layout, which
is what the binary reads when run from a checkout, not a download:
    release/photon-installer[.exe]                    the compiled installer
    release/payload/objs/<stock|durantula|realwings>/<obj filename>
    release/payload/objs/interior/lights_inn.obj      (wing- & airframe-independent)
    release/payload/objs/screens/<airframe>/lights_screens.obj  (display glow)
    release/payload/textures/interior/*.png           (Gus's set, ~57 MiB)
    release/payload/plugin/ToLissPhoton/<arch>/ToLissPhoton.xpl
    release/payload/plugindata/{panelfx.txt,overlays/*.png}   (into the plugin folder)
    release/payload/realwings_patch.json              (resolved from the DSL)

`--bundle` additionally packs the DISTRIBUTABLE per-platform bundle, for the OS
this runs on (the C++ toolchain cannot cross-compile, so one machine produces one
platform's download; CI runs the same command on each of the three runners):

    release/ToLissPhoton-Installer-v<VER>-<Windows|macOS|Linux>[.zip|.tar.gz]
      └ ToLissPhoton-Installer-v<VER>-<OS>/
          photon-installer[.exe]           the installer (GUI)
          photon-installer-console[.exe]   the console version (black-window fallback)
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
and no plugin unless one is built). That is the dev path: point the freshly built
`photon-installer` at it with `--payload-dir`, or copy the binary next to it. The
Python installer used to build OBJs on the fly from the DSL when run out of a
checkout, which made the toolchain an install-time dependency in the one place it
had to not be; staging is what replaced that.

⚠ THERE IS EXACTLY ONE PAYLOAD LOCATION, `release/payload/`, AND `--payload-only`
USES IT TOO (2026-08-14). It used to default `--out` to the REPO ROOT, staging to
`<repo>/payload/` — while `run-installer.ps1` ran the binary with
`--payload-dir release/payload`. So `-Payload` diligently rebuilt one tree and
every test install read the OTHER, which was whatever an earlier full run had left
there. The symptom is the worst kind: the installer works, reports success, and
installs a stale `.xpl` and stale OBJs, which in-sim is indistinguishable from a
half-finished install. Two staging roots is the bug; one is the fix. (`release/`
is also gitignored, which the repo root is not — the old default left a ~57 MiB
untracked tree sitting in `git status`.)

⚠ AND NOTHING HERE BUILDS THE PLUGIN — `stage_plugin` COPIES one. That is right
for CI (the matrix builds the `.xpl` in the same job, then calls this) and was the
other half of the same stale-install bug locally, where the only thing that ever
refreshed `src/native/build/ToLissPhoton/` was `deploy.ps1`. `run-installer.ps1`
now builds it before staging; `stage_plugin` additionally REFUSES an `.xpl` older
than the plugin sources, so the mistake cannot be made silently from any caller.
"""
from __future__ import annotations

import argparse
import os
import platform
import shutil
import stat
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
    SCREENS_AIRFRAMES, WINGS, XPL_NAME,
)
from patch_realwings import PATCH_JSON  # noqa: E402  (build/patch_realwings.py)
# ⚠ THE VERSION COMES FROM core/version.h. That header is the one definition the
# plugin and the compiled installer both read, so a bundle's NAME cannot disagree
# with what the About tab reports (§8c).
from version import VERSION  # noqa: E402  (build/version.py)

# Default location of the compiled native fat-plugin folder (CMake build output).
DEFAULT_PLUGIN_DIR = REPO / "src" / "native" / "build" / PLUGIN_FOLDER

# What the staged .xpl is compared against — the trees whose contents end up
# INSIDE it. Anything newer than the binary means the binary predates the change.
#
# ⚠ Deliberately not "everything under src/". The DSL and the overlays are payload
# inputs, not plugin inputs: they are restaged every run and their being newer says
# nothing about whether the .xpl needs rebuilding. Listing them here would make the
# check cry wolf on every .phdsl edit, and a check that always fires is one that
# gets passed --allow-stale-plugin as a reflex.
PLUGIN_SOURCE_PATHS = (
    REPO / "src" / "native" / "src",
    REPO / "src" / "native" / "CMakeLists.txt",
)

# ⚠ `src/installer/` is EXCLUDED from the freshness comparison: it is compiled
# into `photon-installer` only, never the `.xpl` (CMakeLists: ToLissPhoton =
# plugin.cpp + imgui + sysmetrics + photoncore), so no plugin rebuild can ever
# freshen the binary against an edit there — the check would fire on EVERY
# installer change and stay red until an unrelated plugin source moved. A check
# that always fires gets --allow-stale-plugin typed as a reflex, which is the
# note above proven right: this fired on the first installer-only edit after it
# shipped (2026-08-15).
PLUGIN_SOURCE_EXCLUDE = (REPO / "src" / "native" / "src" / "installer",)

# platform.system() -> the pretty OS name used in the bundle filename + README.
OS_NAME = {"Windows": "Windows", "Darwin": "macOS", "Linux": "Linux"}


def _force_writable(func, path, _exc):
    """`shutil.rmtree` onexc hook: clear the read-only bit and retry once.

    ⚠ NOT DEFENSIVE PROGRAMMING — this repo lives under OneDrive, which leaves
    behind directories that are reparse points with FILE_ATTRIBUTE_READONLY set,
    and `os.rmdir` on one fails with WinError 5. rmtree gives up where it fails,
    so a restage died PART WAY THROUGH: half the payload deleted, none of it
    rebuilt, and the next run pointed at the wreckage."""
    try:
        os.chmod(path, stat.S_IWRITE)
        func(path)
    except OSError:
        raise


def _rmtree(path: Path):
    shutil.rmtree(path, onexc=_force_writable)


def _newest_mtime(paths, exclude=()) -> float:
    """The newest mtime across `paths`, walking directories. Missing paths are
    skipped — a check that cannot see its inputs must not accuse the output.
    Files under any `exclude` directory are not counted."""
    def excluded(f: Path) -> bool:
        return any(x == f or x in f.parents for x in exclude)

    newest = 0.0
    for p in paths:
        if p.is_file():
            newest = max(newest, p.stat().st_mtime)
        elif p.is_dir():
            for f in p.rglob("*"):
                if f.is_file() and not excluded(f):
                    newest = max(newest, f.stat().st_mtime)
    return newest


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
    """The display-glow OBJs, ONE PER AIRFRAME, to
    payload/objs/screens/<airframe>/lights_screens.obj.

    ⚠ Per airframe, unlike the interior — and NOT because of wing variants (there
    are none here either). The A3xx three genuinely are byte-identical, so a single
    staged copy was right while they were the only airframes. The A330-900 is a
    different flight deck against a different datum, so sharing one file would
    install the A320's six positions into it: every light in the wrong place, all
    of them still drawing, and nothing on disk to show for it. Three identical
    copies is the price of that never being possible.

    ⚠ NOT covered by `build_objs.py build` — `screens` is deliberately absent from
    DEFAULT_TARGETS so a bare build cannot slip an experiment into `dist/`. That
    makes this call the only thing that puts it in a bundle, so it is emitted
    straight from the Emitter here rather than copied out of `dist/`."""
    cfg = B.load_config(wing="stock")
    for airframe in SCREENS_AIRFRAMES:
        fname = B.OBJ_TARGETS[airframe]["screens"]
        text = B.Emitter(cfg, airframe, "stock", target="screens").emit()
        dest = payload / "objs" / "screens" / airframe / fname
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
        _rmtree(dest_root)
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
        _rmtree(dest)
    dest.mkdir(parents=True)
    total = 0
    for f in sorted(src.glob("*.png")):
        shutil.copyfile(f, dest / f.name)
        total += f.stat().st_size
    print(f"  staged payload/textures/interior/ "
          f"({len(list(dest.glob('*.png')))} files, {total / 1048576:.1f} MiB)")


def check_plugin_freshness(plugin_dir: Path, arches, allow_stale: bool):
    """⚠ IS THIS `.xpl` OLDER THAN THE CODE IN IT? A stale one is the single most
    expensive thing this script can stage, because every downstream signal says the
    run worked: the installer reports success, the files land, X-Plane loads a
    plugin — just last week's. In-sim that is indistinguishable from a
    half-finished install, and the About tab reporting an old version is the only
    hint (the same trap `tests/test_version.py` exists for).

    Nothing here BUILDS it — CI builds the `.xpl` in the same job before calling
    this, and `run-installer.ps1` builds it locally — so this is the backstop that
    makes forgetting loud from whichever caller forgot.

    ⚠ Compared against PLUGIN_SOURCE_PATHS only, and see there for why the DSL is
    not in that list. `--allow-stale-plugin` exists for the one legitimate case:
    `--plugin-dir` pointed at a merged multi-arch folder assembled from downloaded
    CI artifacts, whose mtimes mean nothing about this checkout."""
    newest_src = _newest_mtime(PLUGIN_SOURCE_PATHS, PLUGIN_SOURCE_EXCLUDE)
    if newest_src == 0.0:
        return                      # cannot see the sources; do not accuse
    stale = [(a, (plugin_dir / a / XPL_NAME).stat().st_mtime) for a in arches]
    stale = [(a, m) for a, m in stale if m < newest_src]
    if not stale:
        return
    lines = "\n".join(
        f"    {a}/{XPL_NAME}  built {time.strftime('%Y-%m-%d %H:%M', time.localtime(m))}"
        for a, m in stale)
    newest = time.strftime('%Y-%m-%d %H:%M', time.localtime(newest_src))
    message = (
        f"the plugin in {plugin_dir} is OLDER than its sources "
        f"(newest source change: {newest}):\n{lines}\n"
        f"Staging it would install a plugin that predates the code — which looks "
        f"exactly like a working install until you read the version.\n"
        f"Build it first:\n"
        f"  cmake --build src/native/build --config Release --target ToLissPhoton\n"
        f"or use src/native/run-installer.ps1, which does that for you.\n"
        f"Pass --allow-stale-plugin only when --plugin-dir holds binaries built "
        f"elsewhere (e.g. merged CI artifacts).")
    if not allow_stale:
        raise SystemExit("error: " + message)
    print(f"  ! {message.splitlines()[0]} (--allow-stale-plugin)")


def plugin_arches(plugin_dir: Path) -> list:
    """Which `<arch>/ToLissPhoton.xpl` this folder holds, refusing an empty or
    absent one. Split out of stage_plugin so main() can ask BEFORE it wipes
    anything — see preflight_plugin."""
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
    return arches


def preflight_plugin(plugin_dir: Path, payload_only: bool, allow_stale: bool):
    """⚠ EVERY REASON THIS RUN MIGHT REFUSE, ASKED BEFORE IT DELETES ANYTHING.

    main() wipes the output tree before it stages, so a refusal raised down in
    stage_plugin — the LAST thing staged — costs the caller the payload they
    already had: no plugin, no OBJs, a minute of texture copying wasted, and
    nothing to run the installer against. The checks are pure reads, so hoisting
    them is free and turns a destructive failure into a no-op one."""
    if payload_only and not plugin_dir.is_dir():
        return                          # tolerated; stage_plugin is skipped
    check_plugin_freshness(plugin_dir, plugin_arches(plugin_dir), allow_stale)


def stage_plugin(payload: Path, plugin_dir: Path):
    """Copy the compiled native fat-plugin folder into the bundle. Each platform's
    build produces one <arch>/ToLissPhoton.xpl; a full release carries all three
    side by side (CI merges the per-OS build matrix). Locally only the arch you
    built is present — fine for a single-platform test bundle.

    Validation lives in preflight_plugin, which main() runs before the wipe."""
    arches = plugin_arches(plugin_dir)
    dest_root = payload / "plugin" / PLUGIN_FOLDER
    if dest_root.exists():
        _rmtree(dest_root)
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
    # ⚠ The .xpl's IDENTITY, printed every run. copytree preserves mtime, so this
    # is the build time of the binary a tester is about to install — the one fact
    # that would have made a day-old plugin obvious instead of invisible.
    for a in arches:
        st = (plugin_dir / a / XPL_NAME).stat()
        print("    %s/%s  %.2f MiB  built %s"
              % (a, XPL_NAME, st.st_size / 1048576,
                 time.strftime('%Y-%m-%d %H:%M', time.localtime(st.st_mtime))))
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
# ⚠ THE FOLDER SAYS NOTHING ABOUT THE FRONT-END — locally `build/` is the no-GUI
# deploy tree and `build-gui/` the GUI; on CI `build/` is the GUI tree and
# `build-console/` the other. Every candidate is CLASSIFIED by content
# (`installer_is_gui`) and the search below simply takes the first of each kind.
INSTALLER_EXE_NAMES = ("photon-installer.exe", "photon-installer")
# ⚠ THE DEDICATED INSTALLER TREES COME BEFORE `build/`. The deploy tree also
# compiles a (console) photon-installer as a side effect of the plugin build,
# but nothing keeps it fresh — run-installer.ps1 rebuilds build-gui/build-core
# on every run and only ever builds `--target ToLissPhoton` in build/. Searched
# first, build/'s copy shadowed a just-built build-core binary with one from
# whenever the whole deploy tree last compiled.
INSTALLER_BUILD_DIRS = (
    REPO / "src" / "native" / "build-gui",
    REPO / "src" / "native" / "build-core",
    REPO / "src" / "native" / "build-console",
    REPO / "src" / "native" / "build",
)

# What the console build is CALLED in a bundle. On disk both builds emit
# `photon-installer`; the rename happens at staging time, and the binary does not
# read its own name (the payload resolves from its directory, not its argv[0]).
CONSOLE_EXE_NAMES = {"photon-installer.exe": "photon-installer-console.exe",
                     "photon-installer": "photon-installer-console"}

# ⚠ CONTENT, NEVER LOCATION. The GUI links Slint, whose backend selector embeds
# this env-var name; a build without the GUI cannot contain it. The same
# content-over-naming rule detection uses on aircraft folders, for the same
# reason: a location rule already shipped the wrong front-end once (release.yml).
GUI_BINARY_MARKER = b"SLINT_BACKEND"


def installer_is_gui(exe: Path) -> bool:
    return GUI_BINARY_MARKER in exe.read_bytes()


def find_installer_binary(explicit: Path | None = None,
                          gui: bool = True) -> Path | None:
    """The compiled `photon-installer` of the requested FRONT-END, or None.

    `explicit` short-circuits the search but not the classification — callers
    that must not mislabel a binary (the bundle) still check `installer_is_gui`
    on what this returns."""
    if explicit is not None:
        return explicit if explicit.is_file() else None
    for base in INSTALLER_BUILD_DIRS:
        for sub in ("", "Release", "RelWithDebInfo"):
            for name in INSTALLER_EXE_NAMES:
                p = base / sub / name if sub else base / name
                if p.is_file() and installer_is_gui(p) == gui:
                    return p
    return None


def stage_compiled_installer(out: Path, explicit: Path | None = None,
                             console_explicit: Path | None = None
                             ) -> tuple[Path | None, Path | None]:
    """Stage BOTH compiled installers beside `payload/` — the GUI under its own
    name, the console build renamed `photon-installer-console`.

    ⚠ THEY GO NEXT TO `payload/`, NOT INSIDE IT. The binary resolves its payload
    from its OWN directory rather than the working directory — that is what lets
    a user run it from Downloads, from a USB stick, or from inside
    Resources/plugins/ — so all three have to be siblings.

    Absent is only a warning HERE, where a contributor with no C++ toolchain (or
    no Rust, for the GUI) may still want a staged payload to develop against.
    ⚠ `--bundle` turns the same conditions into hard errors — see `build_bundle`."""
    def stage(exe: Path | None, label: str, rename: dict | None) -> Path | None:
        if exe is None:
            return None
        dest = out / (rename.get(exe.name, exe.name) if rename else exe.name)
        shutil.copyfile(exe, dest)
        dest.chmod(dest.stat().st_mode | 0o111)   # executable on macOS/Linux
        print(f"  staged {dest.name} ({exe.stat().st_size // 1024} KiB, {label}) "
              f"from {exe}")
        return dest

    gui = stage(find_installer_binary(explicit, gui=True), "GUI", None)
    if gui is None:
        print("  ! photon-installer (GUI) not built — staged without it "
              "(build it: cmake -B src/native/build-gui -DPHOTON_GUI=ON "
              "-DPHOTON_CORE_ONLY=ON, then --build)")
    console = stage(find_installer_binary(console_explicit, gui=False),
                    "console", CONSOLE_EXE_NAMES)
    if console is None:
        print("  ! photon-installer-console not built — staged without it "
              "(build it: cmake -B src/native/build-core -DPHOTON_GUI=OFF "
              "-DPHOTON_CORE_ONLY=ON, then --build)")
    return gui, console


def payload_arch(payload: Path) -> str | None:
    """The one plugin arch this payload carries, for the README's contents list."""
    return next((d.name for d in (payload / "plugin").rglob("*")
                 if d.is_dir() and d.name.endswith("_x64")), None)


def stage_readme(out: Path, os_name: str, exe_name: str, arch: str | None,
                 console_name: str | None = None):
    (out / "README.txt").write_text(
        _readme.render(os_name, exe_name=exe_name, arch=arch,
                       console_name=console_name),
        encoding="utf-8")
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
                 console: Path | None, base_name: str, fmt: str) -> Path:
    """Assemble and pack the distributable per-platform bundle:
    <out>/<base_name>/{photon-installer[.exe], photon-installer-console[.exe],
    data/, README.txt}.

    ⚠ A MISSING BINARY IS FATAL HERE — EITHER OF THEM. Staging a payload without
    them is a reasonable dev state; shipping a download whose executable is
    absent is not. And the console binary is the black-window fallback: a bundle
    without it looks complete, installs fine, and strands exactly the user whose
    GPU cannot draw the GUI — which is the report that made it part of the
    bundle in the first place.

    ⚠ THE TWO ARE CLASSIFIED BY CONTENT BEFORE THEY ARE NAMED. Both builds emit
    `photon-installer[.exe]`; a mixed-up pair would ship a bundle whose
    "console" binary opens a window (or whose GUI opens a terminal), report
    success, and pass every size check. `installer_is_gui` is the arbiter, the
    same way CI greps the configure log."""
    if exe is None:
        raise SystemExit(
            "cannot build a bundle without photon-installer (the GUI) — build it:\n"
            "  cmake -B src/native/build-gui -A x64 -DPHOTON_GUI=ON -DPHOTON_CORE_ONLY=ON\n"
            "  cmake --build src/native/build-gui --config Release\n"
            "(needs Rust >= 1.92; src/native/run-installer.ps1 does all of this)\n"
            "or pass --installer-exe at the binary.")
    if console is None:
        raise SystemExit(
            "cannot build a bundle without photon-installer-console — build it:\n"
            "  cmake -B src/native/build-core -A x64 -DPHOTON_GUI=OFF -DPHOTON_CORE_ONLY=ON\n"
            "  cmake --build src/native/build-core --config Release\n"
            "(no Rust needed; src/native/run-installer.ps1 -Tui does all of this)\n"
            "or pass --console-exe at the binary.")
    if not installer_is_gui(exe):
        raise SystemExit(
            f"{exe} does not contain the GUI (no Slint inside) — it is the console\n"
            f"build. A bundle naming it photon-installer would ship a download that\n"
            f"opens a terminal when double-clicked. Point --installer-exe at a\n"
            f"PHOTON_GUI=ON build.")
    if installer_is_gui(console):
        raise SystemExit(
            f"{console} contains the GUI — it is not the console build. A bundle\n"
            f"naming it photon-installer-console would ship a 'fallback' that fails\n"
            f"exactly when the GUI does. Point --console-exe at a PHOTON_GUI=OFF\n"
            f"build.")
    stage = out / base_name
    if stage.exists():
        _rmtree(stage)
    stage.mkdir(parents=True)
    binary = stage / exe.name
    shutil.copy2(exe, binary)
    binary.chmod(binary.stat().st_mode | 0o111)
    console_name = CONSOLE_EXE_NAMES.get(console.name, console.name)
    console_binary = stage / console_name
    shutil.copy2(console, console_binary)
    console_binary.chmod(console_binary.stat().st_mode | 0o111)
    # ⚠ `data/`, not `payload/` — the name the download has always used and the
    # one its README documents. The binary accepts both (core/payload.cpp).
    shutil.copytree(payload, stage / "data")
    arch = payload_arch(payload)
    os_name = OS_NAME.get(platform.system(), platform.system())
    stage_readme(stage, os_name, exe.name, arch, console_name=console_name)
    archive = _archive(stage, base_name, fmt, out, {exe.name, console_name})
    print(f"  bundle: {base_name}/ ({exe.name} + {console_name} + data/, "
          f"arch: {arch or 'none'})")
    return archive


def main():
    ap = argparse.ArgumentParser(description="Build the ToLiss Photon installer release bundle")
    ap.add_argument("--out", help="output dir (default: release/, for every mode "
                                  "including --payload-only)")
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
                    help="stage only release/payload/ (the dev path: stage, then "
                         "point --payload-dir at it). Tolerates an unbuilt plugin.")
    ap.add_argument("--allow-stale-plugin", action="store_true",
                    help="stage a .xpl older than the plugin sources instead of "
                         "refusing. For --plugin-dir folders built elsewhere "
                         "(merged CI artifacts), never as a way past a rebuild.")
    ap.add_argument("--installer-exe",
                    help="the compiled photon-installer (GUI) to stage (default: "
                         "found by content under src/native/build*/)")
    ap.add_argument("--console-exe",
                    help="the compiled console installer to stage as "
                         "photon-installer-console (default: found by content "
                         "under src/native/build*/)")
    args = ap.parse_args()

    # ⚠ ONE PAYLOAD LOCATION FOR EVERY MODE. --payload-only used to default here
    # to the REPO ROOT, so it staged <repo>/payload while run-installer.ps1 ran
    # the binary against release/payload — restaging one tree and installing from
    # the other. See the ⚠ in the module docstring.
    out = Path(args.out) if args.out else REPO / "release"
    if not out.is_absolute():
        out = REPO / out
    payload = out / "payload"

    plugin_dir = Path(args.plugin_dir)
    if not plugin_dir.is_absolute():
        plugin_dir = REPO / plugin_dir

    # ⚠ BEFORE THE WIPE BELOW. Everything this can refuse over is knowable from
    # reads alone, and a refusal after the wipe leaves no payload at all.
    preflight_plugin(plugin_dir, args.payload_only, args.allow_stale_plugin)

    # ⚠ --payload-only never wipes `out` — a staged bundle and the installer
    # binary beside it are not its to delete. Only the payload/ subtree it owns.
    if args.payload_only:
        if payload.exists():
            _rmtree(payload)
        payload.mkdir(parents=True)
    else:
        if out.exists():
            _rmtree(out)
        out.mkdir(parents=True)

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
    exe, console = stage_compiled_installer(
        out, Path(args.installer_exe) if args.installer_exe else None,
        Path(args.console_exe) if args.console_exe else None)

    if args.bundle:
        os_name = OS_NAME.get(platform.system(), platform.system())
        base_name = args.name or f"ToLissPhoton-Installer-v{VERSION}-{os_name}"
        fmt = ("tgz" if platform.system() == "Linux" else "zip") \
            if args.format == "auto" else args.format
        archive = build_bundle(out, payload, exe, console, base_name, fmt)
        print(f"\nplatform bundle ready: {archive} "
              f"({archive.stat().st_size / 1e6:.1f} MB)")
        return

    print(f"\nrelease staged: {out}")


if __name__ == "__main__":
    main()
