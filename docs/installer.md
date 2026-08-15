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

```powershell
src
ativeun-installer.ps1 -Tui -DryRun      # builds the plugin, stages, runs
```

⚠ **Prefer the script.** It builds the `.xpl` first and restages when the payload has gone
stale — the two things whose absence produced a real stale install (below). By hand it is

```
cmake --build src/native/build --config Release --target ToLissPhoton
python build/make_release.py --payload-only          # stages release/payload/
src/native/build/Release/photon-installer --payload-dir release/payload
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
| `backup.cpp` | WHERE the backups and the manifest live, and the move of an older install's folder out of `objects/` |
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
photon-installer                          the interactive installer (GUI, or the
                                          TUI in a build without PHOTON_GUI)
photon-installer [--dry-run] [--xplane-root P] [--tui]
photon-installer detect|status|install|uninstall|screens|version … [--json]
photon-installer … --payload-dir P        read the bundle from P (any mode)
```

⚠ **To build and run it while working on it, use ONE command** —
`src\native\run-installer.ps1 -DryRun`. It is the counterpart to `deploy.ps1` and
it is where the run/test steps are written down; they change as the installer is
reshaped, so nothing else repeats them. Details: `src/native/README.md` §*Running
and testing `photon-installer`*, and the script's own header.

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
re-download a bundle that is fine. `run-installer.ps1` passes it for you when
`release/payload/` exists, and says so loudly when it does not.

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

### The executable icon

`src/icons/*.png` is the artwork; `python build/make_icon.py` turns it into the two files
the build actually consumes, and **both are committed**:

| Output | Consumed by | Shows up as |
|---|---|---|
| `src/native/res/photon.ico` | `res/photon.rc`, compiled into the `.exe` | Explorer, the taskbar, Alt-Tab, the download in a browser, **and the title bar via `ApplyWindowIcon()`** |
| `src/native/ui/assets/icon.png` | `app.slint`'s `window-icon`, set from C++ | the window icon on **Linux** |

**macOS gets neither**: an unbundled executable is faceless there, and giving it a face
means building an `.app`, which the one-executable bundle rule (§*Release bundles*)
currently forbids.

⚠ **THE TITLE BAR IS A THIRD CASE AND IT IS NOT AUTOMATIC** (measured 2026-08-14; an
earlier version of this section claimed the opposite). Three facts, none of them guessable:

1. **Windows does NOT fall back to the executable's icon resource for the title bar.** With
   no window icon and no class icon set, the frame draws the *generic Windows application
   icon* — confirmed by screenshot. Explorer and the taskbar are fine either way, which is
   what makes this look like an artwork problem rather than a wiring one.
2. **`Window.icon` in Slint is ONE image.** Binding it to the 256 px artwork makes winit
   hand the frame a single 256×256 bitmap as the window's SMALL icon, which the title bar
   then squashes to 16 px with a crude stretch. `WM_GETICON` returned `256x256` while
   `SM_CXSMICON` was 16. The hand-tuned 16 px entry sat in the `.ico` unused. **Pointing
   the property at a smaller PNG does not fix it** — one image cannot serve a 16 px frame
   and a 256 px task-view tile; it just moves the bad scale.
3. So `app.slint` leaves `window-icon` **empty on Windows** and `gui.cpp`'s
   `ApplyWindowIcon()` sets both icon slots from the resource with `LoadImage`, which picks
   the best-matching entry out of the group for the size the shell asked for. Everything
   else fills the Slint property instead. The result was verified against the authored
   16 px composited on the title bar's own background: mean error 0.02, worst channel 1.

⚠ **Two traps inside that fix, both of which shipped a silent no-op first.** It cannot run
at `show()` time — there is no HWND until winit's first event-loop iteration — so it is
armed on a timer, which is also why `ui->run()` is spelled out as `show()` +
`run_event_loop()` + `hide()`. And **the stop condition must be reading the icon back out**:
the first version stopped at the first window `EnumThreadWindows` returned, painted a
transient winit helper window, reported a clean hit and left the real title bar generic.
`tests/test_icon.py` pins every part of this, because the only symptom of any of it is an
icon that looks slightly wrong.

