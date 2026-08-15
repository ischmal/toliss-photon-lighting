---
name: photon-dev-window
description: The PHOTON_DEV-only Dev window in ToLissPhoton.xpl — its tabs (Exterior, Cockpit, Displays, Settings, Perf, Panel FX, Lights, History, Probe, Build, Log, ImGui), the Build tab's worker thread, and the Lights-tab / PI_PhotonDevReload.py pool-ownership standoff. Load before editing Dev-window UI, the Build tab, or the light-editor pool.
---

# The Dev window

`PHOTON_DEV` builds add **Plugins ▸ ToLiss Photon ▸ Dev**, whose one window holds every
knob, tabbed: **Exterior** / **Cockpit** / **Displays** / **Settings** / **Perf** (the
shipping settings panes) ·
**Panel FX** (the layer editor) · **Lights** (the live light editor) · **History** (FX
undo/journal) · **Probe** (panel-FBO probe, plus the display-power readout and the **CB
watch** — the only way to learn a `CBArray` index, since nothing names its 363 elements:
arm, pull the breaker, read the index) · **Build** (runs the repo's Python tooling
in-sim, output captured) · **Log** (this plugin's own log ring) · **ImGui** (the stock demo,
i.e. the backend smoke test).

⚠ **The Probe tab's electrical readout names each failure SEPARATELY, and that is the
point of it** — bus *not found* vs. *bound but reporting no elements* vs. *index past the
end*, and for the DCDUs the bound-command count shown apart from the levels. All of those
look in-sim like "the number never moves" and every one wants a different fix; a single flat
"not bound" line cost a session each time.

⚠ **This is the SECOND window now.** The shipping UI is its own tabbed window (Exterior /
Cockpit / Displays / Settings / About, `BuildMainUi`), reachable from the ordinary menu in
every build, plus a separate shipping **Performance window** opened from a button on the
Settings tab and one at the bottom of the Displays tab. The Dev window's first five tabs call
**the same builders** — `BuildConfigTab(kExtAxis)`, `BuildConfigTab(kIntAxis)`,
`BuildDisplaysTab`, `BuildSettingsTab`, `BuildPerfTab(true)` — deliberately, so a tuning pass
never has to wonder whether a duplicated copy has drifted. Add a control to the shipping pane
and it appears in both; add one *here* and it will not. The Dev **Settings** tab appends one
dev-only section AFTER the shared builder — `BuildDevCockpitTuning()`, the
**simplified-flood intensity multiplier** (`gDbgFloodBoost`, the live `optimize: boost <f>`)
— deliberately outside `BuildSettingsTab`, which the shipping window also builds. ⚠ **It
moved here from the Cockpit tab with the checkbox it depends on** (2026-08-15): the knob
scales the simplified floods and *"Use simplified panel flood lights"* is what decides
whether those are the lights drawing at all.

⚠ **Every Dev tab goes through `DevTab(label, body)`**, which opens the `#23282e` panel
(`UiBeginPanel`) the content sits on. `ImGuiCol_WindowBg` is transparent — a tab that calls
`ImGui::BeginTabItem` directly is text floating over the cockpit.

⚠ **The Perf tab is the SAME pane as the shipping one with `dev` true.** The difference is
the internal levers (the FX compositor as a whole, the waveform engine) and the full sweep —
and that the dev pane offers **manual** lever checkboxes at all. Those are deliberately
**not saved**: on the shipping pane an unsaved switch would look exactly like the Displays
tab's and forget itself on the next launch, which is why that pane shows the state read-only
and only ever moves a lever inside a run. See `src/native/README.md` §The performance tool.

⚠ **The FX compositor is no longer dev-only** — only its **Panel FX** and **History** tabs
are. See the `panel-fx` skill.

- The Build tab needs a **repo path**, persisted in `ToLissPhoton_dev.txt` and seeded from
  `$PHOTON_REPO`. Commands run on a worker thread — a synchronous `std::system` in a draw
  callback freezes the simulator.
- ⚠ **No reload-aircraft button.** That command runs synchronously and unloads the
  aircraft's plugins, so from a draw callback (or a flight loop) it tears the aircraft down
  mid-frame. It is a **Dev menu item**, i.e. menu-handler context.
- The worker thread touches only its own mutex-guarded buffer, **never an XPLM call** —
  XPLM is not thread-safe.
- **The Lights tab is a second implementation of the Python tool's Live Light Debug**
  (2026-08-01): same datarefs, same wire order, same manifest. ⚠ **Only one of the two may
  own the pool.** `PI_PhotonDevReload.py`'s accessors are read-only computed values, so
  writing through the other's is impossible, not merely rude. **Both sides now check first
  and stand down**, the loser showing a banner where its knobs are; both log which way it
  went, because plugin start order is not defined by the SDK. Delete
  `PI_PhotonDevReload.py` (or deploy a non-dev `.xpl`) to pick a side permanently. The one
  deliberate parity gap is the 48-permutation orientation cycler — it searched for a
  hypothesis that turned out to be wrong, since a four-slot rotation is not an axis
  permutation. `tests/test_light_editor.py` pins the constants the two share;
  `test_dev_light_debug.py` pins the Python stand-down.
