---
name: panel-fx
description: The A3xx panel rect table (kPanelRectsHi/kPanelRectsLo) and the SHIPPED FX layer compositor (its editor stays dev-only) — rect derivation and naming, containment groups, blend modes, brightness/power drivers, overlay images, and the ambient day/night blend. Load before touching the rect table, derive_panel_rects.py, FxStack/FxLayer/FxDriverFactor/DrawFxLayer, FxQuadColorFor, FxLayerAtAmbient, kPanelSources, or kBrightnessCurve.
---

# The panel rect table and the FX compositor

⚠ **THE COMPOSITOR SHIPS; ITS EDITOR DOES NOT** (2026-08-01). Compiled unconditionally: the
rect table, target/group resolution, the GL entry points, the blend save/restore, the
texture pool, the drivers, and `DrawPanelFx` itself. Still behind `#if PHOTON_DEV`: the
panel PROBE, the Panel FX tab and its history, and the live light editor. Both mistakes are
SILENT and opposite — a compositor left inside the guard runs fine on the dev machine and
does nothing in a release; a probe left outside it floods a release's cockpit magenta — so
`ShippingBuildTests` in `tests/test_panel_fx.py` reads the guard nesting and pins each side.

- **`panelfx.txt` lives in the PLUGIN FOLDER**, not `Output/preferences`, and the installer
  puts it there (`payload/plugindata/`). ⚠ **A shipping build never writes it** —
  `SavePanelFx` is dev-only. User choices go to `ToLissPhoton_profiles.json` under
  `"$displays"`. On a dev machine the file is a **symlink into `src/native/panelfx.txt`**,
  which `deploy.ps1` maintains and the installer skips; ⚠ it needs `pwsh`, because Windows
  PowerShell 5.1 falls back to a copy and the round trip to the repo ends silently.
- **The user-facing controls are the settings window's Displays tab** — one switch per
  effect plus a strength. ⚠ The screen/other split is decided **per RECT**
  (`FxDisplayUserFactor` → `PanelRectIsScreen`), never per stack: a group target can span
  both, and a per-stack test would put the whole group on its first member's switch. The
  user factor multiplies into `FxRectFactor`, so 0 means "draw nothing at all".
- **"Screen" is `du >= 0` PLUS `kScreenFxExtraRects`** (both MCDUs, both DCDUs, the ISIS) —
  backlit screens with no `DUBrightness` index, and a "backlight visual effect" switch that
  skipped them reads as half-working. ⚠ **Named by rect NAME, not index**, which is this table's
  house rule (`RectIndexByName`): `panelfx.txt` already names its targets, so the name is
  the wire identity and an index literal would survive a rename and silently re-aim the
  switch. A miss logs and falls through to `otherFx`; `ScreenFxRectTests` pins membership.
- **One draw callback serves both features.** `PanelPassDraw` runs the compositor before its
  `#if`; a release also registers only the after-Gauges slot.

`kPanelRectsHi`/`kPanelRectsLo` map every readout face on the A3xx cockpit panel to its UV
rect in the 4096 panel atlas — the six DUs, the FCU row, ISIS, the two DCDUs, the clock's
three windows, the two DME windows, the pedestal strips, the two EFB tablets, the two
MCDUs, the six RMP windows, the transponder code, the rudder trim, the two BAT
voltmeters, the FMS load window and two still-unidentified aft-overhead readouts. Derived
with `build/derive_panel_rects.py`; **byte-identical on A319/A320/A321**, so the table is
A3xx-wide.

- **The complete list came from ONE TRIS BATCH, not from a geometry filter** (2026-08-01).
  ToLiss draws every readout face in the cockpit from a single batch (line 54323 on all
  three A3xx, the one the FCU strip is in), so "every island in that batch" is a closed set
  of 22 and no size or angle threshold can drop a member of it. That is how the radios were
  found after two size-based surveys missed them.
- **Faces are named from the KNOBS AROUND THEM.** Each panel's manipulators carry command
  names (`AirbusFBW/RMP2LargeKnob`, `AirbusFBW/XPDRPower`), so the nearest manipulator names
  the panel. ⚠ **Sum the enclosing `ANIM_trans` first** — a knob inside an ANIM has local
  coordinates and reads as sitting at the origin, which makes every knob in the cockpit look
  equidistant. Five faces started positionally named (`ovhd *`) because nothing is within
  40 cm of them; **three were confirmed with the `?` button in-sim** (2026-08-02) and are now
  `BAT1`, `BAT2` and `FMS Load`, and `ovhd aft L1`/`L2` still are not. Confirm with `?` and
  rename, never guess in the name field. ⚠ A rect or group NAME is wire identity —
  `panelfx.txt` targets by name — so a rename means updating that file in the same change;
  `ShippedTargetNameTests` fails if a target no longer resolves.
