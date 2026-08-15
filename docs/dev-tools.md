# Dev tooling

Detail reference for the in-sim iteration loop. `CLAUDE.md` keeps only the commands and the
one-line tripwires; the reasoning — most of it paid for with hard sim crashes — is here.

**Getting a change into the sim is one command**, whatever you changed:

```powershell
src\native\deploy.ps1        # overlays + OBJs + plugin; add -Dev, -Test
```

Close X-Plane, run it, start X-Plane. It rebuilds the overlay images, the aircraft OBJs
(`--target all --write`) and the `.xpl`, and stops on the first failure. There is no
follow-up `build_objs.py` step — if something tells you otherwise it predates 2026-08-03.
`src/native/README.md` has the options. The two loops below are for *iterating* once a build
is in: `watch.py` for rebuild-on-save, the Dev window for knobs that need no rebuild at all.

**There are two dev runtimes and they do different jobs.** Know which one you want:

| | The **Dev window** (native `.xpl`, `-Dev`) | **`PI_PhotonDevReload.py`** (XPPython3) |
|---|---|---|
| What it drives | plugin state: profiles, categories, the panel probe, the FX compositor | per-light OBJ state: position, aim, cone, size, color of a `--debug` build |
| Needs | nothing but the plugin | XPPython3 **and** a `--debug` OBJ build |
| Ships | never (`#if PHOTON_DEV`) | never (`src/plugin/`, not packaged) |

The light tuner is still the Python plugin because it drives 64 debug datarefs that only a
`--debug` OBJ binds. Everything *else* has moved into the Dev window.

---

## The Dev window — `deploy.ps1 -Dev`

One window, tabbed, reached from **Plugins ▸ ToLiss Photon ▸ Dev ▸ Dev window…**. It
replaced a menu item and a floating window per experiment, which meant four windows to
arrange and no way to see the probe and the effect it was probing at once.

| Tab | What it is |
|---|---|
| **Exterior** / **Cockpit** / **Displays** | the shipping settings panes, same builders |
| **Perf** | the performance tool, with the internal levers and the full sweep |
| **Panel FX** | the layer compositor: target tree, layer stack, copy/paste |
| **Lights** | the live light editor — color, aim, spread, size, position |
| **History** | FX undo/redo and the append-only journal |
| **Probe** | the panel-FBO draw-order probe |
| **Build** | runs the repo's Python tooling, output captured in the pane |
| **Log** | this plugin's own log ring, newest last |
| **ImGui** | the stock demo — the backend smoke test |

### The Build tab

Set the **repo path** once; it persists in `Output/preferences/ToLissPhoton_dev.txt` and is
seeded from `$PHOTON_REPO`. The buttons are the build invocations this project actually
uses, which is the point — the largest single piece of overhead here was alt-tabbing out and
remembering which of six `build_objs.py` forms was the right one.

- ⚠ **Commands run detached on a worker thread.** A synchronous `std::system()` inside a
  draw callback freezes the simulator for as long as the generator takes. The thread touches
  only its mutex-guarded output buffer and **never an XPLM call** — XPLM is not thread-safe,
  and a stray call from there is a crash with no useful stack.
- ⚠ **There is no reload-aircraft button, and there must not be one.**
  `sim/operation/reload_aircraft` runs synchronously and unloads the aircraft's plugins, so
  issuing it from a draw callback tears the aircraft down mid-frame — the same rule that has
  always banned it from flight loops. It is a **menu item**, i.e. handler context. A
  `--write` build already triggers a reload through `watch.py`'s channel anyway.

### The Perf tab (2026-08-03)

The shipping Performance pane with `dev` true — same builder, so the two cannot drift. What
the flag adds is the **internal levers** (the FX compositor as a whole, the waveform engine),
the **full sweep** (baseline, then each lever off in turn, then everything off), the settle
and measure durations, and **manual** lever checkboxes.

- ⚠ **The manual checkboxes are not saved, and nothing a run moves is saved either.** A
  measurement is not a preference. The shipping pane therefore has no switches at all — an
  unsaved one there would look exactly like the Displays tab's.
- ⚠ **A run is driven from a flight loop and restores every lever it moved** — from the end
  of the run, from Abort, and from `XPluginDisable`. Nothing here should be reached for by
  the draw callback: a hidden or popped-out window stops drawing, and the run would stall
  with the levers still moved.
- **The compositor lever minus the two effect switches is the cost of the pixel gate.** The
  gate's `glReadPixels` happens inside the pass, so switching the effects off leaves it
  running and switching the compositor off does not.
- **CPU and GPU** come from `src/native/src/sysmetrics.cpp` — Windows PDH on its own thread,
  no XPLM calls, English counter paths. Elsewhere the pane degrades to frame rate only.

### The Lights tab (2026-08-01)