- ⚠ **THE LIGHTS TAB HAS A READ-ONLY MODE, and which mode you get is decided by the
  INSTALLED OBJ** (2026-08-10). A plain build of the interior/screens targets now drops
  `lights_inn.info.json` / `lights_screens.info.json` beside the normal OBJ; a `--debug`
  build drops `*.debug.json` instead. **Each build deletes the other's file**, so exactly
  one exists at a time and the tab decides by which one it found — tunable, or the same
  tree and numbers disabled. The fallback is **per target** (`haveDebug[c]`), so an
  interior built `--debug` can sit beside stock screens.
  - **`DbgLight::readOnly` is per LIGHT, not per session.** `gDbgReadOnly` is the
    separate "nothing here is tunable" flag, and all it does is hide the toolbar of
    things that only mean something to a debug OBJ (Save tuning, Isolate, Mark, Blink,
    the A/B Tunable switch, the markers, the slot probe) plus the tuning-file row. Hidden
    rather than disabled, deliberately.
  - ⚠ **`variants[i]` is indexed by the CATEGORY DATAREF'S VALUE** — the same contract
    `build_objs.branch_gate` emits the OBJ's `ANIM_hide`s from — because a shipping OBJ
    bakes each lamp once per look and only one branch draws. Never match on the profile
    NAME; it is carried for display only. `DbgApplyVariants` runs every frame from the
    tab (the era can change from the Cockpit tab while it is open) and keeps `base[]`
    equal to `v[]`, so a branch change can never read as an EDIT.
  - ⚠ **A read-only session never touches the tuning file** — `DbgLoadManifest` skips
    `DbgLoadTuning`, `DbgLightChanged` returns false, `DbgApplyTuningRecord` skips
    read-only lights (the mixed case). Those records are edits against a *debug* OBJ;
    applied over an installable one they would show numbers nothing is rendering.
  - **Info rows carry the same slot numbers as debug rows** (same `DEBUG_SLOT_BASE`, same
    order), so `slot 12` is one light whichever build is installed.
  - The Settings tab's flood-boost slider checks `gDbgReadOnly` **before** the `boosted`
    count: an info manifest carries the boost rows too, so the count alone is satisfied
    and the slider would appear over an OBJ that bakes the factor.
- **The Lights tab MERGES both debug manifests** (2026-08-09): the two targets live on
  disjoint slot windows (`DEBUG_SLOT_BASE` — interior 0, screens 48..53), so cockpit and
  screens lights tune side by side. Per-manifest facts (`pos_tunable`, `boost`) are
  per-LIGHT fields on `DbgLight` now, not globals. The Python tool still drives one
  manifest at a time and translates its slot base at the dataref boundary
  (`dbgSlotBase`).
- **"Edit all screens together"** (`gDbgScreensLink`, default on): with a screens light
  selected, every shared attribute — v[0..8] plus pitch/yaw, i.e. everything but
  position — is copied onto the other five AFTER the frame's widgets run. Untick to
  tune one screen alone.
- **Save tuning** (Lights tab toolbar + the Settings tab's dev section) writes every light
  whose values differ from its baked seeds, in .phdsl shape, plus the `boost:` factor,
  to `<repo>/.scratch/light_tuning.txt` (repo path from the Build tab, which
  `deploy.ps1 -Dev` seeds once; falls back to
  `Output/preferences/ToLissPhoton_tuning.txt`). `Log DSL` remains the one-light form.
- ⚠ **THE TUNING FILE IS ALSO AUTO-SAVED AND READ BACK** (2026-08-09). Until then a
  session lived only in memory, and closing X-Plane, reloading the aircraft or
  deploying a new build discarded it with no file, no log line and nothing in git —
  which is exactly how an afternoon of screen-glow color work was lost. So:
  `DbgAutoSaveTuning()` runs from `XPluginDisable` **and** from the aircraft-load path
  **before** `DbgLoadManifest()` reseeds the lights; `DbgLoadTuning()` runs at the END
  of `DbgLoadManifest` (a saved session is EDITS, so the baked values must be in place
  as its seeds first). `TuningPersistenceTests` pins every one of those orderings.
  - **Records are keyed on FIXTURE + NAME, never the slot.** Slots shift when the DSL
    gains or loses a light, and the names repeat (`int_panelflood#a` is two lamps).
  - **Each record carries `from:`, the seed it was tuned against, and a restore SKIPS
    any light whose OBJ no longer bakes that seed** (same for `boost_from:`). Without
    it, editing the .phdsl and rebuilding would appear to do nothing — the saved
    session would keep overwriting the new authored values.
  - **Auto-save writes nothing when nothing was touched**, so loading an aircraft
    cannot blank a previous session's file.
  - The Lights tab shows the resolved path and a **Forget saved** button; delete the
    file once a session has been transcribed into the .phdsl, or it keeps being
    restored over a rebuild.