⚠ **A generated file living in `src/` is deliberate.** CMake must be able to build the
installer on a runner with no Python, so the `.ico` is a build *input* there rather than
something the build produces. The cost is drift — re-export the PNGs, forget the script, and
the installer keeps shipping the old artwork with no symptom whatsoever — and
`tests/test_icon.py` is the only thing that catches it. It re-derives every entry in memory
and compares *pixels*, not file bytes, so it does not fail on a machine whose zlib packs the
PNG entries differently.

**The size list is `BASE_SIZES` UNION whatever is in `src/icons/`** (`icon_sizes()`), and the
union half is not decoration. `BASE_SIZES` used to *be* the list, so an export at 60 and
72 — Windows' large-icon sizes at 125% and 150% scaling — sat in the folder matching no
entry and never reached the `.ico`. ⚠ **Nothing warned, and nothing could**: a dropped size
looks exactly like a size the shell had to scale, which is the entire condition this file
exists to avoid. Re-exporting at a size the script has never heard of now just works.
Anything the artist did not author is box-filtered down from the largest source, in
premultiplied alpha — straight-RGB averaging pulls transparent black into the edges and
haloes precisely the small sizes.

⚠ **Nothing above 256 can be in an `.ico`**, and the union rule is what makes that
reachable: a directory entry stores each side in ONE byte, where 0 means 256, so an authored
512 would be written as a second entry claiming to be 256. `ICO_MAX` drops it and `main()`
says so rather than swallowing it — a 512 is still the best resampling source there is, so
it belongs in the folder. Entries ≤48 are uncompressed BMP for the legacy shell paths and
the rest are PNG, which keeps the 256 entry at 19 KB instead of 262 KB.

### Executable metadata (VERSIONINFO)

The same `res/photon.rc` carries a `VS_VERSION_INFO` block, which is what fills Explorer's
**Properties ▸ Details** tab: company, description, version, copyright, original filename.
⚠ **It is not a substitute for code signing.** SmartScreen's "unknown publisher" warning
answers to an Authenticode certificate and download reputation, neither of which this project
has; what this block buys is that a downloaded mod `.exe` does not present a *completely
blank* Details tab, which is what a suspicious user checks first and what some AV heuristics
score against.

⚠ **The version is READ, never typed.** `core/version.h` is the one definition, so CMake
regexes it out (anchored at line start, exactly as `build/version.py` is, because that header
discusses `kPhotonVersion` in its own comments and the first Python reader matched one),
pads it to the four 16-bit fields `FILEVERSION` requires, and `configure_file`s
`res/version_rc.h.in` → `<build>/generated/photon_version_rc.h`. The `.rc` includes that.
The two shapes are why the generated header exists at all: `FILEVERSION` is a comma quad and
`FileVersion` is a display string, and rc.exe preprocesses rather than computes. ⚠ **Do not
`#include` `core/version.h` from the `.rc` instead** — rc.exe parses what falls out of the
preprocessor as resource script, so the C++ body is a syntax error unless it is wrapped in
`#ifndef RC_INVOKED`, and even then only the string survives.

⚠ **rc.exe takes its include path from the target's**, so
`target_include_directories(photon-installer PRIVATE .../generated)` is load-bearing (CMake
passes `<INCLUDES>` to the RC rule under Ninja and fills `ResourceCompile/
AdditionalIncludeDirectories` under the Visual Studio generator).

⚠ **THE ONE SILENT FAILURE IS THE CODEPAGE PAIR.** The `StringFileInfo` block is named by
the `VarFileInfo` `Translation` value written as two 4-digit hex numbers, language first —
`0x409, 1200` → `"040904B0"`. When they disagree, Windows looks up a language block that is
not there and shows an **empty** Details tab, which is indistinguishable from having shipped
no version resource at all, from a file that compiled without a murmur.
`tests/test_exe_metadata.py` pins that pair, the eight fields, the macro use (a dotted
literal in the `.rc` would be a second version definition) and that `OriginalFilename` is
still a name `make_release.py` stages the binary under.

