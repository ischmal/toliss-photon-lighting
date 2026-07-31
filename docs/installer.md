# Installer, release tooling & CI

Detail reference for the end-user installer and the release pipeline. `CLAUDE.md` keeps
only the commands and the tripwires; this file has the rationale.

---

## What it is

An end-user-facing TUI installer, separate from the dev-only `build/build_objs.py build
--write` workflow. Zero third-party deps (stdlib only: `msvcrt`/`termios`, `argparse`,
`json`, `zipfile`).

**Run from the dev tree:** `python install.py [--dry-run] [--xplane-root PATH]`. With no
`payload/` next to it, install/uninstall actions raise `installer.actions.ActionError`;
detection and navigation still work for UI iteration.

**Module layout** (`installer/`):

| Module | Job |
|---|---|
| `tui.py` | ANSI/keyboard engine — `render`/`menu`/`text_prompt`/`flash`, alt-screen, fixed header/footer with a scrolling body |
| `detect.py` | X-Plane root candidates (env var → running-from-inside-X-Plane → Steam `libraryfolders.vdf` → per-OS dirs); aircraft scan; `xplane_running()` process guard |
| `actions.py` | The **only** module with real filesystem writes — backup/install/uninstall, all behind `dry_run`, atomic temp-file + `os.replace` |
| `payload.py` | Locates installable artifacts relative to `install.py`'s own path, never CWD (two modes, below) |
| `constants.py` | `VERSION`, `AIRFRAMES`, `WINGS_FOR`, `GLOW_AIRFRAMES`, `REALWINGS_DIR` |
| `log.py` | Install log |

There is **no XPPython3 check anymore** — the native plugin has no prerequisite.

---

## payload.py has two modes

Chosen per call (never cached, so tests can repoint `PAYLOAD_DIR`):

- **bundled** — reads the pre-built `payload/` beside `install.py` (a shipped release).
- **dev** — no `payload/`, running straight from the repo: falls back to the repo's own
  build system. OBJs are generated on the fly from the DSL via `build_objs.Emitter`
  (`payload.obj_text`); the native plugin folder comes from the CMake build output
  `src/native/build/ToLissPhoton/` (**so it must be built first — dev mode can't compile
  it**); the RealWings/glow patches import from the repo's `build/`.

`payload.mode()` reports which is active and is logged at startup. The plugin API is
`payload.plugin_files()` (every `<arch>/ToLissPhoton.xpl` as `(rel, src)` pairs) +
`plugin_arches()`; `actions.py` copies each file into
`Resources/plugins/ToLissPhoton/<arch>/`, so it is mode-agnostic.

A single real run only ever uses one mode; the test suite exercises both in one process,
so `test_payload` clears cached `build_objs`/`patch_realwings` modules that another test
imported from a since-deleted release temp dir.

**Where `PAYLOAD_DIR` points** is decided once at import by `_default_payload_dir()`:
normally `ROOT/payload`, but a **frozen loose snapshot** instead reads a `data/` folder
sitting next to the executable — `getattr(sys, "frozen")` and
`Path(sys.executable).parent / "data"`, and only when that folder actually exists. That
`data/` is the only place the payload is surfaced to end users as loose files, and there
it's deliberately renamed from "payload" (internal jargon) to the plainer `data/`; the
module and its name stay `payload` internally. Both a frozen self-contained exe (payload
embedded under its `_MEI…` dir, no sibling `data/`) and every source/test run are
unchanged — `sys.frozen` is False outside PyInstaller — so they keep the `ROOT/payload`
branch. `test_payload.LooseFrozenPayloadTests` locks in all three.

---

## Install-status detection

`detect.photon_status` returns `(version, wing, stale)` from **two** signals:

1. A manifest at `<aircraft>/objects/Photon Backup Files/photon_manifest.json` (written by
   `actions.install`) supplies version/wing.
