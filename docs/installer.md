# Installer, release tooling & CI

Detail reference for the end-user installer and the release pipeline. `CLAUDE.md` keeps
only the commands and the tripwires; this file has the rationale.

---

## What it is

An end-user-facing TUI installer, separate from the dev-only `build/build_objs.py build
--write` workflow. It is **`photon-installer`, compiled C++**, built by the same CMake tree
as the plugin and linking the same `photoncore` static library. No Python, no third-party
dependencies, nothing to install before installing.

⚠ **THE PYTHON INSTALLER IS GONE** (2026-08-09). `install.py`, the `installer/` package,
`build/make_exe.py`, the three `build/patch_*.py` CLI shims and `tests/test_parity.py` were
deleted together — see `installer_cpp_plan.md` §9. This document describes the C++ code that
replaced them; where it says "the installer" it means the binary. The one Python survivor on
the patch side is `build/realwings_apply.py`, and it is a DEV tool (below).

**Run from a dev tree:** stage a payload and point the binary at it —

```
python build/make_release.py --payload-only          # stage ./payload/
src/native/build/Release/photon-installer --payload-dir payload
```

— or use the CLI directly: `photon-installer detect|status|install|uninstall|screens|version`,
`--json` for machine-readable output. With no payload it still detects and navigates, so the
TUI can be iterated on; the install actions raise.

**Module layout** (`src/native/src/core/`, plus `src/native/src/installer/` for the UI):

| Module | Job |
|---|---|
| `installer/tui.cpp` | ANSI/keyboard engine — wrap/clip/frame, alt-screen, fixed header/footer with a scrolling body |
| `installer/screens.cpp` | The five screens, the splash art, the flow between them |
| `installer/cli.cpp` | The headless `detect`/`status`/`install`/`uninstall`/`screens`/`version` commands and `--json` |
| `detect.cpp` | X-Plane root candidates (env var → running-from-inside-X-Plane → Steam `libraryfolders.vdf` → per-OS dirs); aircraft scan; the running-sim guard |
| `actions.cpp` | The **only** module with real filesystem writes — backup/install/uninstall, all behind `dryRun`, atomic temp-file + rename |
| `payload.cpp` | Locates installable artifacts relative to the EXECUTABLE's own path, never CWD |
| `constants.{h,cpp}` | The airframe table, `WingsFor`, `GlowRedirect`, the interior/screens sets — the drift-killer |
| `patch_realwings.cpp` | Applies the pre-resolved RealWings wingtip patch (`realwings_patch.json`) |
| `patch_acf.cpp` | The `.acf` cockpit-spot patch (interior) |
| `patch_acf_screens.cpp` | The `.acf` screen-glow `_obja` attachment |
| `patch_glow.cpp` | The skin-glow dataref redirect |
| `manifest.cpp` | `photon_manifest.json` — read tolerantly, write completely |
| `installer/log.cpp` | Install log |

⚠ **THE STANDALONE PATCH COMMANDS ARE GONE**, with one replacement.
`build/patch_acf.py`, `build/patch_acf_screens.py` and `build/patch_glow.py` were thin CLI
shims over the Python implementations; both halves went. The cockpit-spot and skin-glow
patches now run only as part of `photon-installer install --aircraft <dir>`.

The display-glow attachment kept a standalone command, because it is the one patch a normal
dev loop needs on its own — `deploy.ps1` writes the OBJ but cannot touch the `.acf`, and a
ToLiss update reverts the attachment without touching anything else:

```
photon-installer screens --aircraft P [--detach] [--dry-run] [--json]
```

⚠ **It edits the `.acf` and NOTHING else** — no manifest, no OBJs, no payload (it never
reads one). A hand-detached aircraft whose manifest still claims `screens` will have its
uninstall report a detach that changes nothing, which is correct: `.acf` reversal writes
stock values back rather than restoring a snapshot, so the two compose. Recording it in the
manifest instead would mean a second writer of a file the install owns.