The `.xpl` deliberately has no `VERSIONINFO`: X-Plane does not read one, and the plugin's
version is what the About tab reports.

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
python build/make_release.py --payload-only     # stages release/payload/
src/native/build/Release/photon-installer --payload-dir release/payload
```

`--payload-only` never wipes its `--out`; it replaces only the `payload/` subtree it owns,
and it tolerates an unbuilt native plugin so trying the TUI does not require the C++
toolchain. A missing payload is now an **error naming that
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

1. A manifest at `<aircraft>/Photon Backup Files/photon_manifest.json` (written by
   `actions.install`) supplies version/wing. ⚠ **Earlier installers wrote it one level down,
   inside `objects/`, and that file is still read** — see *Where the backup folder lives*
   below. Never build this path by hand; ask `backup::Dir`.
2. A `# ToLissPhoton version: X wing: Y` comment injected into the `lights_out*.obj` header
   (`actions._inject_marker`, read back by `_obj_marker`) tells whether the edit is *still
   live*.

With no manifest the OBJ marker is the fallback version source. **The key case:** if the
manifest survives but the OBJ marker is gone — a SkunkCrafts aircraft update reverts
`lights_out` to stock without touching `Photon Backup Files/` — the status is **stale**,
surfaced in the UI as "Needs reinstall" (yellow) and a "reinstall — update reverted it"
action label. A surviving manifest alone is **not** proof the edit is in the file; the OBJ
marker corroborates it. Keep both formats in sync if you change either.

## Wing-mod detection (`core/wingmod.{h,cpp}`)

A **second, independent** wing question, and the two must never be folded together:

| | asks | source |
|---|---|---|
| `exterior.wing` | which variant **Photon** was built for | the manifest / the OBJ marker |
| `wing_mod.detected` | which wing mod the **aircraft** is running | the `.acf` table + the wing OBJs |

Them disagreeing is the entire point, so a single field could not report it.

⚠ **WHAT IS IN THE FOLDER IS NOT WHAT IS ON THE AEROPLANE.** Both mods leave their
distribution folders and `.bak` files behind forever. The A320 this was written against
carries `objects/RealWings320/`, `Durantula_ToLiss_Wingflex_V1.3/`,
`wingL.obj.durantula.bak` and `a320.acf.durantula.bak` — and a directory listing
therefore reports "both mods installed" about an aircraft that is running **one** of
them. So every question is asked of the two things X-Plane itself reads.

⚠ **THAT EXAMPLE'S SECOND HALF HAS SINCE CHANGED, WHICH IS THE POINT RESTATED.** It
was written as "and is running stock wings"; re-run against the same folder on
2026-08-14 the verdict is **`durantula`, determined, healthy, 7 converted blocks per
wing, symmetric**. Only the aircraft changed — the RealWings folder is still there and
still unattached on all three airframes here. A wing-mod claim in prose is a
measurement with a date on it, and the detector is the thing to ask.

**Live verdicts, 2026-08-14:** A319 `stock`, A320 `durantula`, A321 `stock`; all
determined, all `acf_variants_agree`, all healthy.

**The two mods need two different questions**, and either one alone gets the other wrong:

- **Durantula is an edit to the aircraft's own wing OBJ.** Its manual (`DATA.txt`) is six
  find-and-replace blocks, three per wing, swapping ToLiss's `anim/winglex` rotation chain
  for `sim/flightmodel2/wing/wing_tip_deflection_deg[n]`. The attachment table is untouched
  — `wingL.obj` is still `wingL.obj` — so the signal is the **driver token inside the file**.
  ⚠ **A converted OBJ still contains `anim/winglex`** (7 in a stock A320 `wingL.obj`, 3 of
  which the mod replaces), so "has winglex" is true of *both* states and can only ever
  answer the negative case. Count the deflection blocks instead.