The Live Light Debug window, rebuilt inside the native Dev window. Same datarefs, same
manifest, same wire order as the XPPython3 tool documented under
[§ Live light debug](#live-light-debug) below — that section is still the reference for
*why* each piece exists; this one is what is different.

The tab has **two modes, and which one you get is decided by the OBJ that is installed**,
not by a setting:

| Installed OBJ | Manifest beside it | The tab |
|---|---|---|
| `--debug` build | `lights_inn.debug.json` / `lights_screens.debug.json` | **tunable** — every knob drives `ToLissPhoton/debug/light/*` live |
| plain build | `lights_inn.info.json` / `lights_screens.info.json` | **read-only** — the same tree and the same numbers, disabled |

A build writes one manifest and **deletes the other**, so exactly one exists at a time and
there is nothing to infer: which file is present *is* the answer to "can this be driven".
The fallback is decided **per target**, so an interior built `--debug` sits happily beside
stock screens — its knobs work, the screens' are shown disabled, and neither is quietly
the other.

Build the OBJ in its tunable shape to *edit*:

```bash
python build/build_objs.py build --target interior --debug --write
```

...but nothing has to be rebuilt to *look*. The read-only mode (2026-08-10) exists because
a `--debug` OBJ is a poor way to answer "what is this light set to": it is not installable,
it drops the category gates, and getting one costs a redeploy and an aircraft reload. A
plain `deploy.ps1 -Dev` now drops the metadata beside the normal OBJ, so the Lights tab
answers that question at any time.

- ⚠ **A read-only light shows the profile branch that is DRAWING.** A shipping OBJ bakes
  each lamp once per look, behind an `ANIM_hide` on its category dataref, so "what colour
  is the map spot" has three answers and only one of them is on screen. The manifest
  carries all of them and **the position in that list is the dataref value** — the same
  contract `branch_gate` emits the gates from. The toolbar's **branch** picker reads
  another era without touching the aircraft's setting; the editor names which one you are
  looking at and whether it is live.
- **What disappears in read-only mode**: Save tuning, Isolate, Mark, Blink, the A/B
  Tunable switch, the markers, the slot probe, the aim presets, both Reverts and the
  tuning-file row. Every one of them drives a dataref a shipping OBJ does not bind, so
  hiding beats disabling — a greyed-out row invites "what would these do", and the answer
  is "nothing, ever, in this build". **Log DSL stays**, since printing the authored values
  is exactly what an inspector is for.
- ⚠ **A read-only session never touches the tuning file** — not saved from, not restored
  into. Those records are edits made against a *debug* OBJ; applying them over an
  installable one would put numbers on screen that nothing in the cockpit is rendering.

- **The hierarchy is the manifest's own: category ▸ fixture ▸ light.** That is not
  cosmetic. A category is what the Cockpit menu switches and a fixture is what was authored
  as one unit in `lights.layout.phdsl`, so "which of these do I edit together" has the same
  answer here as in the DSL. A flat list of thirty `int_*#side` names does not. A fixture
  holding a single light is flattened into its category — a node wrapping one child is the
  same pointlessness a single-member FX group would be.
- **Aim is edited as pitch/yaw and spread in degrees**, and slots 5..7 are derived from
  them on every edit. One direction of flow, always: it is what "Log DSL" prints, and it is
  what makes the tuner structurally incapable of emitting a non-unit `dir` — the bug class
  that made the interior's cones inert.
- **Markers** (green origin, amber aim, blue cone rim) follow the selection.
- **Log DSL** prints the selected light in `.phdsl` shape, split between the style file
  (the palette's `rgb`) and the layout file (`at:`, `aim:`, `spread:`, `size:`). Nothing
  here writes the DSL for you — a tuned value that never gets transcribed is lost on the
  next build.

⚠ **Only one tool may own `ToLissPhoton/debug/light/*`, and BOTH now stand down for the
other.** The Python plugin's accessors are read-only *computed* values, so neither side can
write through the other's — sharing is impossible, not a policy choice. Whichever registers
first wins; the loser says so where its knobs are (a banner across the Python window, a
banner in the native tab) rather than presenting controls that reach nothing. Plugin start
order is not defined by the SDK, so both log which way it went. To pick a side permanently,
delete `PI_PhotonDevReload.py` or deploy a non-dev `.xpl`.

`tests/test_light_editor.py` pins every constant the two implementations share — slot
count, keyframe span, marker layout, dataref names and above all the **parameter order**,
which is not the OBJ line's order — and `tests/test_dev_light_debug.py` pins the stand-down
on the Python side.

**Parity, as of 2026-08-01.** The native tab has: the tree, color, brightness, size,
pitch/yaw with the sign convention spelled out, spread, position offsets, aim presets,
Isolate/Mark/Blink, markers with size and reach, the A/B compare, the **slot probe**,
Revert / Revert all, Log DSL and manifest reload. Two things are deliberately NOT ported:

- **The 48-permutation orientation cycler.** It was the search tool for a hypothesis that
  turned out to be wrong — the fault was a four-slot rotation at the wire, and *a rotation
  of four slots is not an axis permutation*, so that search could never have found it. The
  aim presets answer the same question in three clicks. If the axis mapping is ever in
  doubt again, the probe and the presets are the tools; port the cycler only if both fail.
- **Announcing the datarefs to DataRefTool / DataRefEditor.** Worth adding if a browser is
  in use; nothing in the editor depends on it.

### The History tab

Two different recoveries, because two different things go wrong:

- **Undo/redo** is for the edit you just made. In-memory and instant, and it is what makes a
  wide slider safe to drag.
- **The journal** is for the session you already closed. Every commit appends a timestamped
  snapshot to `ToLissPhoton_panelfx_history.txt`, so a look that was good an hour ago is
  still on disk after two sim restarts. The save file cannot do this — it holds exactly one
  state, and the edit that destroyed a good look overwrote it immediately.

It is **append-only on purpose**. Trimming to "recent" entries would discard precisely the
old snapshot you want when you realize three days later that it was better before.

A continuous edit (dragging a slider) commits **on release**, not per frame — otherwise the
journal fills with hundreds of near-identical records. The value itself is live and on disk
either way; only the history entry waits.

---

## `build/watch.py` — rebuild + auto-reload on save

`python build/watch.py --write` rebuilds the OBJs on every save, installs them into the live
X-Plane aircraft folders, **and** auto-reloads the aircraft in the running sim by firing
`ToLissPhoton/dev/quick_reload` over X-Plane's UDP command port (49000, `CMND` packet —
`send_reload`). `--no-reload` skips it; `--reload-port` overrides the port.

**UDP is the deliberate trigger path**: X-Plane dispatches a `CMND` packet in
command-handler context, exactly like a key press. The dev plugin must **not** watch files
and trigger from a flight loop — that is the reload-from-flight-loop hard crash below.
Fire-and-forget: if the sim is closed or the plugin is busy the packet is silently dropped
(save again once the reload settles).

### Crash lesson (2026-07-20): auto-reload can race its own file writes

`sim/operation/reload_aircraft` is **synchronous** — it reads the installed `lights_out` OBJ
(and, for `--wing realwings`, the RealWings mod's `Main.obj`, rewritten every cycle since the
refresh-not-skip change) directly off disk while it runs. A save landing soon after the
previous one let the next rebuild start **rewriting that same file** while X-Plane was still
mid-read of it. Reported symptom: auto-reload worked N times then hard-crashed the sim, N
varying with save cadence (2, then later 5) — purely a function of timing overlap, not a
deterministic bug. A stress test (one thread hammering writes, four reading concurrently,
~49k reads) confirmed a plain write can hand a reader a torn/partial file; X-Plane's native
OBJ8 parser is not expected to survive that.

**Two independent, complementary fixes — belt and suspenders, deliberately not just one:**

1. **`build_objs.atomic_write_bytes`** (temp file + `os.replace`, with a short retry on the
   transient Windows sharing-violation a concurrent reader can cause). Used by every
   live-file write: `_write_live`, `cmd_build`'s `dist/` write, and `patch_realwings.py`'s
   live OBJ write and `reverse_bak`. This is the layer that eliminates *garbled* content — a
   concurrent reader always gets the complete old file or the complete new one (zero torn
   reads in the same stress test once atomic). It also simplified `_write_live`'s
   stale-symlink handling: `os.replace` overwrites a symlink's own directory entry rather
   than following it (verified on this project's Windows setup), so the old
   unlink-then-copy two-step — with its own gap where the path briefly didn't exist —
   collapsed to one atomic call.
2. **`watch.py`'s `RELOAD_GUARD_SECONDS`** (default 3s, `--reload-guard-seconds` to
   override). After firing a reload, `should_defer_rebuild` keeps the loop from even
   *attempting* the next rebuild until the guard elapses. A save inside that window is
   deferred, not lost: `last` is left stale so it retries next poll, coalescing rapid edits
   into one rebuild once the guard clears. This is a conservative fixed guess, **not** a real
   busy signal from the sim (nothing reports when `reload_aircraft` has finished reading) —
   raise it if trouble persists on a slower machine.

**Do NOT remove either layer thinking the other makes it redundant.** Atomicity guarantees
content safety regardless of timing but doesn't reduce how often a write races a read; the
guard reduces how often it's attempted but is a heuristic, not a proof.

`should_defer_rebuild` was extracted from the poll loop as a pure function specifically to be
unit-testable — see `tests/test_watch.py`, which also locks the `CMND` packet format.

(The old `tools/Link-Objects.ps1` symlink workflow is **removed** — `--write` replaced it,
and clears any leftover symlink it finds.)

---

## `src/plugin/PI_PhotonDevReload.py` — one-key light-position iteration

A **separate, dev-only** XPPython3 plugin that collapses the tedious "reload to see a moved
light" dance into one bindable command. It is **never shipped**: `make_release.py` /
`core/payload.cpp` stage the native `.xpl`, so a `PI_*.py` in `src/plugin/` is ignored by
the release. Install it as a **symlink** into the live `Resources/plugins/PythonPlugins/`
(created by hand — `New-Item -ItemType SymbolicLink`) so repo edits load with an
aircraft/plugin reload, no copy step. Its command, dataref, and signature namespaces are
disjoint from the runtime plugin's, so the two never collide.

All tunables (including the snapshot dataref list) are a block at the top of the file;
everything is try/except-guarded and falls back to idle on error.

Plugins ▸ **Photon Dev** is a **flat** menu (Quick Reload / Plain Reload / Release Camera ─
Cinematic Camera… / Cockpit Light Tuner… / Live Light Debug… ─ Log Plugin List). It must
stay flat: the level-2 submenu crash below binds this plugin, not the native `.xpl`.

### The state machine

Triggered by the command `ToLissPhoton/dev/quick_reload` (bind to a key/joystick button;
also from the menu; also fired remotely by `watch.py --write`). `Trigger` ignores the
command while a sequence is already running.

1. **Snapshot** the external camera pose (`readCameraPosition`) + a configurable dataref
   list. `view_type` is logged here for diagnostics.
2. **Disable** the aircraft's own plugins, scoped by **file path** — every plugin whose
   binary lives under the current aircraft's folder (ToLiss's AirbusFBW + SASL), minus the
   `SkipPluginNameMatches` exclusions.