2. A `# ToLissPhoton version: X wing: Y` comment injected into the `lights_out*.obj` header
   (`actions._inject_marker`, read back by `_obj_marker`) tells whether the edit is *still
   live*.

With no manifest the OBJ marker is the fallback version source. **The key case:** if the
manifest survives but the OBJ marker is gone — a SkunkCrafts aircraft update reverts
`lights_out` to stock without touching `Photon Backup Files/` — the status is **stale**,
surfaced in the UI as "Needs reinstall" (yellow) and a "reinstall — update reverted it"
action label. A surviving manifest alone is **not** proof the edit is in the file; the OBJ
marker corroborates it. Keep both formats in sync if you change either.

## Backup contract

`actions._backup_once` copies the *current* file into `Photon Backup Files/` **the first
time only**, tracked in the manifest's `backed_up` list — a reinstall/upgrade never
re-backs-up, so the true original survives any number of reinstalls. Uninstall restores
every listed file byte-for-byte, then deletes the backup dir + manifest.

**Three interior things a backup cannot express**, each handled separately (see
`docs/interior_plan.md` §5 and `tests/test_interior_install.py`):

1. **Files with no stock original.** `pedals_details_{1,2}.png` are *added*, not replaced,
   so there is nothing to back up and nothing to restore — uninstall must **delete** them.
   Tracked in the manifest's `interior.added[]`, deliberately separate from `backed_up[]`.
   Without this they would linger in the aircraft folder forever.
2. **Livery overrides in another folder.** X-Plane substitutes a `.dds` for the `.png` an
   OBJ names, so a livery shipping `chairs_LIT.dds` beats our `chairs_LIT.png` in
   `objects/` and the mod is simply invisible in that livery. The install backs the file up
   under `Photon Backup Files/liveries/` and **removes** it so the PNG resolves; uninstall
   puts it back. Recorded in `interior.liveries_patched[]`. Liveries installed *later*
   aren't covered — the installer surfaces a "re-run to cover new liveries" hint rather
   than pretending otherwise.
3. **The `.acf` patch**, reversed by writing ToLiss's stock values back rather than
   restoring a snapshot — so other mods' edits to the same file survive (Durantula rewrites
   312 lines of it). See below.

## Shared-plugin removal policy

The plugin is left installed on uninstall by default (other airframes may use it);
`detect.any_other_airframe_has_photon` gates whether `install.screen_plugin_disposition`
even offers removing it.

---

## The three in-place mod patches

Eight of the nine OBJ combinations can be pre-baked into the payload. These three can't —
they rewrite the user's *already-installed* files, so they're only knowable at install time.

### RealWings (`build/patch_realwings.py`)

`actions._patch_realwings` imports the bundled copy from `payload/dsl/build/` and calls its
`run()` / `run(reverse=True)`. The manifest records `realwings_dir` + `realwings_patched`
so uninstall knows to reverse it. RealWings' own `.bak` siblings stay next to the mod files
(not moved into `Photon Backup Files/`) so its existing, tested reversal path keeps working
unmodified.

**Switching *away* from RealWings** (realwings → stock/durantula) must also reverse the
in-place mod patch — `actions.install` does this via the pre-pop `was_realwings` capture.
The stock/durantula `lights_out` carries its own wingtip nav/strobe (those fixtures are
`omit: realwings` only), so leaving the mod patched would double the wingtip lights *and*
leak an untracked patch past a later uninstall.

**Position corrections live in the DSL, not in Python.** See `docs/dsl.md` and the
`@realwings { offset: … }` knob on `wing_nav`/`wing_strobe` in `lights.layout.phdsl`.
CEO and NEO ship as separate mod OBJs (`Main.obj` vs `MainNEO.obj`) and needed different
corrections, so the offset is split by a dedicated **free** `wingtip` axis (`fence`=CEO,
`sharklet`=NEO) used *only* on this offset. `patch_realwings.py` picks the value by mod OBJ
filename (`…NEO.obj → sharklet`, else `fence`) and by airframe, so a319/a321 `extends`
overrides apply. Beware the name clash: the `@fence`/`@sharklet` *conditions* are unrelated
to the `group fence`/`group sharklet` blocks in the same fixtures.

