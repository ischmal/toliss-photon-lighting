# ToLiss Photon — native `.xpl` plugin

The **native C++ plugin** — the shipping runtime for ToLiss Photon Lighting. A compiled
`.xpl` that needs **no XPPython3 and no Python** at runtime (CLAUDE.md TODO #4, now done).
It **replaces** the former Python plugin `PI_ToLissPhoton.py` (retired and removed from the
repo): on startup it deletes any leftover copy of that script — and of the even older
FlyWithLua scripts — so a stale copy can't load under XPPython3 and fight this plugin for
the same per-frame beacon/strobe writes (`RemoveSupersededFiles` in `src/plugin.cpp`).

**Status: full port, compiles clean, confirmed working in-sim.** `src/plugin.cpp` is the
complete implementation: the 9 category datarefs (+ `is_led` + the two `debug/*_always_on`
flags), the waveform engine (after-flight-model per-frame loop), the Plugins menu with radio
profiles, the Custom window (modern `XPLMCreateWindowEx` UI — not the legacy XPWidgets
library, so it scales/pops out/works in VR), per-livery persistence (small self-contained
JSON — no deps), Auto resolution (1 Hz loop), ToLiss detection, and the superseded-file
sweep. Builds clean with MSVC (VS 18), no warnings, and the built `.xpl` exports all five
`XPlugin*` entry points.

The port's original open items are now closed (CLAUDE.md "Open items / TODO"): the strobe
index mapping and Auto gates were confirmed in-sim, and the end-user installer / release
tooling have been repointed at this `.xpl` (they stage the fat-plugin folder) with the
multi-OS build wired into `.github/workflows/release.yml` — see the CI note under
"Implementation notes" below.

## Get the SDK (one-time)

Download the X-Plane SDK from <https://developer.x-plane.com/sdk/> and unzip it so that
`src/native/SDK/CHeaders/XPLM/XPLMDefs.h` exists. `SDK/` is gitignored (it's a
third-party download). Alternatively pass `-DXPLANE_SDK=/path/to/SDK` when configuring.

```
# from src/native/, with curl (matches what CI will do):
curl -sL -o sdk.zip https://developer.x-plane.com/wp-content/plugins/code-sample-generation/sdk_zip_files/XPSDK411.zip
unzip -q sdk.zip && rm sdk.zip     # extracts ./SDK/
```

## Build

Needs CMake + a C++17 compiler. On this machine both ship inside **VS 18 Community**
(VS 2022 lacks the C++ workload), so they aren't on `PATH` — use full paths or a
Developer prompt.

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build -A x64
& $cmake --build build --config Release
```

macOS / Linux: `cmake -B build && cmake --build build` (a Developer prompt / native
toolchain). Output lands in the fat-plugin layout X-Plane expects:

```
build/ToLissPhoton/<arch>/ToLissPhoton.xpl      # arch = win_x64 | mac_x64 | lin_x64
```

## Install (dev)

**Quickest (Windows):** run `deploy.ps1` (or double-click `deploy.bat`) — it builds
Release and copies the `.xpl` into the X-Plane install, refusing if the sim is running
(a loaded `.xpl` is locked). Options: `-NoBuild` (just copy), `-XPlaneRoot <path>`
(default is the Steam install), `-Config <cfg>`.

```powershell
powershell -ExecutionPolicy Bypass -File deploy.ps1
```

**Manual:** copy the whole `build/ToLissPhoton/` folder into
`<X-Plane>/Resources/plugins/`. A fat plugin can carry all three
`<arch>/ToLissPhoton.xpl` files side by side; X-Plane loads the one matching the host
OS. Unlike the Python plugin there is **no** XPPython3 prerequisite and no `__pycache__`
to clear. The plugin also deletes any leftover retired `PI_ToLissPhoton.py` /
`ToLissPhoton.lua` on startup so the old versions can't run alongside it.

## Dear ImGui

`third_party/imgui/` is vendored upstream ImGui (v1.92.9, committed — see its README for
the version table and update steps), and `src/imgui_xplm.{h,cpp}` is **our** X-Plane
backend. The stock backends assume they own the window, the GL context and the event
loop; a plugin owns none of those.

- **Compiled into every build**, not gated behind `PHOTON_PANEL_PROBE`: it is the UI
  framework for tooling and for whatever windows get converted later, so its availability
  shouldn't depend on which experiment is switched on. Release `/OPT:REF` drops what ends
  up unreferenced — the shipping `.xpl` is 192 KB with ImGui compiled in.
- **`opengl32` is therefore linked unconditionally**, where it used to be probe-only.
- **`src/photon_imconfig.h`** holds our ImGui settings (reached via `IMGUI_USER_CONFIG`)
  so `third_party/imgui/` stays byte-identical to upstream. Chief among them: `IM_ASSERT`
  logs and continues instead of calling `abort()`, which inside X-Plane would take the
  simulator down with no message.
- **Nothing user-facing uses it yet.** The Custom, About and Panel Probe windows still
  draw with `XPLMDrawTranslucentDarkBox` + `XPLMDrawString`; converting them is a separate
  change. The only ImGui windows are under Debug, and that submenu only exists in a probe
  build.
- ⚠ **Iterate `draw_data->CmdLists`, never `draw_data->CmdListsCount`.** That counter was
  obsoleted in **1.92.9** (2026-07-20, four days before the version vendored here) and is no
  longer maintained by the renderer: ImGui's own path pushes lists through
  `AddDrawListToDrawDataEx`, and only the public `AddDrawList()` helper — which a backend
  does not call — updates the count. It therefore reads **0 on every frame ImGui renders**,
  while `CmdLists.Size` is 2 and `TotalVtxCount` is in the hundreds. A loop written against
  it compiles, matches every pre-1.92 example and most of the internet, and **silently draws
  nothing**. It blanked the first working version of this backend. If a version bump makes
  the UI vanish again, look here first.
  Defining `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` would turn this into a compile error, which is
  worth doing — but it also removes `GetTexDataAsRGBA32`/`SetTexID`, so it has to wait until
  the font path moves to 1.92's texture protocol (`ImGuiBackendFlags_RendererHasTextures` +
  servicing `ImTextureData` requests). Worth doing then; it also buys smooth font scaling.
- ⚠ **The boxel → pixel transform comes from DATAREFS, not from `glGet`.**
  `sim/graphics/view/{modelview_matrix,projection_matrix,viewport}` are what X-Plane
  publishes for exactly this; under Vulkan the GL our drawing rides is a bridge and the
  fixed-function matrix state `glGetFloatv` reports is not the transform in force during a
  window draw callback. Only `glScissor` needs it — vertices go out in boxels and ride
  X-Plane's own projection — which is precisely what makes it hard to spot: **the geometry
  is correct, every clip rect is nonsense, and a nonsense scissor box discards the whole UI
  with no GL error.** That blanked the first cut of this backend. Note the viewport dataref
  is `left, bottom, RIGHT, TOP` (corners) where GL's is `x, y, width, height`.
  Two further nets, both because the failure mode is invisibility: the transform is
  validated against the window's own corners each frame and clipping is **skipped** rather
  than trusted if it fails (an overflowing child pane is a far better failure than a blank
  window), and scissor rects are normalised with min/max instead of assuming an ordering.
- **A failed font-atlas upload also blanks the window**, not just the text: every ImGui
  vertex is textured — solid rectangles sample the atlas's white pixel — so an upload
  failure makes the whole UI transparent. `BuildFontTexture` checks `glGetError` and says
  so in `Log.txt`.
- **Smoke test:** Plugins ▸ ToLiss Photon ▸ Debug ▸ *Dear ImGui demo*. If the stock demo
  draws, scrolls, responds to the mouse and accepts typing, the backend is correct.
  Either ImGui window also logs a one-shot diagnostic on its first frame — window
  geometry, display size, font texture and atlas size, draw-command and vertex counts,
  the viewport and where it came from, and whether clipping was usable. Read that line
  before forming a theory about a window that looks wrong.
- **A blank window paints two GL sanity markers** in the bottom-left of the client area
  (probe builds, `PhotonImgui::SetDiagnostics`). They draw only when there is no UI
  geometry, so a working window never shows them, and they share as little as possible
  with the UI path: an untextured magenta square (did our immediate-mode GL reach the
  screen at all?) and a square showing the font atlas (did the upload and texturing
  work?). **Neither / magenta only / both** are three disjoint diagnoses, which is the
  difference between one sim start and a restart per hypothesis — that is how the
  `CmdListsCount` bug above was cornered.
- **ImGui bugs are reproducible offline.** The frame is pure computation: a console
  `main()` that links the four ImGui `.cpp` files with the same `IMGUI_USER_CONFIG` and
  makes the same `NewFrame`/`Begin`/`End`/`Render` calls reproduces anything that is not
  GL or X-Plane, with assertions printing to stdout instead of being swallowed. That is
  what found `CmdListsCount`, in about a minute, after two sim restarts had found nothing.

## Implementation notes

- **SDK feature level** is `XPLM300/301` (X-Plane 11.10+) in `CMakeLists.txt`; bump to
  `XPLM400=1` for XP12-only APIs if needed.
- **Per-key closures → refcon.** The Python accessors used a lambda per category; the
  native accessors carry the category index in the read-refcon instead.
- **int *and* float readers** are both registered per dataref — the OBJ reads them as
  float (gotcha #4); keep both.
- **Cross-compilation isn't possible** — one binary per OS. The all-platform build is wired
  into the CI matrix (Win/Mac/Linux) in `.github/workflows/release.yml`: the `native` job
  fetches the SDK (via `XPSDK_URL`), builds each OS's `.xpl` with CMake, and uploads it;
  `bundle`/`exe` merge all three arches (`download-artifact merge-multiple`) into one
  `ToLissPhoton/` folder passed to `make_release.py --plugin-dir`. macOS is built universal
  (arm64+x86_64) via `CMAKE_OSX_ARCHITECTURES`; codesign/notarize is still **not** done
  (unsigned `.xpl` loads fine, but a downloaded one may be Gatekeeper-quarantined until
  cleared). The workflow has not yet been exercised by a real `v*` tag push.