3. `sim/operation/reload_aircraft`, issued **straight from the command handler**.
4. On `MSG_PLANE_LOADED`, **settle**: re-enable the plugins, then **wait out ToLiss's
   cold-start init instead of racing it**. ToLiss resets the switches and pilot view via an
   init that runs *after* `MSG_PLANE_LOADED`, so the settle loop watches the snapshot
   datarefs, waits until it has SEEN ToLiss change them and then go quiet (stable
   `QuietSeconds` ⇒ init finished), and only THEN writes the snapshot back (once, plus a
   short `RestoreGuardSeconds` re-assert). Falls back to `ActivityTimeoutSeconds` if ToLiss
   never visibly touches them; `MaxSettleSeconds` hard-caps the wait.
5. **Restore the camera**: switch to the free-camera view, then a FRAME LATER (state
   `ST_CAMERA`) `controlCamera(Forever)` and hold the saved pose.

**If the log shows `max-cap` instead of `quiesced`, raise `MaxSettleSeconds`.** See the
camera section for why that matters far more than it looks.

### Plain Reload — `ToLissPhoton/dev/plain_reload`

The same reload with **none** of steps 1, 4 and 5: no dataref snapshot, no settle wait, no
camera. You land in a cold cockpit looking wherever the reload put you, in a couple of
seconds rather than 10–45.

It still does step 2/3 — **disabling and re-enabling the aircraft's plugins is not part of
the convenience layer, it is the crash avoidance** (see the crash lessons below), so it is
kept. The one deferred piece is the re-enable, which waits a single 1 s tick for ToLiss's
plugins to finish (re)loading; that is the whole of state `ST_PLAIN`.

### The camera hold

X-Plane exposes **no writable dataref for the external camera position** (`view_x/y/z` are
read-only per `DataRefs.txt`), so `controlCamera` is the only way to place it.

**The hold is permanent-until-input on purpose**, because ToLiss steals the camera a few
seconds *after* load.

**Crash-lesson correction (2026-07-20):** re-writing the pose every `CameraFunc` frame only
wins while we still OWN the camera. ToLiss's late steal takes camera *ownership* (its own
`controlCamera`, or a view-mode change), which fires `inIsLosingControl` and **stops
`CameraFunc` being called at all** — so the per-frame overwrite dies and ToLiss "ultimately
reverts" the view. The old code *tore the hold down* on `inIsLosingControl`, which was the
reported "even with the added time, it still reverts" bug.

**Fix: `CameraWatchLoop`**, a dedicated watchdog flight loop (created in `XPluginEnable`,
polling at `CameraRegrabInterval` ≈ 0.1 s). `CameraFunc`'s losing-control branch no longer
tears down — it just drops `camControlling` and returns 0; the watchdog then **re-grabs**:
`_enter_free_camera()` this tick, then `_grab_camera()` (`controlCamera(Forever)`) the *next*
frame (the same frame-gap that dodges the same-frame view-change race).

`camActive` (we WANT the hold) vs `camControlling` (we currently OWN the camera) is the
distinction that lets the watchdog tell "user released" (`camActive`→False, stop) from
"ToLiss stole it" (`camControlling`→False, re-grab).

**Release on input.** A **key sniffer** (`KeySniffer`, any key) plus a full-screen
click-through window (`_win_click`/`_win_rclick`/`_win_wheel`) catching clicks, right-clicks
and the scroll wheel. Every handler PASSES THE INPUT THROUGH (returns 1 / 0) so the same
right-click-drag that ends the hold also begins the sim's free-camera look. Passive cursor
*motion* is deliberately NOT a trigger — only real clicks/keys/wheel mean "I'm moving the
camera."

