# ToLiss Photon — native `.xpl` plugin

The shipping runtime: a compiled `.xpl` needing **no XPPython3 and no Python**. It replaces
the retired Python plugin `PI_ToLissPhoton.py` and, on startup, deletes any leftover copy of
that script and of the older FlyWithLua scripts, so a stale copy can't load under XPPython3
and fight this plugin for the same per-frame beacon/strobe writes
(`RemoveSupersededFiles` in `src/plugin.cpp`).

**Status: complete, compiles clean, confirmed working in-sim.** `src/plugin.cpp` holds the
9 exterior + 5 interior category datarefs, the waveform engine (after-flight-model loop),
the Plugins menu, every window (Dear ImGui), per-livery persistence (self-contained JSON),
Auto resolution (1 Hz), ToLiss detection, and the superseded-file sweep.

## The three CMake targets

`src/native/CMakeLists.txt` builds four things, not one:

| Target | What | Needs the SDK? |
|---|---|---|
| `ToLissPhoton` | the shipping `.xpl` | yes |
| `photoncore` | STATIC lib: constants, version, the file patchers, detection, the manifest | **no** |
| `photon-installer` | the compiled end-user installer — TUI + headless CLI (`docs/installer_cpp_plan.md`) | **no** |
| `photoncore_tests` | plain-`main()` assert exe over `photoncore` **and the TUI's string engine** | **no** |

⚠ **`photoncore` MUST NOT INCLUDE AN XPLM HEADER OR LINK THE SDK.** The installer and
the core tests build on machines and CI runners with no X-Plane SDK, and that
independence is what keeps the release matrix simple. The dependency runs one way:
`plugin.cpp` calls **into** core, never the reverse.
`test_photoncore_includes_no_xplm_header` in `tests/test_version.py` pins it.

That is what `-DPHOTON_CORE_ONLY=ON` is for — it configures and returns before the
SDK check, so a contributor (or a CI job) with no SDK can still build and run
everything except the plugin:

```powershell
cmake -S src/native -B src/native/build-core -A x64 -DPHOTON_CORE_ONLY=ON
cmake --build src/native/build-core --config Release
src\native\build-core\Release\photoncore_tests.exe
```

⚠ **`photoncore` exists to end a class of drift**, not to save typing. The version,
the interior-OBJ needle and the skin-glow index map were each written down twice —
once in `plugin.cpp`, once on the installer side — and each pair fails SILENTLY when
it disagrees: the version drifted 0.6 against 0.7 through a whole feature (the About
tab just reports an old number, which looks like a half-finished install), and a
glow-index mismatch makes redirected regions go **dark**. `plugin.cpp` now reads
`photon::kPhotonVersion`, `photon::kInteriorObj{,Needle}` and
`photon::GlowIndicesFor` out of core.

⚠ **The CRT is linked STATICALLY on Windows, globally** (`CMAKE_MSVC_RUNTIME_LIBRARY`
at the top of the CMakeLists). It has to be global — `photoncore` goes into both the
`.xpl` and the installer, and mixing `/MT` with `/MD` across a static library and its
consumer is a warning at best and two heaps at worst. Static is the right side:
`photon-installer` is a file an end user downloads and double-clicks, and a
missing-VC++-redistributable dialog on a lighting mod reads as malware. The `.xpl`
gains the same property (it previously took the default `/MD`).

⚠ **`NOMINMAX` / `WIN32_LEAN_AND_MEAN` / `_CRT_SECURE_NO_WARNINGS` are PUBLIC on
`photoncore`, not private.** Every consumer that also includes `windows.h` needs the
same guards, and `NOMINMAX` is the one that bites: without it `windows.h` defines
`min`/`max` as **macros**, and the first `std::min(` in a consumer fails with
*"illegal token on right side of '::'"* — an error naming neither `windows.h` nor the
macro. Setting it on the library is what stops each new target rediscovering it. The
cost is that `WIN32_LEAN_AND_MEAN` rides along, so a consumer wanting
`ShellExecuteW`/`CommandLineToArgvW` includes `<shellapi.h>` itself.

⚠ **`photon-installer` is a CONSOLE subsystem app, permanently.** It is a terminal
UI; a `WIN32_EXECUTABLE ON` build would launch with no console to draw in and look
like nothing happened.

## Get the SDK (one-time)