⚠ **Two gates, and the first one was a real hole.** It refuses an airframe without screens
(a339 — the six positions are the A3xx flight deck's), and it refuses a folder it cannot
IDENTIFY at all. The identify gate started as "unknown is fine" and that was wrong:
`AcfFiles` walks for anything carrying an attachment table, so `--aircraft` pointed one
level too high found a stray `.acf` and patched it, reporting success. Detection is
content-based and finds a ToLiss folder however it has been renamed, so failing to identify
one means it is not one.

There is **no XPPython3 check anymore** — the native plugin has no prerequisite.

---

## The compiled installer

`photon-installer` is the whole installer: detection, payload, actions, manifest, all four
patchers, a headless CLI the Python side never had, and the interactive TUI.

```
photon-installer                          the interactive installer
photon-installer [--dry-run] [--xplane-root P]
photon-installer detect|status|install|uninstall|screens|version … [--json]
photon-installer … --payload-dir P        read the bundle from P (any mode)
```

⚠ **A mistyped flag is an error, not a TUI.** Only `--dry-run`, `--xplane-root` and
`--payload-dir` reach the interactive mode; anything else exits 2 with a message, because
opening a full-screen alt-screen UI over the top of the user's own mistake means quitting it
to read what went wrong.

**One payload, two names.** The shipped per-platform bundle calls the folder `data/` and
`make_release.py` stages `payload/` in a checkout. ONE binary has to run out of both, so
`payload::InitFromExecutable` accepts **`payload/` first, `data/` second** — the order keeps
a staged checkout from being shadowed by a stale `data/` left over from a bundle build.

**`--payload-dir` overrides both**, and exists for the development loop: a freshly built
binary sits in the CMake output directory with no bundle beside it, and restaging one copies
~58 MiB of textures. Stage once, then rebuild-and-run. It is handled **before** the CLI/TUI
split so one implementation serves every mode, and a path that is not a directory is refused
**by name** — falling through to "this build is incomplete" would send someone to
re-download a bundle that is fine. Recipe in `src/native/README.md` § *Running
`photon-installer` while working on it*.

⚠ **`src/native/tests/install_tests.cpp` IS THE NET NOW, AND IT REPLACED ONE.** Until
2026-08-09 `detect.cpp`, `actions.cpp` and `payload.cpp` had no C++ tests at all: they were
covered from Python, plus `tests/test_parity.py`, which ran both installers over two copies
of one fake tree and diffed the results byte for byte. That harness found two shipped
line-ending bugs on its first two runs — and it could only exist while there were two
installers. Deleting the Python one deletes it, so every invariant it and the Python install
tests pinned was ported first: detection by content, the backup-once rule, `added[]` vs
`backed_up[]`, the identical-`.xpl` write skip, the RealWings reversal and its dry run, the
livery `.dds` shadow, the glow redirect's untouched indices. **A change to those three files
is only tested there.**

⚠ **The TUI is the one part no test covers.** Raw-mode key handling cannot be driven from a
pipe (on Windows `_getwch` reads the console directly, not stdin), so the string engine
underneath it is unit-tested case-for-case and the interactive walk is manual QA — a §9
cutover gate. **Windows walked and clean (2026-08-09); macOS and Linux still outstanding**,
and they are separate gates rather than a formality: the POSIX `termios` + ESC-sequence
decode shares no code with the Windows `_getwch` path, so the Windows pass says nothing
about them.

---

## payload has ONE mode: bundled

`core/payload.cpp` reads the pre-built `payload/` beside the EXECUTABLE, resolved relative
to the binary's own path and never CWD — which is what lets it run from Downloads, a USB
stick, or from inside `Resources/plugins/`. The plugin API is `PluginFiles()` (every
`<arch>/ToLissPhoton.xpl` as `(rel, src)` pairs) + `PluginArches()`; `actions.cpp` copies
each file into `Resources/plugins/ToLissPhoton/<arch>/`.

⚠ **The dev-tree fallback is GONE and must not come back** (2026-08-07,
`docs/installer_cpp_plan.md` §2b). Running the Python installer out of a checkout with no
`payload/` used to fall back to the repo's own build system and generate each OBJ on the
fly via `build_objs.Emitter`. That made the DSL toolchain an **install-time** dependency —
which a compiled installer cannot honor, and which is what forced every release to ship
`payload/dsl/` — and it meant two code paths that could drift with only a shipped bundle to
notice on. The dev path is now an explicit staging step:

```bash
python build/make_release.py --payload-only     # stages payload/ at the repo root
src/native/build/Release/photon-installer --payload-dir payload
```

`--payload-only` never wipes its `--out` (it defaults to the repo root); it replaces only
the `payload/` subtree it owns, and it tolerates an unbuilt native plugin so trying the TUI
does not require the C++ toolchain. A missing payload is now an **error naming that
command**, not a silent second code path. The C++ side cannot regrow one — it has no DSL
parser and never will (`core/payload.h` says so at the top).

⚠ **`obj_text` checks `WINGS_FOR` BEFORE it checks for a bundle.** Otherwise
`a339 + durantula` reports "this build is incomplete" and sends the user to re-download
over a combination that does not exist and never will.

**Where `PAYLOAD_DIR` points** is decided once at import by `_default_payload_dir()`:
normally `ROOT/payload`, but a **frozen loose snapshot** instead reads a `data/` folder
sitting next to the executable — `getattr(sys, "frozen")` and
`Path(sys.executable).parent / "data"`, and only when that folder actually exists. That
`data/` is the plainer end-user name for what is internally "payload"; the module and its
name stay `payload`. Both a frozen self-contained exe (payload embedded under its `_MEI…`
dir, no sibling `data/`) and every source/test run are unchanged — `sys.frozen` is False
outside PyInstaller — so they keep the `ROOT/payload` branch.
`test_payload.LooseFrozenPayloadTests` locks in all three.

⚠ **Nothing freezes this installer any more**, so the `sys.frozen` branch is no longer
reachable from a shipped artifact — the bundles carry `photon-installer` instead, and it
resolves `data/` in C++ (`core/payload.cpp`). The branch and its tests stay because the
Python installer stays: it is the reference implementation `tests/test_parity.py` diffs
the compiled one against, and that harness is worth more than the few lines are worth
deleting. Both go together when the Python half is deleted outright.

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

`actions::BackupOnce` copies the *current* file into `Photon Backup Files/` **the first
time only**, tracked in the manifest's `backed_up` list — a reinstall/upgrade never
re-backs-up, so the true original survives any number of reinstalls. Uninstall restores
every listed file byte-for-byte, then deletes the backup dir + manifest.

⚠ **It has THREE outcomes, not two, and the third is easy to collapse into "no".** Besides
"backed up" and "already have one" there is **no stock original** — the target simply was
not there — and the file the installer then writes over it belongs in `added[]`, not
`backed_up[]`. Returning a bool put it in *neither* list: not restorable, not deletable,
left in the user's aircraft forever. `BackupOnce` therefore returns a tri-state and takes
the `added[]` list to record into; **pass `nullptr` at a site that patches the file IN
PLACE** (the glow meshes), where a missing source means nothing was written and deleting
would remove a ToLiss file Photon never created. It also **consults** that list, which is
what keeps a reinstall idempotent — second time round the file exists *because we wrote
it*, and a check keyed only on existence would enshrine our own copy as the stock original.
Reachable on a damaged aircraft folder, or after a ToLiss update that stops shipping a file
we replace. Pinned by the `install:`/`interior:` no-stock-original cases in
`install_tests.cpp`.

The exterior-side list is the manifest's top-level `added[]`. ⚠ **It is written only when
non-empty**, on the `realwings_patched` precedent, so an ordinary install's manifest stays
byte for byte what every prior version wrote (`manifest:` cases in `core_tests.cpp`).

**Three interior things a backup cannot express**, each handled separately (see
`docs/interior_plan.md` §5 and the `interior:` cases in `install_tests.cpp`):

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

When it *is* removed, `actions._remove_plugin_dir` deletes the folder's contents entry by
entry and then `rmdir`s the root — ⚠ **never `rmtree(plugin_root)`**. `constants.PLUGIN_USER_DIRS`
names subfolders that hold **user** content as well as ours, currently `overlays/`, the Panel
FX compositor's image folder. A user's PNGs there are hand-made, have no backup and no stock
counterpart to restore.

**Since the compositor ships, that folder is now both ours and theirs**, and the whole
contract rests on going **by name**: install writes only the names in
`payload.plugin_data_files()`, uninstall removes only those, and a folder still holding
anything else is kept and reported in the step list (an *empty* one is removed — nothing to
lose, and stranding it would leave a plugin folder behind on every dev machine).
`deploy.ps1` seeds it file-by-file for the same reason.

`PLUGIN_USER_DIRS` still guards the two inbound paths, because `--plugin-dir` may legitimately
point at a *deployed* plugin folder (that is how a merged multi-arch bundle gets assembled)
and on a dev machine that folder holds whatever images that dev dropped there:
`payload.plugin_files` skips them so they can't be copied into someone's install, and
`make_release.stage_plugin` skips them so they can't be bundled. Our own copies reach a
bundle by a different road entirely — `stage_plugin_data`, straight from `src/native/`.

⚠ **`panelfx.txt` is skipped when it is a SYMLINK**, by both install and uninstall. A dev
keeps it linked at the repo copy so in-sim FX edits land in source control (`deploy.ps1`
maintains the link); replacing it with a plain file, or deleting it, ends that loop with
nothing to see. `is_symlink()` is checked **before** `is_file()` — the latter follows the
link, so a link whose target has moved would otherwise read as "not a file" and be deleted.

`OverlayFolderTests`, `PanelFxDataTests` and `PluginPayloadExclusionTests` in
`src/native/tests/install_tests.cpp` pins all of it.

---

## The three in-place mod patches

Eight of the nine OBJ combinations can be pre-baked into the payload. These three can't —
they rewrite the user's *already-installed* files, so they're only knowable at install time.

### RealWings — resolved at bundle time, applied at install time

⚠ **This one is SPLIT ACROSS TWO FILES, and that split is the whole point.**

| | file | when |
|---|---|---|
| resolve | `build/patch_realwings.py` | bundle time → `payload/realwings_patch.json` |
| apply | `core/patch_realwings.cpp` | install time, no DSL |

It used to be one module that imported `build_objs` — and therefore the entire DSL parser —
at **install** time, which is what forced `payload/dsl/` into every release. The numbers
(class, color, intensity, cone, direction, category dataref, and the `@realwings { offset }`
placement correction, per airframe × baked class × wingtip × side) are now resolved once by
`make_release.stage_realwings_patch` and carried as JSON; the line-rewriting mechanics —
marker comment, commented-out original, the refresh-from-`.bak` staleness fix — stayed in
the applier. Only *where the numbers come from* moved.

⚠ **`realwings_patch.json` is a GENERATED ARTIFACT, exactly like `payload/objs/`.** It
carries a `_generated` header saying so. This does **not** weaken the "all lighting tuning
lives in the DSL" tripwire, it enforces it: what the tripwire forbids is a hand-*maintained*
table, and nothing here is one. Tune in `lights.layout.phdsl` and rebuild.

⚠ **The dev tuning loop is unchanged.** `build/patch_realwings.run()` resolves the same
structure from a live `load_config()` in memory and hands it straight to the applier, so
`build_objs.py build --wing realwings --write` and `watch.py` still pick up a `.phdsl` edit
with no bundle step in between — and it passes `build_objs.atomic_write_bytes` through, so
the reload-race retry survives on that path (the installer's plain atomic write is used on
the other, where nothing is racing it).

`BundleTimeResolutionTests` in `tests/test_patch_realwings.py` is the correctness net for
the split: resolve → serialize → reload → apply, and require the patched bytes to match the
live-DSL path exactly. The failure it exists to catch is a value that survives a Python dict
but not a JSON round trip — perfect on the dev machine, subtly wrong in every shipped
bundle.

`actions._patch_realwings` calls `realwings.run()` / `run(reverse=True)`. The manifest
records `realwings_dir` + `realwings_patched` so uninstall knows to reverse it. RealWings'
own `.bak` siblings stay next to the mod files (not moved into `Photon Backup Files/`) so
its existing, tested reversal path keeps working unmodified.

⚠ **The REVERSE takes no patch data.** It only restores `.obj.bak` siblings, so it must keep
working against a bundle whose JSON is missing or a schema this installer cannot read —
otherwise a user could end up unable to undo a patch an older build applied successfully.
A missing JSON on the forward path is likewise reported as a skipped step, not a failure:
the base install is complete and the mod's baked lights simply keep their fixed colors.

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
never double-patch it. This refresh behavior is *why* the RealWings live write had to become
the crash-safe `atomic_write_bytes` (see `docs/dev-tools.md`): the mod's `Main.obj`, the
largest file in the loop, now gets rewritten every cycle instead of only the first.

### Skin glow (`core/patch_glow.cpp`)

Gated on `GlowRedirect()` in `core/constants.cpp` (currently `a339` only). A pure per-region dataref token
swap in ToLiss's mesh OBJs: `AirbusFBW/ExternalLightBrightnesses[N]` →
`ToLissPhoton/exterior/glow[N]`, same `N`, trailing nits untouched. `actions.install` backs
up each affected mesh OBJ (via the normal `backed_up` list) **before** patching; uninstall
restores from backup, so no `.bak` siblings are written (`--reverse` exists for the dev
path only). Full rationale and the index table: `docs/a339_plan.md` §4.

### Cockpit spots (`core/patch_acf.cpp`)

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

### Display-glow attachment (`core/patch_acf_screens.cpp`)

Part of the **base install on A3xx** (`constants.SCREENS_AIRFRAMES`), not an opt-in: the
glow works on a stock cockpit, so it has no reason to sit behind the interior's opt-in, and
whether it is visible is a runtime choice on the plugin's Displays tab.

Two steps, and the first alone does nothing: `objects/lights_screens.obj` is written, and
then attached to the `.acf` `_obja/*` table. ToLiss neither ships nor references that OBJ,
so **an install that wrote the file but failed to attach it produces no glow at all** — which
is why the attach result is reported as its own step rather than folded into the OBJ's.

- ⚠ **The attachment row is COPIED from the aircraft's own `lights_inn.obj` row.** That row
  carries the OBJ origin in feet against a per-airframe datum (26.00 / 20.75 / 6.78 ft on
  A319 / A320 / A321) and XP11 files use a different field set. With no such row the patcher
  **refuses** rather than guessing — a guessed datum mis-places every screen light on two
  airframes out of three, invisibly. The installer logs the refusal, reports the step as
  skipped and carries on; the rest of the install is unaffected.
- ⚠ **The OBJ is recorded in the manifest's `screens.added[]`, never `backed_up[]`** — it
  has no stock counterpart, so uninstall must **delete** it. Same rule as the interior's
  `pedals_details_*.png`.
- ⚠ **Uninstall detaches BEFORE deleting.** An `.acf` naming an OBJ that is gone warns on
  every load; an attached-but-unused row left by the reverse order is invisible until
  someone wonders why their `.acf` differs from stock.
- **⚠ This is Durantula's namespace.** Unlike the spot patch we cannot stay out of `_obja` —
  attaching an OBJ *is* an `_obja` edit. So detection is by our FILENAME, never by index and
  never a hash, and detach renumbers the rows above ours down rather than leaving a hole.
- A refusal is handled **per file**, not per run: a ToLiss folder holds four `.acf` variants
  and one that cannot be patched must not abort the other three.

⚠ **A ToLiss update reverts it**, confirmed on the A320 1.3.2 → 1.3.3 update (2026-08-01):
the update replaced the aircraft folder and took the attachment with it. Reinstalling
restores it, and since the attach is part of the base install there is nothing extra to
remember. Covered by the `screens:` cases in `core_tests.cpp` and `install_tests.cpp`.

---

## Release bundles

Each release ships as three downloads (all named `ToLissPhoton-Installer-v<VER>-<X>`), plus
an all-platforms zip:

- **Windows / macOS / Linux** — a per-platform bundle: the compiled `photon-installer`
  binary + a `data/` folder (that platform's own-arch plugin only) + `README.txt`, packed so
  it extracts to one folder. Compression is per-OS: **Windows/macOS `.zip`, Linux
  `.tar.gz`** (convention + native exec-bit preservation).
- **All-Platforms** (`…-All-Platforms.zip`) — the three archives bundled into one file, a
  maintainer convenience.

⚠ **ONE INSTALLER, ONE EXECUTABLE PER BUNDLE.** The frozen Python installer and the
**Python-Universal** bundle are both gone (`installer_cpp_plan.md` §9 / §10 decision 1),
and with them `build/make_exe.py` and the PyInstaller build dependency. Two consequences
worth stating rather than rediscovering:

- **No bundle carries a fat plugin any more.** Python-Universal was the only download whose
  installer ran on every OS, so it was the only one that could usefully carry all three
  `<arch>/` folders. Each per-platform bundle now ships the one arch its own CI runner
  compiled — which is all a user of that OS can load anyway. The fat-plugin *format* is
  untouched; nothing distributes one.
- **`install.py` and `installer/` are GONE** (2026-08-09), along with the parity harness
  and the three `build/patch_*.py` shims. The build scripts take their shared constants from
  `build/constants.py`, which mirrors `core/constants.cpp` and is pinned to it by
  `tests/test_version.py`.

**⚠ Size.** The interior mod ships Gus's texture set bundled (`payload/textures/interior/`,
11 PNGs, ~57 MiB), staged **once** — it is wing- and airframe-independent, not ×3. That puts
each distributed bundle at roughly 65 MB against the former ~8.5 MB, so a release publishes
on the order of 280 MB. Within GitHub's limits; the costs are CI time and download size.
`knobs_LIT.png` alone is 16.8 MB against stock's 148 KB and looks like an uncompressed
export — recompression is tabled (decision #5), not a blocker.

The interior OBJ is likewise staged **once**, at `payload/objs/interior/lights_inn.obj`,
outside the per-wing loop. ⚠ Writing it per-airframe under `payload/objs/<wing>/` would put
all three airframes on the *same path* and silently collapse them into one file. The
display-glow OBJ is staged the same way and for the same reasons, at
`payload/objs/screens/lights_screens.obj`.

⚠ **The screens OBJ is emitted from the Emitter, never copied out of `dist/`**
(`build_screens_payload`). `screens` is absent from `DEFAULT_TARGETS`, so `dist/` may hold a
stale copy — or worse a `--debug` build, which binds every light to
`ToLissPhoton/debug/light/*`, ignores every DU knob, and is not installable.

**The plugin's runtime data** — `panelfx.txt` and the overlay images its layers name — is
staged under `payload/plugindata/`, **mirroring the plugin folder's own layout**, so the
installer copies the tree in verbatim. The overlays are enumerated from `src/native/overlays/`
rather than listed: `panelfx.txt` names images by file name and a layer whose image is
missing is **skipped** at runtime rather than drawn as a solid, so a name list that fell one
behind the look would ship a cockpit quietly missing a layer.

### The build scripts

**`build/make_release.py`** builds *everything*: it stages `release/payload/` — pre-building
every supported OBJ via `build_objs.Emitter` (consulting `SUPPORTED_WINGS`), the native
plugin folder, the plugin data and `payload/realwings_patch.json` — and copies the compiled
`photon-installer` beside it. That staging layout is what the binary reads when run from a
checkout.

**`--bundle`** additionally packs the distributable per-platform download:
`release/<name>/{photon-installer[.exe], data/, README.txt}` plus its archive, choosing
`.zip`/`.tar.gz` by host OS (`--format auto`, overridable; `--name` overrides the base
name). ⚠ **The payload folder is called `data/` in a bundle and `payload/` in the staging
tree**, and the binary accepts both by design (`core/payload.cpp` prefers `payload/`, then
`data/`) — so a checkout and a download both work without the installer knowing which it is
looking at. ⚠ **A missing `photon-installer` is a warning when staging and a hard error
under `--bundle`**: a dev without a C++ toolchain still wants a payload to work against,
but a download whose only executable is absent is 60 MiB with nothing to double-click.

⚠ **No `README.txt` is written at the staging root**, only inside the bundle. The README
documents `data/` beside the binary, and the staging root has `payload/` — a README
describing a layout the reader is not looking at is worse than no README.

⚠ **It no longer stages a DSL toolchain copy.** `payload/dsl/` is gone: it existed solely
because the RealWings patch resolved its numbers at install time, and those are now
pre-resolved into JSON (above). The three other in-place patchers were always stdlib-only
and now live in `installer/` outright, with thin CLI shims left at `build/patch_acf.py`,
`build/patch_acf_screens.py` and `build/patch_glow.py` so the documented dev commands keep
working.

**`--payload-only`** stages just `payload/` — no installer copy, no README, no zip — into
`--out`, defaulting to the **repo root**. That is the dev path that replaced the old
build-on-the-fly mode; point the binary at it with `--payload-dir`.

**Native plugin staging.** `--plugin-dir` points at the `ToLissPhoton/` folder containing
`<arch>/ToLissPhoton.xpl` (`arch` = `win_x64|mac_x64|lin_x64`), defaulting to the CMake
output `src/native/build/ToLissPhoton`. The **fat-plugin format** can carry all three side
by side and X-Plane loads the one matching the host OS, but no bundle ships that way now
(see above): each carries the single arch its own runner built. `make_release.py` errors
clearly if the folder is missing or empty.

**`build/make_allzip.py`** zips whatever `ToLissPhoton-Installer-v<VER>-*.{zip,tar.gz}`
archives it's pointed at (`--archives DIR`) into the All-Platforms zip (stored, not
re-deflated).

**`build/readme.py`** is the single source for every bundle's `README.txt`: per-kind text
(Windows/macOS/Linux) with the installer's own splash art (regexed out of `LogoArt()` in
`src/native/src/installer/screens.cpp`,
kept ASCII-only) and the project links (`GITHUB_URL` / `XPLANE_ORG_URL` from
`build/constants.py`). ⚠ Its **WHAT'S INCLUDED** list is a *contract with the bundle*, not
prose — it has twice described a folder or an executable the download no longer carried (the
`dsl/` folder, then the second installer), which reads to a user as a broken download.
Change it in the same commit as `make_release.build_bundle`.

