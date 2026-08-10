# Installer C++ port — implementation plan

Status (2026-08-09): **THE PORT IS DONE.** Phases 1–5 landed and phase 6 (cutover)
was carried out the same day: the Python installer no longer ships AND no longer
exists — `install.py`, `installer/`, the three `build/patch_*.py` shims and
`tests/test_parity.py` are deleted, with their coverage moved into
`src/native/tests/install_tests.cpp` FIRST. The one thing still open is the §9 step 2
QA gate, **half closed — Windows walked and passed (2026-08-09), macOS and Linux
outstanding.**

This document was the handoff spec for converting the end-user installer from Python
(`install.py` + `installer/`) to C++, sharing a static library with the native plugin;
it is now the record of how it was done and of which ⚠ items are still contracts. Read `docs/installer.md` beside it for the current
system's full rationale, and treat every ⚠ item in §7 as a contract, not a suggestion.

| Phase | What | State |
|---|---|---|
| 1 | DSL decoupling in Python (§2) | **done** — `payload/dsl/` is gone; install time has zero DSL imports |
| 2 | `photoncore` + `photoncore_tests` (§3, §6.1) | **done** — the `.xpl` links it and reads the shared version/needle/glow table |
| 3 | Headless CLI + parity harness (§5, §6.2–3) | **done, and the harness has since been RETIRED** — it diffed both installers' trees byte for byte and could only exist while both did; `tests/install_tests.cpp` replaced it at cutover |
| 4 | TUI + screens + platform glue (§4) | **done** — 1:1 port; 70 C++ cases, the engine's expectations generated from `tui.py` itself |
| 5 | Build/release/CI/version integration (§8) | **done** — version single-sourced; `make_release.py --bundle` packs the one-installer download; CI builds and tests the binary |
| 6 | Cutover (§9) | **done except the QA (2026-08-09)** — the Python installer no longer ships AND no longer exists; its coverage moved to `tests/install_tests.cpp` first. Left: the macOS/Linux half of the manual TUI QA (Windows done 2026-08-09) |

**What is NOT covered by any automated test, and cannot be:** the interactive walk
itself — raw-mode key decoding in a real terminal, on three OSes. The engine
underneath it is pinned case-for-case (§6.1) and the screens call nothing the CLI
does not, so what remains untested is genuinely presentation. §9 step 2 is that QA
and it is a gate, not a formality: **raw-mode key handling is exactly where ports
rot**, and a Windows build that works in Windows Terminal can still be wrong in
conhost.

⚠ **Windows is walked and clean (2026-08-09); macOS and Linux are not, and the
gate is not closed until they are.** Passing on one OS says nothing about the other
two here — the three key-decoding paths share no code (`_getwch` and the
`\x00`/`\xe0` extended-key prefix pairs on Windows; `termios` raw mode and the
ESC-sequence decode on POSIX), so this is three independent gates wearing one
number, and the Windows one is the only one a Windows dev machine can retire.

## What the port has already found

Two shipped bugs, both surfaced by the parity harness on its first two runs, both in
the **Python** installer, both the same shape: **a text-mode read silently
normalizing a file's line endings.**

1. **`payload.obj_text()`** universal-newlined every installed OBJ from CRLF to LF.
   `build_objs.Emitter.emit()` ends with `.replace("\n", "\r\n")` — our OBJs are CRLF
   like X-Plane's own — so every install wrote an LF copy of a CRLF artifact.
2. **`realwings.patch_file()`** and **`patch_glow.patch_file()`** did the same to the
   user's *other mods'* files. In the RealWings case this made the patcher's own
   `nl = "\r\n" if "\r\n" in text else "\n"` line **dead code**: the CRLF branch could
   never be reached, so a handful of intended line changes came out as a whole-file
   rewrite of somebody else's mod.

Neither breaks anything — X-Plane parses either ending — which is exactly why both
survived. The only symptom was that a byte comparison against the source artifact
reported a total difference with no cause visible in either tree. Fixed with
`newline=""`, the rule the `.acf` patchers already followed. **This is the argument
for the parity harness in one paragraph: a tree diff catches what reading the code
does not.**

---

## 0. Goal, motivation, non-goals

**Goal.** One compiled `photon-installer` binary per platform, built by the same CMake
tree as `ToLissPhoton.xpl`, with the file-patching / detection / manifest machinery in a
shared static library (`photoncore`) that the plugin, the installer, and future dev CLI
tools all link.

**Motivation** (per Alex, 2026-08-07): the core plugin is already C++; a shared library
ends the class of drift where the plugin and installer each carry their own copy of a
fact (today: `kPhotonVersion` vs `installer/constants.VERSION`, pinned only by
`tests/test_version.py` after they drifted for a whole feature; `kInteriorObjNeedle` vs
the DSL gate names vs `constants.INTERIOR_OBJ`). It also removes the PyInstaller freeze
step and the "needs Python" caveat.