Download from <https://developer.x-plane.com/sdk/> and unzip so that
`src/native/SDK/CHeaders/XPLM/XPLMDefs.h` exists. `SDK/` is gitignored. Or pass
`-DXPLANE_SDK=/path/to/SDK`.

```bash
# from src/native/, matching what CI does:
curl -sL -o sdk.zip https://developer.x-plane.com/wp-content/plugins/code-sample-generation/sdk_zip_files/XPSDK411.zip
unzip -q sdk.zip && rm sdk.zip     # extracts ./SDK/
```

## Build and install

Needs CMake + a C++17 compiler. On this machine both ship inside **VS 18 Community** (VS
2022 lacks the C++ workload), so they aren't on `PATH`.

```powershell
.\deploy.ps1           # EVERYTHING: overlays, OBJs, plugin -> X-Plane
.\deploy.ps1 -Dev      # the same, as a DEV build (see below)
.\deploy.ps1 -Test     # + the unit suite first, stopping if it fails
.\deploy.ps1 -NoObjs   # plugin only (the old behavior)
.\deploy.ps1 -NoBuild  # just copy what's already built
```

**`deploy.ps1` is the one command**, and that is the point of it: a change to the C++, to
`lights.*.phdsl`, or to `build/make_overlays.py` all reach the sim the same way. It runs, in
order, `build/make_overlays.py` → **the overlay copy** → the X-Plane-is-running check →
`build/build_objs.py build --target all --write` → cmake → the copies below. Any step
failing stops the deploy, because a run that installs the `.xpl` but not the OBJs leaves the
sim half-updated and looking like a lighting bug.

⚠ **`--target all`, not `both`** — `both` omits the screens OBJ, which is the documented way
an edited screens fixture appears not to rebuild. Override with `-Target both` if you want
the narrower set.

Options: `-XPlaneRoot <path>` (default: the Steam install), `-Config <cfg>`, `-Target`,
`-Wing <stock|durantula|realwings>`. `-XPlaneRoot` is **exported as `XPLANE_ROOT`** so the
OBJ build targets the same sim — it locates X-Plane by its own rules otherwise, and without
this the two halves of a deploy can land in two different installs.

It refuses to run while X-Plane is running — a loaded `.xpl` is locked — and that check
happens **before** the OBJ build and the compile, so the refusal costs a `-Test` run at
worst rather than a full compile.

⚠ **The overlay images are the ONE thing that lands ahead of that check** (2026-08-10), and
deliberately: the plugin reads each one into a texture once and closes the file, and the
folder is not polled, so nothing holds them open and replacing them mid-session is safe. So
`deploy.ps1` with the sim **up** is the supported way to push an edited overlay — it copies
the images, says so, and then refuses the rest. `Rescan images` on the Panel FX tab picks up
the new bytes without a restart; a shipping build has no rescan button, so there it waits
for the next sim start. A file that will not copy (open in a paint program) warns and the
others still go.

The one thing it does **not** do is attach `lights_screens.obj` to the `.acf`; that belongs
to `install.py`'s base install (or `build/patch_acf_screens.py`), and a ToLiss update reverts
it. The summary reminds you whenever a screens OBJ was built.