`MaxCameraHoldSeconds` is **0 (= hold indefinitely)** by default; a positive value is an
optional safety cap, enforced in *both* `CameraFunc` and `CameraWatchLoop` (so it still fires
during a steal/re-grab window when `CameraFunc` isn't running). "Release Camera"
(bindable/menu) is the always-available manual escape. Every exit goes through
`_end_camera_hold`, which clears `camActive`, deactivates the watchdog, tears down the
watchers, and — except when called from inside `CameraFunc`, where returning 0 already
relinquishes — calls `dontControlCamera`.

Watchers + watchdog are torn down on every exit path, via `_release_camera` on plugin
disable/stop, and **at the start of every new `Trigger`** (after the camera snapshot): a
persistent hold from the previous cycle must NOT stay active through the next reload's ToLiss
init, or it re-creates the forced-external-during-init exterior culling. The settle
re-establishes it at `ST_CAMERA`.

**Known limitation:** if ToLiss ever stole the camera *continuously* (every frame) the
watchdog would war with it (visible flicker). In practice the steal is a one-shot post-load
reset, so a single re-grab wins. If a war ever shows up, that's the signal to gate re-grabs
by `view_type`.

**Why a permanent hold is correct here when it was a dead end before.** The object culling
that got the old hold rejected was NOT caused by the hold itself — it was the settle
**capping out before ToLiss finished** (`max-cap` in the log) and restoring mid-init, so
ToLiss reset the view mode *while the camera was forced external*, culling the exterior (and
clobbering the power dataref — the same-session external-power regression). The real fix was
raising `MaxSettleSeconds` so the settle reaches true `quiesced`; the hold must only begin
after ToLiss is done.

**Dead ends kept as warnings:**
- `ControlCameraUntilViewChanges` released the moment the *view switch itself* happened
  (same-frame race — hence the frame gap before `ST_CAMERA`).
- A 0.3–0.4 s plant-and-release got stolen back by ToLiss's late reset.
- `getMouseLocationGlobal` polling aborted on mere cursor movement (wrong signal — replaced
  by the click/wheel window).
- An earlier version re-asserted the datarefs on a fixed window and lost the race whenever
  ToLiss's init finished last — quiescence detection replaced it.

### Crash lessons — do not regress

**1. Never match plugins to disable by NAME, especially not `"sasl"`.** SASL is a shared
scripting engine used by many unrelated add-ons, so a name match also caught a scenery plugin
(`NIMBUS_AIRPORTS (SASL)`); disabling that mid-session tore down its live draw callbacks and
hard-crashed the sim (`XPLMDrawCallbackRecord ... is being deleted by a different plugin` →
`Forwarding exception to previous handler...`). The fix is **path scoping** — disable only
plugins whose binary lives under the aircraft folder, which structurally cannot touch
scenery/global plugins.

**2. Some aircraft-folder plugins still can't be disabled.** `librain` (rain effects) and
`KOSP` hard-crash if disabled/reloaded mid-session, so they're carved out by a name-based
**exclusion** list (`SkipPluginNameMatches = ("librain", "kosp")`) applied *on top of* the
path scope. Add more substrings there if other aircraft-folder plugins turn out fragile.

**3. (The important one.) Never issue `reload_aircraft` from a flight loop.** It runs
SYNCHRONOUSLY and unloads the aircraft's plugins, so it must come from the **command/menu
handler context**. An earlier version deferred the reload through the driver flight loop (to
add a settle delay after disabling); that ran the whole synchronous aircraft+plugin teardown
nested inside X-Plane's flight-loop dispatch and hard-crashed with the SAME signature as the
plugin-set bugs (`XPLMFlightLoopRecord ... does not exist so cannot be deleted`, draw
callbacks `deleted by a different plugin`) — which is why those errors were a red herring for
the plugin scoping. The manual Developer-menu reload is crash-free precisely because it runs
in a handler context; `_issue_reload()` now matches it, called straight from `Trigger` with
no flight-loop hop.

**Corollaries:** `Trigger` must only ever be invoked from the command/menu handlers, and
`state = ST_AWAIT_LOAD` is set BEFORE `commandOnce` because `MSG_PLANE_LOADED` can fire
synchronously inside that call.

### Snapshot datarefs & the external-power ref

`SnapshotDataRefs` is snapshotted generically via `getDataRefTypes` type detection — scalars,
whole int/float arrays, and (via a `"path[idx]"` subscript parsed by `_parse_ref`) a single
array element, restored with a `setDatavi/f` offset so the rest of the array is left to
ToLiss. Confirmed refs:

- `AirbusFBW/OHPLightSwitches` — all exterior-light switches, whole int array.
- `AirbusFBW/ElecOHPArray[3]` — the external/ground-power pushbutton, index 3 only. Note
  `AirbusFBW/EnableExternalPower` is a *different* thing (GPU-cart availability) and did not
  restore the bus.

Whatever is in this list also feeds the quiescence detector, so adding a ref both restores it
and makes ToLiss touching it count as init activity.

### Cinematic camera — `ToLissPhoton/dev/cinematic_camera`

A slow, eased camera for recording demo footage. Menu ▸ *Cinematic Camera…* opens a small
window (speed readout, Slower/Faster, Start/Stop); the toggle and the two speed steps are
also bindable commands.

**There is no free-camera speed dataref.** `DataRefs.txt` has nothing for it (checked), and
the sim's own `sim/general/*_slow` variants are still far too fast for a pan. So the mode
drives the camera itself via `controlCamera(Forever)` — the same mechanism as the reload
hold, including the `inIsLosingControl` → re-grab watchdog, and the two are mutually
exclusive (starting either ends the other).

**Input comes from your existing bindings.** `_cine_hook_commands` registers *before-phase*
handlers on `sim/general/{forward,backward,left,right,up,down,rot_*,zoom_*}` plus their
`_fast`/`_slow` siblings, and returns **0** (consumed) while the mode is on, so X-Plane never
also moves its own view. Mode off ⇒ the handlers return 1 and every binding behaves exactly
as before, which is why they can stay registered for the session. Held state is tracked per
command via the `(axis, sign, scale)` refcon each was registered with.

Two details that are load-bearing:

- **Axes are camera-relative** — `fwd` follows pitch as well as heading, `side` strafes
  level, and `up` is **world** up (not camera up), because a level rise while pitched down is
  what a crane shot wants.
- **Easing is time-constant based**, `alpha = 1 - exp(-dt/tau)`, not a fixed per-frame
  fraction. A ramp therefore takes the same wall-clock time at 30 fps as at 120, so a
  recording does not change character with framerate. `dt` is clamped at 0.25 s so a stall
  can't fling the camera.

### Cockpit light tuner

Menu ▸ *Cockpit Light Tuner…* — find a cockpit light's position/aim/cone/size in-sim.

**Why it edits the OBJ.** An OBJ light's geometry is baked and read once, at aircraft load;
no dataref moves it afterwards. (Color and brightness *can* be driven live — that is exactly
what `LIGHT_SPILL_CUSTOM` + `ToLissPhoton/interior/spill/map` does for the map spot — but
position, direction, size and cone cannot.) So edits are staged in memory, written into the
installed `objects/lights_inn.obj` in one batch, and picked up by the quick reload, whose
camera restore means consecutive iterations frame the identical shot.

**The deliverable is "Log DSL", not the file.** `dist/` and the installed tree are build
output; the next `build_objs.py build --write` overwrites whatever the tuner wrote. Paste the
logged `pos:` / `dir:` / `size:` / `cone:` line into `src/lights/lights.layout.phdsl`.

Parsing notes (each of these was a bug the round-trip test caught):

- A light is keyed by `(fixture, ordinal)` where the fixture comes from the generator's
  `# <Fixture> -> <Branch>` comment. **The ordinal resets on that comment, never on
  `ANIM_begin`** — mounted fixtures (the panel floods) nest several `ANIM_begin` levels for
  their `ckpt/lights/<n>` transform stack, so resetting there restarts the count mid-fixture.
- **Each line's own leading whitespace is preserved.** Mounted lights are four tabs deep; a
  fixed indent silently reflows 30 lines.
- `LIGHT_PARAM` and `LIGHT_SPILL_CUSTOM` share one editor: after each command's own prefix
  (a class name / nothing) the same twelve numbers line up, and only slot 6 differs in
  meaning (rheostat index vs. baked alpha). Nothing touches slot 6 or r/g/b, so applying an
  edit to all three branches **cannot flatten the three color variants into one**.
- Writes go through temp + `os.replace`, matching `build_objs.atomic_write_bytes` — a reload
  must never race a half-written OBJ (that crashed the sim once; see the crash lessons).

### Live light debug

Menu ▸ *Live Light Debug…* — the other half of the tuner above. Where that one moves
geometry by rewriting the OBJ and reloading, this drives **color, aim, cone, size and
brightness live**, with no rebuild and no reload at all.

```
python build/build_objs.py build --target interior --debug --write
python build/build_objs.py build --target screens  --debug --write   # display glow
```

That emits an OBJ in which **every** light is a `LIGHT_SPILL_CUSTOM` bound to
`ToLissPhoton/debug/light/<n>`, plus a manifest naming each one. The dev plugin
registers the dataref pool at `XPluginStart` (it must — an OBJ binds dataref *names* at
load time, and one that doesn't exist yet reads 0 forever), reads the manifest when the
aircraft loads, and feeds all nine params per light every frame.

**Two targets are debuggable, and since 2026-08-09 both at once.** Each writes its own
manifest — `lights_inn.debug.json` (cockpit, 30 lights: the 26 authored plus the 4
simplified flood copies) and `lights_screens.debug.json` (screens, 6) — and each
target's slots start at its own base (`DEBUG_SLOT_BASE` in `build_objs.py`: interior
from 0, screens from 48), so the two OBJs never share a slot. The **native Dev
window's Lights tab merges every manifest it finds**; `deploy.ps1 -Dev -DebugObjs`
installs both debug OBJs in one command. The Python tool (`PI_PhotonDevReload.py`)
still drives one manifest at a time, the most recently written, and names it in the
window (`screens: 6 lights`).

**A PLAIN build of those same two targets writes the read-only twin instead** —
`lights_inn.info.json` / `lights_screens.info.json` (`INFO_MANIFESTS`, 2026-08-10) — and
deletes the debug one; a `--debug` build deletes the info one. So exactly one manifest per
target exists at any moment, which is what lets the Lights tab tell "tunable" from
"inspectable" by filename alone. The info rows carry the **same slot numbers** as the debug
rows (same bases, same order), so slot 12 names one light whichever build you are looking
at — `test_a_light_is_the_same_light_and_the_same_slot_in_both` pins it. They add a
`variants` list, one entry per profile branch, **ordered by the category dataref's value**.
The exterior gets neither manifest: frozen goldens, no debug build, nothing to read against.

⚠ These are **dev-only sidecars that nothing ships.** They land in `dist/` and, under
`--write`, in the installed aircraft; `make_release.py` stages OBJs by name and so has no
way to pick one up.

A debug interior build also keeps the `ToLissPhoton/interior/optimized` gate and
emits the **simplified flood copies as tunable lights of their own** (manifest rows
carrying `boost`), so the Dev window's Cockpit tab can drive the `optimize: boost`
factor as one live multiplier. "Use simplified lighting" on the Cockpit tab picks
which set draws, exactly as on a shipping OBJ.

**Brightness source.** Each manifest row carries a `source`, which is the knob the
plugin multiplies the light's alpha by — `LIGHT_SPILL_CUSTOM` has no `INDEX` slot, so
without it every debug light would ignore the very knob it is identified by:

| `source` | Dataref | Used by |
|---|---|---|
| `panel` | `sim/cockpit2/electrical/panel_brightness_ratio[index]` | most cockpit lamps |
| `inst` | `sim/cockpit2/electrical/instrument_brightness_ratio[index]` | the console strips |
| `map` | `ckpt/lights/map` | the re-added map spot |
| `du` | `AirbusFBW/DUBrightness[index]` | the six screen-glow lights |

`du` is also the **test for whether the screen mapping is right**: dim one display and
exactly one glow should follow it. If the wrong one moves, fix `kScreenDU` in
`plugin.cpp` and the matching `index:` in the DSL together — a test fails if only one
of the two moves. See `docs/screens_plan.md`.

**What it is actually for is identification.** "Which of the ten panel flood lamps is
scorching the glareshield?" is a question the geometry tuner answers in a dozen
edit/reload cycles and this one answers in seconds:

| Control | Does |
|---|---|
| **Isolate** | blacks out every light except the selected one |
| **Blink** | square-wave flashes the selected one, everything else left lit |
| **Mark** | paints the selected one magenta — nothing in a cockpit is magenta |
| **`<< Cat` / `Cat >>`** | jump to the next *category*, i.e. the next menu row's lights |
| **Debug lights / Original lines** | A/B against the untouched originals (see below) |
| **Norm dir** | scale the selected light's `dir` to unit length, aim unchanged |
| **Orient `<` / `>`**, **Dir `<` / `>`**, **Link**, **Reset** | cycle the axis-orientation hypothesis (see below) |
| **Log DSL** | dump the current values to `XPPython3Log.txt`, split by which `.phdsl` file they belong in |

**The A/B toggle is the one to reach for when the lights look wrong.** The debug OBJ
emits *both* the untouched original light line and the tunable spill copy for every
light, gated on `ToLissPhoton/debug/compare` (0 = original, 1 = debug). This exists
because converting a light to `LIGHT_SPILL_CUSTOM` is not self-evidently a no-op:
`airplane_panel_sp` is a **named** light whose `SPILL_SW` overload X-Plane resolves
through its own handler, and whether that handler treats position, direction, a
non-normalized `dir` — or **overall intensity** — differently from a raw custom spill
is not documented anywhere checkable. ⚠ **Intensity is the live suspect** (reported
2026-08-11: the dome extremely dim under `-DebugObjs`, normal on regular OBJs): in
XP12's photometric pipeline a raw custom spill gets the fixed legacy-luminance
calibration, while the named handler can scale its lights however it likes, and our
conversion replicates only the rheostat (linearly, into alpha). An audit that day found
the rest of the chain sound — wire order translated, the omni encoding right, the
rheostat name/index corroborated by the `.acf` spot rows, handles dropped on aircraft
load — so if Original is bright and Debug is dim **with identical numbers**, that is
the conversion's calibration, not a Photon bug, and brightness judged on the tunable
side is biased dim for every `airplane_{panel,inst}_sp` light. Geometry judgments
(position, aim, cone) are unaffected. So:

- **Original looks right, Debug does not** → the conversion is at fault, and the tuned
  numbers are not trustworthy until it is understood.
- **Both look the same** → the conversion is sound and the authored `.phdsl` values are
  what is wrong. Tune away.

While *Original lines* is showing, the tuner controls nothing — that is expected, and the
button that is lit tells you which you are looking at.

**`Norm dir` tests a specific suspicion.** Several transcribed direction vectors are a
long way from unit length — `int_panelflood`'s `0 -0.15 -0.1` is 0.18, `int_console`'s
`0 -0.3 0` is 0.30 — and `cone` is compared against a *dot product* with that vector. If
X-Plane does not normalize internally, a short vector drags every dot product toward the
cone threshold, so **aim and spread interact**. Two standing in-sim reports fit that
shape: cone edits to the left-most main panel flood doing nothing, and cone edits to the
screen glow appearing to move the light. One click tests it.

**Nothing normalizes `dir:` automatically** — not the DSL, not the generator, not the
plugin. `build` now *reports* every non-unit vector with its unit form ready to paste:

```
note: 6 light type(s) have a non-unit `dir:`. ...
    int_panelflood   dir 0 -0.15 -0.1   |d| 0.180   unit: 0 -0.832 -0.555
```

Deliberately a warning rather than a fix. Most offenders are Gus's *measured* values, and
silently rescaling the trusted baseline is exactly the kind of quiet rewrite this project
guards against; normalizing is a visible, reviewable `.phdsl` edit. Note that `Norm dir`
in the window changes the light's aim by **nothing** — it only changes the length — so if
the beam moves when you click it, the sim is not normalizing and that is the answer.

### Step size — one button per decade

The `-`/`+` buttons on every row move by whatever the lit **Step** button says: `10`,
`1`, `0.1`, `0.01` or `0.001`, in that row's own units. The label *is* the amount added.
`10` exists for the angle rows — swinging an aim through a quadrant at 1° a click is 90
clicks — and doubles as a coarse position jump inside the ±12 m span.

This replaced a `Fine`/`Coarse` pair whose amount also varied per row *kind* (`cone` fine
was 0.005, `size` coarse 0.25, and so on). Two hidden variables meant the distance a click
moved something was never on screen — and "did that click do anything?" is the question
this window exists to answer. One ladder serves every row because each value is either a
0..1 fraction (rgb, alpha, cone) or meters (pos, size, dir), and 1 → 0.001 spans both.

Nudges are **rounded to 6 dp** as they are applied: 0.1 added ten times is
`0.9999999999999999` in IEEE754, and those digits would otherwise fill the window and get
pasted into the `.phdsl` via "Log DSL". Per-row *clamping* is unchanged — rgb and alpha
stay in 0..1, `cone` in −1..1, `size` non-negative, position within `DebugPosRange`.

### Aim is edited as angles, not as a vector

The rows are **Pitch**, **Yaw** and **Spread**, all in degrees. There is no `Dir X/Y/Z`
and no `Cone`.

Steering a light by its three direction components was reported in-sim as "very difficult
to get things pointed correctly", and it has a second failure mode beyond awkwardness: a
hand-edited vector's **length is not free**, because X-Plane compares `cone` against a dot
product with it (below). Angles cannot express a bad length at all.

- `Pitch` — degrees above horizontal; clamped to ±90. Past the pole an aim would start
  coming back down while yaw silently flipped 180°, which is unsteerable.
- `Yaw` — aircraft heading: **0 is forward**, 90 right, 180 aft. Wraps, because −1 and
  359 are the same heading. The window prints the direction in words next to the number
  (`down`, `fwd-left`, `aft`) so the sign convention never has to be recalled.
- `Spread` — the **half-angle**, the right way round: widening the beam raises this. Slot
  8 still holds the cosine the light reads and the row shows it in brackets.

`Aim` presets sit under the step row: `Fwd / Aft / Left / Right / Up / Down`, one click
each, and they clear any leftover `Dir` orientation so the state is known.

⚠ **`Spread` has been reported twice as having no visible effect** — see the note at the
end of this section; it is an open bug, not a convention you are misreading.

Internally slots 5..7 remain dx/dy/dz and are **derived** from pitch/yaw, one direction
of flow only. So every vector this window can emit is unit length — which is why the old
`Norm dir` button is gone: it can no longer apply.

"Log DSL" prints `aim:`/`spread:` ready to paste, with the vector form beside it as a
comment. Both are first-class in the DSL — see `docs/dsl.md`.

### Position markers — seeing where a light actually is

A spill light is **invisible in mid-air**. You see the pool it casts, never the source,
and on a close-range light the bright part of that pool is nowhere near the origin — so
placing one by eye has been guesswork on this project, and the same complaint has now
been raised against the cockpit spots and the screen glow.

So a debug build also emits, per light, a marker in three parts:

| Part | Color | What it is |
|---|---|---|
| origin | green | where the light actually sits |
| axis | amber | 5 dots out to `reach`, along the aim |
| cone rim | blue | 4 dots on the beam edge at that same distance |

**The rim is what makes aim judgeable.** The first version drew only a 20 cm axis line,
and that was reported as reflecting position well but "not fully oriented with the spill
light" — which is what a short line looks like beside a pool of light a meter across. The
ring draws the beam the `Spread` row claims you asked for, so "is the lit patch inside the
ring?" becomes a question with a yes/no answer. Its radius is `tan(half-spread) × reach`,
and the last axis dot lands exactly on the ring's plane.

`reach` is adjustable for the same reason: the right arrow length depends entirely on how
far the light throws.

The `Marker` row switches it:

| | |
|---|---|
| `Off` | default — a debug build looks like the cockpit until you ask it not to |
| `This` | the selected light only; follows `Next >` |
| `All` | every light at once — good for "is the whole set in the right plane?" |
| `size - / +` | the sprite's size, stepped by the current **Step** |
| `reach - / +` | how far out the arrow and the cone ring are drawn |

`size` and `reach` are on that row and not elsewhere on purpose: a marker too small to
see, one drawn too short to read, and one that is not being drawn at all look exactly
alike, so the fixes have to sit next to the switch.

Things that are load-bearing:

- **One dataref gates every light's markers**, by value: `0` = off, `n+1` = light *n*,
  `-1` = all. Each light's block hides on `[-0.5, n+0.5]` and `[n+1.5, big]` — the same
  paired-`ANIM_hide` encoding the DSL uses for a ternary category — so `-1` escapes both
  ranges and shows everything. That is what avoids 64 more registrations.
- **The arrow is always `reach` long, whatever the vector's length.** It answers "which
  way", and a light authored with a short `dir` would otherwise draw a stub next to its
  neighbors', which the eye reads as a *weaker light* rather than a shorter vector.
- **⚠ The arrow shows the aim you ASKED FOR, not the one the orientation cycler
  produces.** It reads slots 5..7 directly instead of going through `_dbg_params`. This
  is deliberate and it is the reverse of the first version: `_dbg_params` is where the
  cycler is applied, so a marker that read through it moved the arrow and the light
  *together* — which made the only instrument capable of exposing an axis mismatch
  incapable of exposing one. The marker is the **reference**; the light is the sim's
  answer. See "Finding an axis mismatch" below.
- **The perpendicular basis for the ring seeds off the axis the aim leans on least**, so
  the cross product cannot collapse. A fixed seed would degenerate for any light aimed
  along it — and straight down and straight aft are the two commonest cockpit aims.
- **The arrow goes through `_dbg_params`**, so it carries the orientation hypothesis
  exactly as the light does. A marker still pointing the authored way mid-sweep would look
  like evidence for whichever answer you were hoping for.
- **The dots are outside the A/B compare gate.** "Where is this light" is a question you
  have just as much on the Original side.
- **No `ANIM_rotate` in the OBJ.** Rotation is about the *object* origin, so aiming a
  cockpit light's arrow that way would swing it through a five-meter arc unless a static
  translate went first — and the rig would then duplicate maths the plugin already does.
  Every dot rides three dataref-driven `ANIM_trans` instead, exactly like the position
  wrappers, and the plugin recomputes the offsets each frame.
- **The primitive is `LIGHT_CUSTOM` with a `none`-style param dataref.** It is the only
  OBJ8 light that is a plain textured billboard with author-controlled color and size and
  no dependency on sim state — a named `airplane_*_bb` is gated on that light being
  switched on, which in a parked cockpit it is not. The accessor writes **all nine**
  params rather than modifying in place, because whether X-Plane pre-fills a custom
  light's buffer is still an open question here and a black marker would be read as the
  feature not working.

### Position tuning is on by default — `--no-debug-pos` opts out

A `--debug` build wraps every light in three `ANIM_trans` blocks, so X/Y/Z are live knobs
like everything else.

This was briefly the other way round. On 2026-07-28 the debug lights turned up mis-placed
in-sim and the wrappers — the one part of a debug build that changes how a light *renders*
rather than merely where its parameters come from — were the obvious suspect, so they went
behind a flag. That was a red herring: the cause was elsewhere (2026-07-29), and the
default is back.

```bash
python build/build_objs.py build --target interior --debug --write                 # X/Y/Z live
python build/build_objs.py build --target interior --debug --no-debug-pos --write  # without
```

**Keep the opt-out working.** It is how the wrappers get ruled out the next time position
looks wrong, and it drives real plumbing: the manifest records which you built as
`pos_tunable`, and the dev window **hides every position control** when it is false — the
X/Y/Z rows and the position half of the orientation cycler both disappear, and the readout
says why. A knob that moves nothing is worse than no knob: in a cockpit it reads as the
light refusing to move, not as the feature being switched off. The OBJ header states which
half it carries, for the same reason.

Aim stays tunable either way — `dx/dy/dz` go through the light's own dataref and never
needed `ANIM_trans`.

Swapping between the two builds changes the window's row count; it rebuilds itself on
`Rescan` and on aircraft reload, since its resizing limits are fixed at creation.

### The slot probe — measuring what the nine dataref floats actually mean

**ANSWERED 2026-07-30.** The dataref's order is not the OBJ line's:

```
OBJ8 line:   LIGHT_SPILL_CUSTOM x y z  r g b a  size  dx dy dz  semi  <dataref>
dataref:                              r g b a  size  SEMI  dx dy dz
```

The cone comes **first** in the trailing group of four. Probing our slots 5/6/7/8 one at a
time gave **omni / right / up / aft** — `SEMI / DX / DY / DZ`. Fixed by one translation at
the wire (`DebugDataRefOrder`); `dbgVals` keeps the readable order everywhere else.

That single rotation produced every symptom that had accumulated: `aim Fwd` pointing
straight down (our `dz` → `DY`), pitch swinging the light horizontally (our `dy` → `DX`),
the cone moving the aim (our `cone` → `DZ`), a direction changing the beam width (our `dx`
→ `SEMI`), and `spread` appearing inert (`SEMI` was never being fed the cone). It is also
why the **48-permutation axis hunt could not have found it** — a rotation of four slots is
not an axis permutation — and why the A/B verdict was "Original right, Debug wrong": only
dataref-driven aim was affected.

Three successive explanations were offered before anyone measured, and **all three were
wrong**: an axis permutation, non-unit `dir` lengths, and "the array is read one slot late"
(which predicted `SIZE` = our `dx` = 0, an invisible light, contradicted by the light being
visible). The probe below exists so that does not happen again.

**`Probe 0`…`8`** sends `DebugProbeBaseline` with that one slot set to 1:

```
r=1 g=1 b=1 a=1 size=4 dx=0 dy=0 dz=0 cone=0      <- baseline
                        ^ slot 5..7 zero, so a probe of one is the ONLY direction present
```

Nine clicks map our array onto X-Plane's parameters with no inference. Read it against the
expectation printed on the row (`slot 5 = 1 -> expect: aim RIGHT (+x)`); markers switch on
with the probe, so the arrow is the reference and the light is the answer.

Load-bearing choices:

- **The probe bypasses everything else** — orientation, rheostat, `Isolate`, `Blink`,
  `Mark`. Each would be a second variable in an experiment designed to have one.
- **Every other light goes dark.** Two lit lights and you cannot say which one moved.
- **`dir 0 0 0` and `cone 0` in the baseline.** Omnidirectional means a probe of 5/6/7
  introduces the only direction present; `cone 0` means that if slot 8 turns out to be a
  direction component after all, it contributes nothing to the other probes.
- **`size 4` m.** Big enough that the pool lands on real geometry — a cone restriction is
  nearly invisible when every lit surface is two centimeters from the light, which is
  probably part of why cone edits read as inert.

Now that the layout is known, the probe doubles as its **regression test**: with
`DebugDataRefOrder` right, all nine probes match the expectation printed on the row. Slot 8
is the discriminator — pre-fix it swung the light aft, post-fix it does nothing (the
baseline's `dir 0 0 0` is omnidirectional, so there is no cone to narrow).

**What did NOT need changing, and must not be "fixed":**

- `build/build_objs.py` bakes the **OBJ literal order** (`dx dy dz semi`). Correct as is.
- `plugin.cpp` writes **only alpha**, and alpha is index 3 in *both* orderings, so
  `kSpillAlphaSlot = 3` is right and the shipping lights were never affected. A test pins
  that invariant — if alpha ever moves, the map spot and screen glow go dark or
  full-bright.

⚠ **Anything tuned through the debug tool before 2026-07-30 is not trustworthy.** Every
`--debug` light was aimed by a rotated vector carrying its cone value in the `DZ` slot, so
the interior numbers found that way need redoing.

### Finding an axis mismatch

**Reported in-sim 2026-07-30, and open.** On the screen-glow lights, with `Yaw 180`:
editing **Pitch** to +22° swung the *light* 22° to the **right**; −22° swung it left. The
markers moved up and down, as asked. So the aim the DSL states and the aim X-Plane renders
disagree about which component is which.

The arithmetic pins the candidate. At yaw 180 the authored vector is
`(0, sin p, cos p)` — pitch only ever touches **y**. A light that moves horizontally on a
y-only change is reading our y as its x, i.e. the mapping is `Y X Z` (or `Y -X Z`; the
sign on the other axis is not constrained by one observation).

**Use the `Aim` presets, not a sweep.** `Fwd / Aft / Left / Right / Up / Down` each put
the selected light on one exact axis and **clear any leftover `Dir` orientation**, so one
click fully determines what was asked for. Sweeping an angle and watching a pool travel
through a cockpit full of surfaces is not a clean read — an interpolated arc can be
interpreted several ways, which is how "pitching up makes it point right" ended up hard to
act on.

The method:

1. `Isolate` on, `Marker: This`, and crank `size` up with the `10` step so the pool falls
   on distant geometry instead of the panel two centimeters away.
2. Click **`Up`**. Does the light go up? Then `Left`, then `Aft`. Three clicks name the
   mapping outright: "Up sends it right" means our y is the sim's x.
3. If it is wrong, cycle **`Dir >`** (turn `Link` **off** first, so position stays put)
   until the pool lands inside the blue cone ring. **The arrow does not move**; only the
   light does.
4. The readout under the buttons and every `Log DSL` dump name the winning mapping. Report
   it — it then gets baked into the aim conversion as a documented correction rather than
   left as a live control.

Predicted from the 2026-07-30 report: `Y X Z` (index 16), possibly `Y -X Z` (18). One
observation does not fix the sign on the unobserved axis, which is what step 2 settles.

`Log DSL` also prints the exact nine floats X-Plane reads for that light, labeled. That
is what separates "our maths is wrong" from "the sim disagrees about what slot 5 means" —
the two are indistinguishable from inside the cockpit.

⚠ **Do not assume this is the same bug as the panel floods'** (`CLAUDE.md` thread 1b).
That one has a competing explanation in the `dir` lengths; this one is on lights whose
`dir` is unit by construction.

### The orientation cycler

*(Its position half needs the wrappers, i.e. anything but a `--no-debug-pos` build.)*

Reported in-sim 2026-07-28: on **Original** the lights are right, on **Debug** they sit in
the wrong places and face the wrong way — yet they are still in *distinct* places and they
still respond to the position knobs.

That pattern rules out most explanations. The baked x/y/z are byte-identical between the
two lines (a test asserts it) and the offsets start at zero, so nothing in the tool is
moving them; the geometry pipeline plainly works. What is left is that the two light forms
disagree about **which axis is which**.

There are 48 signed axis permutations and no way to reason out the right one from outside
the sim, so the tool enumerates all 48 and puts them on a button. **Click until the cockpit
snaps into place; the label under the buttons is then the answer**, and it also goes to
`XPPython3Log.txt` on every click and in every "Log DSL" dump.

- The correction applied to position is `permute(pos) − pos`, i.e. the light is physically
  *relocated* from where the OBJ baked it to where the hypothesis says it belongs. It has
  to be the difference: `permute(pos)` alone would land the light at double the coordinate
  and make every hypothesis look wrong.
- **Index 0 is the identity**, so a fresh session behaves exactly as it did before this
  existed, and nothing moves until you cycle.
- It is **global, not per-light** — one click re-tests the whole cockpit, because "the
  cockpit suddenly looks right" is the signal you are watching for.
- **Manual X/Y/Z nudges are never permuted.** They stay in plain OBJ axes and add on top,
  so "X +" always moves along the axis the readout calls X even mid-search — otherwise the
  two effects cannot be told apart.
- **Link** (default on) steps position and direction together. A genuine coordinate-space
  mismatch would hit both the same way, so that 48-click sweep is the one to run first;
  unlink for the 48×48 case only if the linked sweep finds nothing.
- This is why `DebugPosRange` is **12 m** and not the couple of meters tuning needs: a
  permutation of a cockpit coordinate is meters, and X-Plane clamps outside the outermost
  `ANIM_trans` keyframe. Too narrow a span would silently truncate exactly the hypotheses
  worth testing and show a *correct* permutation as a wrong one — the one failure this
  search cannot survive. A test checks the span covers every permutation of a realistic
  cockpit coordinate.

**Result so far (2026-07-28).** `pos 1` with `dir 9` makes the **middle two** main-panel
flood lamps look right — and several other directions do the same — but the **outer**
floods stay wrong under every combination. A single rigid axis swap cannot do that: it
would be right for all of them or none. That points at the transform *stacking* with
something else per-fixture (the mount rigging is the obvious suspect) rather than at a
global coordinate-space mismatch, which is a different investigation.

Things worth knowing before using it:

- **Everything is editable** — color, alpha, size, aim, cone, and (unless you built
  `--no-debug-pos`) x/y/z. Position takes a different route: `LIGHT_SPILL_CUSTOM` keeps x/y/z
  baked in the OBJ and passes only the other nine params through its dataref, but an OBJ8
  `ANIM_trans` takes a dataref of its own, so the debug build wraps every light in three
  of them (one per axis) reading `ToLissPhoton/debug/pos[3n+axis]`. Keyframes span
  ∓12 m keyed on the same values, so the dataref reads directly as **meters of offset**
  and X-Plane's keyframe clamping bounds it. (The span is set by the orientation cycler's
  needs, not by tuning's — see above. It costs no precision.)
  - The X/Y/Z rows are therefore **offsets**, not coordinates. The window prints the
    resulting absolute position beside each, and "Log DSL" emits the absolute `pos:`.
  - For a mounted lamp the offset applies *inside* ToLiss's `ckpt/lights/<n>` stack, so
    it nudges the light relative to where the animation puts it — which is the frame the
    `.phdsl` fixture writes in anyway.
  - `DebugPosRange` (plugin) and `DEBUG_POS_RANGE` (generator) are the same keyframe span
    and must not drift, or every position edit is silently rescaled. A test pins them.
- **A debug OBJ has no category gating at all**, so the Cockpit profile menu does nothing
  while one is installed — which is indistinguishable from the menu being broken. The
  generated file carries a `DEBUG LIGHT BUILD — NOT INSTALLABLE` banner for exactly that
  reason. Put the real one back with a plain
  `build_objs.py build --target interior --write`.
- **Brightness is applied by the plugin, not the sim.** `LIGHT_SPILL_CUSTOM` has no INDEX
  slot, so the manifest records each light's rheostat (`panel[n]`, `inst[n]`, or
  `ckpt/lights/map`) and the plugin multiplies it into alpha. Without that every debug
  light would sit at full brightness and Isolate would be the only usable control.
- **`cone` runs backwards** — it is cos(half-spread), so a *bigger* number is a
  *narrower* beam. The window prints the angle beside the value so you don't have to hold
  that in your head, and the `Spread` row edits the angle directly.
- **⚠ OPEN: `Spread` appears to do nothing.** Reported on the main panel floods
  (2026-07-28) and again on the screen glow (2026-07-30). The `cone/|dir|` explanation
  offered in between is **doubtful**: it predicted that normalizing `dir` would restore
  control, and the screens' `dir` is unit by construction yet their spread is still inert.
  What has been ruled out: the accessor's `count < 0` handling (correct — it would
  otherwise silently drop slot 8, which is exactly this symptom), the slot layout
  (`LIGHT_SPILL_CUSTOM`'s 9 floats put `semi` at index 8, the same position
  `airplane_panel_sp` puts `WIDTH`), and an attachment-row rotation (all
  `_v10_att_{psi,the,phi}_ref` are 0).
  Still to try, cheapest first:
  1. Sweep `Spread` 1° → 89° with the `10` step — nine clicks. If *nothing* changes across
     the entire range, slot 8 is not being read at all, which is a different bug from a
     subtle one. Crank `size` up first: a cone restriction is nearly invisible when every
     lit surface is 2 cm from the light.
  2. Check the A/B row. On **Original lines** you are looking at a light whose cone is
     baked and untunable — while the markers respond regardless, because they sit outside
     the compare gate. That alone produces "indicators move, light does not".
  3. `Log DSL` prints the exact nine floats X-Plane reads, labeled. Confirm slot 8 is
     carrying what the row claims.