**Honest scoping note.** The plugin itself patches no files today, so the immediately
shared surface is *constants + version + the interior-OBJ needle scan*, not the patchers.
The patching library's real second consumer is the installer's headless CLI mode (§5) and
future dev tooling. Say this now so nobody hunts for a plugin/patcher integration that
was never the point; the drift-killing constants sharing alone justifies the library.

**Non-goals — these stay Python:**

- The **build/DSL toolchain**: `build_objs.py`, `photon_dsl.py`, `make_overlays.py`,
  `derive_panel_rects.py`, `watch.py`, `make_release.py`, `make_allzip.py`, `readme.py`.
  They run on dev machines and CI only, never on a user's.
- `src/plugin/PI_PhotonDevReload.py` (XPPython3 dev tool, never shipped).
- The **test harness** stays stdlib `unittest` (repo rule: no pytest). Tests drive the
  compiled installer through its CLI mode (§6).

---

## 1. Inventory — what converts, what changes shape

| Today (Python) | Lines | Fate |
|---|---|---|
| `install.py` (flow/screens) | 689 | Port → `installer/screens.cpp` + `main.cpp` |
| `installer/tui.py` (ANSI engine) | 496 | Port → `installer/tui.{h,cpp}` (§4) |
| `installer/detect.py` | 379 | Port → `core/detect.{h,cpp}` |
| `installer/actions.py` | 844 | Port → `core/actions.{h,cpp}` (§7 checklist) |
| `installer/payload.py` | 367 | Port, minus dev mode → `core/payload.{h,cpp}` (§2) |
| `installer/constants.py` | 208 | Merge into `core/constants.{h,cpp}` + `core/version.h` |
| `installer/config.py`, `log.py` | 106 | Port → `core/config`, `installer/log` |
| `build/patch_acf.py` | 293 | Port → `core/patch_acf` (data tables + engine) |
| `build/patch_acf_screens.py` | 302 | Port → `core/patch_acf_screens` |
| `build/patch_glow.py` | 154 | Port → `core/patch_glow` (token swap + `REDIRECT` table) |
| `build/patch_realwings.py` | 329 | **Split** (§2): resolution stays Python at bundle time; application ports as a data-driven patcher |
| `build/make_exe.py` (PyInstaller) | 165 | **DELETED (2026-08-09)** — CMake builds the installer; bundle assembly folded into `make_release.py` |
| `build/make_release.py` | 312 | Stays Python; stages the compiled binary instead of freezing, and packs the per-platform bundle under `--bundle` (§8) |

---

## 2. Phase 1 — decouple install time from the DSL (still in Python)

This is the load-bearing restructuring, and it happens **before any C++ is written**, in
the Python installer, so the new data flow is proven by the existing test suite first.

**The problem.** Two places import the DSL toolchain at *install* time:

1. `actions._patch_realwings` → bundled `payload/dsl/` → `build_objs.load_config()` —
   resolves each baked RealWings light's replacement class, color, intensity, cone,
   direction, category dataref, and the `@realwings { offset: … }` placement correction
   out of the `.phdsl` files.
2. `payload.py` **dev mode** — running `install.py` from a repo checkout with no
   `payload/` builds each OBJ on the fly via `build_objs.Emitter`.

A C++ installer will not embed the DSL parser. The fix is the same shape the OBJs
already use: **resolve at bundle time, apply at install time.**

**2a. Precompute the RealWings patch data.** `make_release.py` (and the new staging
command in 2b) gains a step that calls the existing resolvers and writes
`payload/realwings_patch.json`: for each (airframe, baked light class, side) the fully
resolved replacement-line parameters and the placement offset — everything
`patch_realwings.photon_block` currently computes at install time. Then rewrite
`patch_realwings`'s *application* half to consume that JSON instead of importing
`build_objs`, and switch `actions.py` to it. The line-rewriting/reversal mechanics
(marker comments, commented-original preservation, X-mirroring for starboard) stay in
the patcher; only *where the numbers come from* moves.

⚠ **This does not violate the "all tuning lives in the DSL" tripwire — it enforces it.**
The JSON is a *generated build artifact* derived from `lights.layout.phdsl` at bundle
time, exactly like the OBJs in `payload/objs/`. It must never be hand-edited, and the
generator must emit a header comment saying so. What the tripwire forbids is a
hand-maintained position table in Python (or now C++) source; nothing here creates one.

**2b. Replace dev mode with an explicit staging step.** Add
`python build/make_release.py --payload-only` (stages `payload/` next to `install.py`
without zipping or README): the dev path becomes "stage, then run", instead of the
installer silently building OBJs itself. Update `docs/dev-tools.md`; `test_actions.py`
already builds a real release per module, so tests barely change. `payload.py`'s dev
branches are then deleted, and `payload/dsl/` **drops out of the bundle contract
entirely** — the bundle shrinks by the toolchain and the `sys.path` import machinery in
`actions.py` goes away.