By hand:

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build -A x64 -DPHOTON_DEV=OFF
& $cmake --build build --config Release
```

macOS / Linux: `cmake -B build && cmake --build build`. Output lands in the fat-plugin
layout X-Plane expects — `build/ToLissPhoton/<arch>/ToLissPhoton.xpl`, arch =
`win_x64 | mac_x64 | lin_x64` — and all three can sit side by side.

## Running and testing `photon-installer`

**One command.** `deploy.ps1` puts the plugin and the OBJs into X-Plane;
`run-installer.ps1` builds the thing users double-click and starts it.

```powershell
src\native\run-installer.ps1 -DryRun            # the GUI. THE usual one.
src\native\run-installer.ps1 -Tui -DryRun       # the terminal installer
src\native\run-installer.ps1 -Cli detect --json # a headless subcommand
src\native\run-installer.ps1 -Test -DryRun      # + both suites first
```

It **builds the plugin and restages the payload by itself** — `-Payload` forces a
restage, `-NoPlugin` skips the `.xpl` build. See the table below for why.

⚠ **`-DryRun` is how you test an install.** Without it the installer writes to a
real X-Plane install — it is the shipping tool, not a sandbox.

⚠ **THE SCRIPT IS THE DOCUMENTATION.** These steps change as the installer is
reshaped, and prose about them goes stale silently — so everything a run needs
lives in `run-installer.ps1` and its header comment, and nothing here repeats it.
**When the steps change, change the script.** What it exists to stop you having to
remember:

| Trap | What the script does |
|---|---|
| A fresh binary has **no payload** and installing fails with "this build is incomplete" | passes `--payload-dir release\payload` when one is staged, and says so loudly when one is not |
| Rust is installed but **not on `PATH`** (`%USERPROFILE%\.cargo\bin`) | adds it; "rustc: command not found" reads as *not installed* |
| CMake is inside VS 18, not on `PATH` | finds it, same as `deploy.ps1` |
| GUI and no-GUI builds share a tree → Slint recompiles on every flip | separate `build-gui/` and `build-core/` |
| A cached `PHOTON_GUI=ON` silently makes a "plain" build need Rust | states the flag explicitly every configure |
| A **GUI-subsystem process does not block the shell**, so `-Cli` would return before it ran | `Start-Process -Wait`, and propagates the exit code |
| The `.xpl` needs the X-Plane SDK | keeps `build-gui`/`build-core` on `-DPHOTON_CORE_ONLY=ON`; the plugin gets its own tree, and a missing SDK **skips** that step with a warning rather than failing |
| **Nothing built the `.xpl`**, so a test install laid down whatever plugin `deploy.ps1` last sent to the sim | builds the `ToLissPhoton` target into `build/` before staging |
| **`--payload-only` staged `<repo>\payload` while the script ran against `release\payload`** — restaging a tree nothing read | one payload location, `release/payload`, for every mode |
| A staged plugin silently older than the source | restages when the staged `.xpl` is older than the built one, prints its build time before every run, and `make_release.py` refuses to stage one older than `src/native/src/` |

⚠ **THE LAST THREE ROWS ARE ONE SHIPPED BUG** (2026-08-14) and it had no symptom:
the installer reported success, the files landed, X-Plane loaded a plugin — last
week's. The only visible trace is the About tab reporting an old version, which
looks exactly like a half-finished install. That is why the `.xpl`'s build time is
now printed next to the binary on every run: it is the one fact that makes this
failure legible from outside.

Only the first Slint build is slow (it compiles from source); after that it is
cached in `build-gui/` and a rebuild is seconds.

### Doing it by hand

Rarely necessary, and the script's header is the reference. The one flag worth
knowing on its own is `--payload-dir`: it is accepted by **every** mode, because it
is handled before the GUI/TUI/CLI split. Stage the bundle **once**
(`python build\make_release.py --payload-only`, which writes `release\payload`)
and the loop is a rebuild —
restaging re-copies ~58 MiB of interior textures every time, which is why the flag
exists. ⚠ A path that is not a directory is refused **by name** (exit 2) rather
than falling through to "this build is incomplete", which would send you to
re-download a bundle that is fine over an argument you mistyped.

## `PHOTON_DEV` — the one dev flag

`-DPHOTON_DEV=ON` (or `deploy.ps1 -Dev`) compiles in **everything that must not ship**: the
Dev menu and its window, the panel-FBO probe, the FX layer **editor** and its history, the
live light editor, and the in-sim build commands. OFF by default, so a plain build is always
a release build and `make_release.py` cannot bundle something that paints over the captain's
PFD.

⚠ **The FX COMPOSITOR itself is no longer behind this flag** (2026-08-01). It ships, driven
by the `panelfx.txt` the installer writes into the plugin folder and controlled from the
settings window's Displays tab. What is dev-only is the tooling that *authors* that file.
The two ways to get this wrong are symmetric and both silent — a compositor left inside the
guard runs fine here and does nothing in a release; a probe left outside it floods a
release's cockpit magenta — so `ShippingBuildTests` in `tests/test_panel_fx.py` reads
`plugin.cpp`'s guard nesting and pins which side each symbol is on.

**The two flavors use separate build trees** — `build/` and `build-dev/` — so flipping the
flag is a copy, not a full recompile. `deploy.ps1` always passes `-DPHOTON_DEV` explicitly,
because a cached `ON` would otherwise be sticky.

**The settings window** (Plugins ▸ ToLiss Photon ▸ … ) is the SHIPPING UI and is one tabbed
window: Error / Exterior / Cockpit / Displays / About, and every menu path that opens it
names a tab. The **performance tool is a separate window**, opened from a button at the
bottom of the Displays tab and from nowhere else — a run is watched against the cockpit, so
it has to be able to sit somewhere the settings window is not.

⚠ **Two of those five tabs come and go, on two different questions**, and both used to be
one bool:

- **Error** appears when `gInstallStale` — a ToLiss update or a reinstall has put the stock
  exterior OBJ back — and when it does it **replaces** Exterior / Cockpit / Displays rather
  than joining them, because the plugin has switched every feature off and a grayed page
  beside the explanation invites trying it. The menu changes shape with it: one row,
  *Reinstall required…*, and About. See CLAUDE.md §*A reverted install*.
- **Cockpit** has THREE states, not two (2026-08-14): live where the mod is installed;
  **present but wholly disabled**, under a line naming the installer option, where the
  airframe supports the mod and it is not installed; and absent where the airframe has no
  cockpit mod at all (the A330-900). Its **submenu** is still installed-only — a menu row
  that opens a page of dead controls is worse than a tab that says so once you are looking
  at it.

⚠ **`ImGuiCol_WindowBg` is TRANSPARENT** and the opaque `#23282e` surface is painted by each
pane, as a child window (`UiBeginPanel` / `UiEndPanel`, color from `PhotonImgui::PanelBg()`).
A fill on the root also covers the strip behind the tab bar, and that row is X-Plane's own
chrome. The cost is that a pane which forgets the panel is text floating over the cockpit —
which is why the Dev window's eleven tabs go through `DevTab` rather than spelling it out.

