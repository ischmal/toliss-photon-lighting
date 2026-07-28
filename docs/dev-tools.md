# Dev tooling: watch.py and the quick-reload plugin

Detail reference for the in-sim iteration loop. `CLAUDE.md` keeps only the commands and the
one-line tripwires; the reasoning — most of it paid for with hard sim crashes — is here.

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
`installer/payload.py` stage the native `.xpl`, so a `PI_*.py` in `src/plugin/` is ignored by
the release. Install it as a **symlink** into the live `Resources/plugins/PythonPlugins/`
(created by hand — `New-Item -ItemType SymbolicLink`) so repo edits load with an
aircraft/plugin reload, no copy step. Its command, dataref, and signature namespaces are
disjoint from the runtime plugin's, so the two never collide.

All tunables (including the snapshot dataref list) are a block at the top of the file;
everything is try/except-guarded and falls back to idle on error.

Plugins ▸ **Photon Dev** is a **flat** menu (Quick Reload / Plain Reload / Release Camera ─
Cinematic Camera… / Cockpit Light Tuner… ─ Log Plugin List). It must stay flat: the level-2
submenu crash below binds this plugin, not the native `.xpl`.

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
no dataref moves it afterwards. (Colour and brightness *can* be driven live — that is exactly
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
  `ANIM_begin`** — mounted fixtures (the map lamps) nest several `ANIM_begin` levels for
  their `ckpt/lights/<n>` transform stack, so resetting there restarts the count mid-fixture.
- **Each line's own leading whitespace is preserved.** Mounted lights are four tabs deep; a
  fixed indent silently reflows 30 lines.
- `LIGHT_PARAM` and `LIGHT_SPILL_CUSTOM` share one editor: after each command's own prefix
  (a class name / nothing) the same twelve numbers line up, and only slot 6 differs in
  meaning (rheostat index vs. baked alpha). Nothing touches slot 6 or r/g/b, so applying an
  edit to all three branches **cannot flatten the three colour variants into one**.
- Writes go through temp + `os.replace`, matching `build_objs.atomic_write_bytes` — a reload
  must never race a half-written OBJ (that crashed the sim once; see the crash lessons).

---

## XPPython3 API notes

These apply to the dev plugin only — the shipping runtime is C++ (see
`src/native/README.md`). Kept because they were verified the hard way.

- `xp.registerDataAccessor(name, readInt=, writeInt=, readFloat=, writeFloat=)` → accessor;
  `xp.unregisterDataAccessor(a)` at `XPluginStop`.
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