**Deliverable:** the Python installer, passing the full suite, with zero install-time
DSL imports. Independently shippable; ship it as its own release if the timing fits.

---

## 3. Target architecture

```
src/native/
  CMakeLists.txt            + three targets (see below)
  src/core/                 photoncore — static lib, shared plugin/installer/tests
    version.h               kPhotonVersion — THE one definition (§8c)
    constants.{h,cpp}       AIRFRAMES, AIRFRAME_CFG_IDS, INTERIOR_*, SCREENS_*,
                            GLOW_AIRFRAMES, WINGS_FOR, PLUGIN_*, URLs, marker format,
                            interior-OBJ needle (today kInteriorObjNeedle in plugin.cpp)
    fsutil.{h,cpp}          atomic_write_bytes, files_identical, UTF-8 path helpers
    marker.{h,cpp}          OBJ version-marker inject / parse
    manifest.{h,cpp}        photon_manifest.json read/write (schema unchanged, §7)
    acf.{h,cpp}             the numeric-parse / style-re-render .acf engine (§7a)
    patch_acf.{h,cpp}       cockpit-spot patch (PATCH/STOCK tables + sentinel)
    patch_acf_screens.{h,cpp}  _obja attach/detach, frame copied from lights_inn row
    patch_glow.{h,cpp}      dataref token swap + REDIRECT table
    patch_realwings.{h,cpp} data-driven: applies payload/realwings_patch.json
    detect.{h,cpp}          root candidates, Steam VDF, aircraft scan, statuses
    payload.{h,cpp}         bundled-payload location/enumeration (no dev mode)
    actions.{h,cpp}         install / uninstall orchestration
    config.{h,cpp}          saved X-Plane root (per-OS config dir)
  src/installer/            photon-installer — console exe
    main.cpp                argparse, flow state machine (port of install.py main)
    screens.{h,cpp}         the six screens + X-Plane-running guard
    tui.{h,cpp}             ANSI engine port (§4)
    cli.{h,cpp}             headless subcommands (§5)
    platform.{h,cpp}        browser-open, editor-open, process check, console setup
    log.{h,cpp}             timestamped flush-on-write log file
  third_party/nlohmann/     vendored json.hpp single header (+ README, like stb/)
```

**CMake targets.** `photoncore` (STATIC), linked by `ToLissPhoton` (the `.xpl`),
`photon-installer` (console exe), and `photoncore_tests` (plain-`main` assert exe — the
same offline pattern that found the `CmdListsCount` bug). ⚠ **`photoncore` must not
include any XPLM header or link the SDK** — the installer builds on machines/runners
with no SDK-dependent step, and that independence is what keeps the CI matrix simple.
Anything needing XPLM stays in `plugin.cpp` and calls into core, never the reverse.

**Binary hygiene (real end-user gotchas):** Windows links the CRT statically (`/MT`,
`CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"`) so users don't
hit a missing-VC++-redistributable dialog; macOS builds universal
(`CMAKE_OSX_ARCHITECTURES "arm64;x86_64"`, same as the `.xpl`); Linux links
`-static-libgcc -static-libstdc++` (same flags the `.xpl` already uses).

**JSON:** vendor nlohmann/json (single header, MIT — the repo already vendors imgui and
stb this way). Used for the manifest, `installer_config.json`, and
`realwings_patch.json`. The manifest **schema does not change**, and reads must be
tolerant exactly as `read_manifest` is today (missing/corrupt → treated as absent):
manifests written by every prior Python installer version must keep working.

---

## 4. The TUI port

**Recommendation: port the bespoke engine 1:1, no TUI library.** `tui.py` is ~500 lines
of self-contained ANSI + keyboard code with zero deps, and the flow code is written
against its exact API (`render`, `menu`, `text_prompt`, `flash`, `read_key_timeout`).
FTXUI or notcurses would mean rewriting every screen against a different model and
re-QA-ing the look for no user-visible gain.

Port these faithfully — each encodes a fixed bug or a deliberate choice:

- `enable_vt` → `SetConsoleMode(…, ENABLE_VIRTUAL_TERMINAL_PROCESSING…)` **and**
  `SetConsoleOutputCP(CP_UTF8)` (the `sys.stdout.reconfigure(encoding="utf-8")`
  equivalent — the header rules and glyphs `▁ ▔ ► ▲ ▼ ✓ …` are multi-byte UTF-8).
  Source files hold them as UTF-8 string literals.
- Key decoding: Windows `_getwch()` with the `\x00`/`\xe0` extended-key prefix pairs;
  POSIX raw-mode `termios` + the ESC-sequence decode incl. `ESC[5~`-style PgUp/PgDn,
  and the short `select()` that distinguishes a lone ESC from a sequence.
  `read_key_timeout` keeps the "poll `kbhit` / `select`, then delegate to `read_key`"
  split so key handling stays in one place.