- **RealWings replaces the geometry wholesale.** Its ReadMe is a Plane Maker session: clear
  the `wingL` / `wingR` / `wings_glass` rows, add six objects out of
  `objects/RealWings<nnn>/`. The aircraft's own wing OBJs stay on disk, unmodified and
  undrawn — so the signal is the **attachment table**, and reading the wing OBJ would report
  stock forever.

⚠ **RealWings wins when both are present.** If the table does not name `wingL.obj`, whatever
Durantula wrote into `wingL.obj` does not draw.

**Why Photon cares:** `lights.layout.phdsl`'s `wing_left`/`wing_right` mounts are *copies of
that same rotation chain*. A Photon built for `stock` and installed on a Durantula aircraft
hangs its wingtip lights off a driver the wing no longer uses — the lights hold still while
the wing flexes. Nothing errors, nothing looks wrong on the ground, and it takes a loaded
aircraft in the air to see.

### What it calls a problem

Each of these is a real mis-install, reachable by following the mod's own instructions
imperfectly, and each is pinned by a case in `install_tests.cpp` (§`wingmod:`):

- a RealWings row **past `_obja/count`** — in the file, never read, draws nothing. The
  hardest one to see by eye, because every instruction the user followed *is* in the `.acf`.
  Same rule `patch_acf_screens::IsAttached` already applies to our own row.
- RealWings attached **beside** the stock wing rows — step 3 skipped, two sets of wings.
- a RealWings object **named but not on disk**.
- Durantula applied **unevenly** — ⚠ asymmetry is the version-proof test, since the mod is
  symmetric by construction; the absolute count (`kDurantulaBlocksPerWing`, 3 in V1.3) is
  only ever a *note*, because a later release may add a case.
- one wing converted and the other not.
- the `.acf` variants **disagreeing** — ⚠ the most likely RealWings mis-install there is,
  since its instructions are a Plane Maker session on *one* file. Photon installs one
  `lights_out` for all of them, so this cannot be papered over here; the fix is in Plane
  Maker. ⚠ **The XP11 pair is not asked** (2026-08-15, see *Which `.acf` variants Photon
  touches*): it made a correct RealWings install report a disagreement it had no way to
  act on. `_StdDef` still counts — X-Plane 12 loads it.

A mod folder present but attached by nothing is a **note**, never a problem — that is the
ordinary state of a reverted install.

⚠ **IT REPORTS; IT NEVER SUBSTITUTES.** `actions::Install` logs the verdict and every
finding, warns when the detected wing differs from `--wing`, and then installs exactly what
was asked. Silently swapping the variant would be unexplainable from the screen the user
clicked through — and wrong precisely when the detection is wrong. Read-only and
non-throwing throughout: a detector that can abort an install is a worse bug than the
mismatch it reports.

⚠ **THE GUI SEEDS ITS WING RADIO FROM IT, AND THAT DOES NOT BREAK THE RULE ABOVE**
(2026-08-14, `SeedWing` in `installer/gui.cpp`). The rule is about not overriding an
answer; this runs on the way from Aircraft to Features, **before anyone has answered**,
and fills in a default *on screen* where it can be seen and changed. The install-time
warning stays exactly as it was and is the backstop: a wrong seed produces the same
WARN a wrong manual choice would. It seeds **once per aircraft** — re-seeding on every
visit would silently undo a correction, and believing the detector is wrong is the only
reason to touch that radio. Full note: `installer_gui_plan.md` §13.1.

Reachable as `photon-installer status --aircraft P --json` → `wing_mod`.

## Where the backup folder lives

```
now                <aircraft>/Photon Backup Files/
every earlier      <aircraft>/objects/Photon Backup Files/
installer
```