- ⚠ **The containment mask is `PanelMask` = `std::uint64_t`, and that is load-bearing.** It
  was `unsigned` at 23 rects; 36 rects makes `1u << 35` undefined behavior that wraps, so
  rects past the 32nd alias the first four and the derived tree quietly rearranges itself.
  A `static_assert` and `tests/test_panel_rects.py` hold the line at 64.

- ⚠ **Two survey traps, both of which hid the MCDUs for a whole pass** (`fcu_tint_plan.md`
  §6b). `derive_panel_rects.py` used to report each island's **axis-aligned x/y extents**,
  so a screen lying 8° off horizontal measured "11.4 × 1.4 cm" and read as a label strip;
  and `is_readout` required `n[2] > 0.2`, which drops any near-horizontal face — i.e. the
  whole pedestal. Both are fixed (`in_plane_size`, and `n[1] > 0.5` as an alternative), and
  the A320 survey went 51 → 74 candidates. **A chain of correct negative checks around a
  bad measurement gives a confident wrong answer; the in-sim flood probe settles in one
  look what the geometry argues for a page.**
- **The MCDUs sit directly above the ISIS**, one island per side (so tintable separately),
  ungated, not resolution-switched. `v0` is trimmed 2822.0 → 2824.0 because the ISIS island
  ends at 2823.7 and the two overlap by ~1.7 px. UV is unrotated: `u` with +x, `v` with −z.
- ⚠ **A `GATED` island may be in a LOCAL coordinate frame** — the EFB faces sit inside two
  `ANIM` levels and all read x≈0, z≈−0.1, four meters aft of the panel. Side and position
  come from the enclosing `ANIM_trans`, never from the survey's position column. This
  nearly filed both tablets as cabin geometry.
- **Each EFB tablet has two faces** (`AirbusFBW/EFBShowsAvitab[n]`: ToLiss's EFB at 0,
  AviTab at 1), only one drawn at a time — but **both tablets share the AviTab island**, so
  it cannot be tinted per side, exactly like `ped strips`.

- ⚠ **APPEND ONLY.** The index is persisted and is the value of
  `ToLissPhoton/debug/panel_probe_rect`; inserting a row silently re-aims a saved target.
- ⚠ **Names below the six DUs are inferred from geometry, not confirmed in-sim.** ToLiss
  labels none of these faces. Confirm with the FX target tree's `?` button (floods it
  magenta), then fix the name here rather than working around it.
- **Only the six DUs are resolution-switched** (`AirbusFBW/DisplayResolutionFallback`);
  every other row is identical in both tables. A `static_assert` pins the two to the same
  length. ⚠ **That asymmetry is also a DIAGNOSTIC**: a fault that hits the six DUs and
  nothing else is almost certainly `gDisplayFallbackRef`, not the rect table. It was
  resolved once in `XPluginEnable` and never rebound, so after an aircraft reload it pointed
  into an unloaded ToLiss plugin — the third instance of the cached-handle tripwire, and the
  best-hidden, because *"the FX works on the MCDUs but not the PFDs"* reads like bad UVs.
  It is now dropped/rebound in `UpdateMenuVisibility` and retried in `AutoLoop`; null is the
  safe state (`PanelRects` falls back to full-res).
- **The four pedestal faces share ONE atlas island**, so they cannot be tinted separately
  at all — one rect is the truth, not a simplification.