- The ANSI-aware `_vis_len` / `_clip` / `_wrap_ansi` (with `_active_sgr` re-opening the
  live color on continuation lines), the compose-then-single-write `render`, the
  focus-following scroll window, and the terminal-size `-1` last-column guard.
- Ctrl-C → the same immediate-clean-exit path as ESC-quit (alt-screen left, cursor
  shown — `_quit`'s ordering).

The interactive flow (`screens.cpp`) is a straight port of `install.py`'s six screens
and its Back-exception state machine (use a `Back` exception or a sentinel return —
either is fine, but keep the "ESC steps back exactly one stage, except
aircraft-selection → root" topology and the Complete screen's no-back rule). Keep the
worker-thread + animated-ellipsis Perform screen (`std::thread`, progress via a mutexed
string — the Python version leans on GIL atomicity, C++ must lock).

**Platform glue table:**

| Python | Windows | macOS | Linux |
|---|---|---|---|
| `webbrowser.open` | `ShellExecuteW(…"open", url…)` | `open <url>` | `xdg-open <url>` |
| log viewer | `notepad.exe <path>` (⚠ deliberately not the shell-association route — see `log.py`'s comment) | `open <path>` | `xdg-open` |
| `tasklist /FI` X-Plane check | `CreateToolhelp32Snapshot` + `Process32FirstW` (in-process beats spawning tasklist — the 2 s poll loop runs it repeatedly) | `ps -A -o comm=` popen | same |
| config dir | `%APPDATA%\ToLissPhoton` | `~/Library/Application Support/ToLissPhoton` | `$XDG_CONFIG_HOME`/`~/.config` |
| `os.replace` | `MoveFileExW(…, MOVEFILE_REPLACE_EXISTING)` | `rename(2)` | `rename(2)` |
| `is_symlink()` | `std::filesystem::is_symlink` (⚠ on the path itself, never after following — see §7's uninstall rules) | same | same |

⚠ **Paths are UTF-8 end to end.** All internal strings UTF-8; convert at the Win32
boundary (`std::filesystem::path` from `u8string`). An aircraft folder named by a user
in any language must survive the scan, the manifest (JSON is UTF-8), and the TUI table.
The process check must never make the installer *fail* — port `xplane_running`'s
"any error means not running" contract.

---

## 5. Headless CLI mode

New, and load-bearing for tests (§6) and CI smoke checks:

```
photon-installer                        # no args → the TUI, exactly as today
photon-installer detect  [--xplane-root P] --json
photon-installer status  --aircraft P --json
photon-installer install --aircraft P --wing stock|durantula|realwings
                         [--interior] [--dry-run] [--xplane-root P] --json
photon-installer uninstall --aircraft P [--remove-plugin] [--dry-run] --json
photon-installer version
```

`--json` prints a machine-readable result (steps list, error, manifest summary) on
stdout; exit code 0/1. No prompts in CLI mode — missing required input is an error, and
the X-Plane-running guard becomes a hard refusal plus `--force`. This replaces
`--dry-run`'s current role as the only scriptable surface and is the layer the Python
tests drive.

---

## 6. Test strategy

Three layers, replacing the current direct-import tests:

1. **`photoncore_tests` (C++, plain asserts, no framework)** — the pure logic:
   the `.acf` engine (⚠ §7a's XP11/XP12 format trap gets exhaustive cases), marker
   inject/parse, manifest round-trip + tolerant reads, `_wrap_ansi`/`_clip`/`_vis_len`,
   `files_identical`, glow token swap, realwings-JSON application. Runs in CI on all
   three OSes; also the offline reproduction harness when something misbehaves.
2. **Python `unittest` integration tests** — port `test_actions.py`, `test_detect.py`,
   `test_screens_install.py` to build fake trees exactly as today but drive
   `photon-installer <subcommand> --json` via `subprocess` (binary path from a
   `PHOTON_INSTALLER_EXE` env var, defaulting to the CMake output; **skip with a loud
   message** when absent, mirroring how the suite already stubs the plugin rather than
   requiring the C++ toolchain). The fake fat-plugin stub, the real-release-per-module
   fixture, and the RealWings/glow round-trip tests all carry over.
3. **The parity harness (transition only).** While both installers exist *in the repo* —
   which outlasts the point where both SHIP, since only one ever did: one test runs
   the Python installer and the C++ installer against two copies of the same fake tree
   (same actions, same payload) and diffs the resulting trees byte-for-byte plus the
   manifests key-for-key. This is the single strongest correctness net for the port —
   `actions.py`'s value is in accumulated edge-case handling, and a tree diff catches a
   missed branch that reading the code will not. Delete it at §9 cutover.

The TUI itself gets no automated driver (today's monkeypatched `tui.read_key` tests die
with the Python module); manual QA per §9. Keep `tests/test_version.py` but repoint it:
it now pins `core/version.h` against whatever Python-side copies remain
(`make_release.py`'s parse, `readme.py`).

---

## 7. Behavior-preservation checklist

Every item below is a shipped bug-fix or a deliberate contract in the current code.
The port is done when each one demonstrably survives; most are covered by the parity
harness, but the implementer should check them off explicitly against
`installer/actions.py` / `detect.py` / `payload.py`, which remain the reference spec
until cutover.

**7a. The `.acf` engine** (`patch_acf.py`'s module docstring is the spec):
parse numerically, re-render **in each file's own detected float style** (XP12
`-70.000000000` vs XP11 `-70.0`); detection by the **string** sentinel
`_spot_name_3d/* = none`, never a hash; reversal writes recorded stock values, never a
snapshot; read and write **binary/CRLF-preserving** (`newline=""` both sides — in C++
read bytes, split on the file's own line endings, write bytes). All four
`ACF_SUFFIXES` files per airframe. The screens patcher additionally: copies its
position frame **from the file's own `lights_inn.obj` `_obja` row** and refuses
(logged, non-fatal, install continues) when none exists.

**7b. Install ordering & idempotence:** plugin binary **first** (a locked-`.xpl`
failure must abort before the OBJ marker is injected — the "installer thinks it
succeeded" bug); skip byte-identical `.xpl`s (`plugin_is_current` is also what lets
`screen_check_not_running` skip the close-X-Plane wait); `_backup_once` never
overwrites an existing backup; marker injected after `POINT_COUNTS`, preserving the
file's own newline flavor.

**7c. Uninstall asymmetries:** `backed_up[]` restores; `added[]` **deletes**
(`pedals_details_{1,2}.png`, `lights_screens.obj` — no stock counterpart exists);
screens **detach the `.acf` first, then delete the OBJ**; livery overrides restored
from `backup/liveries/` with the `/`→`__` name flattening; plugin-folder removal
prunes `PLUGIN_USER_DIRS` **by name against the shipped-data list**, never `rmtree`,
with the ⚠ `is_symlink()`-checked-**before**-`is_file()` rule (a dangling dev symlink
must survive); `rmdir` only-if-empty semantics on the backup dir and plugin root.

**7d. Symlinks:** install and uninstall both **skip a symlinked `panelfx.txt`** (check
the destination, don't follow); overlay images touched only by shipped name in both
directions.

**7e. Detection:** content-based aircraft identification (cfg `module` URL → cfg
`name` → OBJ filename → folder glob, in that order); the `os.walk` prune-on-qualify;
marker **corroborates** the manifest (manifest present + marker gone = stale/needs
reinstall); `interior_status` stays an independent axis; `lights_inn.obj` must never
enter the airframe-fingerprint slot; Steam `libraryfolders.vdf` hand-parse; the
`$XPLANE_ROOT` → running-from-inside → Steam → common-dirs candidate order; remembered
root revalidated before use.

**7f. Strictness gates:** `WINGS_FOR` checked before any payload resolution (a339
offers `stock` only, has no interior, no screens); `GLOW_AIRFRAMES`/`REDIRECT` must
stay in sync with `GlowMapForIcao` — which becomes automatic once both read
`core/constants` (call that out in the code as the reason the table lives there);
interior textures: a missing file is an **error**, not a skip; wing-switch away from
RealWings reverses the old in-place patch (the `was_realwings` dance).

**7g. UX contracts:** interior texture copy reports per-file progress (~57 MiB reads
as a hang otherwise); step-reveal pacing skipped under `--dry-run`; the
X-Plane-running screen's poll/ESC/ENTER semantics; drag-and-drop quote stripping in
the manual path prompt; `Refresh list…`; the Complete screen's disabled-back.

---

## 8. Build, release, CI

**8a. CMake.** The three targets live in the existing `src/native/CMakeLists.txt`
(one `build`/`build-dev` tree pair keeps working). `deploy.ps1` builds
`photon-installer` incidentally but its job is unchanged — deploy does not run the
installer.

**8b. Release staging — DONE (2026-08-09).** `make_release.py` keeps building `payload/`
(OBJs, textures, plugin, plugindata, now `realwings_patch.json`; **no `dsl/`**), stages the
compiled installer binary beside it, and under **`--bundle`** packs the per-platform
download itself: `<name>/{photon-installer[.exe], data/, README.txt}` + archive, `.zip` or
`.tar.gz` by host OS. `make_exe.py` and the PyInstaller dependency are **deleted**;
`readme.py` lost its `Python` kind and its two-installer paragraph.

⚠ **The bundle names the payload folder `data/` and the staging tree names it `payload/`.**
Both are accepted by `core/payload.cpp` (in that precedence), which is what lets one binary
run from a checkout and from a download without knowing which it is in. Do not "unify" them:
`payload/` is internal jargon that the end-user download deliberately does not use, and
`data/` in a checkout would shadow a staged payload with a stale bundle copy.

**8c. Version single-sourcing.** `core/version.h` becomes the one definition.
`make_release.py` and `readme.py` regex-read it (same trick test_version.py uses
today). `installer/constants.VERSION` dies with the Python installer.

**8d. CI (`release.yml`) — WIRED, and now CUT DOWN (2026-08-09).** One `cmake --build` in
the per-OS `build` job already produced all four targets, so the added steps are: locate the
two executables (multi-config VS puts them under `Release/`, single-config does not), run
`photoncore_tests`, then run the whole Python suite with `PHOTON_INSTALLER_EXE` pointed at
the fresh binary. The PyInstaller step and the **`universal` job are now gone** per the §10
decision — the chain is `build` → `collect` → `release`, three jobs.

⚠ **The transient `xpl-<arch>` upload went with it.** It existed for exactly one consumer:
the `universal` job merging three arches into one fat plugin. No bundle is fat any more
(each ships its own runner's arch), so the artifact had no consumer left — and a
`retention-days: 1` upload that nothing downloads is pure CI cost that still *looks* load-bearing
in the log.

⚠ **The locate step FAILS THE JOB when either binary is missing, and that refusal is
the feature.** `tests/test_parity.py` SKIPS when `photon-installer` is absent —
correct for a contributor with no C++ toolchain, wrong in CI, where a
silently-skipped correctness net is worse than none. Without the hard check, a
CMake change that stopped building the installer would turn the strongest net in the
port off and leave the suite green.

⚠ The CI chain has **never been exercised by a real `v*` tag push** — that risk
predates this work and compounds with it; a full `workflow_dispatch` dry run is a
gate for §9, not an afterthought. It now also covers steps that did not exist when
the risk was first written down.

**8e. macOS signing** stays an open thread, and now covers the installer binary too —
an unsigned downloaded *executable* trips Gatekeeper harder than a `.xpl` does. Flag
in the release notes; the existing "codesign/notarize not done" thread absorbs it.

---

## 9. Cutover

1. Parity harness green across install / reinstall / upgrade / wing-switch /
   uninstall × exterior / +interior / +screens × fake XP11+XP12 trees. **DONE —
   re-run 2026-08-09** on the local matrix (23 cases, and it *ran*: the harness
   skips loudly when the binary is absent, so "green" only counts with
   `PHOTON_INSTALLER_EXE` set or the CMake output in place). CI runs it per-OS
   from §8d onward.
2. **Manual TUI QA on all three OSes** — real terminals: Windows Terminal **and**
   conhost, Terminal.app, a couple of Linux emulators. ⚠ **The one gate no test
   covers**, because raw-mode key handling cannot be driven from a pipe: on Windows
   `_getwch` reads the console directly, not stdin. Walk every screen, and
   specifically check the arrow/PgUp/Home keys (the extended-key prefix pairs), a
   lone ESC at each stage, Ctrl-C from a menu **and** during the install itself, a
   terminal resized mid-run, and a drag-and-dropped path with spaces.
   **WINDOWS: DONE, clean (2026-08-09)** — walked against a throwaway tree built
   from `test_parity.build_tree` (both `.acf` float styles, a RealWings folder, a
   livery override, a path with a space), payload staged by
   `make_release.py --payload-only` and passed with `--payload-dir`.
   **macOS and Linux: OUTSTANDING**, and they are the majority of what this step
   was protecting — the POSIX `termios`/ESC-decode path shares no code with the
   Windows one, so nothing about the Windows result carries over. Neither can be
   retired from a Windows dev machine.
3. ~~One release ships **both** installers side by side.~~ **WITHDRAWN, not performed
   (2026-08-09, Alex's call).** No release ever shipped two installers: 0.8.1 was built
   with both, looked at, and cut back to one before it left the building. **DONE in that
   pass** — `make_exe.py`, the PyInstaller dependency, the Python-Universal bundle and the
   `universal` job are deleted, `make_release.py --bundle` packs the one-executable bundle,
   and `readme.py`, `make_allzip.py`, `release.yml`, `docs/installer.md` and `CLAUDE.md` are
   updated to match.

   ⚠ **The reasoning the transition rested on did not survive contact with the artifact.**
   Two installers in one folder was meant as a safety net — "if one gives you trouble, try
   the other" — but what it actually hands a user is a folder with two executables and a
   paragraph explaining which to run, on a download whose entire job is to be
   double-clicked. The net was for *us*, and the parity harness is the same net without
   the user paying for it.

   **AND THE DELETION IS DONE TOO** (2026-08-09, same day, second pass). Gone:
   `install.py`, the whole `installer/` package, `build/patch_acf.py`,
   `build/patch_acf_screens.py`, `build/patch_glow.py` and `tests/test_parity.py`, plus the
   ten Python test modules that drove them.

   ⚠ **THE COVERAGE WENT FIRST, NOT AFTER.** `core/detect.cpp`, `core/actions.cpp` and
   `core/payload.cpp` had **no C++ tests at all** — they were covered from Python and by the
   parity harness, so the deletion would have removed the only net over three of the five
   riskiest files in the port. `src/native/tests/install_tests.cpp` (50 cases) was written
   and green BEFORE anything was removed. That ordering is the whole reason this was safe;
   reversing it would have left a green build with nothing checking it.

   What the retarget needed, since `installer/constants.py` was imported by three build
   scripts and most of `tests/`:

   - **`build/constants.py`** — the Python-side mirror of the handful of facts the build
     tooling and the DSL tests need, pinned to `core/constants.cpp` by
     `tests/test_version.py`. Deliberately small: detection tables, backup/manifest names,
     `.acf` suffixes, wing labels and the glow map stayed C++-only.
   - **`build/realwings_apply.py`** (was `installer/realwings.py`) — kept as a DEV tool.
     `build_objs.py build --wing realwings --write` resolves the patch from `.phdsl` in
     memory and applies it immediately; the compiled installer has no DSL parser, so
     without this a `.phdsl` edit would need a full `make_release` to see in-sim.
     ⚠ It must stay byte-compatible with `core/patch_realwings.cpp` and **nothing checks
     that automatically any more.**
   - **`build/readme.py`** now regexes the splash art out of `installer/screens.cpp`
     instead of importing `install.LOGO_ART`.

   ⚠ **ONE CAPABILITY WAS LOST, not replaced**: the standalone `.acf`/glow patch commands.
   `photon-installer install --aircraft <dir>` does all of them as part of the base install,
   but there is no detach-only equivalent of `patch_acf_screens.py --reverse` — `uninstall`
   is the reversal. Worth a CLI subcommand if the dev loop misses it.

   **Left of §9:** the macOS and Linux halves of the manual TUI QA (step 2).

---

## 10. Decisions (all four taken as recommended, 2026-08-07)

1. **Fate of the Python-Universal bundle.** DROPPED at cutover; keeping the Python
   installer alive as a fourth bundle recreates the dual-maintenance problem this
   port exists to end. **DONE 2026-08-09** — a release is now three downloads plus the
   all-platforms zip.
2. **Transition length.** ~~Exactly one release side by side, then delete.~~
   **REVISED 2026-08-09: zero releases side by side.** The two-installer bundle was built
   for 0.8.1, reviewed, and cut before publication — see §9 step 3 for why. `make_release.py`
   stages exactly one installer, and `build_bundle` has no way to express a second.
3. **TUI approach.** 1:1 bespoke port (§4). Adopting FTXUI is a redesign, not a port.
   **Done** — and the 1:1-ness is enforced, not asserted: every expectation in the
   engine's test cases was generated by running `installer/tui.py`, so the two agree
   character for character on wrapping, clipping and color re-opening.
4. **JSON library.** nlohmann/json vendored at
   `src/native/third_party/nlohmann/json.hpp` (v3.11.3, MIT), alongside imgui and
   stb. See its README for what compiles it and why exceptions stay on.

## 10a. What Phase 4 landed, and the traps it hit

Four files under `src/native/src/installer/`: `tui.{h,cpp}` (the ANSI engine),
`screens.{h,cpp}` (the six screens and the Back state machine), `platform.{h,cpp}`
(the §4 glue table) and `log.{h,cpp}`. `main.cpp` now dispatches: a recognized
subcommand goes to the CLI, a bare invocation opens the TUI, and **anything else is
an error** — ⚠ a mistyped flag must not open a full-screen UI over the top of the
user's own message.

Things that were not obvious from the Python and are worth not rediscovering:

- ⚠ **Every length in the engine is in CODEPOINTS, not bytes.** The rules and arrows
  (`▁ ▔ ► ▲ ▼ ✓ ← →`) are multi-byte UTF-8 literals, so a byte-counting `VisLen`
  makes the header rule three times too long and clips every row to a third of the
  window. Python got this free; C++ has to say it in `VisLen`, `Clip`, `WrapAnsi`
  and the backspace in `TextPrompt`.
- ⚠ **`Clip` always appends a RESET, so a "blank" row is not an empty string.** It
  is the right behaviour (a row ending mid-color would bleed into the next, and the
  frame is blitted as one string) and it is a trap for anything asserting on rows.
- ⚠ **Ctrl-C needs three separate routes to one handler.** POSIX raw mode clears
  ISIG so `\x03` arrives as a keystroke; between reads it is a real SIGINT; and on
  Windows `_getwch` **cannot see Ctrl-C at all** — it goes to the console control
  handler. Miss the Windows one and the alt screen is never left, i.e. Ctrl-C wrecks
  the user's terminal.
- ⚠ **`NOMINMAX` had to become PUBLIC on `photoncore`.** Without it `windows.h`
  defines `min`/`max` as macros and the first `std::min(` in any consumer fails with
  *"illegal token on right side of '::'"* — an error naming neither `windows.h` nor
  the macro. Setting it on the library rather than per target is what stops the next
  target from rediscovering it. `WIN32_LEAN_AND_MEAN` rides along, which is why
  `main.cpp` includes `<shellapi.h>` explicitly.
- **`Render` is split into `ComposeFrame` + a blit**, so the layout is testable
  without a terminal. The composition — scroll window, focus remapped through
  wrapping, per-row clip — is where a port goes wrong invisibly, and capturing
  stdout to check it would be worse than not checking it.
- ⚠ **The Perform screen's progress string is MUTEXED.** The Python leans on GIL
  atomicity for the equivalent dict assignment; an unsynchronized `std::string`
  written by the worker and read by the animation loop is a torn read waiting to
  happen.

**One payload, two names.** The shipped per-platform bundle calls the folder `data/`
(the plainer end-user name, documented in its README) and `make_release.py` stages
`payload/` in a checkout. **One binary has to run out of both** — a dev restages and
reruns, a user downloads — so `payload::InitFromExecutable` accepts **`payload/`
first, `data/` second**. The order matters: a repo checkout that has staged
`payload/` is never shadowed by a `data/` left over from a bundle build. Both
directions are pinned in `tests/test_parity.py`.

**`--payload-dir P` overrides both** (2026-08-09), for the development loop: CMake
leaves the binary in an output directory with no bundle beside it, so a bare run
there reports *"this build is incomplete"* — which reads as a broken checkout rather
than a missing step — and restaging costs ~58 MiB of texture copies per edit. Stage
once, then rebuild-and-run. Three properties worth keeping:

- ⚠ **Handled in `main.cpp` BEFORE the CLI/TUI split**, so one implementation serves
  every mode. Threading it through both parsers gives two chances to forget it, and a
  silently-ignored flag reading the wrong bundle looks exactly like a broken payload.
- ⚠ **A path that is not a directory is refused BY NAME**, exit 2. Falling through to
  the generic message sends the user to re-download a bundle that is fine.
- The stored path is made **absolute**, so the log line and the `--json` report name
  something that still means the same thing when read later.

---

## 11. Phase order and rough sizes

| Phase | What | Ships alone? | Rough size |
|---|---|---|---|
| 1 | DSL decoupling in Python (§2) | **Yes** | ~300 lines Python moved/changed |
| 2 | `photoncore` + `photoncore_tests` (§3, §6.1) | No | ~2.5–3.5k lines C++ |
| 3 | Headless CLI + ported integration tests + parity harness (§5, §6.2–3) | No | ~600 lines C++, ~1k lines Python tests |
| 4 | TUI + screens + platform glue (§4) | No | ~1.5–2k lines C++ |
| 5 | Build/release/CI/version integration (§8) | No | CMake + YAML + Python edits |
| 6 | Cutover (§9) | Release | deletions + docs |

Phases 2–4 are sequential (each layers on the last); 5 can start once 3 exists. The
parity harness comes up with Phase 3 and stays green through everything after.

---

## 12. The DSL toolchain: considered and DEFERRED — do not reopen mid-port

Porting the dev/CI-side Python toolchain (`build_objs.py`, `photon_dsl.py`,
`make_overlays.py`, `derive_panel_rects.py`, `watch.py`, `make_release.py`) to C++ was
considered and **rejected** (decision with Alex, 2026-08-07). The scope of THIS plan is
exactly §1's table; an implementer finding Python "left over" after Phase 6 is looking
at the intended end state, not unfinished work.

Why it stays Python:

- **After this plan lands, no Python ever runs on a user's machine or in the install
  path.** What remains is build-machine/CI tooling — the role CMake scripts and code
  generators play in any C++ project. There is no distribution win left to collect.
- **The blast radius is the test suite, not the toolchain.** ~3.7k lines of toolchain,
  but ~4.2k lines of tests import `build_objs`/`photon_dsl` *in-process* and assert on
  internals — including most of the named tripwire guards in `tests/test_interior.py`.
  A port rewrites the deepest correctness net simultaneously with the thing it guards.
- **`check` requires byte-identical emitter output.** The frozen goldens in
  `reference/photon/` pin the Python emitter's exact float formatting; a C++ port that
  differs by rendering makes every golden "fail" as noise and puts the "never repoint
  `check` at generated output" tripwire under pressure for the worst possible reason.
- **No shared-library payoff.** The plugin never parses the DSL; the emitter and the
  plugin share facts (gate names, `INTERIOR_OBJ`, dataref names), not code — and those
  facts land in `core/constants` via §3, which Python-side tooling can read/pin without
  an emitter port.
- **`watch.py`'s edit→auto-reload loop is the toolchain's whole job**; a no-compile
  edit-run loop is the right tool for live `.phdsl` tuning.

**Standing direction:** Python is *frozen infrastructure* — it shrinks or holds steady,
never grows. New logic that could live on either side of the line defaults to C++
(`photoncore` or the plugin).

**Revisit trigger:** the plugin (or another shipped artifact) needing to understand the
DSL *at runtime*. That would create a real second consumer of a C++ parser/emitter and
reopens the question; nothing today does.
