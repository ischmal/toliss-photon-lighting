---
name: screen-glow
description: The six LIGHT_SPILL_CUSTOM screen-glow lights (target `screens`, objects/lights_screens.obj) and their .acf attachment, now part of the base A3xx install. Load before touching the screens DSL fixtures, core/patch_acf_screens.cpp, kScreenCount/kScreenDU/kSpillScreenPrefix, or kScreenGain.
---

# The six screen-glow lights

Six `LIGHT_SPILL_CUSTOM` lights, one per display unit, in their own OBJ
(`objects/lights_screens.obj`, target `screens`). Full spec: `docs/screens_plan.md`.

⚠ **A339 IS COVERED (2026-08-11)**, unlike the interior — it needs only six positions and
`AirbusFBW/DUBrightness`, both of which it has. **Position is the only thing that differs**:
color, alpha, size, aim and spread live in the shared `screen_glow` light type, and a test
fails if anything else diverges. Its numbers are its own DU face centers (from
`derive_panel_rects.py`) plus the A3xx standoff — **seeds, never seen in-sim**.
⚠ **The payload is per airframe** (`payload/objs/screens/<airframe>/`) because every
airframe writes the same *filename*: one shared copy installs the A320's positions into an
A330 and every light draws, in the wrong place, with nothing on disk to show for it.

⚠ **IT SHIPS (2026-08-01)** — part of the BASE install (`SCREENS_AIRFRAMES`), not an
opt-in. It works on a stock cockpit, so it does not sit behind the interior's opt-in; whether
the glow is visible is a runtime choice on the plugin's Displays tab. Much of
`screens_plan.md` still reads "experiment"; the mechanisms are right, the status is not.

**Optionality is still structural, and that is what makes the install reversible.** ToLiss
neither ships nor references this OBJ, so it is inert on disk until
`core/patch_acf_screens.cpp` adds it to the `.acf` `_obja/*` table, as part of the
installer's base install on A3xx — or on its own via
`photon-installer screens --aircraft <dir> [--detach]`.

- **The OBJ goes in the manifest's `screens.added[]`, never `backed_up[]`** — no stock
  counterpart, so uninstall must DELETE it. ⚠ Uninstall DETACHES BEFORE DELETING: an `.acf`
  naming an OBJ that is gone warns on every load.
- ⚠ **`make_release.py` emits it from the Emitter, never from `dist/`** — `dist/` may hold a
  `--debug` build, which binds every light to `ToLissPhoton/debug/light/*` and is not
  installable. On a dev machine that is the normal state of the file in the sim.
- ⚠ **A ToLiss aircraft update reverts the attachment** (seen on A320 1.3.2 → 1.3.3).
  Reinstalling restores it; nothing special to remember, since the attach is base install.
- ⚠ **THE DU KNOB DRIVES SIZE, NOT ALPHA** (2026-08-09). A screen is an area source: up
  spreads its wash further, it does not make a fixed pool more opaque. So the plugin writes
  slot **4** (`kSpillSizeSlot`) and `alpha:` in the DSL is a CONSTANT it never touches — the
  authored intensity, drawn identically plugin-present or absent. `size:` is the reach at a
  screen turned fully up. Slots 3 and 4 are both index-stable across the two custom-light
  orderings, which is what makes driving either safe; the map spot (`ReadSpillMap`) still
  drives ALPHA, and swapping the two looks plausible in a screenshot and wrong in motion.
- **The authored size is CAPTURED from the pre-filled buffer on first read**
  (`gScreenBakedSize`), then assigned — never multiplied in place (it would compound to zero
  if X-Plane fed our output back) and never duplicated as a constant (the .phdsl stays the
  only place a size is written). ⚠ **The capture is dropped on aircraft load**, or a rebuilt
  OBJ is scaled against the previous build's reach.
- **The user's own multiplier is `gDisplays.spill`/`spillGain`**, applied AFTER
  `BrightnessResponse` and after `kScreenGain`. Off writes 0 into `gScreenSizeFactor` rather
  than skipping the loop — a stale factor would leave the lights lit at whatever they last were.
- **The dev tuner drives `source: "du"` lights on size too**, so a tuning pass on a debug OBJ
  judges the look the release produces. Isolate/Blink stay on alpha for every light.