- **Groups nest by CONTAINMENT, derived from the rect sets, never declared.** Sibling groups
  must be disjoint or nested — a partially overlapping group ("everything on the captain's
  side") is fine for the compositor but has no single place in a tree.
- **Stacks composite BROADEST FIRST**, not in creation order. That is what makes a group a
  base and a member a refinement.
- **Blend modes are limited to what fixed-function GL expresses** — `src·sf OP dst·df`.
  Overlay, Soft/Hard Light, Color Burn/Dodge and the HSL modes need `dst` as a shader
  *input* and are impossible here. Don't ship an approximation under a real mode's name.
- **Every mode fades to a no-op at opacity 0**, which means fading toward *that mode's*
  identity. Six carry it as `color × opacity` (identity black); **Multiply carries it in
  the blend func** (`DST_COLOR, ONE_MINUS_SRC_ALPHA`, quad `(C×o, o)`), which is the same
  arithmetic per *pixel* and is what lets it take an image. **Darken** is the one still
  fading in the color, because `GL_MIN` ignores the blend factors.
- ⚠ **The blend EQUATION is saved and restored like the func.** A leaked `GL_MIN` turns
  every later blended draw in the pass into a darkening op — a whole-cockpit artifact.

**Brightness and power drivers** (§8d, added 2026-08-01): a target's layers can FOLLOW a
dataref, so the tint dims with the knob the pilot turns instead of glowing on its own over
a screen dimmed to black.

- **Two drivers per target**: `bright` scales every layer's opacity (`lo`..`hi` → 0..1, with
  a floor), `power` gates the target off below a threshold. `gFxFollowBrightness` is the
  master switch, on by default and off while picking a color.
- **Both switches are per-target tri-states** (`FxStack::follow`/`curve`, `kFxInherit` = −1
  default, wire keys `tfollow`/`tcurve`, absence = inherit). ⚠ **Inherit keeps tracking the
  master** — it is not a copy, because the master is a tuning escape hatch. ⚠ **`follow =
  Off` still power-gates**: it means "full strength *when powered*", so `FxRectFactor`
  evaluates power before consulting `follow`. The master, when off, outranks every target
  (nothing dims, nothing gates). ⚠ **`FxDriverFactor` takes the curve flag as a parameter**,
  never reading `gFxCurve` — two stacks can share a driver and want different shapes.
- **The lookup is PER RECT, not per target** — `PaintPanelTarget` now hands the paint
  function the rect index — so a group follows *each member's own* knob: "Main displays"
  tracks six DU rheostats, not one. A driver set on the stack overrides that for the whole
  target.
- **The normalized value then goes through `kBrightnessCurve`** — knots
  `(0,0) (0.2,0) (0.5,0.2) (1,1)`, so the bottom fifth of the knob is dead and half a turn
  is still dim. **The six screen-glow spill lights use the same curve**, which is the point
  of sharing it: a tint and the spill under it come up together, and nothing in-sim would
  flag them drifting apart. Order in `FxDriverFactor` is `lo`/`hi` → curve → floor.
  ⚠ **Interpolation is MONOTONE cubic (Fritsch–Carlson).** Plain Catmull-Rom undershoots to
  ≈ −0.012 across the dead zone (the middle knot's tangent pulls the flat first segment
  down), and a clamped undershoot is a visible step mid-fade — the exact artifact the curve
  exists to remove; linear has a 4× slope break at the middle knot and pops. **The knots are
  the spec** — nothing derives an exponent or dead-zone constant from them. The tab's
  `Curve` switch is dev tuning only and **does not reach the spill lights**.
- **`FxQuadColorFor(layer, o)` is split out of `DrawFxLayer` for this.** At opacity 0 each
  blend mode must reach *its own* identity (white for Multiply and Darken, black for the
  rest), so a dimmed quad is the color recomputed at the scaled opacity, never a fade
  toward black. The func and equation don't vary with opacity, which keeps the state change
  outside the loop.
