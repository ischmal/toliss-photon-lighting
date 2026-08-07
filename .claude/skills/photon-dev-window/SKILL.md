---
name: photon-dev-window
description: The PHOTON_DEV-only Dev window in ToLissPhoton.xpl — its tabs (Exterior, Cockpit, Displays, Perf, Panel FX, Lights, History, Probe, Build, Log, ImGui), the Build tab's worker thread, and the Lights-tab / PI_PhotonDevReload.py pool-ownership standoff. Load before editing Dev-window UI, the Build tab, or the light-editor pool.
---

# The Dev window

`PHOTON_DEV` builds add **Plugins ▸ ToLiss Photon ▸ Dev**, whose one window holds every
knob, tabbed: **Exterior** / **Cockpit** / **Displays** / **Perf** (the shipping settings
panes) ·
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
Cockpit / Displays / About, `BuildMainUi`), reachable from the ordinary menu in every build,
plus a separate shipping **Performance window** opened from a button at the bottom of the
Displays tab. The Dev window's first four tabs call **the same builders** —
`BuildConfigTab(kExtAxis)`, `BuildConfigTab(kIntAxis)`, `BuildDisplaysTab`,
`BuildPerfTab(true)` — deliberately, so a tuning pass never has to wonder whether a
duplicated copy has drifted. Add a control to the shipping pane and it appears in both; add
one *here* and it will not.

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