- **`DebugMaxLights` in the dev plugin and `DEBUG_MAX_LIGHTS` in `build_objs.py` size the
  same pool** and must not drift; a light past the end of the pool binds to a dataref that
  was never registered and draws black. A test pins them together.
- **The manifest is read on first ACCESS, not on window-open.** Accessors are registered
  at `XPluginStart`, when no aircraft exists and so no manifest can be read; the window
  might never be opened at all. Loading lazily from the first accessor call is what
  guarantees the read happens after the OBJ has loaded and before anything draws.
  Window-open-only left every light reading zeros — a cockpit of black lights that looked
  exactly like the tool being broken. `MSG_PLANE_LOADED` re-reads on top of that, since a
  reload can swap the OBJ underneath.

---

## XPPython3 API notes

These apply to the dev plugin only — the shipping runtime is C++ (see
`src/native/README.md`). Kept because they were verified the hard way.

- `xp.registerDataAccessor(name, readInt=, writeInt=, readFloat=, writeFloat=)` → accessor;
  `xp.unregisterDataAccessor(a)` at `XPluginStop`. Array datarefs take
  `readFloatArray=cb` with `cb(refCon, values, offset, count)`.

  **Three conventions here that all fail silently. All three were shipped wrong once**
  (2026-07-28) and the only symptom was "the lights just sit there, and no dataref shows
  up in DataRefTool":

  - **The keyword is `readRefCon` / `writeRefCon`, never `refCon`.** There are two, one
    per direction. Passing `refCon=` raises `TypeError` — so *nothing registers at all*.
  - **`values` is an EMPTY list you `.extend()`**, not a pre-sized buffer you index.
    `values[i] = x` raises `IndexError`; a try/except around the callback turns that into
    a light that draws nothing, forever.
  - **`count == -1` means "all elements"**, not "none". `for i in range(count)` quietly
    does nothing.

  Plus `values is None` is X-Plane asking for the array *size* — return the element count.
  `XPPython3/utils/datarefs.py` in the install is the reference implementation; read it
  rather than guessing. `tests/test_dev_light_debug.py` pins all of the above against a
  stub `xp`, which is the only way to catch them outside a running sim.