- ⚠ **The built-in `kPanelSources` names are read off ToLiss's plugin binary, not measured.**
  Only the six `DUBrightness` indices are settled (they are `kScreenDU`'s). Every row is
  editable in the tab, which shows the live value and the factor beside it — that display is
  the point, because a name alone cannot tell you whether the dataref means what you hoped.
- **Integral lighting follows THE PANEL A FACE SITS IN, not the unit it belongs to**
  (2026-08-02, user). `AirbusFBW/PanelBrightnessLevel` for the main panel and the pedestal —
  clock, DME, RMP1/2, transponder, rudder trim, `ped strips`; `AirbusFBW/OHPBrightnessLevel`
  for the overhead — the BAT windows, `FMS Load`, the two `ovhd aft` and **RMP3, which is on
  the overhead despite being an RMP**. The per-unit `RMP<n>Lights` and the pedestal FLOOD
  lamp were the earlier readings and are the shape of the mistake: a name that resolves
  perfectly and reports a different quantity. `RMP<n>Available` stays the power gate —
  availability really is per unit. The two BAT voltmeters gate on their own battery
  (`AirbusFBW/BatVolts[0]`/`[1] > 3`): a flat battery leaves that window blank, not dim.
- ⚠ **A driver reads via `FxReadScalar`, which prefers the WIDEST advertised type**
  (double → float → int), unlike `ReadDataRefTyped`, which tries int first and is right to —
  its callers read flags. A dataref carrying both an int and a float reader, read as int,
  **truncates a 0..1 knob to 0** for its whole travel bar the top; the symptom is a tint that
  never appears, with no error and a healthy-looking value in any inspector.
- **A zero factor names its cause** — the pane shows `x0.00 power` vs `x0.00 bright`, and
  every driver's live value beside its name. Showing only names cost a session: a driver that
  binds fine and reads the wrong quantity looks exactly like one that never bound.
- ⚠ **Handles are dropped on every aircraft load** (`FxForgetDrivers`). They point into
  ToLiss's plugin, which is unloaded with the aircraft; keeping one across the swap is the
  one way this could take the sim down rather than merely look wrong.
- **A missing or unreadable driver means factor 1, never 0.** "Everything went black and the
  log says nothing" is a far worse failure than an undimmed tint.

**Three electrical gates, not one** (`FxRectFactor`; any one failing is a hard zero):

- `power` — the face's own availability source, **overridable by a stack** because it is
  part of the authored look. ⚠ **`AirbusFBW/XPDRPower` is not one of these** (removed
  2026-08-02): it is the STBY/AUTO/ON selector, i.e. whether the transponder is
  *interrogating*, and the code window stays lit in STBY — so the tint blanked on a face in
  plain sight. `test_the_transponder_has_no_power_source` keeps it out. Same shape as
  `RMP<n>Lights` and `PedestalFloodBrightnessLevel`: **this table's characteristic failure
  is a name that resolves, reads a plausible number, and answers the question next door.**
- **bus** — `AirbusFBW/DCBusVoltages[0] > 24` on every DRIVEN readout (`kPanelBuses`): the
  six DUs, both DCDUs, the transponder, the rudder trim, `FMS Load` and `ovhd aft L1`/`L2`.
  The test is "does this face go DARK with the bus", not "is it a screen" — an integral-lit
  legend keeps its tint, because the tint is then a property of the lighting.
  ⚠ **The same threshold zeroes the six SPILL lights** in `EngineLoop`: a screen that stops
  glowing into the cockpit while its tint stays painted is worse than neither being gated.
  ⚠ ToLiss's `DUBrightness` does NOT fall with the bus — it is the knob — so this cannot be
  folded into `BrightnessResponse`.
- **breaker** — `AirbusFBW/CBArray[n] > 0.5`, optional, `kPanelBreakers`. ⚠ **The table is
  DELIBERATELY EMPTY**: 363 elements and nothing names them (not the OBJ, not ToLiss's
  binary, not the manuals), so a row is only legitimate once confirmed with **Dev ▸ Probe ▸
  CB watch** — arm, pull the breaker, read the index. A plausible guess looks right until
  that breaker is pulled.

⚠ **Bus and breaker are NEVER a stack's to override** — they are aircraft state, not tuning.
Only the master switch escapes them, and it escapes everything. In the editor they get **a
column each**; they shared one headed `bus / cb`, which reads as one gate with two numbers,
and left the deliberately-empty breaker column looking like a quirk of the bus beside it.

**DCDU brightness is INFERRED** from `AirbusFBW/CPDLC{1,2}/Brightness{Up,Down}` and
published as `ToLissPhoton/screens/dcdu_brightness` (float[2]). ⚠ The handler **returns 1**
(returning 0 eats the command, so ToLiss never dims and the tint tracks a level the screen
never reached) and steps on **`xplm_CommandBegin` only** (Continue would run the range in a
fraction of a second while held). `kDcduLevelMax`/`kDcduLevelDefault` are assumptions;
`kPanelSources` takes `hi` from the constant rather than repeating it.

⚠ **`inBefore = 1`, and only because the handler returns 1.** It was 0 — "a monitor should
stay out of the way" — and that is wrong: a *before* handler returning 0 stops the command
dead and later handlers **are never called**, so whether we heard a press rested on ToLiss's
return value. Running first removes a dependency on someone else's plugin internals.

⚠ **The four commands are RE-tried at 1 Hz** (`RetryDcduCommands` from `AutoLoop`). At
aircraft-load time `XPLMFindCommand` finds **none of them** on a stock install — ToLiss's
plugin starts after our hook. Retry binds null slots only (double registration = every press
counted twice) and never touches `gDcduLevel` (that reset belongs to the load).

⚠ **Anything cached at aircraft-load time needs a line in that retry list.** The FX drivers
resolve **lazily on first draw** and are immune by construction — which is exactly why they
looked like proof the datarefs were fine while the bus gate and then the DCDU commands were
both silently unbound for a whole session.

⚠ **`FxForgetDrivers()` and `BindDcduCommands()` run OUTSIDE `#if PHOTON_DEV`** on the
aircraft-load path. The first regressed once: the compositor shipped while its call site
stayed behind the guard, so a release held ToLiss datarefs across an aircraft swap.

**Overlay images** (a layer can be an image, not just a color — §8c):

- Images live in `Resources/plugins/ToLissPhoton/overlays/`, picked by **name**; decoded
  by vendored **stb_image** (`third_party/stb/` — the SDK has no image loader).
  `deploy.ps1` seeds four starters from `build/make_overlays.py`, copied by name so the
  folder's own contents survive.
- ⚠ **Refresh is MANUAL and is ONE operation** (2026-08-04). The pool used to stat its files
  at 1 Hz *from the compositor* — a developer's inner loop, paid for by every user's render
  thread. `ScanFxTextures` already stamps and marks changed files stale, so `Rescan images`
  does adding, renaming **and** editing; shift-click forces a re-read of every image.
  ⚠ There is deliberately **no `PollFxTextureChanges`** (two paths deciding what "changed"
  means is how it grows back), and the **initial scan runs from `LoadPanelFx`**, off the
  render thread. ⚠ A reload must upload into the **same texture number**; nothing here can
  free one.
- ⚠ **Uploaded PREMULTIPLIED, and an overlay's shape lives in its ALPHA** — RGB is usually
  white, because the layer color tints. A straight-alpha PNG's bright RGB under alpha 0
  would otherwise be added or multiplied in at full strength.
- ⚠ **Burn and Darken cannot carry an image** — Burn emits `1−color` and Darken is
  `GL_MIN`, and `GL_MODULATE` expresses neither per pixel. Such a layer is **skipped** and
  the editor says so; approximating it would be a mode named "Darken" that brightens.
- **One texture unit for the whole pass**; a solid layer binds a 1×1 white texture. Toggling
  it per layer would re-enter `XPLMSetGraphicsState` inside the loop, under our own func.
- **The image name is LAST on the layer line** (names have spaces; read with `getline`,
  `-` = none), and ⚠ **tile is read into a local first** — `operator>>` zeroes on failure,
  and tile 0 samples one texel, which reads as a flat color, not as a defaulting bug.

**The ambient day/night blend** (§8e, 2026-08-02): a layer carries a night look and
optionally a **daytime** one, and the compositor interpolates between them.

- **Source: `sim/graphics/misc/cockpit_light_level_{r,g,b}`**, which is what ToLiss reads
  too (those three names are in the AirbusFBW binary). **MAX of the three**, not the mean —
  the night tint is blue-shifted, so an average reads a moonlit cockpit as too dark.
  `gFxAmbientLo`/`Hi` (0.15/0.70) are the calibration; `lo` above `hi` inverts.
- ⚠ **An INTERPOLATION, not a switch.** Dusk lasts twenty minutes and is where the effect
  is judged. **Smoothed** (`tau` 2.5 s) against the **wall clock**, because the raw value
  jumps when a cloud crosses the sun; the **first sample snaps**, or every noon departure
  fades up from night.
- **Only color and opacity vary** — never the blend mode or the image (there is no defined
  midpoint between `GL_MIN` and additive). ⚠ **That costs nothing**: `dayOpacity` 0 is a
  night-only layer and `opacity` 0 with a `dayOpacity` above it is a **day-only** one, so
  two eras with different blend modes are **two layers, not two stacks** — and they still
  composite in the authored order while both are partly present at dusk.
- ⚠ **`FxQuadColorFor` takes a blend mode and a COLOR, never an `FxLayer`.** Passing the
  layer compiles and draws the night color at every hour with only the opacity moving.
- ⚠ **`DrawFxLayer` tests the DAY-BLENDED opacity.** Testing `layer.opacity` means a
  day-only layer never draws — the feature failing in exactly its own use case.
- ⚠ **`if (!l.hasDay) return n;`** is the backward-compatibility guarantee. Every layer in
  the shipped `panelfx.txt` predates this; lerping toward the default day values would fade
  the whole look toward white by mid-morning, in a release.
- Wire: **`day <r> <g> <b> <opacity>` on its OWN LINE** under the layer (the layer line
  ends with an image *name* read by `getline`, so nothing can follow it there), plus global
  `ambient <lo> <hi> <tau>` and optional `ambientref`. Lazily resolved → **no `AutoLoop`
  retry line**; the sim's own refs are *not* dropped by `FxForgetDrivers`, the override is.

**A layer's color ramp** (§8k, 2026-08-07): `FxLayer::rampColor` is the color at a
**dark** knob and `FxLayer::color` the color at a **full** one, interpolated on the same
RAW dataref position (`FxLayerColorAtDrive`). A dim LCD is not the authored color
more faintly — it goes warm and its black stops being lifted — which is why opacity cannot
express it.

- ⚠ **The RAW knob, NOT the effect intensity** (corrected 2026-08-07): `FxDriverNorm`'s
  value, after `lo`/`hi` and **before** the curve and the floor. Fed the factor, a ramp
  moves whenever a target's curve is edited, puts its midpoint wherever that curve crosses
  0.5 instead of at half a knob, and never reaches its start color at all on a driver with
  a floor. A ramp describes the SCREEN; the factor describes the EFFECT.
  ⚠ **One read produces both** — `FxDriverNorm` → `FxShapeFactor` → `FxRectFactor` — so a
  quad's color and its opacity can never describe two frames of a moving knob.
- ⚠ **NOT gated on `follow`.** That switch is about OPACITY; this is about COLOR. A color
  correction is by definition the layer set to stay at full strength while powered (§8f)
  and is the layer most likely to want a ramp, so tying the two together would remove the
  feature from its own primary case. `FxRectFactor` gained a `driveOut` and reads the knob
  when **either** question wants it (`wantsDrive`); a layer with neither pays nothing.
- ⚠ **`DrawFxRect`'s fast path tests the ramp, not just `f >= 0.999f`.** A ramped layer
  with `follow` off sits at `f == 1` at a quarter knob; the old path would draw the
  full-brightness color and the ramp would look inert.
- ⚠ **`FxLayerColorAtDrive` takes a COLOR, not an `FxLayer`** — same rule as
  `FxQuadColorFor`, and it interpolates toward the **ambient-resolved** color so the day
  half still moves the top of the ramp.
- The master switch still outranks it (drive 1 = the authored color) and the gates still
  come first. **No daylight half and no opacity of its own** on the start color — two eras
  is two layers, and a third opacity would make a dim layer unattributable.
- Wire **`ramp <r> <g> <b>`**, own line, written only when set. ⚠ Read into a local: a
  short line read straight into the layer leaves a BLACK start color with `hasRamp` true.
- `ColorRampTests` in `tests/test_panel_fx.py`.

**The held brightness pin** (§8l, dev only, Panel FX ▸ *Brightness knob override*). While
the mouse is down inside the slider it does **two** things, neither redundant:

- **the READ pin** (`gFxDriveManual`, in `FxDriverNorm`) — every driver reports the slider's
  value, so the tint sweeps even where the dataref is read-only or unbound;
- **the WRITE** (`FxDriveTheAircraft`) — the value goes back to the aircraft's own datarefs,
  so ToLiss dims the display underneath.

The read pin alone was the first version and is not enough: a tint judged over a screen at
full brightness is judged against a picture it will never sit on. The pane reports wrote/
refused counts and **names the refusals** — that is the answer to "the tint swept but that
screen stayed lit".

- ⚠ **NOTHING IS RESTORED** (user's call, 2026-08-07). A bench instrument for a parked
  aircraft; the knobs stay where the slider left them, and a checkbox turns writes off.
- ⚠ **The pin lands after `lo`/`hi` and BEFORE the curve** — pinning the factor would draw
  a straight line through the shape the curve editor plots beside it, and it is what walks
  that editor's live marker along the curve as you drag. ⚠ **The WRITE goes back through
  `lo`/`hi` and NEVER through the curve**: the curve is Photon's model of how a screen
  responds, the knob is where the pilot put it.
- ⚠ **Brightness only** — `FxDriverPowered` is untouched. Power, bus and breaker are facts
  about the aircraft; a slider able to fake them would let a look be authored against an
  electrical state the cockpit is not in.
- ⚠ **Held, not latched, so it needs a WATCHDOG.** The pane is not drawn when the Dev window
  is hidden, so `FxExpireDriveOverride` (called from `DrawPanelFx`) drops it half a second
  after the last hold. An `else` branch alone leaves it pinned for the session with the
  control out of sight.
- Writes are **deduped on the resolved `(ref, index)`** (six DUs on one array are six
  `FxDriver` objects), **length-probed** (a write past the end is UB in ToLiss's memory),
  **widest type first** (an int writer quantizes a 0..1 knob to 0/1), cover **every** driver
  plus every stack override, and repeat **every frame** of the drag because ToLiss may write
  its own value back between two of ours.
- ⚠ `FxWriteDriverValue`/`FxDriveTheAircraft` are **wholly inside `#if PHOTON_DEV`** — a
  shipping build has no path that writes an aircraft dataref. The pinned float itself is
  declared outside the guard and written only inside it, the `gFxAmbientManual` shape.
  Never saved. `BrightnessPinTests` in `tests/test_panel_fx.py`.

**Per-layer dimming** (§8f): `follow` exists at **three** levels — master, target, layer —
each Inherit deferring to the next one out (`FxLayerFollows` → `FxStackFollows` → master).
A **color correction** is a property of the glass and should stay put while powered; a
**bleed or wash** is the screen's own light and must track the knob. ⚠ Off still never
escapes the power/bus/breaker gates. Wire key `lfollow`, own line, written only when
not Inherit.

**A layer's own response curve** (§8i, 2026-08-03): `FxLayer::curve` holds knots that
replace `kBrightnessCurve` for that layer alone — a bleed that should only show on an
already-bright screen over a correction that must be there the moment the readout is
readable is two shapes on one target, which no per-target curve can express.

- ⚠ **A layer's knots OUTRANK the target's response switch, INCLUDING "Linear"**
  (`FxShapeFor`). The editor plots the curve it edits, so anything able to override it
  silently would make that plot a lie. Per-target panes use `FxShapeForStack` and say in
  orange how many layers draw their own shape instead.
- **The shape is ONE POINTER, three states** — `nullptr` linear, `&FxBuiltInCurve()`
  built-in, `&layer.curve` authored — so no call site can express a fourth.
  `FxDriverFactor` takes it in place of the old `bool useCurve`.
- ⚠ **It NEVER reaches the spill lights** (`BrightnessResponse`, unconditional), and
  ⚠ **`follow` is still the outer question**: a layer that does not dim has no curve.
- **ONE interpolator** (`BuildMonotoneTangents`/`EvalMonotoneCurve`); the built-in pair are
  now wrappers. A custom curve reading differently through the same knots would look
  exactly like a mis-placed knot. The builder's local-extremum pass never fires for the
  built-in table, so its numbers — and the tests' independent Python implementation — are
  unchanged.
- **`FxCurveNormalize` forces the DOMAIN, not the range**: ends pinned to x 0 and 1 (a
  curve starting at 0.3 holds its first y across the bottom of every knob), y left alone
  because a wash that peaks mid-travel is a real look. `kFxCurveMaxKnots` = 12.
- **Editor** (`FxCurveEditor`, dev only): ⚠ ONE `InvisibleButton` + draw list, not a widget
  per knot — a drag must survive the pointer leaving the canvas, and holding `ActiveId`
  there is what makes the layer pane commit a drag as one history entry. Drag to move,
  click empty canvas to add, right-click a knot to remove. Draws the linear diagonal and
  the built-in curve faintly (a custom curve is only ever a deviation from one of those)
  and the LIVE knob position brightly — without that marker, "the layer never appears" and
  "the knob never leaves the dead zone" are the same picture.
- **Wire `lcurve <n> <x0> <y0> …`**, own line, written only when set. ⚠ The COUNT comes
  first and the pairs stay on ONE line: a knot per line would let a lost line become a
  different, perfectly valid shape. ⚠ Read into a local and adopted whole.
- ⚠ **ONE SHIPS** (2026-08-04): the **FCU row's Lighten layer**, five knots. It is no longer
  a dev-only capability, so a malformed line is a shipped layer silently losing its shape —
  ⚠ **the reader FALLS BACK to the target's curve on a bad count**, logging one line nobody
  reads. `LayerCurveTests` now validates the shipped lines instead of asserting there are
  none: count matches the pairs, 2..`kFxCurveMaxKnots`, and **already in canonical form**
  (ascending x, ends pinned to 0 and 1, all values 0..1) so `FxCurveNormalize` cannot load
  a different shape than the file reads as — or the next editor save would rewrite the
  shipped default with no edit having been made.

**The pixel power gate and the stencil are BOTH REMOVED** (2026-08-04, §8g/§8h — numbers
retired, not reused). Both read the panel framebuffer back; neither was ever used. The
gate shipped with no target enabling it, the stencil with no layer using it, and a better
answer was found first in both cases (the bus gate covers the one, the FCU look was
settled without a mask). `glReadPixels` is a synchronous pipeline stall, and carrying the
most expensive call in the file for a feature nobody enables is the wrong trade.

- ⚠ **`SetFxBlend` still pins the alpha half of every blend to `(GL_ZERO, GL_ONE)`, and
  that is NOT now-unmotivated.** It was the guarantee the alpha probe rested on, but it
  does not exist for the probe's sake: nothing in this compositor has any business
  changing the panel's alpha, additive would saturate it and Darken would floor it. Do
  not "clean it up".
- ⚠ **If either is wanted back, the two expensive traps are:** a color-channel mask must
  be built at the TOP of the pass or it measures our own paint and latches open a frame at
  a time; and alpha testing must go through `XPLMSetGraphicsState` (parameter four), never
  `glEnable` — X-Plane caches all seven, so enabling behind the cache leaks an alpha test
  into the sim's own drawing.

**The per-pass cost** (§8j, 2026-08-04). A **hard-coded production compositor** was
considered and rejected: `DrawFxLayer` **already is** "set texture and blend once, then loop
the rects varying only brightness", so a fork would emit the same GL sequence and add a
dev/release divergence to maintain by hand. The shipped look is **21 layers over 61 quads** —
the structure was never the cost. What was:

- ⚠ **The winding query and the quad batching are ONE change.** `glGetIntegerv` is illegal
  between `glBegin` and `glEnd`, so half of it draws **nothing** — which reads as the
  compositor not running. `PanelQuadBatch` owns both (one batch per LAYER, since blend and
  texture are per layer); `EmitRectQuad` owns neither and refuses to emit outside a batch.
  ⚠ The probe's `ClearProbeRect` is deliberately **not** batched — `glScissor`/`glClear` are
  illegal inside one, and that path's value is touching no vertices.
- ⚠ **The driver value cache is keyed on the resolved `(ref, index)`, not the `FxDriver`** —
  six rects on one bus are six objects naming `DCBusVoltages[0]`, so keying on the object
  leaves the per-rect repetition, which is most of it. ⚠ **Armed only inside `DrawPanelFx`**
  (`FxValueCacheBegin`/`End`): the Dev panes must keep reading LIVE, or the moving number
  that distinguishes a mis-reading driver from an unbound one freezes. Failed reads are
  cached too. Bonus: one electrical snapshot per pass.
- **Group members resolve to indices once** (`GroupMembers()`) — names stay the wire
  identity, only the strcmp walk is hoisted; safe because a name means the same index in
  both rect tables.
- **`FxDrawOrder` reuses its buffer but still SORTS.** Caching the order would need
  invalidating everywhere the editor touches a stack, and a stale order silently composites
  a base over its own refinement. The allocation was the cost, not the sort.
- **Measure with `kLeverDisplayFx`** before extending. Next suspect if it ever shows up is
  the 21 blend-EQUATION switches through the Vulkan→GL bridge — which no restructuring
  removes, since they are per-layer and broadest-first order forbids sorting by mode.

Pinned by `CompositorHotPathTests` in `tests/test_panel_fx.py`.

**Two ImGui house rules** (§8a, `tests/test_ui.py`):

- ⚠ **Never `ImGui::SetTooltip` — use `UiTooltip`/`UiItemTooltip`.** A tooltip auto-sizes to
  its content, a paragraph with no wrap position is ONE line however long, and ImGui
  **clips** it to the X-Plane window rather than re-flowing — so long tooltips lose their
  ends, and only near a screen edge.
- ⚠ **An item's ID is its LABEL hashed with the ID stack, so SCOPE THE ROW.** The recurring
  shape is a row helper called more than once from one pane: its widgets land in the
  caller's scope and identical labels collide. `PushID(label)` around the helper.

Full spec: `docs/fcu_tint_plan.md` §8.