---

## CI (`.github/workflows/release.yml`)

A three-job chain, since the C++ toolchain cannot cross-compile:

1. **`build`** (matrix win/mac/linux) — fetches the X-Plane SDK, compiles that OS's `.xpl`
   and `photon-installer`, runs both test suites, then packs that platform's bundle on the
   same runner (`make_release --plugin-dir … --bundle`). Uploads `bundle-<OS>`.
2. **`collect`** — downloads all `bundle-*`, runs `make_allzip`, uploads `all-platforms`.
3. **`release`** (tag `v*` **only**) — attaches the three `bundle-*` archives to a GitHub
   Release; `generate_release_notes`.

⚠ **There is no cross-job hand-off left.** The short-lived `xpl-<arch>` artifact existed
only so the `universal` job could merge three arches into one fat plugin; with that bundle
dropped, nothing consumes it, and an artifact with no consumer is CI time and storage spent
to produce something nobody downloads.

A manual `workflow_dispatch` run does everything *except* `release`, so bundles can be
downloaded and tested before publishing.

**Gotchas.** The manual **Run workflow** button only appears once `release.yml` is on the
default branch (`main`); nothing auto-runs on a branch push (no `on: push: branches`) — only
a `v*` tag or the manual dispatch triggers it. Locally, `make_release.py`'s
`shutil.rmtree(release/)` can hit a transient WinError 5 when OneDrive/AV holds a handle on
freshly-written payload files — just re-run (or `rm -rf release` first). **Python is a
build-tooling dependency only** — the DSL emits the OBJs — and no longer an install-time or
runtime one anywhere; there is no `pip install` step left in the workflow.