- **A custom dataref is not discoverable.** DataRefTool and DataRefEditor list X-Plane's
  own table plus whatever plugins explicitly announce, so an unannounced accessor works
  perfectly and appears nowhere — which reads as "it was never created". Announce with
  `xp.sendMessageToPlugin(pid, 0x01000000, name)` to
  `com.leecbaker.datareftool` and `xplanesdk.examples.DataRefEditor`.
- **Register accessors in `XPluginStart`, not `XPluginEnable`.** An OBJ resolves dataref
  names when the *aircraft* loads; a name registered later is bound to nothing for that
  aircraft's whole life and reads 0. Same rule the shipping `.xpl` follows, and the same
  failure (a light that silently never lights).
- Flight loops: `xp.createFlightLoop(callback, phase, refCon)` → loopID;
  `xp.scheduleFlightLoop(loopID, interval, relativeToNow)` (interval `-1.0` = every frame);
  `xp.destroyFlightLoop(loopID)`. Phase `FlightLoop_Phase_AfterFlightModel` (getattr fallback
  `1`). Callback signature `(sinceLast, sinceLoop, counter, refCon)` → next interval. Also
  the older `registerFlightLoopCallback(cb, interval, refCon)`.
- **`xp.getDatavf(ref, values, offset, count)` fills the list you pass and returns the
  count** (an int), not a new list. Passing `values=None` returns an int and blows up
  downstream — pass `[]` and read it back. `xp.setDatavf(ref, values, offset, count)` is the
  symmetric writer (probe-verified).