**The panel meets the tab bar's 1 px underline**, and that takes two things: `kRootPadding`
is **0** so nothing insets the fill from the bar's own width, and `UiBeginTabBar` zeroes
`ItemSpacing.y` **around `BeginTabBar`**. ⚠ The second one is not optional and not
relocatable — ImGui captures that spacing into `tab_bar->ItemSpacingY` at the bar and places
tab content at `BarRect.Max.y + ItemSpacingY`, so a push anywhere later is silently inert.
The inset the content still gets is the panel's own `AlwaysUseWindowPadding`, inside the
fill rather than around it.

**The Dev window** (Plugins ▸ ToLiss Photon ▸ Dev ▸ Dev window…) is a second, dev-only
window holding every knob: Exterior / Cockpit / Displays / Perf (the same four builders the
shipping window uses, so a tuning pass never wonders whether a copy has drifted) · Panel FX ·
Lights · History · Probe · Build · Log · ImGui demo.

- **Lights** is the live light editor, and since 2026-08-10 it is also an **inspector**:
  with no debug OBJ installed it reads the `*.info.json` manifest a *plain* build leaves
  beside the normal OBJ and shows every cockpit and screen light read-only — the same
  tree, the same numbers, knobs disabled. A build writes one manifest kind and deletes the
  other, so which file is present decides the mode, per target. A read-only light shows
  the profile branch that is actually drawing (`variants` is indexed by the category
  dataref's value) and the toolbar's **branch** picker previews the other eras without
  changing the aircraft. Full notes: `docs/dev-tools.md` § The Lights tab.
- **Build** runs the repo's Python tooling without leaving the sim. It needs a repo path,
  persisted in `Output/preferences/ToLissPhoton_dev.txt` and seeded from `$PHOTON_REPO`.
- ⚠ Commands run **detached on a worker thread**. A synchronous `std::system()` in a draw
  callback freezes the simulator for as long as the generator takes. The thread touches only
  its own mutex-guarded output buffer and never an XPLM call — XPLM is not thread-safe.
- ⚠ **There is no reload-aircraft button, deliberately.** `sim/operation/reload_aircraft`
  runs synchronously and unloads the aircraft's plugins, so from a draw callback it tears
  the aircraft down mid-frame. It is a **menu item**, which is handler context. A `--write`
  build already triggers a reload through `watch.py`'s channel.
- **History** is the FX compositor's undo/redo plus an append-only journal
  (`ToLissPhoton_panelfx_history.txt`). Undo covers the edit you just made; the journal
  covers the session you already closed, which the single-state save file cannot. It is
  append-only on purpose — trimming it would discard exactly the old snapshot you want when
  you realize three days later that the look was better before.
- **Panel FX layers can be images**, not only colors — see "Overlay images" below.
- **Panel FX has two HOLD-THE-WORLD-STILL controls**, folded side by side at the top of the
  tab, because a look has two ends and neither can be authored while the sim moves the other
  one. *Ambient day / night blend* ▸ **Pin** holds the dusk blend at a value; *Brightness
  knob override* pins every brightness driver **while the mouse is down inside its slider**.
  ⚠ The second is HELD rather than latched, so it has a watchdog (`FxExpireDriveOverride`,
  from the compositor pass): the pane that would release it is not drawn when this window is
  hidden, and a drag interrupted by anything that hides it would otherwise pin the whole
  cockpit for the session. It pins the driver READ *and* writes the aircraft's own
  brightness datarefs, so the display underneath dims with the tint — ⚠ **and puts nothing
  back**: a bench control for a parked aeroplane, with a checkbox to leave the aircraft's
  state alone. Details: `docs/fcu_tint_plan.md` §8l.

## The performance tool

The **Performance window** (shipping — Displays tab ▸ *Performance analysis…*), and the Dev
window's **Perf** tab — **one builder, `BuildPerfPane(bool dev)`**. It switches effects off one at a time and times
the result: live fps, frame time, the 1% low, a frame-time plot, and per-run averages with
the difference from the baseline. `tests/test_perf.py` pins the contracts below.

- ⚠ **The run is driven from a FLIGHT LOOP, never the draw callback.** A window that is
  hidden, popped out or covered stops drawing, and a run abandoned halfway leaves the levers
  it moved wherever the last step put them, with nothing on screen to say so.
- ⚠ **Every lever is restored — from the end of a run, from Abort, and from
  `XPluginDisable`** (which calls `PerfShutdown` **before** its own `StopEngine`, because the
  restore may start the waveform engine again). A benchmark that leaves the user's settings
  changed is a bug report about the effect it was measuring.
- ⚠ **Nothing a run moves is saved.** `PerfLeverSet` calls no `Save*`, and the **shipping
  pane therefore has no manual switches at all** — an unsaved checkbox there would look
  exactly like the Displays tab's and forget itself on the next launch. The Dev pane has
  them, and says so.
- **A lever's `on` is always the EXPENSIVE state**, so the sweep's "everything on" baseline
  really is the worst case. `kLeverCockpitLights` (the cockpit's flood-light stacks, added
  2026-08-04) therefore inverts `gCockpit.optimized` — and it is a **dev** lever because the
  shipping A/B run does not move it: it is a Cockpit-tab setting rather than a display
  effect, and the shipping pane's "Current Settings" list reads as *what this test will
  switch off*.
- **A step is a complete lever mask**, not a delta. A sweep toggling one lever per step
  carries every earlier step's state forward, so one mistake reads as a cost in every row
  after it.
- **The settle wait before each measurement is load-bearing.** Queued GL work, X-Plane's own
  frame pacing and a cold texture cache all mean that without it the first configuration in a
  run always looks the worst.
- A frame longer than a second is a scenery load, not a frame: excluded from every figure and
  counted separately, so two runs that disagree have somewhere to show why.

**CPU and GPU load** come from `src/sysmetrics.cpp` — Windows PDH, on **its own thread**.
⚠ Collecting a wildcard PDH query walks every GPU engine instance on the machine and is not
bounded in time, so doing it on the simulator thread would put a spike into the frame times
this tool exists to measure. ⚠ It makes **no XPLM call** (the Build tab's rule, for the same
reason); a failure is recorded as a note the UI reads and logs from the sim thread. ⚠ The
counter paths go through `PdhAddEnglishCounter` — `PdhAddCounter` wants the *localized* name,
which resolves on an English Windows and on nobody else's. Elsewhere the sampler is a stub
and the pane degrades to fps only. `Threads::Threads` is linked unconditionally for this;
it was a `PHOTON_DEV` dependency until the tool shipped.

## Overlay images

An FX layer can carry a PNG/JPG/BMP/TGA that **modulates** its color, so a gradient,
vignette, scanline pattern or painted sheen works with every control the solid layer had.

Images live in `Resources/plugins/ToLissPhoton/overlays/` — the folder beside the `.xpl`,
created on first use with a `README.txt` — and are picked by **name** from a combo on each
layer's row. `deploy.ps1` seeds the repo's `src/native/overlays/` **by name**, never as a
folder mirror, since that folder is also where you drop your own — and it does that copy
**before** its X-Plane-is-running check, so a deploy pushes an edited overlay into a live
sim. The cost of by-name is the other direction: an image you delete from the repo stays in
the install until you delete it there too. Four of them are generated
(`build/make_overlays.py`); the rest are hand-painted and cannot be regenerated.
`Rescan images` in the Panel FX tab re-reads the folder without a restart.

The end-user installer now writes those images too, and the by-name rule is what lets it:
install replaces only the names we ship, uninstall removes only the names we ship, and a
folder still holding anything else survives instead of being `rmtree`d with the plugin.
`installer/constants.py`'s `PLUGIN_USER_DIRS` is the one list; see `docs/installer.md`
§Shared-plugin removal policy.

## `panelfx.txt` — the shipped layer definition

The authored FX look lives at `Resources/plugins/ToLissPhoton/panelfx.txt`, beside the
`.xpl` and next to the images its layers name — **not** in `Output/preferences`. It is a
shipped artifact an upgrade replaces, on the same footing as the overlays; the user's own
choices (the Displays tab's switches and strengths) go to `ToLissPhoton_profiles.json` under
`"$displays"` instead.

⚠ **Only a `PHOTON_DEV` build writes it** (`SavePanelFx`). Two reasons, and both bite
silently: an installer upgrade would clobber whatever a user's session had written, and on a
dev machine the file is a **symlink into the repo**, so anything a user-facing control wrote
would land in source control.

⚠ **The symlink needs `pwsh`.** `deploy.ps1` creates and maintains it so an in-sim tuning
pass commits straight to the repo with no export step — but Windows PowerShell 5.1 never
passes `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`, so under 5.1 it demands elevation
even with Developer Mode on and falls back to a **copy**. After that, FX edits go to a file
`git status` will never mention. `deploy.bat` prefers `pwsh` for this reason alone; the
script says which of the two it did.

Install and uninstall both **skip a symlink** at that path, so running `install.py` against a
dev X-Plane does not quietly end the loop.

`third_party/stb/` is vendored [stb_image](https://github.com/nothings/stb) (v2.30, public
domain). The X-Plane SDK has no image loader — XPLM4 dropped `XPLMLoadTexture` and left only
`XPLMGenerateTextureNumbers` / `XPLMBindTexture2d`, which hand out a texture *number* and
expect decoded pixels. It is compiled unconditionally now that the compositor ships.

- ⚠ **Uploaded PREMULTIPLIED, and an overlay's shape lives in its ALPHA channel** — RGB is
  usually just white, because the layer color does the tinting. Alpha is coverage in every
  blend mode, and a straight-alpha PNG stores bright RGB under alpha 0, which would be added
  or multiplied in at full strength. Premultiplying on upload fixes that once for all modes,
  and is why `Normal`'s source factor is `ONE` and not `SRC_ALPHA`.
- ⚠ **Burn and Darken cannot carry one.** Burn emits `1 − color` and Darken is `GL_MIN`
  whose opacity has to fade the emitted color toward white; `GL_MODULATE` expresses neither
  per pixel, and both would need `ARB_texture_env_combine` or a shader. Such a layer is
  **skipped** and the editor says so on the row.
- **One texture unit for the whole pass**: a solid layer binds a 1×1 white texture instead of
  switching the unit off, which keeps it arithmetically identical to a textured one.
  Toggling per layer would mean `XPLMSetGraphicsState` *inside* the loop, re-entering
  X-Plane's blend tracker under a func we set ourselves.
- **Uploads happen lazily from a draw callback**, because that is where a GL context is
  current. Both call sites — the panel pass and the ImGui window — qualify.

## Dear ImGui

`third_party/imgui/` is vendored upstream ImGui (v1.92.9 — see its README for the version
table and update steps); `src/imgui_xplm.{h,cpp}` is **our** X-Plane backend. The stock
backends assume they own the window, the GL context and the event loop; a plugin owns none
of those.

**Every window in the plugin is ImGui**, shipping ones included. That replaced ~900 lines of
hand-rolled column measuring, hit-testing and press-state tracking that ImGui does for free.

- **The theme is `ApplyPhotonStyle()`**, over `StyleColorsDark`. Every accent is derived
  from **one** color — `kUiBlue`, `#1678b5`, the blue X-Plane uses in its own UI — through
  `UiLighten` / `UiDarken` / `UiFade`. ⚠ **Do not hand-pick a shade.** The dark theme spends
  four or five variants of its accent, and listing them individually is how a palette drifts
  into looking like several palettes; deriving them means re-tuning the whole UI is editing
  `kUiBlue` and nothing else.
  - **The panel fill is `kUiWindowBg`, `#23282e`, opaque.** It is a surface the UI sits on,
    not a tint over the cockpit; text over a bright daytime windscreen is unreadable through
    anything less.
  - ⚠ **`kRootTopMargin` (20 px) MOVES the root window down and shortens it — it is not
    padding.** The strip it vacates is X-Plane's own window showing through, which is what
    keeps the **"un-pop out" button** in a popped-out window's top right corner from being
    painted over. Widening `WindowPadding` instead looks identical inside the panel and
    leaves that button buried. ⚠ `io.DisplaySize` is deliberately **not** shrunk to match:
    it is the mouse and clip space for the whole client area, and trimming it would offset
    every hit test by 20 px while everything still looked right.
- **`src/photon_imconfig.h`** holds our settings (via `IMGUI_USER_CONFIG`) so
  `third_party/imgui/` stays byte-identical to upstream. Chief among them: `IM_ASSERT` logs
  and continues instead of calling `abort()`, which inside X-Plane would take the simulator
  down with no message.
- ⚠ **Iterate `draw_data->CmdLists`, never `draw_data->CmdListsCount`.** That counter was
  obsoleted in **1.92.9** (four days before the version vendored here) and is no longer
  maintained: ImGui's own path pushes lists through `AddDrawListToDrawDataEx`, and only the
  public `AddDrawList()` helper — which a backend does not call — updates the count. It
  reads **0 on every frame ImGui renders** while `CmdLists.Size` is 2 and `TotalVtxCount` is
  in the hundreds. A loop written against it compiles, matches every pre-1.92 example, and
  **silently draws nothing**. It blanked the first working version of this backend. If a
  version bump makes the UI vanish again, look here first.
- **The UI font is Roboto, loaded from X-PLANE'S OWN `Resources/fonts/`** (2026-08-02,
  `LoadUiFont`). ⚠ That is why this is three lines and not a vendored binary: the sim has
  shipped `Roboto-Regular.ttf` and its licence since XP11, so there is **no new payload
  file, no installer row, no licence to redistribute** and nothing an upgrade can leave
  stale. ImGui's built-in face is ProggyClean at 13 px — a bitmap frozen at one size.
  - ⚠ **Failure is silent and that is deliberate.** A missing file means
    `AddFontFromFileTTF` returns null, ImGui falls back to its own font, and the UI comes up
    looking slightly worse rather than not at all. A typeface is not worth a window that
    does not open.
  - ⚠ **Added in the constructor, before anything asks the atlas for pixels.** Adding a font
    after the first `GetTexDataAsRGBA32` rebuilds the atlas behind an already-uploaded
    texture, and every glyph then samples the wrong place in an image nothing re-uploads.
  - ⚠ **`kUiScale` goes into the rasterised SIZE, not into `FontGlobalScale`, when a TTF
    loads** — applying both doubles it. The built-in font still takes `FontGlobalScale`,
    because a bitmap face has no size to be rasterised at.
- **Defining `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` would make that a compile error**, and is
  worth doing — but it also removes `GetTexDataAsRGBA32`/`SetTexID`, so it waits until the
  font path moves to 1.92's texture protocol (`ImGuiBackendFlags_RendererHasTextures` +
  servicing `ImTextureData`). That also buys smooth font scaling. **Meanwhile, nothing we
  write may call into that block** — `GetContentRegionMax()` is the one that tempts you;
  use `GetCursorPosX() + GetContentRegionAvail().x - w` to right-align instead.