- **The attachment row is COPIED from a known-good row in the SAME FILE**, never written
  from constants — that row carries the OBJ origin in feet against a per-airframe datum
  (26.00/20.75/6.78 ft on A3xx), and XP11 files use a *different field set* than XP12
  (`_v10_att_part` vs body/gear/wing, floats `0.0` vs `0.000000000`). No candidate row →
  the patcher **refuses** rather than guessing.
  ⚠ **Which row is a LIST, matched by content** (`ScreensFrameTemplates`, `core/constants.cpp`,
  mirrored in `build/constants.py` and pinned by `tests/test_version.py`): `lights_inn.obj`
  for the A3xx, then `CockpitLighting_XP12.obj` and `CockpitLighting.obj` for the A330-900,
  which has no `lights_inn.obj` anywhere. **Both A330 spellings are listed on purpose** —
  the patcher runs on all four `.acf` variants, and omitting the XP11 name leaves two
  refusal lines on an airframe where nothing is wrong. First match wins, so order is a
  decision: the two frames are different datums.
- **`is_attached()` requires the row to be inside `_obja/count`** — a row at index ≥ count
  looks installed to a grep and draws nothing.
- **Detach renumbers rows above ours down**, never leaving a hole.
- **Detection is by our FILENAME**, never index, never a hash.
- **No light here is gated on a LOOK** — there is one look, so nothing to switch, and the
  per-frame level rides the spill dataref's SIZE slot.
- ⚠ **But the whole OBJ sits inside ONE master `ANIM_hide`** (2026-08-04) on
  `ToLissPhoton/screens/disabled`, from `TARGET_MASTER_GATE` in `build_objs.py`. With
  *backlight illumination* off the renderer skips the block instead of setting up six spill
  lights and calling six 9-float accessors: a zero driven slot made them invisible, never
  free. The
  dataref is **derived** from `DisplayFeatureOn(gDisplays.spill)`, never stored. ⚠ It is
  named for the **OFF** state because a missing dataref reads 0 forever, so 0 must mean
  *draw* — `.../enabled` would make an older plugin look like a failed `.acf` attach. ⚠
  **Both gates stay**: the block answers the user's switch, the per-frame size factor follows
  the DU knobs and the DC bus. Not emitted in a `--debug` build.
- **Three places must agree** or a light binds the wrong screen, invisibly: the six
  `spill_dref:`/`index:` pairs in the DSL, `kScreenCount`/`kSpillScreenPrefix`/`kScreenDU`
  in `plugin.cpp`, and `source: "du"` in the dev plugin. The two numbers per fixture are
  **different**: `spill_dref` is the light's own slot, `index:` its `DUBrightness` element.
- **`AirbusFBW/DUBrightness` is NOT left-to-right.** Settled in-sim 2026-07-29: it is the
  Airbus DU numbering, so `kScreenDU = {0, 1, 4, 5, 3, 2}`.
- **`kScreenGain` is 1.0, not a headroom knob** — the dev tool drives the same multiplier,
  so 0.85 shipped an approved tuning 15% short. Tone down with `size`, `alpha` or palette.
- **`DUBrightness` reaches the driven slot through `BrightnessResponse`, not linearly** —
  the shared `kBrightnessCurve` (see the `panel-fx` skill), unconditionally on this side.
- **Baked `alpha` is `1` and is the SHIPPING intensity** (retuned 2026-08-10). It was 0.15
  while alpha was the driven slot and that number was only ever the degraded, plugin-absent
  fallback; once size took over the dimming, 0.15 became the intensity at every knob
  position and shipped the approved look ~85% dim. ⚠ It still must never be 0 — an
  invisible light reads in-sim as a failed `.acf` attachment. With the plugin absent the
  glow is now full-intensity at full reach and ignores the knobs, which is loud but
  unmistakably *installed*.
- **Both debug OBJs can be live at once** (2026-08-09) — slots come from per-target
  bases (`DEBUG_SLOT_BASE`: interior 0, screens 48..53), the native Lights tab merges
  both manifests, and `deploy.ps1 -Dev -DebugObjs` installs them in one command. The
  Python dev tool still drives one manifest at a time (newest wins there).