It moved to the aircraft root on **2026-08-14**. ⚠ **The era is told from the FOLDER, never
from a version number** — nothing reads the manifest's `version` to decide it, and nothing
should: a tester bundle can carry any version with either layout, and what is on disk is the
only thing that has never been wrong. `objects/` is the aircraft's **asset** folder —
what X-Plane resolves OBJ and texture references against, what SkunkCrafts syncs on an
update, and what any tool walking an aircraft treats as "the files this aeroplane is made
of". A second, byte-identical copy of ToLiss's own OBJs sitting one level down inside it is
a thing waiting to be found by something that recurses: our own `patch_glow` scan iterates
one level and is safe *by accident*, and on the A330-900 that copy runs to hundreds of
megabytes of mesh backups. Nothing about it was load-bearing; the aircraft root is where
Durantula already keeps its `a320.acf.durantula.bak`, and the shorter path buys ~8
characters of MAX_PATH headroom on the longest names we write (a flattened livery backup
carries the livery folder's own name).

⚠ **`core/backup.h` OWNS THE PATH AND NOTHING ELSE MAY SPELL IT OUT.** `backup::Dir` is the
one resolver, and `constants.kBackupDirName` is the folder's NAME only. The rule it states:

> **The legacy folder wins while it still holds files.** `Migrate()` is the only thing that
> changes the answer.

That is what keeps the backups and the manifest that indexes them in the same folder — even
when a migration fails halfway, both keep resolving to `objects/` and the next run tries
again. ⚠ **Reading no manifest is the expensive failure, not moving one**: an installer that
cannot see an existing manifest believes nothing is installed, and then backs up the OBJ it
wrote *last time* as the stock original. That is unrecoverable, and it is the whole reason
`manifest::PathFor` resolves rather than concatenates.

**The retroactive move.** `actions::Install` calls `backup::Migrate` once, after the plugin
write and before anything touches a backup:

- The whole folder is renamed — a metadata operation on the same volume by construction,
  so the A339's gigabyte of mesh backups costs the same as an empty folder. Per-file moves
  are the fallback for when the destination already exists or a rename is refused (an
  antivirus holding a handle is the realistic case).
- ⚠ **A legacy file OVERWRITES a same-named one at the destination**, which runs the
  opposite way to "newest wins". Nothing has written to `objects/` since the upgrade, so
  the legacy copy is the *older* one — the closest to stock — and a newer file at the
  destination can only be a backup of Photon's own work.
- ⚠ **The destination is written before the source is removed.** A failure partway leaves
  the file in both places and the legacy folder still in use, so the next run merges again:
  it converges rather than losing a user's only copy of a stock file.
- A **dry run** reports the move and performs none of it — and keeps resolving to
  `objects/`, so every other line it prints names the folder the files are really in.
- It is a **priced progress step** (`Backup folder`), added and entered under the identical
  condition, per the unpriced-step rule below.

**Uninstall does NOT migrate**, deliberately: it is about to empty the folder and remove it,
so a rename would buy nothing and would add a step that can fail to the one path a user runs
when something is already wrong. It restores from whichever folder `backup::Dir` resolves to
and then `RemoveDirIfEmpty`s **both**, so an empty legacy folder left by a partial move does
not survive an uninstall.

**Downgrading is the one thing this does not cover.** An older installer run against a
migrated install sees no manifest in `objects/` and reports "not installed"; uninstalling
with it refuses outright rather than doing damage, and re-running a current one recovers. Nothing
warns about this, and nothing can — the old binary is the one doing the looking.

The folder also carries a **`README.txt`** (`backup::ReadmeText`, rewritten on every
install, version- and date-free so a reinstall rewrites it identically). At the aircraft
root the folder is somewhere a user actually browses, so it says what it is and that
deleting it costs them their originals. Uninstall removes it before `RemoveDirIfEmpty`.

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

The **XP12 pair** per airframe is patched — see *Which `.acf` variants Photon touches*
below — found **by content** (a `_spot_name_3d/0` line), never by an assumed
`a320{,_StdDef,_XP11,_XP11_StdDef}.acf` naming — the folder or file may have been renamed.
`.bak` siblings are excluded: Durantula keeps an `a320.acf.durantula.bak` of this very
file, and patching it would corrupt *its* uninstall.

Two traps, both with regression tests in `tests/test_patch_acf.py`:

- **⚠ XP12 and XP11 render the same values differently** — `-70.000000000` vs `-70.0`. A
  literal string patch works on the two XP12 files and **silently no-ops on both XP11
  ones**. So the patcher always parses numerically and re-renders in each file's own
  detected style, and detection compares numerically with a tolerance. ⚠ **Still live even
  though an install no longer writes an XP11 file** — an *uninstall* does, and a renderer
  that lost the narrow style would leave those files half-reverted.
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
  ⚠ **`ScreensFrameTemplates` lost its third entry on 2026-08-15.** The A330-900 names its
  cockpit lighting `CockpitLighting_XP12.obj` in the XP12 pair and plain
  `CockpitLighting.obj` in the XP11 pair (verified on disk — the two spellings never
  co-occur in one file), and the bare spelling was listed *only* so the attach would not
  refuse on the two variants Photon now never writes to. If a future ToLiss XP12 `.acf`
  drops the suffix, add the bare spelling back to that list — do not widen the scope.
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
  and one that cannot be patched must not abort the others.

### Which `.acf` variants Photon touches

A ToLiss folder ships four: `a320.acf`, `a320_StdDef.acf`, `a320_XP11.acf`,
`a320_XP11_StdDef.acf`. Since **2026-08-15** (`acf::Variants` in `core/acf.h`):

| Operation | Scope | Why |
|---|---|---|
| Interior spot patch, screens attach | `Variants::Xp12` | nothing Photon builds is for X-Plane 11 |
| Their reversals (uninstall, `screens --detach`) | `Variants::Every` | Photon ≤ 0.8.4 wrote all four |
| `wingmod::Detect` | XP12 only | the XP11 pair is not evidence about the aeroplane |

**The bug that prompted it.** `wingmod`'s consensus asked all four variants to agree, and
RealWings' install is a Plane Maker session on **one** file — so a correct, complete
RealWings install was reported as *"this aircraft's .acf variants do not agree about the
wing mod"*, naming a file X-Plane 12 never loads as the dissenter. A Problem the user
cannot act on, on an aeroplane where nothing is wrong. Confirmed on the A320 here: both
XP12 variants carry 6 `RealWings320/` rows and no stock wing rows, both XP11 variants are
stock — a 2–2 split.

The install side was never a bug, only wasted work: `lights_out3xx_XP12.obj` is XP12-only,
so an XP11 variant got patched sim spots and an attached OBJ with no reader.

- ⚠ **The test is the file's own FORMAT STAMP, not `_XP11` in its name.** Line 2 of every
  `.acf` X-Plane writes is `1200 Version` (XP12) or `1100 Version` (XP11) —
  `acf::FormatVersion` / `acf::IsForXp12`, verified across all four airframes here. The
  four-variant naming is ToLiss's convention; the stamp is X-Plane's own, and
  content-over-naming is already the rule governing every other question this installer
  asks about an aircraft folder.
- ⚠ **Not `acf::DetectStyle`.** That answers a different question — how floats are
  *rendered*, so a patch can be written back in the file's own style — and it has no
  "cannot tell": it must return one of two styles for any text. The stamp is either there
  or it is not.
- ⚠ **An unstamped file reads as XP12.** The failure to avoid is a **silent skip** — an
  install quietly doing nothing to a file it was meant to patch, which is the exact shape
  of the float-format trap above. Every caller has already matched a content needle by the
  time the stamp is asked, so "no stamp" means an `.acf` unlike any X-Plane writes.
- ⚠ **`_StdDef` is in, and not by accident.** X-Plane 12 loads it, so
  "`a320.acf` has RealWings and `a320_StdDef.acf` does not" is a real mis-install with a
  real consequence — whichever variant you load decides which wings you get, and one
  Photon build cannot be right for both. That warning stays; only the XP11 half was noise.
- ⚠ **Reversal is `Every`, and the asymmetry is deliberate** — the same rule as the backup
  folder (*Where the backup folder lives*): write to the new place, clean up both. An
  uninstall scoped to XP12 would leave an XP11 file with its cockpit spot block still
  disabled and an `_obja` row naming `lights_screens.obj` — a file that same uninstall
  deletes, so the leftover is a missing-object warning on every load of a variant nothing
  will ever revisit.

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
`payload/objs/screens/<airframe>/lights_screens.obj` — one per airframe, because the A330-900's six lights sit in a different flight deck against a different datum and a shared copy would install the A320's positions into it.

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
`--out`, which defaults to `release/` **for every mode**. That is the dev path that replaced
the old build-on-the-fly mode; point the binary at it with `--payload-dir`.

⚠ **IT USED TO DEFAULT TO THE REPO ROOT, AND THAT SHIPPED A STALE INSTALL** (2026-08-14).
`run-installer.ps1` ran the binary against `release/payload`, so `-Payload` rebuilt
`<repo>/payload` and every test install read the other tree — whatever an earlier full
`make_release.py` run had left behind. Nothing about it looks wrong: the installer reports
success, the files land, X-Plane loads a plugin, and only the version says it is last
week's. One staging root now, and `release/` is gitignored where the repo root is not.

⚠ **NOTHING HERE BUILDS THE PLUGIN — `stage_plugin` copies one.** Right for CI, where the
matrix builds the `.xpl` in the same job; locally it meant the only thing that ever
refreshed `src/native/build/ToLissPhoton/` was `deploy.ps1`, so an installer test installed
whatever plugin had last been *deployed to the sim*. `run-installer.ps1` builds it now, and
`make_release.py` **refuses to stage an `.xpl` older than `src/native/src/`** — before it
deletes anything, so a refusal costs you nothing and leaves the previous payload standing.
`--allow-stale-plugin` is for `--plugin-dir` folders assembled from downloaded CI artifacts,
whose mtimes say nothing about the checkout; it is not a way past a rebuild.

⚠ The `.xpl`'s **build time and size are printed on every staging run**, and
`run-installer.ps1` prints them again beside the binary it is about to launch. That line is
the only thing that would have made the day-old plugin visible.

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
   and `photon-installer` at `-DPHOTON_GUI=${{ matrix.gui }}` (**ON for Windows, OFF for
   macOS and Linux** — see below), runs both test suites, then packs that platform's bundle
   on the same runner (`make_release --plugin-dir … --bundle`). Uploads `bundle-<OS>`. Rust
   and Slint's Linux `-dev` packages install only where `gui` is ON.
2. **`collect`** — downloads all `bundle-*`, runs `make_allzip`, uploads `all-platforms`.
3. **`release`** (tag `v*` **only**) — attaches the three `bundle-*` archives to a GitHub
   Release; `generate_release_notes`.

⚠ **There is no cross-job hand-off left.** The short-lived `xpl-<arch>` artifact existed
only so the `universal` job could merge three arches into one fat plugin; with that bundle
dropped, nothing consumes it, and an artifact with no consumer is CI time and storage spent
to produce something nobody downloads.

A manual `workflow_dispatch` run does everything *except* `release`, so bundles can be
downloaded and tested before publishing.

### ⚠ The GUI ships on all three platforms — and what it took to get there

**`matrix.gui` is the one statement of which front-end a platform ships**, and it is
ON everywhere as of 2026-08-15. It stays a per-row value so a platform can be dropped
back to the TUI by editing one word — a state this matrix was in for exactly one
commit, when the GUI turned out not to build on Unix at all.

**What was wrong.** The GUI was developed on Windows against a Figma design, and
`run-installer.ps1` still says *"Windows/win_x64 only"* in its header. Turning it on
for the whole matrix made CI the first-ever Unix build of it, and both Unix jobs
failed to **link**: Corrosion does not propagate Slint's transitive system libraries,
the same defect the `opengl32`/`imm32` note in `CMakeLists.txt` describes for Windows.
Both lists are now explicit, derived from the actual undefined-symbol dumps:

- **Linux — exactly one library.** Every undefined symbol was `Fc*`, from `fontique`
  under Slint's text stack. ⚠ **Installing `libfontconfig-dev` is not enough**, and
  that is the trap: the headers satisfy the Rust build script, the compile succeeds,
  and the failure lands at the final C++ link where nothing put `-lfontconfig` on the
  line. Resolved through pkg-config, with a link-by-name fallback and a warning rather
  than a hard configure error.
- **macOS — ten frameworks plus `-lobjc`**: CoreFoundation, Foundation, AppKit,
  CoreGraphics, CoreText, CoreVideo, QuartzCore, OpenGL, Carbon, CoreServices.
  ⚠ **This is the least-proven thing in the build.** On Windows exactly two libraries
  were under-reported; on macOS *nothing* propagated — ~300 symbols across every
  framework the backend touches — which points at the universal (`arm64;x86_64`) lipo
  path dropping the link interface wholesale rather than at an incomplete list. **If
  it regresses, test single-arch first** (`-DCMAKE_OSX_ARCHITECTURES=arm64`) before
  adding frameworks.

⚠ **Linking was never the whole gap.** `platform::PickFolder` returned "" on
everything but Windows, so a Unix GUI would have shipped a **Browse button that did
nothing**. It is now `choose folder` via osascript on macOS and zenity-then-kdialog on
Linux — see `platform.cpp`, and note the no-shell rule there: the start path is
user-supplied, so it never goes through a command string.

⚠ **The flag's default is OFF, and that is not the same as "OFF here."**
`option(PHOTON_GUI ... OFF)` stays, because a contributor's checkout and the core-only
tree must build with no Rust. The workflow states the value it wants on **every**
runner, both ways, because an omitted or typo'd `-D` does not fall back to the previous
behavior — it silently publishes the wrong front-end, on a green run, with a
well-formed download to show for it. That is exactly what happened between the GUI
landing and 2026-08-15, and the only symptom was that the thing users double-clicked
opened a console.

So the configure step `grep`s its own log **in both directions** for CMakeLists'
`GUI build - photon-installer links Slint`, printed only from inside `if(PHOTON_GUI)`:
present is required where `gui` is ON, and forbidden where it is OFF — a Unix bundle
that quietly picked Slint up would either fail to link or ship the untested GUI, and
neither would fail any other step. **Change that `message(STATUS)` string and the check
breaks** — change both together.

**What the GUI costs.** Rust is installed explicitly rather than taken
from the runner image: Slint v1.17.1 declares `rust-version = "1.92"`, and an image
drifting below that would break the build on a tag push — the one run nobody wants to
debug. `stable` is deliberate; the reproducibility that matters is pinned at the
**Slint tag** (`v1.17.1`, `GIT_SHALLOW`, in `src/native/CMakeLists.txt`), while pinning
rustc would instead break the day a transitive crate needs something newer. Cargo's
*downloads* are cached, and the Slint **compile** is cached on `workflow_dispatch`
only — a published bundle is built from nothing. Windows ran ~15 min with a cold
cache; expect macOS to be the long pole, since Corrosion builds Slint once per
architecture and lipos the results for the universal binary.

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
  shadow, the screens attach/detach ordering, and the backup folder's move out of `objects/`
  (the `backup:` cases — ⚠ they build the legacy layout by MOVING a finished install's
  folder back into `objects/`, because two folders at once is a state no released version
  could produce and proving anything against it would prove the wrong thing).
- **The one deliberate stub:** the plugin is a fake fat-plugin folder (`<arch>/`
  `ToLissPhoton.xpl` for all three arches, distinct bytes), so the multi-arch install path is
  exercised without a real `.xpl`. The tests assert each one lands byte-for-byte.

What remains in `tests/` is the DSL, the OBJ emitter, the plugin's source-level contracts
and the build tooling — `test_build_objs`, `test_interior`, `test_screens`, `test_panel_fx`,
`test_patch_realwings` (the BUNDLE-TIME resolver, plus `build/realwings_apply.py`, which is a
dev tool with no C++ twin), `test_version` (the `build/constants.py` ↔ `core/constants.cpp`
pin) and the rest.
