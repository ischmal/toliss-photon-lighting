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