**Not yet exercised by a real tag push.** The first `v*` tag is the true end-to-end test —
watch for: SDK URL still live, macOS unsigned-`.xpl`/notarization, and MSVC-vs-VS18
compiler differences on the Windows runner. macOS ships a universal (arm64+x86_64) `.xpl`
via `CMAKE_OSX_ARCHITECTURES`; codesign/notarize is still not done (unsigned plugins load,
but a downloaded `.xpl` may be Gatekeeper-quarantined until the user clears it).

---

## Tests

**TWO SUITES, AND BOTH ARE REQUIRED.** The split is by language, not by importance:

```
src/native/build/Release/photoncore_tests      the installer  (C++, 121 cases)
python -m unittest discover -s tests -v        the DSL + build tooling (Python)
```

⚠ **Everything about installing is in the C++ suite now** (2026-08-09). It used to be in
Python, with `tests/test_parity.py` diffing the two implementations' output trees; deleting
the Python installer deleted both, so the coverage was ported first rather than after.

- `src/native/tests/core_tests.cpp` — the pure-logic layer: the `.acf` engine (⚠ the
  XP11/XP12 float-format trap gets exhaustive cases, being the one bug here that fails
  SILENTLY on half the files it touches), the marker, the glow token swap, the attachment
  table's renumbering, the RealWings text rewrite, and the fsutil primitives under all of it.
- `src/native/tests/install_tests.cpp` — the layer above, where a fake payload meets a fake
  aircraft folder: detection by content (renamed folders, sub-hangars, the cfg-module /
  cfg-name / OBJ-fingerprint ladder), install/uninstall round trips, the backup-once rule,
  `added[]` vs `backed_up[]`, the identical-`.xpl` write skip, the interior's livery `.dds`
  shadow, and the screens attach/detach ordering.
- **The one deliberate stub:** the plugin is a fake fat-plugin folder (`<arch>/`
  `ToLissPhoton.xpl` for all three arches, distinct bytes), so the multi-arch install path is
  exercised without a real `.xpl`. The tests assert each one lands byte-for-byte.

What remains in `tests/` is the DSL, the OBJ emitter, the plugin's source-level contracts
and the build tooling — `test_build_objs`, `test_interior`, `test_screens`, `test_panel_fx`,
`test_patch_realwings` (the BUNDLE-TIME resolver, plus `build/realwings_apply.py`, which is a
dev tool with no C++ twin), `test_version` (the `build/constants.py` ↔ `core/constants.cpp`
pin) and the rest.