- Scalars: `getDatai`/`getDataf`; `getDatas` for string datarefs.
- `xp.getNthAircraftModel(0)` → `(filename, fullpath)` for the user aircraft.
- `XPluginReceiveMessage(inFromWho, inMessage, inParam)`; `inParam == 0` = user aircraft.
  `MSG_PLANE_LOADED` (102), `MSG_LIVERY_LOADED` (108) — getattr fallbacks.
- Menu: `findPluginsMenu`, `appendMenuItem`, `appendMenuSeparator`, `createMenu`,
  `checkMenuItem`, `destroyMenu`, `removeMenuItem`. Handler `(menuRefCon, itemRefCon)`. Check
  states `Menu_Checked`/`Menu_Unchecked` (getattr fallbacks 2/1).
- `xp.getSystemPath()` → X-Plane root. `xp.log(...)` → `XPPython3Log.txt`.
- Plugin file must be named `PI_*.py`, placed **directly** in
  `Resources/plugins/PythonPlugins/` (no subdirectories). Class `PythonInterface` with
  `XPluginStart`/`Enable`/`Disable`/`Stop`/`ReceiveMessage`.

**Menu crash note:** XPPython3 4.7a1 crashes the sim in native menu click-dispatch for any
item in a submenu-of-a-submenu (menu level 2+). This is why the runtime plugin's Custom
profile is a **window, not a nested submenu** — every menu item stays at level 1.
