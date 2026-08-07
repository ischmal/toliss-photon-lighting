---
name: screen-glow
description: The six LIGHT_SPILL_CUSTOM screen-glow lights (target `screens`, objects/lights_screens.obj) and their .acf attachment, now part of the base A3xx install. Load before touching the screens DSL fixtures, patch_acf_screens.py, kScreenCount/kScreenDU/kSpillScreenPrefix, or kScreenGain.
---

# The six screen-glow lights

Six `LIGHT_SPILL_CUSTOM` lights, one per display unit, in their own OBJ
(`objects/lights_screens.obj`, target `screens`). Full spec: `docs/screens_plan.md`.

⚠ **IT SHIPS (2026-08-01)** — part of the BASE install on A3xx (`SCREENS_AIRFRAMES`), not an
opt-in. It works on a stock cockpit, so it does not sit behind the interior's opt-in; whether
the glow is visible is a runtime choice on the plugin's Displays tab. Much of
`screens_plan.md` still reads "experiment"; the mechanisms are right, the status is not.

**Optionality is still structural, and that is what makes the install reversible.** ToLiss
neither ships nor references this OBJ, so it is inert on disk until
`build/patch_acf_screens.py` adds it to the `.acf` `_obja/*` table.

- **The OBJ goes in the manifest's `screens.added[]`, never `backed_up[]`** — no stock
  counterpart, so uninstall must DELETE it. ⚠ Uninstall DETACHES BEFORE DELETING: an `.acf`
  naming an OBJ that is gone warns on every load.
- ⚠ **`make_release.py` emits it from the Emitter, never from `dist/`** — `dist/` may hold a
  `--debug` build, which binds every light to `ToLissPhoton/debug/light/*` and is not
  installable. On a dev machine that is the normal state of the file in the sim.
- ⚠ **A ToLiss aircraft update reverts the attachment** (seen on A320 1.3.2 → 1.3.3).
  Reinstalling restores it; nothing special to remember, since the attach is base install.
- **The user's own multiplier is `gDisplays.spill`/`spillGain`**, applied AFTER
  `BrightnessResponse` and after `kScreenGain`. Off writes 0 into `gScreenAlpha` rather than
  skipping the loop — a stale alpha would leave the lights lit at whatever they last were.

- **The attachment row is COPIED from the aircraft's own `lights_inn.obj` row**, never
  written from constants — that row carries the OBJ origin in feet against a per-airframe
  datum (26.00/20.75/6.78 ft on A3xx), and XP11 files use a *different field set* than XP12.
  No `lights_inn.obj` row → the patcher **refuses** rather than guessing.
- **`is_attached()` requires the row to be inside `_obja/count`** — a row at index ≥ count
  looks installed to a grep and draws nothing.
- **Detach renumbers rows above ours down**, never leaving a hole.
- **Detection is by our FILENAME**, never index, never a hash.
- **No light here is gated on a LOOK** — there is one look, so nothing to switch, and
  brightness rides the spill alpha.
- ⚠ **But the whole OBJ sits inside ONE master `ANIM_hide`** (2026-08-04) on
  `ToLissPhoton/screens/disabled`, from `TARGET_MASTER_GATE` in `build_objs.py`. With
  *backlight illumination* off the renderer skips the block instead of setting up six spill
  lights and calling six 9-float accessors: alpha 0 made them invisible, never free. The
  dataref is **derived** from `DisplayFeatureOn(gDisplays.spill)`, never stored. ⚠ It is
  named for the **OFF** state because a missing dataref reads 0 forever, so 0 must mean
  *draw* — `.../enabled` would make an older plugin look like a failed `.acf` attach. ⚠
  **Both gates stay**: the block answers the user's switch, the per-frame alpha follows the
  DU knobs and the DC bus. Not emitted in a `--debug` build.
- **Three places must agree** or a light binds the wrong screen, invisibly: the six
  `spill_dref:`/`index:` pairs in the DSL, `kScreenCount`/`kSpillScreenPrefix`/`kScreenDU`
  in `plugin.cpp`, and `source: "du"` in the dev plugin. The two numbers per fixture are
  **different**: `spill_dref` is the light's own slot, `index:` its `DUBrightness` element.
- **`AirbusFBW/DUBrightness` is NOT left-to-right.** Settled in-sim 2026-07-29: it is the
  Airbus DU numbering, so `kScreenDU = {0, 1, 4, 5, 3, 2}`.
- **`kScreenGain` is 1.0, not a headroom knob** — the dev tool's Alpha row is the same
  multiplier, so 0.85 shipped an approved tuning 15% dim. Tone down with `size` or palette.
- **`DUBrightness` reaches alpha through `BrightnessResponse`, not linearly** — the shared
  `kBrightnessCurve` (see the `panel-fx` skill), unconditionally on this side.
- **Baked `alpha` stays low (0.15)** — with the plugin absent X-Plane draws the baked params
  unmodified, so the degraded state must be a faint glow, never a glare.
- **Only one debug OBJ at a time** — both `.debug.json` files number slots from 0.