**Re-patching REFRESHES, never skips.** `patch_file` re-derives an already-patched mod OBJ
from its pristine `.obj.bak` (which it never overwrites on a refresh), so tuned offsets, DSL
changes, and new Photon versions reach a previously-patched install automatically. No manual
`--reverse` is needed in the tuning loop (`--reverse` remains the uninstall path). The only
skip left: a patched OBJ whose `.bak` is missing — nothing pristine to re-derive from, so
never double-patch it. This refresh behaviour is *why* the RealWings live write had to become
the crash-safe `atomic_write_bytes` (see `docs/dev-tools.md`): the mod's `Main.obj`, the
largest file in the loop, now gets rewritten every cycle instead of only the first.

### Skin glow (`build/patch_glow.py`)

Gated on `constants.GLOW_AIRFRAMES` (currently `{"a339"}`). A pure per-region dataref token
swap in ToLiss's mesh OBJs: `AirbusFBW/ExternalLightBrightnesses[N]` →
`ToLissPhoton/exterior/glow[N]`, same `N`, trailing nits untouched. `actions.install` backs
up each affected mesh OBJ (via the normal `backed_up` list) **before** patching; uninstall
restores from backup, so no `.bak` siblings are written (`--reverse` exists for the dev
path only). Full rationale and the index table: `docs/a339_plan.md` §4.

### Cockpit spots (`build/patch_acf.py`)

Part of the **opt-in interior** install only. ToLiss defines three sim 3D cockpit spot
lights in the aircraft's `.acf`, color included — which means their color can only be
changed by editing the file, so it can't follow a runtime switch. The patch writes Gus's
geometry values, **disables all three** (`_spot_name_3d/{0,1,2} = none`, the A339's own
idiom), and the interior OBJ re-adds them as `ANIM_show`-gated lights instead.

All four `.acf` per airframe are patched, found **by content** (a `_spot_name_3d/0` line),
never by an assumed `a320{,_StdDef,_XP11,_XP11_StdDef}.acf` naming — the folder or file may
have been renamed. `.bak` siblings are excluded: Durantula keeps an `a320.acf.durantula.bak`
of this very file, and patching it would corrupt *its* uninstall.

Two traps, both with regression tests in `tests/test_patch_acf.py`:

- **⚠ XP12 and XP11 render the same values differently** — `-70.000000000` vs `-70.0`. A
  literal string patch works on the two XP12 files and **silently no-ops on both XP11
  ones**. So the patcher always parses numerically and re-renders in each file's own
  detected style, and detection compares numerically with a tolerance.
- **⚠ `.acf` files are CRLF**; reading or writing them without `newline=""` rewrites every
  line ending in the file. (Caught by the byte-for-byte uninstall test, not by eye.)

Install-state detection is by **presence of our lines, never a hash** — the `.acf`
legitimately carries other mods' edits. The sentinel is `_spot_name_3d/* == "none"`, a
*string* value, which sidesteps the float-format problem entirely and is the property that
actually means "our patch is live"; `verify_geometry()` corroborates numerically.

**Coexistence.** We touch only `acf/_spot*`, disjoint from Durantula's `_obja/*`, so its
edits and ours don't collide. We are **not** safe from its *uninstall*: restoring a `.bak`
that predates our patch wipes it. The presence check reports that as needs-reinstall — the
same ordering hazard as the RealWings interaction, documented rather than prevented.

---

## Release bundles

Each release ships as four downloads (all named `ToLissPhoton-Installer-v<VER>-<X>`), plus
an all-platforms zip:

- **Windows / macOS / Linux** — a per-platform *frozen* bundle: the PyInstaller installer
  binary + a `data/` folder (that platform's own-arch plugin only) + `README.txt`, packed so
  it extracts to one folder. Compression is per-OS: **Windows/macOS `.zip`, Linux
  `.tar.gz`** (convention + native exec-bit preservation). The binary is named to match
  (`…-Windows.exe`, `…-macOS`, `…-Linux`).
- **Python-Universal** (`…-Python-Universal.zip`) — the loose `.py` bundle (`install.py` +
  `installer/` + `payload/` + `README.txt`), run with `python install.py`. Needs only Python
  3.8+, works on any OS, and its `payload/plugin/` is the **fat** folder (all three arches).
  The "read it before you run it" option for the exe-wary.
- **All-Platforms** (`…-All-Platforms.zip`) — the four archives bundled into one file, a
  maintainer convenience.

**⚠ Size.** The interior mod ships Gus's texture set bundled (`payload/textures/interior/`,
11 PNGs, ~57 MiB), staged **once** — it is wing- and airframe-independent, not ×3. That puts
each distributed bundle at roughly 65 MB against the former ~8.5 MB, so a release publishes
on the order of 280 MB. Within GitHub's limits; the costs are CI time and download size.
`knobs_LIT.png` alone is 16.8 MB against stock's 148 KB and looks like an uncompressed
export — recompression is tabled (decision #5), not a blocker.

The interior OBJ is likewise staged **once**, at `payload/objs/interior/lights_inn.obj`,
outside the per-wing loop. ⚠ Writing it per-airframe under `payload/objs/<wing>/` would put
all three airframes on the *same path* and silently collapse them into one file.

### The build scripts

**`build/make_release.py`** builds the Python-Universal bundle: stages `release/` =
`install.py` + `installer/` + `payload/` + `README.txt`. It pre-builds every supported OBJ
via `build_objs.Emitter` (consulting `SUPPORTED_WINGS`), stages the native fat-plugin
folder, and copies a **relative-layout-preserving** copy of the DSL toolchain
(`release/payload/dsl/build/{build_objs,photon_dsl,patch_realwings,patch_glow,patch_acf}.py` +
`release/payload/dsl/src/lights/*.phdsl`). `build_objs.py` locates its config two
directories up from its own file, **so flattening that copy silently breaks it** — hit and
fixed once already; keep the nesting. `--zip` packs into `release/…-Python-Universal.zip`
extracting to one same-named folder (written *inside* `release/` and skipped from its own
contents). It also stays the payload provider that `make_exe --skip-release` consumes.

**Native plugin staging.** `--plugin-dir` points at the `ToLissPhoton/` folder containing
`<arch>/ToLissPhoton.xpl` (`arch` = `win_x64|mac_x64|lin_x64`), defaulting to the CMake
output `src/native/build/ToLissPhoton`. A **fat plugin** can carry all three side by side;
X-Plane loads the one matching the host OS. Since the C++ toolchain can't cross-compile, one
machine only produces one arch — locally a one-arch bundle is fine for testing; CI assembles
all three. `make_release.py` errors clearly if the folder is missing or empty.

**`build/make_exe.py`** builds ONE per-platform frozen bundle, for the OS it runs on
(PyInstaller can't cross-compile). It freezes `install.py` one-file `--console` *without*
`--add-data` (payload ships beside the exe, not inside), then stages `<name>/{exe, data/,
README.txt}` and packs it (`zip`/`tgz` via `--format auto`). `data` (not `bin`) is
deliberate — it holds OBJs/plugin/DSL, assets the installer consumes. **No source change was
ever needed to freeze:** `detect._running_from_inside_xplane` already prefers `sys.argv[0]`
(the exe's real path when frozen), and the dynamic `import build_objs` the patches do
resolves off `sys.path` from the shipped `data/dsl/build/`.

**`build/make_allzip.py`** zips whatever `ToLissPhoton-Installer-v<VER>-*.{zip,tar.gz}`
archives it's pointed at (`--archives DIR`) into the All-Platforms zip (stored, not
re-deflated).

**`build/readme.py`** is the single source for every bundle's `README.txt`: per-kind text
(Windows/macOS/Linux/Python) with the installer's own splash art (imported from
`install.LOGO_ART`, kept ASCII-only) and the project links (`GITHUB_URL` / `XPLANE_ORG_URL`
from `installer.constants`).

---

## CI (`.github/workflows/release.yml`)

A four-job chain, since neither the C++ plugin nor PyInstaller cross-compiles:

1. **`build`** (matrix win/mac/linux) — fetches the X-Plane SDK, compiles that OS's `.xpl`
   *and* freezes+packs its platform bundle on the same runner. Uploads `bundle-<OS>` plus a
   short-lived `xpl-<arch>` (`retention-days: 1`), the only cross-job hand-off.
2. **`universal`** — merges the three `xpl-*` (`download-artifact merge-multiple`) into a fat
   plugin, runs `make_release --plugin-dir … --zip`, uploads `bundle-Python-Universal`.
3. **`collect`** — downloads all `bundle-*`, runs `make_allzip`, uploads `all-platforms`.
4. **`release`** (tag `v*` **only**) — attaches the four `bundle-*` archives to a GitHub
   Release; `generate_release_notes`.

A manual `workflow_dispatch` run does everything *except* `release`, so bundles can be
downloaded and tested before publishing.

**Gotchas.** The manual **Run workflow** button only appears once `release.yml` is on the
default branch (`main`); nothing auto-runs on a branch push (no `on: push: branches`) — only
a `v*` tag or the manual dispatch triggers it. Locally, `make_release.py`'s
`shutil.rmtree(release/)` can hit a transient WinError 5 when OneDrive/AV holds a handle on
freshly-written payload files — just re-run (or `rm -rf release` first). Requires `pip
install pyinstaller` locally (build-only dep, not shipped).

**Not yet exercised by a real tag push.** The first `v*` tag is the true end-to-end test —
watch for: SDK URL still live, macOS unsigned-`.xpl`/notarization, and MSVC-vs-VS18
compiler differences on the Windows runner. macOS ships a universal (arm64+x86_64) `.xpl`
via `CMAKE_OSX_ARCHITECTURES`; codesign/notarize is still not done (unsigned plugins load,
but a downloaded `.xpl` may be Gatekeeper-quarantined until the user clears it).

---

## Tests

`tests/`, stdlib `unittest` (no pytest in this environment):
`python -m unittest discover -s tests -v`

- `test_detect.py` / `test_actions.py` — pure filesystem tests against fake trees in
  `tempfile.mkdtemp()`. `test_actions.py` builds a **real release** via `make_release.py`
  once per module (`setUpModule`), so install/uninstall — including the RealWings live-patch
  round trip and the a339 glow round trip — run against real bundled artifacts, not stubs.
- **The one deliberate stub:** the native plugin is a fake fat-plugin folder
  (`<arch>/ToLissPhoton.xpl` for all three arches, distinct bytes, passed via
  `--plugin-dir`), so the suite exercises the multi-arch install path **without needing the
  C++ toolchain**. The tests assert each `.xpl` lands byte-for-byte.
- `test_tui.py` drives `menu()`/`text_prompt()` with a scripted `ScriptedKeys` callable
  swapped onto `tui.read_key` (a bare module-level function, so no production refactor).
- `test_install_e2e.py` drives `install.main()` the same way. **`install.py` calls
  `tui.read_key()` via the module, not `from … import read_key`,** specifically so this
  monkeypatch reaches it — keep that pattern for any new blocking key-read.
- `test_patch_realwings.py` / `test_patch_glow.py` / `test_atomic_write.py` import the
  repo's `build/` modules in `setUpModule`, **not at module level** — `test_actions`
  (alphabetically earlier) caches bundled copies from a deleted release temp dir, and clears
  its own module cache on teardown.
- `test_watch.py` imports `watch` at module level (watch.py only imports stdlib), so it
  doesn't hit that module-cache issue.