- ⚠ **A row button next to a `SpanAvailWidth` tree node needs
  `ImGuiTreeNodeFlags_AllowOverlap`** — on the *node*, not on the button. `SpanAvailWidth`
  extends the node's hit box to the right edge of the pane, i.e. over anything `SameLine()`
  puts there, and `ItemHoverable` refuses hover to any later item once `g.HoveredId` is
  taken (`if (g.HoveredId != 0 && g.HoveredId != id && !g.HoveredIdAllowOverlap) return
  false`). The overlapped buttons still **draw, at full opacity, and are completely dead**:
  no hover, no tooltip, no click, no assertion, nothing in `Log.txt`. It reads in-sim as
  "that button does nothing" and sends you looking at what the button *calls*. This cost the
  Panel FX target tree its `?` and `x` buttons for a release; ImGui's own comment on the
  flag says exactly this, one line above the flag you actually want.
- ⚠ **The boxel → pixel transform comes from DATAREFS, not from `glGet`.**
  `sim/graphics/view/{modelview_matrix,projection_matrix,viewport}` are what X-Plane
  publishes for exactly this; under Vulkan the GL our drawing rides is a bridge and the
  fixed-function matrix state `glGetFloatv` reports is not the transform in force during a
  window draw callback. Only `glScissor` needs it — vertices go out in boxels and ride
  X-Plane's own projection — which is what makes it hard to spot: **the geometry is correct,
  every clip rect is nonsense, and a nonsense scissor box discards the whole UI with no GL
  error.** Note the viewport dataref is `left, bottom, RIGHT, TOP` (corners) where GL's is
  `x, y, width, height`. Two further nets, both because the failure mode is invisibility:
  the transform is validated against the window's own corners each frame and clipping is
  **skipped** rather than trusted if it fails (an overflowing child pane beats a blank
  window), and scissor rects are normalized with min/max instead of assuming an ordering.
- **A failed font-atlas upload also blanks the window**, not just the text: every ImGui
  vertex is textured — solid rectangles sample the atlas's white pixel — so an upload
  failure makes the whole UI transparent. `BuildFontTexture` checks `glGetError` and says so
  in `Log.txt`.
- **Smoke test:** Dev window ▸ ImGui tab. If the stock demo draws, scrolls, responds to the
  mouse and accepts typing, the backend is correct. Every ImGui window also logs a one-shot
  first-frame diagnostic — geometry, display size, font texture, draw-command and vertex
  counts, the viewport and where it came from, whether clipping was usable. Read that line
  before forming a theory about a window that looks wrong.
- **A blank window paints two GL sanity markers** in the bottom-left (dev builds,
  `PhotonImgui::SetDiagnostics`). They draw only when there is no UI geometry, so a working
  window never shows them, and they share as little as possible with the UI path: an
  untextured magenta square (did our immediate-mode GL reach the screen at all?) and a
  square showing the font atlas (did the upload and texturing work?). **Neither / magenta
  only / both** are three disjoint diagnoses — one sim start instead of a restart per
  hypothesis. That is how the `CmdListsCount` bug was cornered.
- **ImGui bugs are reproducible offline.** The frame is pure computation: a console `main()`
  linking the four ImGui `.cpp` files with the same `IMGUI_USER_CONFIG` and making the same
  `NewFrame`/`Begin`/`End`/`Render` calls reproduces anything that is not GL or X-Plane,
  with assertions printing to stdout instead of being swallowed. That found `CmdListsCount`
  in about a minute, after two sim restarts had found nothing.

## Implementation notes

- **SDK feature level** is `XPLM300/301` (X-Plane 11.10+); bump to `XPLM400=1` for XP12-only
  APIs if needed.
- **Per-key closures → refcon.** The Python accessors used a lambda per category; the native
  accessors carry the category index in the read-refcon instead.
- **int *and* float readers** are both registered per dataref — the OBJ reads them as float.
- **`opengl32` is linked unconditionally** (the ImGui backend renders with GL and is always
  compiled). XPWidgets is not linked at all.
- **Cross-compilation isn't possible** — one binary per OS. The all-platform build is wired
  into the CI matrix in `.github/workflows/release.yml`: the `native` job fetches the SDK
  (via `XPSDK_URL`), builds each OS's `.xpl`, and uploads it; `bundle`/`exe` merge all three
  arches into one `ToLissPhoton/` folder passed to `make_release.py --plugin-dir`. macOS is
  built universal (arm64+x86_64); codesign/notarize is still **not** done. The workflow has
  not yet been exercised by a real `v*` tag push.
