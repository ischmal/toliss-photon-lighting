# FCU Tint — recoloring the FCU readouts (EXPERIMENTAL)

The A320's FCU digital windows (SPD/MACH, HDG/TRK, ALT, V/S) render yellow-green
in ToLiss. The real unit reads distinctly more orange. This tints them without
touching ToLiss's own drawing, by compositing over the panel framebuffer after
ToLiss has drawn into it.

**Status: multiply works in-sim; the look is now a tuning job.** The mechanism is
proven — a magenta probe lands on the captain's PFD and ND exactly, on the FCU
strip it replaces the digits **in pass 2**, and the multiply tint reads well
(all in-sim, 2026-07-31). §5 **steps 1 and 3 are done**.

What that opened rather than closed: multiply alone cannot brighten, and the
wanted look needs an **additive pass on top of it** (§3). So the single tint has
been generalized into a **layer compositor** — an ordered stack of color ×
blend mode × opacity per target, with a Dear ImGui editor (§8). The FCU target
is now the whole readout **row**: the strip plus the captain's and F/O's baro
windows (§2).

Still open: **step 2** (the black-background assumption is still unverified) and
**step 4**, the tuning pass itself. Step 5 is deliberately not started — it is
gated on the look being signed off — though its CMake half is incidentally done
(§8a).

All of it is still inside `#if PHOTON_DEV` — build with
`src/native/deploy.ps1 -Dev`.

This is the first Photon feature that draws **pixels** rather than placing lights.
Everything else in this project emits OBJ geometry and drives datarefs; this
reaches into X-Plane's renderer. Section 1 is the contract that makes that safe,
and it is the part worth reading even if the FCU idea is dropped.

---

## 0. Why the panel FBO at all

ToLiss draws the FCU readouts from its own plugin, into the aircraft's panel
texture. There is no dataref for their color, no OBJ to edit, and no texture file
to replace — the pixels do not exist until ToLiss writes them each frame.

That leaves exactly one intervention point: **draw over the same framebuffer,
after ToLiss**. X-Plane calls plugin draw callbacks in `xplm_Phase_Gauges` while
the panel FBO is bound, so a quad in panel coordinates lands on the panel texture,
which the cockpit mesh then samples as normal. No shader injection, no texture
replacement, no `.acf` edit.

The whole approach rests on `cockpit_INN.obj` carrying **no `TEXTURE` line** and
declaring `GLOBAL_cockpit_lit`: its `ATTR_cockpit` geometry samples the panel
texture, and it uses plain `ATTR_cockpit` throughout — zero `ATTR_cockpit_region`
— so UV 0..1 maps to the whole 4096 atlas.

---

## 1. The panel-FBO drawing contract

Every value here was **measured in-sim on 2026-07-30/31** with the probe, not read
from documentation. Three earlier rounds of this feature were lost to reasoning
from first principles about a renderer that does not behave as documented, so:
measure, then build.

| Property | Value | How it was established |
|---|---|---|
| Framebuffer | FBO **2**, non-zero | `glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING)` |
| Color attachment | texture **254**, type `GL_TEXTURE` | `glGetFramebufferAttachmentParameteriv` |
| Viewport | **0,0 4096×4096** | `glGetIntegerv(GL_VIEWPORT)` |
| Coordinates | **raw panel pixels, 0..4096** | probe rects land exactly on their screens |
| Projection | `glOrtho(0, 4096, 0, 4096)` **already set** | proj diag `0.0005 ≈ 2/4096`, tx,ty `−1,−1` |
| Modelview | identity | proj/modelview dump |
| Shader | **none bound** (`program=0`) | `glGetIntegerv(GL_CURRENT_PROGRAM)` |
| Face culling | **ON**, front = **CW**, cull `GL_BACK` | `cull=1(front=CW mode=0x405)` |
| Depth test | off | state dump |
| Blending | **already enabled** | `blend=1` |
| Color writemask | all four channels on | `writemask=1111` |
| Renderer | Vulkan 12.4.3 + OpenGL bridge | `Log.txt` line 1 |

### ⚠ Wind clockwise

Culling is enabled and the front face is **CW**. A counter-clockwise quad is
silently discarded, which in the cockpit is indistinguishable from "plugin drawing
does not work here" — it cost a full debugging round. `DrawProbeRect` queries
`GL_FRONT_FACE` and emits the matching winding; the real feature should do the
same rather than assume, since nothing guarantees the convention is stable across
X-Plane versions.

### ⚠ Restore every state toggle you change

`glDisable(GL_CULL_FACE)` without a matching restore leaks into **X-Plane's own
rendering** for the remainder of that pass — backfaces of cockpit geometry start
drawing. This shipped in the probe on 2026-07-31 and did real damage beyond the
visual: it made the immediate-mode draw path appear to start working, because one
run of the "Forced" method left culling off for everything afterwards. The
comparison the probe existed to make was silently invalidated. Save and restore.

Blending is *already on*, so the tint only needs to set the blend func — but that
func is shared state too, and must be saved and restored on the same principle.
`PushMultiplyBlend`/`PopMultiplyBlend` do exactly that, querying all four
`GL_BLEND_{SRC,DST}_{RGB,ALPHA}` factors and restoring them through
`glBlendFuncSeparate`. Alpha needs no color mask: the source factor applies to
all four channels, so `dst.a` becomes `dst.a × src.a`, and holding `src.a` at 1.0
passes the framebuffer's alpha through by construction.

### ⚠ Coordinates are NOT `panel_total_pnl_*`

Those four datarefs report **1024** on the A320, because the aircraft's 2-D panel
is empty (`PANEL_2D_BEGIN` immediately followed by `PANEL_2D_END` in `a320.acf`)
and they describe *that*, not the 3-D panel texture. Scaling atlas pixels through
them shrinks everything 4× into the bottom-left corner, onto atlas area no cockpit
geometry samples — invisible, in every phase, regardless of draw order. The 3-D
panel really is the 4096 space: `a320.acf` places `GROUP NDCapt` at
`POS 1139, 2953`, the exact x-center of the derived capt ND rect.

### The two passes

`sim/graphics/view/panel_render_type` is `0` = 2-D panel/popups, `1` = 3-D
non-lit, `2` = 3-D lit. The texture browser's `"…/lit panel fbo:0"` is pass **2**.
Both 3-D passes bind a 4096 framebuffer; the probe can be restricted to either
with `ToLissPhoton/debug/panel_probe_pass`.

**The FCU strip composites in pass 2 (3-D lit)** — measured in-sim 2026-07-31.
On the FCU strip rect, pass 1 puts down nothing visible at all, while pass 2 and
`−1` (both) are indistinguishable, which is what "pass 1 contributes nothing"
looks like. Pass 1 stays reachable: it appears to carry a "glass" overlay layer,
and it is the control that makes the pass-2 result mean something.

**And ToLiss does not draw over us.** A flat fill in pass 2 *replaces* the
digits, so the end of `xplm_Phase_Gauges` is late enough — §6's first open
question is settled for the FCU, and the feature is buildable in this form.

---

## 2. The target rect

Derived by `build/derive_panel_rects.py`, which groups `ATTR_cockpit` triangles
into UV islands by shared vertex index and filters geometrically. Re-run it to
reproduce or to port to another airframe:

```bash
python build/derive_panel_rects.py \
    "<X-Plane>/Aircraft/ToLissA320_V1p3p2/cockpit_INN.obj" [--gating]
```

| Face | u0..u1 | v0..v1 | px | Size / position |
|---|---|---|---|---|
| **capt baro** | 896.5 .. 1023.7 | 954.0 .. 1020.2 | 127×66 | 3.6 × 1.8 cm at x −0.270, y 0.408, z −4.421 |
| **FCU strip** | 1046.1 .. 1863.9 | 954.0 .. 1020.2 | 818×66 | 23.0 × 1.8 cm at x −0.001, y 0.419, z −4.425 |
| **FO baro** | 1890.4 .. 2017.6 | 954.0 .. 1020.2 | 127×66 | 3.6 × 1.8 cm at x +0.267, y 0.408, z −4.421 |

The FCU strip is one island, one quad, covering the whole readout row. Individual
SPD/HDG/ALT/VS windows are **not** separately derived — and under the approach in
§3 they do not need to be.

The two **baro-reference windows** on the captain's and F/O's EFIS control panels
were added 2026-07-31. They are the same kind of readout, they flank the strip,
and they share its **exact** v-row — the three are one row of the atlas,
interrupted only by bezel. Like the strip, neither is inside any `ANIM` block
(checked by full-file nesting depth at their TRIS batch, not by the 40-line
window `--gating` uses) and neither is resolution-switched, so both appear
unchanged in `kPanelRectsLo`.

### Groups: the FCU row

They are **three rects, not one wide one**, addressed together as the group
`"FCU row"` (`kPanelGroups` in `plugin.cpp`). The gaps between them —
1023.7..1046.1 and 1863.9..1890.4, 22 and 26 px — are atlas that *other cockpit
geometry samples*, so a single 896..2017 quad would recolor something unrelated
that happens to be parked between our faces. Three quads cost nothing.

Groups live in the **same index space** as rects: `[0, kPanelRectCount)` is a
rect, above that is a group. That is what `ToLissPhoton/debug/panel_probe_rect`
and the layer editor's target list both select from. Members are named rather
than indexed, so a group is resolved against the *current* rect set and follows
the resolution switch for free.

⚠ **`kPanelRectsHi`/`Lo` are APPEND-ONLY.** The index is persisted in the dev
prefs file and is the value of the probe's rect dataref, so inserting a row
silently re-aims a saved target at a different screen.

### 2a. The rest of the panel (2026-07-31)

`derive_panel_rects.py --all` drops the display-unit size floor and surveys every
aft-facing readout island. Everything it found that is worth tinting is now in
the table:

| Face | u0..u1 | v0..v1 | px | Position, size |
|---|---|---|---|---|
| **ISIS** | 3266.6 .. 3670.7 | 2417.7 .. 2823.7 | 404×406 | x −0.143 y +0.110, 6.4 × 6.1 cm |
| **DCDU capt** | 3675.6 .. 3944.9 | 2618.4 .. 2820.0 | 269×202 | x −0.181 y −0.085, 9.6 × 6.9 cm |
| **DCDU FO** | 3675.6 .. 3944.9 | 2410.1 .. 2611.7 | 269×202 | x +0.194 y −0.085, 9.6 × 6.9 cm |
| **clock CHR** | 22.1 .. 100.8 | 943.2 .. 973.8 | 79×31 | x +0.166 y +0.129, 3.8 × 1.4 cm |
| **clock UTC** | 9.8 .. 115.6 | 896.9 .. 927.4 | 106×31 | x +0.167 y +0.108, 5.1 × 1.4 cm |
| **clock ET** | 24.5 .. 98.4 | 850.4 .. 880.9 | 74×31 | x +0.166 y +0.087, 3.5 × 1.4 cm |
| **ctr readout L** | 727.6 .. 855.7 | 889.7 .. 930.5 | 128×41 | x −0.168 y +0.040, 2.8 × 0.8 cm |
| **ctr readout R** | 872.9 .. 1000.9 | 889.7 .. 930.5 | 128×41 | x −0.127 y +0.040, 2.8 × 0.8 cm |
| **ped strips** | 990.1 .. 1055.6 | 3607.1 .. 3714.4 | 66×107 | pedestal, ×4 faces, 1.8 × 3.4 cm each |
| **EFB capt** | 4.7 .. 1330.7 | 1076.2 .. 1878.1 | 1326×802 | side-rail tablet, 12.0 × 17.2 cm |
| **EFB FO** | 1336.7 .. 2662.3 | 1077.7 .. 1884.7 | 1326×807 | side-rail tablet, 10.2 × 17.9 cm |
| **EFB AviTab** | 2120.0 .. 3142.1 | 8.7 .. 645.0 | 1022×636 | **both** tablets, 12.1 × 17.2 cm |
| **MCDU capt** | 3265.5 .. 3541.3 | 2824.0 .. 3079.1 | 276×255 | x −0.194 z −4.442, 9.4 × 11.4 cm |
| **MCDU FO** | 3541.8 .. 3817.6 | 2824.0 .. 3079.3 | 276×255 | x +0.194 z −4.442, 9.4 × 11.4 cm |
| **MCDU 3** ⚠ A330 only | 3821.1 .. 4091.0 | 2830.6 .. 3070.6 | 270×240 | x +0.010 z −28.317, aft pedestal |

Seven things about that table are load-bearing:

- ⚠ **The names are inferred from geometry, not told to us.** ToLiss labels none
  of these faces. ISIS (a 6.4 cm square left of the E/WD at DU height) and the
  clock (three stacked windows, the middle widest because UTC is six digits) are
  safe reads. **DCDU** is the project's best reading of a symmetric 9.6 × 6.9 cm
  pair on the lower main panel, supported by ToLiss shipping `DCDUs.obj` and
  `CPDLC.png` — no more than that. The two **center readouts** are named by
  position precisely because the geometry does not say what they are; a wrong
  name is worse than no name. Confirm any of them with the FX target tree's `?`
  button, which floods that target magenta, then **fix the name in the table**
  rather than working around it.
- **The four pedestal faces share ONE atlas island.** They cannot be tinted
  separately at all, so one rect is the truth rather than a simplification.
- The survey also returns ~51×52 and 29×18 squares paired with the DUs and DCDUs.
  Those are the **off-state** patches — a plain gray corner of the atlas that the
  unpowered face samples — and they are deliberately *not* in the table: they are
  shared, and nothing is lit on them anyway.
- ⚠ **The EFB tablets' SIDE comes from the ANIM index, not from their vertex
  positions.** Both sit inside two levels of `ANIM`, so their vertices are in a
  *local* frame — all four faces read x ≈ 0 and z ≈ −0.1, four meters aft of the
  panel and apparently in the cabin. The side is in the animation: the captain's
  chain is `AirbusFBW/CockpitWindowPosition[0]` with
  `ANIM_trans −1.27387 0.15733 −3.63657`, i.e. negative x. Reading the survey's
  position column instead is what nearly filed both of these as cabin geometry
  and left them off the map. **Any island reported as `GATED` may be in a local
  frame; check the `ANIM_trans` before believing its position.**
- **Each tablet has two faces**, gated by `AirbusFBW/EFBShowsAvitab[n]`: ToLiss's
  own EFB at 0, AviTab at 1. Only one draws at a time, so tinting the hidden one
  is harmless — nothing samples its atlas region while it is hidden. ⚠ But **both
  tablets share the AviTab island**, so AviTab cannot be tinted per side at all —
  the same shape as `ped strips`. The `v` ranges also stop ~7 px short of each
  screen's own UV extent, because the tablet's bezel strip starts there and the
  screen quad overlaps it; losing 7 px off an 800 px screen is invisible, tinting
  7 px of a 75 px bezel is a colored sliver of frame.
- **The MCDUs sit directly above the ISIS in the atlas**, one island per side, so
  unlike AviTab they *can* be tinted separately. Ungated and not
  resolution-switched. ⚠ Their `v0` is trimmed 2822.0 → 2824.0 because the ISIS
  island ends at 2823.7 and the two are packed with ~1.7 px of overlap. ⚠ Their
  UV is near-uniform and **not** rotated — `u` runs with +x, `v` with −z, i.e.
  the top of an overlay image lands at the far edge of the screen, away from the
  pilot. §6b is the write-up of how these were missed on the first pass; it is
  worth reading before trusting any survey row on a non-vertical face.
- ⚠ **`MCDU 3` is the one row here that does NOT exist on an A3xx** (2026-08-13).
  The A330 has a third MCDU on the aft pedestal; the A3xx atlas carries no readout
  face at those coordinates at all (`--all` on the A320: two MCDU islands, nothing
  in 3821..4091 × 2830..3071). So the row is listed in `kPanelRectFamilies` and
  gated off everywhere but the A330 — see §2c. It is also the one row carrying the
  **A330's own numbers**: `MCDU capt`/`MCDU FO` keep their A3xx values, from which
  the A330's own faces are 6–10 px inset, and this face has no A3xx value to be
  consistent with.

  Identified by its **manipulators**, which is this table's naming rule and here
  is unusually strong evidence. ToLiss gives the A330 a full `AirbusFBW/MCDU3*`
  command set that the A3xx does not have; summing the enclosing `ANIM_trans`,
  that keyboard's 72 faces center at x 0.009 y 0.299 z −28.219 against this
  island's x 0.010 y 0.294 z −28.317 — 1 mm across and 10 cm in front of the
  keys, which is where a screen sits relative to them. The same method puts
  `UndockMCDU1` and `UndockMCDU2` exactly on the other two islands, which is what
  says the method is sound rather than merely suggestive. ⚠ Source-level only;
  not yet confirmed with Probe ▸ `?`.

### 2c. When a rect is not on every aircraft (2026-08-13)

`MCDU 3` is the first row that needed this, and the mechanism is deliberately the
smallest one that answers it.

- **`kPanelRectFamilies`** — a side table keyed by rect NAME, like `kPanelSources`
  and `kPanelBuses`. Absence means "every aircraft", so 36 of the 37 rows say
  nothing and the default is what the table was before it existed. The resolved
  answer is one `PanelMask` (`gRectsHere`), rebuilt with the per-rect drivers, so
  it costs a shift and a test in the hot path. ⚠ The mask is built by CLEARING
  bits from "all of them"; built the other way round, a rect nobody listed would
  draw nowhere.
- ⚠ **It is NOT group membership.** `MCDU 3` is a member of `MCDU`, `Glass
  displays` and `Whole panel` on *every* aircraft. Membership is authored
  structure: it is what the editor tree, the containment nesting and — through
  `PanelTargetBreadth` — the broadest-first composite order are derived from, and
  a group that quietly changed size with the aeroplane would change its own
  breadth. Existence is a runtime fact and is answered per frame instead, in
  `FxRectGated`, beside power and the bus.
- ⚠ **The test sits ABOVE the master switch** in `FxRectGated`. "Follow display
  brightness" outranks every ELECTRICAL gate, because those all answer *how
  strongly to paint*; this one is the per-rect twin of a stack's `family` scope,
  which nothing overrides. Below that early-out, turning the hatch off — a thing
  done while picking a color — would paint the A330's third MCDU onto a patch of
  A3xx artwork, in the editor, which is where it would be believed.
- **`kScreenFxExtraRects` lists it anyway.** Which Displays switch owns a face is
  independent of which aircraft the face is on: left off, the third MCDU would
  follow "other readouts" while the two beside it followed "screens", so turning
  the screen effects off would leave one of three MCDUs tinted.
- **The probe is deliberately NOT gated.** Flooding a rect magenta is how a face
  is confirmed, and an instrument that refused to paint where it is pointed would
  be lying about where the rect is. On an A3xx the source table's factor column
  says `x0.00 not on this aircraft` in the same pane, which is the counter-evidence
  in the place it is needed.

`RectExistenceTests` in `tests/test_panel_rects.py`.

### 2b. Not A320 only, as it turns out

Every airframe has its own atlas layout in principle, so the original table
carried an "A320 only, re-derive before using elsewhere" warning. Re-derived on
**A319 (1.0.12) and A321 (1.7.1) on 2026-07-31: every rect above and every DU is
byte-identical across all three.** The table is A3xx-wide.

**And it very nearly reaches the A330-900 too** (derived 2026-08-11 from
`A330-900_cockpit_INN.obj`, source-level only — not confirmed in the sim). ToLiss
reuses one atlas layout across the fleet rather than laying out each aircraft
afresh:

| A339 vs the table | Rects |
|---|---|
| within 3.3 px | the six DUs, both DCDUs (0.2 px), ISIS, `ped strips` |
| within ~6 px | FCU strip, FO baro, clock ×3, RMP1/2 |
| same slot, A339 face 6–10 px inset | capt baro, MCDU ×2, XPDR code, rudder trim, RMP3, BAT1 |
| same slot, 24 px narrower | the two DME windows |
| **no A330 face there at all** | BAT2 (47 px), `ovhd aft L1`/`L2`, `FMS Load` (95 px), the three EFB rects (570+ px) |
| **no A3xx face there at all** — the traffic runs both ways | `MCDU 3`, the A330's third MCDU (§2a, §2c) |

⚠ **A matching rect is not a matching look.** The FCU row lands within ~6 px and
still reads as wrong on an A330, because its layers are tuned to the A3xx's
artwork. That is what the per-target `family` scope in §4 is for, and it is why
the answer to "does the table port" is *per target*, not per airframe.

### ⚠ The DU rects are runtime-gated; the FCU strip is not

Each display-unit face exists **twice** in the OBJ, under
`ANIM_hide … AirbusFBW/DisplayResolutionFallback` — a ~748 px variant visible at
0 and a ~499 px one at 1, anchored at the same origin. Draw the wrong set and an
overlay is two-thirds size on a correctly-aimed screen. `PanelRects()` reads that
dataref per frame.

The FCU strip has a **single** variant and is not resolution-switched, so the tint
specifically does not need to care. Do not generalize that to the DUs.

---

## 3. The approach: multiply, and let black do the masking

Blend the strip with `glBlendFunc(GL_DST_COLOR, GL_ZERO)` — a straight multiply of
the framebuffer by our color — using something green-suppressing, around
`(1.00, 0.82, 0.90)`.

The reason this is attractive rather than merely workable: **the FCU background is
black, and black × anything is black.** A multiply over the *entire* 818×66 strip
therefore leaves the background untouched and recolors only the lit digits. No
per-window sub-rects, no masking geometry, no alpha texture. The mask is the
content ToLiss already drew.

It also scales itself. A multiply is proportional, so a dim readout receives a
proportionally small absolute shift and a bright one a large shift — the effect
tracks the FCU brightness knob with no dataref read at all.

**Risks specific to this trick, all to be checked in-sim:**

- If the background is dark *gray* rather than true black, it tints too. Probably
  imperceptible at those levels, but confirm rather than assume.
- Lit **legends and labels** inside the strip get tinted along with the digits. If
  the strip carries white captions that should stay white, the whole-strip multiply
  is wrong and sub-rects become necessary after all.
- Multiply can only ever *darken*. Pulling yellow-green toward orange by removing
  green is a darkening operation, so this is the right tool — but it cannot make
  the readout *brighter* or more saturated. If the result looks muddy, the
  alternative is a low-alpha additive orange pass, which brightens but does not
  suppress green, or both in sequence.

  **Confirmed in-sim 2026-07-31: multiply alone works well, and an additive pass
  is wanted on top of it.** So the answer to the last bullet is "both in
  sequence", and it is not a fallback — it is the design. One blend mode over one
  rect was never going to be the shape of this feature, which is what drove the
  **layer compositor** in §8: an ordered stack of (color × blend mode × opacity)
  per target, rather than a second hard-coded pass bolted next to the first.

### ⚠ Additive does NOT self-mask, and that changes what step 2 is worth

The whole elegance of §3 is that black × anything = black, so multiply needs no
mask. **Addition has the opposite property**: black + orange = orange. An
additive layer over the same 818×66 quad lights the *background* as much as the
digits — a glowing bar across the FCU, not brighter digits.

So the moment a second layer is additive, the black-background trick stops
carrying the feature and **§5 step 2 goes from a formality to the thing that
decides the design**. Three ways out, in increasing cost:

1. Keep the additive layer's opacity low enough that background lift is below
   perception while the digits still gain. Plausible, since the digits are
   already bright and the eye judges the bar against its surround — but it is a
   judgement call to be made in-sim, not asserted here.
2. Sub-rects per readout window (SPD/HDG/ALT/VS), so the additive pass only
   covers glass that is mostly lit. Requires deriving four more islands, and §2
   notes they are *not* separately derived today.
3. Screen instead of Add. It rolls off rather than clipping, so it lifts a black
   background less aggressively than a straight add — worth trying first because
   it costs one combo box, not a derivation pass.

Multiply-only remains a complete, shippable look if none of that pans out.

---

## 4. Where the code should live

The current panel-FBO code is entirely inside `#if PHOTON_DEV`, which is
**correct and must stay** — `make_release.py` must never bundle a build that
paints magenta over the captain's PFD. A shipping tint feature needs its own home:

- A separate `#if PHOTON_PANEL_FX` (or equivalent) block, independent of the probe,
  so the diagnostic and the feature can be built separately. They will want
  different lifetimes: the probe is throwaway, the feature is not.
- **`CMakeLists.txt` links `opengl32` unconditionally** (the ImGui backend needs it). A shipping
  feature that draws GL needs that link unconditionally — a real build change, and
  easy to miss until a release build fails to link on someone else's machine.
- The draw callback registration is already re-run on aircraft load
  (`ReseatPanelProbe`) to push us later in the phase than the aircraft's own
  plugin. The feature needs the same, for the same reason.
- Per project convention, and following `screens_plan.md`: dev-only and opt-in
  first, no menu row, nothing in `install.py` or the release bundle until the look
  is accepted in-sim.

Three places would have to agree once this ships, in the usual Photon shape: the
rect table in `plugin.cpp`, the derivation in `build/derive_panel_rects.py`, and
whatever the installer needs to know about airframe coverage.

### ⚠ IT SHIPPED — 2026-08-01

The bullets above are the plan as written; this is what was actually done, and the
last one is now history.

- **The COMPOSITOR left `#if PHOTON_DEV`; the EDITOR did not.** What still needs a
  dev build is the panel probe, the Panel FX tab and its history, and the live
  light editor. `ShippingBuildTests` in `tests/test_panel_fx.py` reads the guard
  nesting in `plugin.cpp` and pins which side each symbol is on — because both
  mistakes are silent, in opposite directions: a compositor left inside the guard
  runs fine on the dev machine and does nothing in a release; a probe left outside
  it floods a release's cockpit magenta.
- **The draw callback is one function for both.** `PanelPassDraw` (was
  `PanelProbeDraw`) runs the compositor before its `#if`, so a release build's
  callback does exactly one thing. A shipping build also registers only the
  after-Gauges slot; the other two exist so the probe can compare slots.
- **`panelfx.txt` moved to the plugin folder** and is installed, not authored in
  place. See §8's persistence note.
- **The user-facing control is the settings window's Displays tab**, not a menu
  row: one switch per effect plus a strength, split per RECT into "the screens"
  and "everything else". A screen is a rect with a `DUBrightness` index *or* one
  named in `kScreenFxExtraRects` — the two MCDUs, the two DCDUs and the ISIS,
  which have no such index but are backlit screens all the same. `CLAUDE.md` ▸ The
  Displays axis is the contract.
- ⚠ **Airframe coverage is a PER-TARGET `family` scope** (2026-08-11). This entry
  used to read: *"the rect table is A3xx-wide and the A339 has its own atlas, so on
  an A339 the named targets simply do not resolve and the stacks are dropped with a
  log line."* **That was never true of the code.** `PanelTargetByName` resolves
  against `kPanelRectsHi`/`kPanelGroups`, both `static const`, so a target name
  resolves identically whatever aircraft is loaded — and nothing in `DrawPanelFx`
  ever asked. Every stack painted on every cockpit, including non-ToLiss ones.

  Two things were wrong with the belief and they pull in opposite directions:

  - **The A339 does NOT have its own atlas.** ToLiss reuses one 4096 panel layout
    across the fleet. Re-derived from `A330-900_cockpit_INN.obj`: all six DUs match
    the A3xx rects within 1.5–3.3 px, both DCDUs within 0.2 px, the ISIS, FCU
    strip, `ped strips`, clock and RMP1/2 within ~6 px. 19 of the 36 rects land on
    a real A330 face at the same coordinates.
  - **Landing the rect is not the same as the look being right.** The FCU row's
    layers are tuned to A3xx *artwork* and read as wrong on an A330 even though the
    rect very nearly matches — reported from the sim, and the reason this exists.
    Separately, `BAT2`, `ovhd aft L1`/`L2` and `FMS Load` have no A330 counterpart
    at those coordinates at all (47–95 px away), and the three EFB rects none
    within 570 px.

  So the scope is authored per target — `family a3xx [a330 …]` under the `target`
  line, absent = every aircraft — and resolved from the ICAO type code at aircraft
  load and on the 1 Hz loop. ⚠ **The ToLiss A320 reports `A20N`, not `A320`**;
  a family list built from the obvious three misses the aircraft this is mostly
  used on. Shipped today: the screen stacks (`Main displays`, `ISIS`, `DCDU`,
  `MCDU`) are unscoped and confirmed good on both; `FCU row`, `DME`, `Radios`,
  `XPDR code`, `Overhead readouts` and `rudder trim` are `a3xx`.
  `AircraftFamilyTests` in `tests/test_panel_fx.py`.

### 4a. One family mask, three places that carry it (2026-08-13)

The per-target scope above is a blunt instrument: it can say *"this look is not for
that cockpit"* and nothing finer. Two things it could not say came up as soon as
the A330 was looked at properly, and both are now expressible.

**A LAYER carries a scope of its own — wire key `lfamily`, own line under the
layer.** Two families often want the *same* target with *one* layer different: the
same rects, the same composite order, one correction that is only right on one of
them. That cannot be said with two stacks, because **a stack IS its target** — two
stacks named `FCU row` are two entries the tree and the file both key on the same
name. So the mask moved one level in as well.

- It **narrows** the target's scope and cannot widen it: `DrawPanelFx` skips the
  stack before reaching its layers, so a layer naming a family its target excludes
  draws nowhere at all. The layer pane says so in orange, and
  `test_no_shipped_layer_is_scoped_outside_its_target` says so about the file.
- The gate is in the layer loop **above the `enabled` test**, so a scoped-out layer
  costs no texture bind, no blend equation and no quad — all of which are per layer
  and all spent before the first vertex (§8j.2).
- ⚠ `lfamily`, not `family`. Parsing dispatches on the key alone, so a layer scope
  spelled like its target's would be read as the TARGET's by any file whose lines
  are ever reordered — silently widening the target to whatever the last layer
  named. Same reason `lfollow` and `lcurve` carry the `l`.

**A ROW OF THE BUILT-IN DRIVER TABLE carries one too** (`PanelSource::families`),
which is the other half of the same report: on an A330 the FCU's integral lighting
is `AirbusFBW/SupplLightLevels[1]`, an ordinary 0..1 knob, and not the A3xx's raw
`1..270` animation value. A table with one answer per face can only be right about
one family; the rect is the same rect, the dataref behind it is not.

- **Rows are applied IN ORDER and a scoped row is an OVERRIDE**, not a separate
  table: the unscoped row says what a face reads by default, and a row naming
  `kFxFamilyA330` says what it reads there instead. ⚠ Moving a scoped row *above*
  the row it overrides is not a compile error and not a log line — it is one face
  reading another aeroplane's knob, which resolves, reads a plausible number and
  dims on the wrong input. `BrightnessSourceTests` holds the order
  (`test_a_family_scoped_row_comes_after_the_row_it_overrides`).
- **An empty field in a scoped row INHERITS, it does not clear.** `bright` and
  `power` are written independently and only when non-empty, so an override that
  names a brightness source keeps the unscoped row's power gate. The cost is that
  "this family has no power gate on this face" cannot be said here; it needs a
  stack override in `panelfx.txt`.
- ⚠ **The table therefore belongs to a family, so "has it been built" is the wrong
  question.** `EnsureRectDrivers()` replaced the old `if (!gRectDriversBuilt)` at
  every call site and rebuilds when `gRectDriversFamily != gFxFamily`. A table
  built for an A320 and read on an A330 is stale in the invisible way: every driver
  resolves, every value reads, and one face follows another aeroplane's knob.
  Making that a call site's job would be six places to remember it.

**The rule itself is stated ONCE**, in `FxFamilyAllows(unsigned)`; the stack, the
layer and the driver row are one-line readers of it. Three copies of
`== kFxFamilyNone || & gFxFamily` is how one of the three would eventually fail
OPEN on an aircraft nobody tests on, and `test_the_rule_is_stated_once` counts
them. The wire has the same shape: one `FxReadFamilyList` and one
`FxWriteFamilyLine` serve both keys, because the way two readers would drift is a
layer scope that fails open where the target scope fails closed.

---

## 5. Order of work

1. **Establish which pass draws the FCU digits.** ☑ **Answered 2026-07-31 — pass
   2 (3-D lit).** Pass 1 showed nothing on the FCU strip; pass 2 and −1 were
   indistinguishable and fully opaque. The fill *replacing* the digits also
   settles the draw-order question in our favor (§1, §6).

   ⚠ **One loose end: which draw method produced that result was not recorded.**
   If it was the default (0, Clear), draw order is proven but the *immediate-mode
   quad* path is not — `Clear` bypasses the vertex and fragment stages entirely,
   and Multiply is an immediate-mode quad. Selecting the FCU preset settles it on
   sight: a tint that appears at all has proven the quad path too.
2. **Confirm the black-background assumption.** ☐ **Not done.** Still with a flat
   opaque fill, check the strip's boundaries against the visible readout area —
   whether the quad covers exactly the digits' region, and what else is inside it
   that is lit. The magenta fill from step 1 already shows the boundary; this is
   a matter of looking at it deliberately rather than of running anything new.
3. **Add a multiply mode** to the probe: `glBlendFunc(GL_DST_COLOR, GL_ZERO)` with
   a settable color, saving and restoring the previous blend func. Reuse the
   existing rect/pass/method datarefs rather than adding a parallel set.
   ☑ **Done 2026-07-31** — draw method **3 (Multiply)**, see §5a.
4. **Tune the color in-sim**, same discipline as the light tuner: never guess a
   color from a screenshot. ☐ **Not done.** The multiply half is confirmed to
   read well but has not been tuned; `(1.00, 0.82, 0.90)` is still the plan's
   seed, carried over unexamined, and the additive pass has no numbers at all.
   **Tune it in the layer editor (§8), not through the datarefs** — it does the
   whole stack, it persists across reloads, and `Log stack` prints the result
   ready to paste.
5. **Promote out of the probe** into its own compile unit once the look is signed
   off. ☐ **Not started, and deliberately so** — it is gated on step 4. Its
   CMake half is already done for a different reason: `opengl32` is now linked
   unconditionally because the ImGui backend needs it (§8a).

### 5a. Driving it — **Plugins ▸ ToLiss Photon ▸ Debug**

Added 2026-07-31. Retyping six dataref paths into DataRefEditor after every sim
reload was friction in the measurement loop, which is the one place this project
cannot afford it — every wrong conclusion in this document's history came from
reasoning instead of looking.

```
Plugins ▸ ToLiss Photon ▸ Dev ▸ Dev window... ▸ Probe tab
                                  ─────────
                                  FCU tint preset
                                  Probe off
```

- **Panel Probe…** opens a window of buttons: rows for **Probe** (which callback
  slot), **Pass**, **Draw**, **Rect** (built from the live rect table, so
  re-deriving it cannot leave a stale name here), **Tint R/G/B** as −/+ steppers,
  **Step** granularity (0.005 / 0.01 / 0.05), and three actions. Two header
  lines restate the whole state in plain English and show the tint as a swatch
  drawn *in* the tint color.
- **FCU tint preset** — one click to the tuning state: after Gauges, pass 2
  (lit), Multiply, FCU strip. Pass 2 is the measured answer to step 1, not a
  guess.
- **Probe off** — stop painting over the cockpit without hunting for the row.
- **Re-log state** (in the window) forces the next frame of each pass to re-dump
  the GL state to `Log.txt`. Previously the only way to get a fresh dump was to
  toggle a knob and toggle it back.

**Settings persist across reloads**, in
`Output/preferences/ToLissPhoton_paneldebug.txt` — deliberately its own file, not
the livery prefs, since this is throwaway state on a knob that does not ship.
They are restored through the same writers the datarefs use, so a stale or
hand-edited file cannot introduce a value the dataref path would have rejected.
⚠ It also means **a probe build can come up already painting** — the startup log
line says so.

The datarefs are unchanged and still work; the window is a second way in, not a
replacement. Only the window and menu write the prefs file — a dataref write
does not, since something writing every frame would then write a file every
frame.

| Dataref | Set to |
|---|---|
| `ToLissPhoton/debug/panel_probe` | `1` (after Gauges) — the slot the real feature would use |
| `ToLissPhoton/debug/panel_probe_rect` | `6` — the FCU strip |
| `ToLissPhoton/debug/panel_probe_pass` | `2` — **measured**; `1` is the control, `-1` is both |
| `ToLissPhoton/debug/panel_probe_draw` | `1` Immediate (flat fill, for step 2) → `3` Multiply |
| `ToLissPhoton/debug/panel_tint_{r,g,b}` | the multiply color, clamped 0..1 |

Every tint write logs the whole triple to `Log.txt`, so the accepted numbers can
be copied straight back into `gPanelTint` rather than reconstructed from memory.

The Debug submenu is level-2 nesting, like Exterior and Cockpit. That is fine
here for the reason recorded in `CreatePhotonMenu`: the "no menu level 2+" rule
in `CLAUDE.md` is an **XPPython3 4.7a1** defect and this plugin is native C++.
The rule still binds `src/plugin/PI_PhotonDevReload.py`.

**Multiply is the only draw method that is an effect rather than a diagnostic**,
and it is the only one that leaves blending enabled. It cannot be combined with
method 2 (Forced), which disables `GL_BLEND` — the very thing multiply needs. If
the tint is invisible *and* the state dump shows a bound shader program, that is
a real conflict to solve, not a matter of picking the other method.

The rect index is looked up by name (`FcuStripRect()`), not hard-coded, so
re-deriving or reordering `kPanelRectsHi` cannot silently aim the tint at a
display unit.

---

## 6. Open questions

- ~~**Does ToLiss draw the FCU after us even at the end of `xplm_Phase_Gauges`?**~~
  **Answered 2026-07-31: no.** A fill on the FCU strip in pass 2 replaces the
  digits, so the end of Gauges is late enough and the feature is buildable in
  this form. Callback order between plugins within a phase is still unspecified
  by the SDK, so `ReseatPanelProbe` (re-registering on aircraft load, which
  pushes us later in the list) stays load-bearing rather than incidental — it is
  plausibly *why* the answer came out this way.
- **Is the FCU on its own brightness knob**, and does the multiply's natural
  proportionality actually track it? Not yet checked.
- **Does `DUBrightness[n]` collapse to 0 on an unpowered DU?** Shared with
  `screens_plan.md` — if it does, neither feature needs a separate power dataref.
  Candidates otherwise: `AirbusFBW/ACBusVoltages`, `AirbusFBW/DCBusVoltages`,
  `AirbusFBW/DUSelfTestTimeLeft`. A two-minute check with the battery off.
- **A319/A321 atlas layouts** — assumed to differ until derived.
- **Does the tint survive a panel-texture resolution change?** The FCU strip is not
  gated by `DisplayResolutionFallback`, but nothing confirms the *atlas* stays 4096
  under every X-Plane texture-quality setting. If the viewport ever reports
  something other than 4096, the raw-pixel coordinates are wrong.

---

## 6b. ⚠ How the MCDUs were missed — two measurement traps

The MCDUs are in the table (§2a) and always were derivable. This section exists
because the first pass concluded, at length and with five supporting checks, that
they were **not on the panel at all** — and every one of those checks was true
and irrelevant. Both real errors were in *reading the survey*, which is the part
that felt like data rather than inference.

**Trap 1 — `w`/`h` were the AXIS-ALIGNED x and y extents.** The MCDUs lie almost
flat on the pedestal (normal `0.00 0.99 0.14`, 8° off horizontal), so a
9.4 × 11.4 cm screen measured **11.4 × 1.4 cm** — because its second dimension
runs in *z*, not *y*. It appeared in the survey, was read as a label strip, and
was dismissed. Anything on a console, a pedestal or a glareshield top lands in
the same trap. `derive_panel_rects.py` now measures **in the face's own plane**
(`in_plane_size`).

**Trap 2 — `is_readout` required `n[2] > 0.2`, i.e. a vertical face.** The MCDU's
`n[2]` is 0.14, so `--all` never listed it at all; it only ever showed up in
ad-hoc scratch queries. The filter now also accepts `n[1] > 0.5`, and the A320
survey went from 51 candidates to 74.

**The lesson is not "check harder".** It is that a chain of correct negative
checks around a bad measurement produces a confident, well-evidenced, wrong
answer — and that the in-sim probe settles in one look what the geometry
argued for a page. `Probe ▸ flood` was what actually reopened this.

Two things from that wrong pass are still worth keeping, because they were
independently verified and remain true:

- `objects/panels_pedestal.obj` really does have **zero `ATTR_cockpit`** and its
  own texture, and `panels_pedestal_LIT.png` really is entirely black. So the
  pedestal *housing* is not on the panel — only the screen faces, which live in
  `cockpit_INN.obj` with everything else. Do not go looking for MCDU geometry in
  the pedestal object.
- `cockpit/OpenGLTextures/MCDU_Popup.png` is ToLiss's MCDU art: bezel bitmap, lit
  mask, and a **glyph font atlas**. The MCDU is composed character by character,
  the same way `PFD_ADI.png` and `ND_Main.png` build the PFD and ND — and, as it
  turns out, into the same panel FBO. Its screen cut-out is ~295×270 px, an
  aspect of 1.09, which matches the derived island's 276/257 = 1.07 and is the
  independent corroboration that these two faces are the MCDUs.

## 6a. Still to map: the rest of the cockpit's digital readouts

**Every digital readout in the cockpit lands on this same render target.** The
FCU row is the first three faces of many, and nothing about the mechanism is
specific to them — the panel FBO carries the whole 4096 atlas, and any face whose
UV island can be derived can be tinted, bled or overlaid by exactly the machinery
in §5a and §8.

So the standing task is to **derive the remaining islands and name them**:
the radio management panels, the transponder and TCAS readouts, the standby
instruments, the rudder-trim readout, and whatever else the survey turns up. This
is a derivation job, not a design one — the list above is a *starting guess* and
must not be treated as findings. Nothing here is confirmed until it has been
aimed at in-sim, the same standard the six DUs and the FCU row were held to.

The clock, the EFB tablets and the two MCDUs have since been mapped and are in
§2a. ⚠ **Read §6b before running the survey** — the MCDUs were missed on the
first pass because of two flaws in this very script, both now fixed, and the
failure mode was a confident wrong answer rather than an empty result.

⚠ **`is_display()` in `derive_panel_rects.py` will not find them.** Its filter
requires **more than 200 px in both u and v**, which is right for the display
units and wrong for everything else: the FCU strip is 66 px tall and the baro
windows are 127 px wide, so *none of the three faces this document is about*
appear in the default listing. They were found with a loosened filter over the
glareshield band. `--all` exists for that — it drops the display heuristic and
lists every `ATTR_cockpit` island above a small floor, which is the right tool
for the survey and the wrong one for reading a DU rect off.

Two things to check per face as it is added, both of which the FCU row happened
to pass and neither of which is guaranteed:

- **Is it inside an `ANIM` block?** Check the nesting depth over the *whole file*
  at its TRIS line. `--gating` only looks back 40 lines, which is enough to show
  a block it finds but not enough to prove there is none.
- **Is it resolution-switched?** If a second island of the same face exists at a
  different size, it needs a row in `kPanelRectsLo` too.

## 7. Relationship to the bleed effect

The intended sibling feature — backlight bleed around each DU's edge — uses the
same primitive: a quad in panel coordinates over a known rect, in the lit pass.
The differences are blend mode (additive `GL_SRC_ALPHA, GL_ONE`, since bleed is
light leaking out rather than color being removed), geometry (a gradient ring
rather than a single quad), and brightness input (explicit `DUBrightness[du]`
scaling, since additive does not self-scale the way multiply does).

All six DU faces are **square** (748×751, 623×626, …; aspect 1.00 to within a
pixel), so one square gradient texture — or one ring mesh — serves all six with no
aspect correction.

A texture is probably unnecessary for bleed: XPLM ships no image loader, so a PNG
means vendoring a decoder, whereas vertex-color alpha on a ring of quads gives a
soft inward gradient directly and is tunable by numbers rather than by re-exporting
art. Go to a texture only if an irregular, non-uniform bleed pattern is wanted.

---

## 8. The layer compositor — **Dev window ▸ Panel FX**

Added 2026-07-31, once multiply had proven itself in-sim and the next thing
wanted was an additive pass on top of it (§3). One hard-coded blend over one
rect was never the shape of this feature; a second hard-coded pass beside the
first would only have to be undone at the third.

**The model.** A **target** — a rect or a group, from the shared index space in
§2 — carries an ordered stack of **layers**, each a color, a blend mode and an
opacity. The compositor walks every stack once per lit-pass frame from the
after-Gauges callback. Layer 0 composites first and later layers see its result;
**the editor lists them top-down**, so the topmost row is the last to draw, the
way a paint program's layer panel reads. Only the display is reversed — the
vector, the save file and the `Log stack` dump all stay in composite order, and
the index shown on each row's drag grip is that composite index.

| Mode | `glBlendEquation` / `glBlendFunc` | Image? | What it is for |
|---|---|---|---|
| **Multiply** | `ADD` · `DST_COLOR, ONE_MINUS_SRC_ALPHA` | ✓ | Darkens only. Black masks itself — the §3 trick. |
| **Add** | `ADD` · `ONE, ONE` | ✓ | Brightens; cannot suppress a channel. Multiply's partner. |
| **Screen** | `ADD` · `ONE_MINUS_DST_COLOR, ONE` | ✓ | Brightens with a roll-off instead of clipping. |
| **Normal** | `ADD` · `ONE, ONE_MINUS_SRC_ALPHA` | ✓ | Flat alpha blend, ignores what is under it. Premultiplied, hence `ONE` and not `SRC_ALPHA`. |
| **Subtract** | `REVERSE_SUBTRACT` · `ONE, ONE` | ✓ | `dst − src`. Removes a color instead of scaling it — take a little blue out to warm a readout without changing how bright it is. |
| **Burn** | `REVERSE_SUBTRACT` · `ONE, ONE`, emitting `1−color` | ✗ | Linear burn, `dst + src − 1`. Deepens harder than Multiply and reaches black sooner. |
| **Darken** | `MIN` | ✗ | `min(src, dst)` per channel. A **ceiling** — caps each channel and leaves anything already darker alone. Clamps a blown-out white. |
| **Lighten** | `MAX` | ✓ | `max(src, dst)` per channel. A **floor** — a minimum glow that never touches anything brighter. |

**That is the entire budget**, and it is worth knowing why. The compositor draws
a quad over the panel FBO, so all it can express is what the fixed-function
blender computes: `src·sf OP dst·df`, with the factors chosen per *draw*.
**Overlay, Soft/Hard Light, Color Burn/Dodge, Hue/Saturation/Color/Luminosity
and anything gamma- or contrast-shaped are not possible** — every one of them
needs the destination pixel as a shader *input* (a branch or a divide on `dst`),
which means binding the panel FBO as a texture and running a fragment shader.
That is a different feature, and one X-Plane gives no hook for. Don't add a
mode that approximates one of those; a mode named "Overlay" that isn't is worse
than its absence.

Things in there that are load-bearing:

- ⚠ **Every mode fades to a no-op at opacity 0**, which means fading toward
  *that mode's own identity*, not toward black. Get this wrong and it doesn't
  look like a wrong number — it looks like the opacity slider doing the opposite
  of what it says.

  Six of the eight carry it the same way: the quad emits `color × opacity` and
  the identity is black, so opacity scales plainly. **Multiply pays for it in the
  blend func rather than in the color** —
  `src=DST_COLOR, dst=ONE_MINUS_SRC_ALPHA` with a quad of `(C×o, o)` gives
  `dst×C×o + dst×(1−o)`, i.e. exactly the old "interpolate the color toward
  white" arithmetic, but **per pixel**. That is the whole reason Multiply can
  take an image (§8c). **Darken is the one left fading in the color**, because
  `MIN` ignores the blend factors entirely — and that is why *it* can't.
- ⚠ **The blend EQUATION is saved and restored exactly like the func**, and it
  matters more: a leaked `GL_MIN` or reverse subtract turns every subsequent
  blended draw in the pass into a darkening operation — a whole-cockpit
  artifact, not a wrong-looking rect. `gBlendSaved` carries both.
- **`glBlendEquation` has no GL 1.1 substitute**, unlike the func. On a driver
  that doesn't export it the four equation modes are **skipped** and a single
  log line says so; drawing them additively instead would give a mode named
  "Darken" that brightens.
- **The alpha channel is never written.** Every mode is set through
  `glBlendFuncSeparate` with alpha factors `ZERO, ONE` and
  `glBlendEquationSeparate` with a plain `ADD` on the alpha half, so `dst.a`
  passes through. Additive would otherwise saturate it and `MIN` would floor it.
  Without those entry points the alpha rides the RGB half — the pre-existing
  behavior, and better than refusing to draw — so each mode picks a source
  alpha (1 for the min-side modes, 0 for the add-side ones) that survives that
  fallback.
- **The compositor is NOT gated on the probe's knobs.** It runs whatever
  `panel_probe` is set to, because turning the diagnostic off to look at the
  effect must not turn the effect off with it. The probe still paints *over* the
  FX when both are on: a diagnostic fill that were silently modulated by an
  effect stack would be worthless.

### 8b. The target hierarchy

Targets **nest**, so "warm every display" and "warm the SD only" are different
operations rather than two entries in a flat list that happen to overlap.

Nesting is **derived from containment, never declared**: `PanelTargetMask()`
gives the set of rects a target paints, one bit each, and a target *contains*
another when its mask is a superset. `Screens` ⊃ `capt PFD` falls out of the
rect names; nothing states it. So adding a group is enough to place it in the
tree, and no group can end up claiming a rect it does not actually paint. The
groups are `Whole panel` ⊃ { `Screens` ⊃ the six DUs, `FCU row` ⊃ the strip and
the two baro windows }.

- ⚠ **Stacks composite BROADEST FIRST**, not in the order they were added. That
  is what makes the hierarchy mean anything: a layer on `Screens` is a base
  every display receives and a layer on `SD` refines what the base already put
  down. Creation order would make the result depend on which target happened to
  be clicked first, so two sessions building the same two stacks would look
  different. The sort is stable and `Log stack` uses it too — reading a tuning
  log in a different order than the compositor applied it is how a stack gets
  transcribed back into source wrong.
- ⚠ **Sibling groups must be disjoint or nested — never partially overlapping.**
  A group spanning half of each existing one ("everything on the captain's
  side") is fine for the compositor, which only sorts by breadth, but has no
  single place in a tree: `capt baro` would have to appear under two parents.
  Add one only together with that handling.
- **Every target is always listed**, whether or not it carries layers; one with
  none is dimmed. The earlier pane split them into a "current" list and an "Add"
  list, which put a group and its own members in different places and lost the
  one thing a tree is for.
- **Selecting a target creates NOTHING** (changed 2026-07-31). It used to start a
  stack with one layer in it, so selecting and adding were the same gesture —
  which meant every mis-click on the way down the tree left a real, saved,
  invisible-until-tuned layer behind on a target nobody meant to touch. A stack
  begins to exist when something is actually put in it: `Add layer`, `Paste
  layer`, or a stack paste. The layer pane for an empty target is the same pane
  with those buttons and no rows.
- **The selection is a target, not an index into the stack vector.** Deleting a
  target shuffles that vector, so a stored index would silently re-point the
  layer pane at someone else's layers — the editing form of the retargeting bug
  the save format's name lookup exists to prevent. It also means the selection
  survives a target having no stack, which is what the previous point needs.
- **Targets are deletable** from the `x` on their row, from the right-click menu,
  and from `Delete target` in the layer pane. Deletion is deferred to after the
  tree walk: erasing mid-walk invalidates the stack the recursion below it is
  about to read.
- **A whole stack copy/pastes between targets** — right-click any row. Copy takes
  a *copy* of the layers, not a reference to the target, so the source can be
  retuned or deleted afterwards without changing what pastes. Two pastes, because
  they are genuinely different edits and neither is guessable from context:
  **replace** makes this target match the source, **add on top** refines what is
  already here. The layer pane carries `Copy stack` / `Paste stack` for
  discoverability, with the pane's paste being the non-destructive one — replace
  stays on the right-click menu where it cannot be hit by reflex. This exists
  because siblings want identical stacks (the six DUs, the two MCDUs) and
  rebuilding a three-layer stack by hand is how five of six end up matching and
  the sixth quietly does not.
- **The `?` on each row floods that target magenta** so an inferred name can be
  checked against the cockpit (§6b: the in-sim flood settles in one look what the
  geometry argues for a page). It drives the *probe*, not the compositor — a
  layer would have to be added, tuned to something visible, and removed again.
  Clicking it on the target it is already showing turns the probe back off, and
  the button is magenta while it is the active one; without that it had no
  visible off state and the way back was a different tab.
- ⚠ **Those two row buttons need `ImGuiTreeNodeFlags_AllowOverlap` on the node.**
  `SpanAvailWidth` extends the node's hit box over them, and ImGui gives hover to
  the *first* claimant — so both buttons drew at full opacity and were completely
  dead: no hover, no tooltip, no click, nothing logged. Full write-up in
  `src/native/README.md` ▸ Dear ImGui.

**Persistence** is `Resources/plugins/ToLissPhoton/panelfx.txt` — beside the
`.xpl` and next to the overlay images its layers name, **not** in
`Output/preferences` (where it lived until 2026-08-01). It is a shipped artifact
an installer upgrade replaces, on the same footing as those images; a user's own
choices go to `ToLissPhoton_profiles.json` under `"$displays"` instead.

It is saved on every edit rather than behind a Save button — the point of the tool
is that a look survives the sim restart that in-sim tuning requires, and a tuning
pass lost to a forgotten click is exactly the friction it exists to remove.
Targets are stored **by name**; an unknown name is dropped with a log line rather
than silently retargeted. **Reload** re-reads the file, and **Log stack** dumps the
whole thing to `Log.txt` in the same syntax, so accepted numbers reach source
without being reconstructed from memory.

⚠ **Only a `PHOTON_DEV` build writes it.** `SavePanelFx` is inside the guard and
nothing user-facing calls it. Two reasons and both bite silently: an installer
upgrade would clobber whatever a user's session wrote, and on a dev machine the
file is a **symlink into `src/native/panelfx.txt`**, so anything a user-facing
control wrote would commit itself.

⚠ **That symlink is how a tuning pass reaches source control**, with no export
step — which also means the loop can break invisibly. `deploy.ps1` creates and
maintains it, but Windows PowerShell 5.1 never passes
`SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`, so under 5.1 it demands elevation
even with Developer Mode on and falls back to a **copy**; after that every edit
goes to a file `git status` will never mention. `deploy.bat` prefers `pwsh` for
this reason alone, and the script prints which of the two it did. The installer
skips a symlinked copy on both install and uninstall.

⚠ Same consequence as the probe's prefs: **any build can come up already
compositing** — and now that includes a release, which is the point. The startup
log says how many stacks were restored, and the master checkbox is the top-left
control in the Panel FX tab.

### 8c. Overlay images (2026-07-31)

A layer can be an **image** instead of a flat color: a gradient, a vignette, a
scanline pattern, a hand-painted sheen. Everything else about the layer is
unchanged — the image **modulates** the layer color (fixed-function
`GL_MODULATE`, `src = texel × color`), so the color still tints and the opacity
still fades.

Images live in **one folder next to the plugin**,
`Resources/plugins/ToLissPhoton/overlays/`, and are picked by name from a combo
in each layer's row. No file dialog: ImGui has no portable one, a typed path is a
support burden, and a drop folder is what makes a look reproducible on another
machine. `deploy.ps1` seeds it with four starter images generated by
`build/make_overlays.py` — copied **by name**, never as a folder mirror, because
that folder is also where you put your own.

Decoding is [stb_image](https://github.com/nothings/stb), vendored at
`src/native/third_party/stb/`. The SDK has no image loader at all: XPLM4 dropped
`XPLMLoadTexture` and left only `XPLMGenerateTextureNumbers` /
`XPLMBindTexture2d`, which hand out a texture *number* and expect the pixels
already decoded. It is inside `#if PHOTON_DEV`, so the release `.xpl` does not
change size at all.

Load-bearing details:

- ⚠ **Alpha is coverage, and images are uploaded PREMULTIPLIED.** Every mode
  carries the layer's contribution in the color channels and its coverage in
  alpha, so a texel at alpha 0 must contribute nothing to *either* — and a
  straight-alpha PNG happily stores bright RGB under a zero alpha, which would
  then be added or multiplied in at full strength. Premultiplying on upload is
  the one place that can be fixed once for all the modes. It is also why
  `Normal`'s source factor is `ONE` rather than `SRC_ALPHA`.
- ⚠ **An overlay's shape lives in its ALPHA channel**; its RGB is usually just
  white, because the layer color does the tinting. Hence `vignette_edges.png`
  being opaque at the *edges*: the layer supplies the dark color and Multiply,
  and the image only says where it lands. Inverted, it would darken the middle of
  a readout and leave the frame alone.
- ⚠ **Burn and Darken cannot carry an image**, and it is a property of
  fixed-function GL rather than an unfinished corner. Burn emits the
  *complement* of the layer color (that is how `dst + src − 1` folds into one
  reverse subtract) and `1 − texel` is not a modulate; Darken is `GL_MIN`, whose
  opacity must fade the emitted color toward white — per *pixel* once an image
  is involved, which modulate also cannot express. Both would need
  `ARB_texture_env_combine` or a fragment shader. A layer in that state is
  **skipped**, and the editor says so on the row, for the same reason a mode
  named "Darken" must never brighten.
- **One texture unit for the whole pass.** A solid layer binds a 1×1 opaque
  white texture rather than switching the unit off, which makes it
  arithmetically identical to a textured one. Toggling per layer would mean
  calling `XPLMSetGraphicsState` *inside* the loop, i.e. re-entering X-Plane's
  blend tracker while our own func is set — the ordering trap `PushBlendFunc`
  exists to avoid.
- **The texture env is saved and restored like the blend func.** It is shared
  state and not part of `XPLMSetGraphicsState`.
- **Layers are saved by file NAME**, so renaming a file orphans the layers using
  it. Deliberately, and reported: the row shows `[MISSING]` and the layer is
  skipped rather than quietly drawn as a solid, which would look like a tuning
  change nobody made.
- ⚠ **Refresh is MANUAL, and it is ONE operation (2026-08-04).** It was live: the
  pool `stat()`ed its files once a second *from the compositor*, on the reasoning
  that what you watch while iterating on an overlay is the cockpit and the Panel
  FX tab is usually not the visible window. That reasoning is sound and it
  describes **authoring a look**, which is a developer activity — a shipped
  install's overlays change when the installer replaces them and at no other time,
  so every user was paying a per-second filesystem walk on the render thread for
  it. `ScanFxTextures` already compared stamps and marked changed files stale, so
  `Rescan images` was always doing the whole job — adding, renaming *and* editing
  — and the two-operation split was the part that needed explaining rather than
  the part that earned its keep. Size and mtime are both still compared: a repaint
  that keeps the pixel count keeps the size, and unzips and some tools preserve
  the mtime.
  - `Rescan images` re-reads the folder and re-uploads any file whose bytes
    moved; its shift-click forces a re-read of *every* image, for the stamp-proof
    case of a tool that rewrites a file to the same size and restores the mtime.
    Clicking a layer's thumbnail re-reads that one image.
  - ⚠ **The initial scan runs from `LoadPanelFx`, not on first draw.** It walks a
    directory, may create it and may write a README; the lazy scan
    `FxTextureByName` still carries as a backstop would do all of that inside the
    panel pass, on the first frame a layer with an image is drawn, in a release
    build.
  - ⚠ **There is deliberately no `PollFxTextureChanges` any more.** Its whole body
    was a subset of `ScanFxTextures`, and two code paths deciding what "changed"
    means is how this grows back.
  - ⚠ **A reload uploads into the SAME texture number.** Nothing here can free
    one — X-Plane hands them out and the ImGui backend takes the same line with
    its font atlas — so allocating a fresh one per save leaks one number per
    keystroke-and-save, and every layer, the thumbnail and the compositor would
    have to be told the new value.
  - **A file is stamped before it is read, on failure paths included**, so the
    stamp means "the bytes we last looked at". An undecodable file is then
    retried when it *changes* rather than once a second forever, and a decode
    that fails on reload keeps the pixels already on the card — the least
    surprising thing a half-written PNG can do. A save is not atomic, so the
    upload re-stamps afterwards and asks for one more pass if the file moved
    underneath it.
- **The image name is the LAST field on the layer line** and is read with
  `getline`, because file names contain spaces. `-` is the no-image marker; an
  empty tail would be indistinguishable from a truncated line. A layer written
  before images existed simply ends after the opacity — ⚠ and the tile value is
  read into a *local* first, because `operator>>` zeroes its target on failure
  and a tile of 0 samples a single texel, which looks like a flat color rather
  than like a defaulting bug.

### 8d. Brightness and power drivers (2026-08-01)

A tint that stays at full strength while the screen behind it is dimmed to nothing
is the single most obvious way this feature reads as fake: at 3 a.m. with the PFD
knob at its stop the display is black and the tint is still there, glowing on its
own. So a target's layers can **follow a dataref**.

Two drivers per target, both the same struct because they read the same way and a
wrong one has to be visible in the same place either way:

| driver | effect |
|---|---|
| `bright` | scales every layer's opacity: `lo`..`hi` maps to 0..1, with a floor |
| `power` | skips the target entirely while the value is not above a threshold |

`gFxFollowBrightness` (the "Follow display brightness" checkbox) is the master
switch. It defaults ON — a tint that ignores the knob is the bug, not the feature —
and turning it off is for picking a color without having to hold a rheostat up.

**The lookup is per RECT, not per target.** `PaintPanelTarget` hands the paint
function the rect's index, so a group dims each member by that member's own knob:
a layer on "Main displays" follows six DU rheostats at once. Setting a driver on
the stack overrides that for the whole target, which is what you want on a group
whose members really do share one knob.

- **`FxQuadColorFor(layer, o)` was split out of `DrawFxLayer` for this.** Every
  mode fades to *its own* identity (white for Multiply and Darken, black for the
  rest), so a dimmed quad has to be the layer color recomputed at the scaled
  opacity — fading the quad toward black would make Multiply *darken* as it
  dimmed. The blend func and equation depend only on `layer.blend`, so the state
  change stays outside the per-quad loop.
- **A rect with no driver, or the master switch off, costs one compare**; a fully
  dark screen draws nothing at all.

**Where the numbers come from.** `kPanelSources` maps rect NAME → dataref, read off
ToLiss's plugin binary (`AirbusFBW_A320_XP11.xpl`, string scan):

| face | brightness | power | bus |
|---|---|---|---|
| the six DUs | `AirbusFBW/DUBrightness[kScreenDU]` | — | `DCBusVoltages[0] > 24` |
| ISIS | `AirbusFBW/ISIBrightness` | `AirbusFBW/ISIAvailable` | — |
| MCDU capt / FO | `AirbusFBW/MCDUScreenBrightnessArray[0,1]` | — | — |
| MCDU 3 (A330 only) | `AirbusFBW/MCDUScreenBrightnessArray[2]` — ⚠ the INDEX is an assumption; an index past the end reads UNREADABLE, i.e. undimmed, and says so in red | — | — |
| DCDU capt / FO | `ToLissPhoton/screens/dcdu_brightness[0,1]` — Photon's own, counted from the CPDLC buttons | `AirbusFBW/HasCPDLC` | `DCBusVoltages[0] > 24` |
| FCU strip, both baro | `ckpt/fcu/lights/right/anim` **(1..270)** | `AirbusFBW/FCUAvail` | — |
| …the same three on an A330 | `AirbusFBW/SupplLightLevels[1]` **(0..1)** — user, 2026-08-13; a family-scoped override row, §4a | inherited | — |
| RMP1 / 2 (pedestal) | `AirbusFBW/PanelBrightnessLevel` | `AirbusFBW/RMP<n>Available` | — |
| RMP3 (overhead) | `AirbusFBW/OHPBrightnessLevel` | `AirbusFBW/RMP3Available` | — |
| XPDR code | `AirbusFBW/PanelBrightnessLevel` | — (see below) | `DCBusVoltages[0] > 24` |
| clock, DME windows | `AirbusFBW/PanelBrightnessLevel` | — | — |
| ped strips | `AirbusFBW/PanelBrightnessLevel` | — | — |
| rudder trim | `AirbusFBW/PanelBrightnessLevel` | — | `DCBusVoltages[0] > 24` |
| BAT1 / BAT2 | `AirbusFBW/OHPBrightnessLevel` | `AirbusFBW/BatVolts[0,1] > 3` | — |
| FMS Load, ovhd aft L1 / L2 | `AirbusFBW/OHPBrightnessLevel` | — | `DCBusVoltages[0] > 24` |

**Revised 2026-08-02 (user).** Three changes worth their own note, because each
replaced a name that resolved perfectly and read the wrong quantity:

- **Integral lighting follows THE PANEL, not the unit.** `RMP<n>Lights` and
  `PedestalFloodBrightnessLevel` were per-unit and per-lamp readings of faces that
  are in fact lit by the panel they sit in — so the pedestal radios, the
  transponder, the rudder trim and the pedestal strips are all on
  `PanelBrightnessLevel`, and **RMP3, which is on the overhead**, is on
  `OHPBrightnessLevel` with the BAT windows. The `RMP<n>Available` power gates
  stayed: availability really is per unit.
- **The two BAT voltmeters gate on their own battery** (`BatVolts[0]`/`[1] > 3`).
  A BAT window with a flat battery behind it is blank, not dim, and 3 V separates
  "there is a battery here" from "there is not" with room either side.
- **Five more faces joined the bus gate** — see the bus row above and §8d's gate
  table. The rule is "does this face go DARK with the bus", which is a question
  about the readout being driven, not about it being a screen.
- ⚠ **The transponder's power gate was REMOVED, and must not come back.**
  `AirbusFBW/XPDRPower` is the STBY/AUTO/ON selector — whether the unit is
  *interrogating* — and the code window stays lit and readable in STBY, so gating
  the tint on it blanked a face the pilot can plainly see. This is the fourth
  instance of the same shape on this page: a name that resolves, reads a sensible
  number, and answers the question next door. The bus gate covers the face
  instead, and `test_the_transponder_has_no_power_source` holds the line.

- ⚠ **Only the DU indices are settled in-sim.** Everything else is a name that
  exists and looks right; what each one *means* — 0..1 or 0..100, the screen's own
  brightness or its panel's integral lighting — is unverified. Hence the editor
  shows the live value and the resulting factor per rect, and every row is
  editable. Fix it there, then in the table.
- ⚠ **`lo`/`hi` are not decoration, and the FCU row proves it.** Its source is the
  raw knob animation and runs **1..270**, not 0..1. Every other row here happens
  to be a 0..1 fraction, which makes it easy to assume the mechanism only ever
  normalizes one. `lo` **above** `hi` is the supported way to say a source is
  inverted — that is the fix if a tint fades the wrong way round, not a sign flag.
- ⚠ **`ckpt/fcu/lights/right/anim` could not be found in the install here**
  (2026-08-01, reported by the user, set as asked). No `ckpt/*/lights/*/anim` on
  any of the four ToLiss airframes present carries that name, and every `ckpt`
  light-knob animation that does exist keys 0..1 or 0..2, never 1..270 — so it is
  presumably a newer aircraft build or another add-on. The failure is safe and
  announces itself: an unresolvable driver logs, reads as factor 1 (undimmed,
  never black) and shows **UNREADABLE** in red in the source pane. The names that
  do resolve are `ckpt/fcu/integLight/anim` (the FCU's own integral-light knob
  animation) and `AirbusFBW/FCUIntegralBrightness`, both 0..1.
- ⚠ **A driver reads through `FxReadScalar`, which prefers the WIDEST advertised
  type** — double, then float, then int — unlike `ReadDataRefTyped`, which tries
  int first and is right to, because its callers read flags. A driver reads a
  *knob*: a dataref registered with both an int and a float reader (ToLiss does
  this, as does this plugin on its own category refs) read through the int one
  **truncates**, so a 0..1 brightness is 0 across the whole travel of the knob bar
  its very top. The symptom is a tint that never appears, with no error, nothing
  in the log, and a dataref inspector showing a perfectly healthy value the whole
  time. Nothing a driver reads is a count.
- ⚠ **The source pane shows each driver's LIVE VALUE, not just its name**
  (2026-08-01). It did not until then, and the omission cost a debugging session:
  a driver that resolves perfectly and reads the wrong quantity is
  indistinguishable from one that never bound, when all you can see is a name and
  a combined factor. The factor column also **names which driver zeroed it**
  (`x0.00 power` vs `x0.00 bright`) — both roads end at zero and they need
  opposite fixes. `LogPanelFx` dumps the same strings.
- ⚠ **Handles are dropped on every aircraft load.** They point into ToLiss's
  plugin, which goes away with the aircraft. This is the one path in the feature
  that could crash the sim rather than look wrong.
- **Unreadable = factor 1, never 0.** A missing dataref leaves the tint undimmed
  and says so in the log; blanking it instead would produce "everything went black
  and nothing explains why", which is much harder to chase.
- The wire format gains two optional per-target lines, written only when set:
  `bright <dataref> <index> <lo> <hi> <floor>` and `power <dataref> <index> <above>`.
  ⚠ The dataref name comes FIRST and is read with `>>`, unlike a layer's image name
  — dataref paths cannot contain spaces, image file names routinely do.

#### The response curve

A knob reads 0..1 linearly; the glow a screen throws does not follow it linearly.
Fed raw, the tint came up almost at once and then barely changed across the top
half of the knob. So the normalized value goes through **`kBrightnessCurve`**,
knots (asked for 2026-08-01, x = knob, y = opacity):

```
0.0 -> 0.0     dead
0.2 -> 0.0     the glow starts here
0.5 -> 0.2     half a turn is still dim
1.0 -> 1.0
```

- **The six screen-glow spill lights run through the same curve** (`gScreenSizeFactor`
  — the curve shapes their SIZE since 2026-08-09, and there unconditionally, no dev
  switch). That is the point of sharing it: a
  tint and the spill underneath it come up *together*, and nothing in the sim
  would flag them drifting apart. `tests/test_brightness_curve.py` pins both ends.
- **Order inside `FxDriverFactor` is `lo`/`hi` → curve → floor.** Normalizing
  first or the curve is handed a value in the source's own units; the floor lifts
  afterwards or "never fully off" stops meaning that once the curve has flattened
  the bottom of the knob.
- ⚠ **The interpolation is MONOTONE cubic (Fritsch–Carlson), and both alternatives
  are wrong in a way you would see.** Plain Catmull-Rom/Hermite **undershoots to
  about −0.012** across the dead zone, because the middle knot's tangent pulls the
  flat first segment down — and a clamped undershoot is a visible step in the
  middle of a fade, the exact artifact this curve exists to remove. Piecewise
  linear has a 4× slope break at the middle knot, which reads as a pop.
  Fritsch–Carlson cannot leave the knots' own range by construction, and it
  flattens the take-off for free: a zero secant pins both of its tangents to zero,
  so the curve leaves 0.2 horizontally instead of with a corner. A test computes
  the Catmull-Rom dip rather than taking this paragraph's word for it.
- **The knots are the spec.** No exponent, gain or dead-zone constant is derived
  from them anywhere, so reshaping the fade is editing four numbers.
- The tab has a **`Curve` checkbox** (persisted as `curve <0|1>`) and plots the
  live shape beside the per-rect factor table. It is the **default** for targets
  left on Inherit (below) — and it does **not** reach the spill lights, so a dev
  session left with it off cannot ship one.

#### Per-target overrides of both switches (2026-08-01)

Neither switch is a look that suits every face. The FCU's integral lighting does
not track its readout brightness the way a DU's does, so the right behavior there
is "tint at full strength whenever the FCU is powered" — while the DUs go on
following their knobs. So `follow` and `curve` are **tri-states on `FxStack`**,
not two more globals:

| stored | means |
|---|---|
| `kFxInherit` (−1, the default) | use the global checkbox, and keep tracking it |
| `kFxOff` (0) | `follow`: full strength when powered. `curve`: linear `lo`..`hi` |
| `kFxOn` (1) | `follow`: dim with the driver. `curve`: through `kBrightnessCurve` |

- ⚠ **Inherit is not "the master's value, copied when you set it".** A target left
  on Inherit follows the master afterwards. That is the whole reason for a third
  state: `gFxFollowBrightness` is an escape hatch you flip while picking a color
  and flip back, and if setting one target's switch froze every other target's,
  the hatch would quietly stop working.
- ⚠ **`follow = Off` does NOT disable the power gate.** `FxRectFactor` evaluates
  power first and only then consults `follow`, because "always on" means always on
  *when powered* — returning 1.0 ahead of the gate would paint a tint over a dead
  screen, which is the artifact the whole driver mechanism exists to remove. A
  test pins the ordering.
- **The master switch still outranks a target, in one direction only.** With
  "Follow display brightness" off, nothing follows and nothing is power-gated, even
  a target set to On. A per-target setting that could beat it would make the hatch
  look broken months after the target was set, with nothing on screen linking the
  two.
- ⚠ **`FxDriverFactor` takes the curve flag as a PARAMETER** rather than reading
  `gFxCurve`. Two stacks can share one driver and want different shapes; a global
  read there would make the second silently follow the first.
- The wire format gains `tfollow <0|1>` and `tcurve <0|1>`, per target, written
  only when not Inherit. ⚠ **Their own keys, not the global `follow`/`curve`** —
  parsing dispatches on the key alone, so a per-stack line spelled `follow` would
  be read as the master switch. Inherit is *absence*: writing it would bake today's
  global into every stack the first time anything saved.
- The UI is two combos at the top of "Brightness / power sources", whose Inherit
  entry **names the value it is inheriting** ("Inherit (Curved)"), because the
  master lives in a different pane and the question this one answers is what *this*
  target is doing. `LogPanelFx` prints the resolved pair per target with
  `(inherited)`/`(target)` beside each.

### 8e. The ambient day/night blend (2026-08-02)

A tint authored at night is wrong at noon, and not by a little. The effects that
sell a backlit LCD in the dark — bleed, a lifted black, a cool cast — are exactly
what a sunlit screen does *not* do: it washes out, its contrast collapses, and the
glass starts reflecting the cockpit rather than glowing. So **a layer can carry a
second color and opacity for full daylight**, and the compositor interpolates.

**Where the number comes from.** `sim/graphics/misc/cockpit_light_level_{r,g,b}` —
the sim's own "how much daylight is in this cockpit", already accounting for
terrain and cloud. ⚠ **This is what ToLiss reads too**: those three names are in
the AirbusFBW plugin binary alongside `outside_light_level_*`. Reading the same
quantity the aircraft reads is why the two cannot disagree about dusk.

- **MAX of the three channels, not the average.** The night tint is strongly
  blue-shifted, so an average reads a moonlit cockpit as darker than it looks, and
  max degrades gracefully if a future sim version stops writing one channel.
- ⚠ **It is an INTERPOLATION, not a switch.** Dusk is where every cockpit lighting
  effect is judged, it lasts twenty minutes, and a threshold would pop mid-approach.
- **`gFxAmbientLo`/`Hi` (0.15 / 0.70) are the calibration**, not 0 and 1: the raw
  value sits well above zero under any moon and tops out long before the sun is
  overhead, so almost none of its travel is in the range that matters. `lo` above
  `hi` inverts, exactly as on an `FxDriver`.
- ⚠ **Smoothed, with `gFxAmbientTau` (0.5 s), and that is not cosmetic.** The raw
  value jumps the instant a cloud crosses the sun or the aircraft banks, and every
  screen in the cockpit would shift color with it. The step is taken against the
  **wall clock** (`XPLMGetElapsedTime`), so the fade runs at one speed at 20 fps and
  at 120, and a loading pause does not bank up a jump. **The first sample snaps** —
  fading up from "night" over the first two seconds of a noon departure is a visible
  artifact with nothing to smooth away yet.
- **Sampled once per compositor pass**, at the top of `DrawPanelFx`, rather than
  from a flight loop: fresh exactly where it is used, nothing to keep in step, and
  lazily resolved — so it needs **no line in `AutoLoop`'s 1 Hz retry list**. These
  are X-Plane's own datarefs, so they are also *not* dropped by `FxForgetDrivers`;
  the optional `ambientref` override is, because it can name anything.
- **Unreadable holds the last value**, never collapses to 0 — the same house rule
  the drivers follow. A missing dataref must not repaint the whole cockpit.
- ⚠ **The sun-elevation gate** (2026-08-11): `cockpit_light_level_*` is
  **analytic** — computed from the light sources, not metered off the rendered
  frame — so a red-heavy dome lamp slams one channel to ~1.0 and the MAX calls it
  daylight: the screens swapped to their day look at midnight because a cabin
  lamp was on. The blend **target** is therefore multiplied by a smoothstep of
  `sim/graphics/scenery/sun_pitch_degrees` across `gFxAmbientSunLo`/`Hi`
  (−5°/+5° by default; editable beside the calibration pair, wire
  `ambientsun <lo> <hi>`, always written). Deliberate properties: it scales the
  TARGET, inside the sampler, so dawn arrives through the same smoothing and the
  first-sample snap; daytime behavior is untouched (clouds, shadow and a hangar
  still read through the light level — the gate only forbids "day" while the sun
  is down); equal ends make a hard step, **both at −90 hold it open (the "off"
  position)**; an unresolved ref holds it **open** (same house rule as above);
  sim-owned, so lazily resolved once, no `AutoLoop` retry line, not dropped by
  `FxForgetDrivers`. The manual Pin outranks it — it is read in `FxAmbientNow`,
  which the pin bypasses — so the daylight half is still authorable at 02:00.

**Only the color and the opacity vary.** The blend mode and the image do not, and
that is a deliberate limit: there is no defined halfway point between `GL_MIN` and
additive, and cross-fading two textures would need a second texture unit inside a
loop this file works to keep flat.

⚠ **It costs nothing, because opacity 0 is a real state on both sides.** A
night-only layer has `dayOpacity` 0; a **daylight-only** layer has `opacity` 0 and
a `dayOpacity` above it. So "additive bleed after dark, a subtractive wash at
noon" is **two layers with opposite day settings, not two stacks** — and the two
still composite in the authored order while both are partly present at dusk. This
is why the design is per layer rather than a second layer list per target.

- ⚠ **`FxQuadColorFor` takes the blend mode and a COLOR, not an `FxLayer`.**
  Passing the layer would compile and would draw the night color at every hour
  with only the opacity following the sun — a half-working blend is much harder to
  spot than one that does nothing. A test pins the signature.
- ⚠ **`DrawFxLayer` tests the DAY-BLENDED opacity**, not the authored one. Skipping
  on `layer.opacity` would mean a daylight-only layer never draws — the day half
  silently not working, in exactly the configuration it exists for.
- **A layer with no day half is returned untouched** (`if (!l.hasDay) return n;`).
  That is the backward-compatibility guarantee: every layer in the shipped
  `panelfx.txt` predates the blend, and lerping toward the struct's default day
  values would fade the whole look toward white by mid-morning, in a release.
- Wire format: **`day <r> <g> <b> <opacity>` on its own line under the layer**, plus
  a global `ambient <lo> <hi> <tau>`, a global `ambientsun <lo> <hi>` (degrees of
  sun pitch, read into locals and adopted whole — the ramp rule) and an optional
  `ambientref <dataref> <index>`.
  ⚠ **A line of its own, not extra columns** — the layer line ends with an image
  *name* read by `getline` precisely because names contain spaces, so nothing can
  ever follow it there. Same shape as `tfollow`.
- The editor is a folded "Ambient day / night blend" pane on the Panel FX tab. Its
  **Pin** control holds the blend at a value so the daylight end of a look can be
  authored after dark; the raw reading keeps updating behind it, because otherwise
  pinning freezes the readout that tells you what to pin it to. Dev-only, never saved.

### 8f. Per-layer dimming (2026-08-02)

`follow` now exists at **three** levels — the master switch, the target, the layer —
and each `kFxInherit` resolves to **the next one out** (`FxLayerFollows` reads
`FxStackFollows`, which reads the master). The master still outranks all of it.

It exists because the two things a layer does are not the same kind of thing:

| a layer that is… | should… | because |
|---|---|---|
| a bleed, a backlight wash | **follow the knob** | it *is* the screen's light |
| a color correction | **stay put while powered** | it is a property of the **glass** |

Dimming a correction toward nothing as the knob comes down lets the readout drift
back to its untinted color exactly where the correction is most visible. Before
this, one setting had to cover both and a stack was tuned to whichever mattered
more.

⚠ **Off does not mean ungated.** The power, pixel, bus and breaker gates all still
apply — the same rule as a target with `follow = Off`. "Persistent while powered"
is what this offers; "painted over a dead screen" is not. Wire key `lfollow`, on
its own line under the layer, written only when not Inherit.

### 8g / 8h. The pixel power gate and the stencil — BOTH REMOVED (2026-08-04)

Both read the panel framebuffer back, and both are **gone**. Their section numbers
are **retired, not reused**, the same way interior deviation #2 is.

- **8g, the pixel power gate** — an opt-in per-target gate that sampled a strip of a
  rect's ALPHA and treated "no alpha anywhere in it" as "this unit is off", for the
  faces whose `kPanelSources` row is a guess or absent.
- **8h, the stencil** — a per-layer mask that cut a layer to the pixels above or
  below a threshold, built for the FCU, whose digits and background are too close in
  hue for a blend mode to separate. Alpha was free (`glCopyTexSubImage2D`); the
  color channels cost a `glReadPixels`.

**Why they went:** neither was ever used. The gate shipped with no target enabling it
and the stencil shipped with no layer using it, and in both cases a better answer was
found first — the bus gate covers what the pixel gate was reached for, and the FCU
look was settled without a mask. Weighed against that, the reasons to remove:

- **`glReadPixels` is a synchronous pipeline stall**, and this is a renderer add-on
  whose whole value proposition is that it is cheap. Carrying the one genuinely
  expensive call in the file for a feature nobody enables is the wrong trade.
- **They constrained the compositor's blend path.** The stencil made every future
  blend mode owe two decisions (`FxBlendTakesStencil`, and the alpha-1 rule in
  `FxQuadColorFor`), and both features made `SetFxBlend`'s alpha pinning load-bearing
  for a third and fourth reason.
- **`XPLMSetGraphicsState`'s alpha-test parameter is a constant again**, which is one
  fewer thing a later edit can silently break.

⚠ **What SURVIVES them, and must not be "cleaned up" as now-unmotivated:**
**`SetFxBlend` still pins the alpha half of every blend to `(GL_ZERO, GL_ONE)`.** That
was the guarantee the alpha probe rested on, but it does **not** exist for the probe's
sake — nothing in this compositor has any business changing the panel's alpha channel,
and additive would saturate it while Darken would floor it. The pinning is correct on
its own terms and stays.

⚠ **If either is ever wanted again, read this first.** The traps are in the git
history, and two of them cost real time: the stencil's mask must be built at the TOP
of the pass or a color channel measures our own paint and latches open one frame at
a time; and alpha testing must go through `XPLMSetGraphicsState` (parameter four)
rather than `glEnable`, because X-Plane caches all seven and enabling behind that
cache leaks an alpha test into the sim's own drawing.

### 8i. A layer's own response curve (2026-08-03)

`FxLayer::curve` holds knots that replace `kBrightnessCurve` **for that layer alone**.
The case that forced it: a **bleed** that should only show on an already-bright
screen, over a **color correction** that has to be there the moment the readout is
readable, are two shapes on one target — and no per-*target* curve can express two
shapes. §8f made `follow` per layer for the same reason; this is the other half.

- ⚠ **A layer's knots OUTRANK the target's response switch, INCLUDING "Linear"**
  (`FxShapeFor`, tested first, before `FxShapeForStack`). The editor **plots the curve
  it is editing**, so anything able to override that silently would make the plot a
  lie — and a plot you cannot trust is worse than no plot. Per-target panes use
  `FxShapeForStack` and say in orange how many of the target's layers draw their own
  shape instead.
- **The shape is ONE POINTER with three states** — `nullptr` linear,
  `&FxBuiltInCurve()` built-in, `&layer.curve` authored — so no call site can express
  a fourth. `FxDriverFactor` takes it in place of the old `bool useCurve`.
- **A half-built curve is not a curve**: fewer than two knots falls back rather than
  evaluating an identity nobody asked for.
- ⚠ **It NEVER reaches the spill lights.** `BrightnessResponse` is unconditional — the
  six screen-glow lights ship, are unswitchable, and share `kBrightnessCurve` with the
  tints on purpose (§8d). ⚠ **And `follow` is still the outer question**: a layer that
  does not dim has no curve to apply.
- **ONE interpolator.** `BuildMonotoneTangents`/`EvalMonotoneCurve` serve the built-in
  table and every authored curve; the built-in pair are now wrappers. Two
  implementations reading differently through the same knots would look exactly like a
  mis-placed knot. The builder's local-extremum pass never fires for the built-in
  table, so its numbers — and the tests' independent Python implementation — are
  unchanged.
- ⚠ **`FxCurveNormalize` forces the DOMAIN, not the range.** Ends are pinned to x 0
  and 1, because a curve starting at x 0.3 silently holds its own first y across the
  bottom third of every knob. y is left alone: a wash that peaks mid-travel is a real
  authored look, and `BuildMonotoneTangents` zeroes the tangent at the turn.
  `kFxCurveMaxKnots` = 12.

**Wire: `lcurve <n> <x0> <y0> …`**, own line, written only when set. ⚠ **The COUNT comes
first and the pairs stay on ONE line** — a knot per line would let any edit that loses a
line turn a shape into a *different, perfectly valid* shape with nothing marking the
loss. ⚠ Read into a local and adopted whole, never straight into the layer.

**The editor** (`FxCurveEditor`, dev only): ⚠ **one `InvisibleButton` plus a draw list,
not a widget per knot** — a drag has to survive the pointer leaving the canvas, and
holding `ActiveId` there is what lets the layer pane commit a whole drag as one history
entry. Drag to move, click empty canvas to add, right-click a knot to remove. It draws
the linear diagonal and the built-in curve faintly, since an authored curve is only ever
a deviation from one of those, and the **live knob position** brightly — without that
marker, "the layer never appears" and "the knob never leaves the dead zone" are the same
picture.

⚠ **One authored curve now SHIPS** (2026-08-04): the **FCU row's Lighten layer**, five
knots. That changes what `panelfx.txt` has to be — its curves must be *right*, not
merely parseable:

- ⚠ **A bad count falls back to the target's curve** and logs one line. In a dev build
  that line is in the Log tab; in a release it is a shipped layer quietly losing its
  shape, with the rest of the look drawing normally around it.
- ⚠ **A shipped curve must already be in canonical form** — ascending x, ends at 0 and
  1, all values within 0..1 — because `FxCurveNormalize` enforces exactly that on load.
  A file outside it **loads as a different shape than it reads as**, and the next save
  in the editor rewrites the shipped default with no edit having been made: an
  unreviewable diff that arrives on its own.

`LayerCurveTests` in `tests/test_panel_fx.py` validates the shipped lines against all
three. It used to assert there were none, which was the tripwire that caught this one.

### 8j. What the per-pass cost is allowed to be (2026-08-04)

The question that started this was whether a **hard-coded production compositor**
would be faster than the data-driven one — set the texture and blend once, then
loop the six DUs varying only brightness. The answer is that **`DrawFxLayer`
already is that**: texture, blend func, blend equation and the ambient day/night
lerp resolve once per layer, `PaintPanelTarget` walks the target's rects, and only
color and alpha vary per quad. A hard-coded fork would emit the same GL call
sequence and would additionally have to be kept in step with `panelfx.txt` by
hand — the dev/release divergence this file has been bitten by three times
(`ShippingBuildTests` exists for it). **The structure was not the cost.**

The shipped look was **21 layers over 61 quads** at the time (24 over 70 now —
see §8j.2). What that was actually spending, and what each became:

| Was | Per pass | Now |
|---|---|---|
| `glGetIntegerv(GL_FRONT_FACE)` per quad | 61 driver queries | once per layer, in `PanelQuadBatch` |
| `glBegin`/`glEnd` per quad | 61 pairs | 21 — one batch per layer |
| Driver dataref reads | ~200 XPLM calls | ~15, via the per-pass value cache |
| `RectIndexByName` walks | ~50 strcmp sweeps of the rect table | once, in `GroupMembers()` |
| `FxDrawOrder` | a heap allocation | a reused static buffer |
| Overlay `stat()` | every file, 1 Hz, on the render thread | manual, from the editor's button |

⚠ **The winding query and the batching are ONE change.** `glGetIntegerv` is not
legal between `glBegin` and `glEnd`; doing either half alone raises
`GL_INVALID_OPERATION` and draws **nothing**, which reads as the compositor not
running at all. `EmitRectQuad` therefore owns neither, and refuses to emit outside
a batch rather than letting GL drop the vertices silently. ⚠ The probe's
**`ClearProbeRect` is deliberately not batched** — `glScissor`/`glClear` are not
legal inside one either, and that path's value is that it touches no vertices.

⚠ **The driver value cache is keyed on the resolved `(ref, index)`, not on the
`FxDriver`.** Six rects on one bus are six distinct `FxDriver` objects naming
`DCBusVoltages[0]`; keying on the object collapses only the per-layer repetition
and leaves the per-rect repetition, which is most of it. ⚠ **And it is armed only
between `FxValueCacheBegin` and `FxValueCacheEnd` inside `DrawPanelFx`** — the Dev
panes call `FxRectFactor` and `FxDriverState` to show a **live** value, and that
moving number is the only thing that distinguishes a driver which binds and reads
the wrong quantity from one that never bound (§8d). A cache they read through
would freeze exactly the display that exists to catch that. The side benefit is
worth as much as the calls saved: every layer in a pass now sees one electrical
snapshot, so a voltage crossing the threshold mid-pass cannot gate one layer of a
screen and not the next.

**What was deliberately NOT done:** caching the draw *order*. It would need
invalidating at every site that adds, removes or re-targets a stack, and the
editor has many; a stale order is a silent visual bug — a base layer compositing
over its own refinement — of exactly the kind §8b's broadest-first rule exists to
prevent. Sorting ten ints costs nothing. **The allocation was the cost, not the
sort.**

**Overlay refresh is now manual** (see §8c): the 1 Hz poll from the compositor was
a developer convenience — the inner loop of authoring a look — being paid for by
every user's render thread. `ScanFxTextures` already compared stamps and marked
changed files stale, so the tab's *Rescan images* button was always doing the
whole job; the split into "rescan for the list, poll for the bytes" was the part
that needed explaining rather than the part that earned its keep. The initial scan
moved to `LoadPanelFx`, i.e. off the render thread entirely.

**Measure before extending this.** The perf tool's `kLeverDisplayFx` already
switches the compositor off as one lever, and the shipping quick run is FX-on vs
FX-off. If the compositor ever does show up at a size that surprises, the next
suspect is not call count but the **blend-equation switches** (`GL_MAX`/`GL_MIN`
/`GL_FUNC_ADD`) cycling through X-Plane's Vulkan→GL bridge, which no restructuring
removes: they are semantically per-layer, and the broadest-first rule means they
cannot be sorted by mode. (Half of them turned out to be *redundant* rather than
unremovable — §8j.3 — but the remainder stand exactly as described here.)

Pinned by `CompositorHotPathTests` in `tests/test_panel_fx.py`.

#### 8j.2 The second pass: not doing it at all (2026-08-10)

The first pass made each thing the compositor does cheaper. This one asks whether
it has to happen. The shipped look is now **24 layers over 70 quads**.

| Was | Per pass | Now |
|---|---|---|
| `PanelTargetMask` re-derived from member NAMES | ~2 400–3 800 strcmps in `FxDrawOrder`'s sort alone | resolved once, read as an array |
| The whole pass with the effects switched off | the sort, the graphics state, and a texture bind + blend equation + winding query + begin/end per layer — to emit nothing | one flag test and `return` |
| A target every rect of which is gated shut | the same per-layer GL, per layer, emitting nothing | one `FxTargetGateMask` walk of that target's rects |
| `glGetIntegerv(GL_FRONT_FACE)` | once per layer, ~24 | once per pass, under `PanelQuadWindingHold` |

⚠ **`PanelTargetMask` was the expensive one, and not mainly because of the
compositor.** It walked a group's member names — `RectIndexByName` per member, a
strcmp sweep of the rect table each — on **every call**, and it is both the
comparator of the per-pass sort and the whole basis of the editor's target tree.
`Whole panel` alone costs **666 strcmps** to re-derive. `BuildFxTargetPane` asks
`PanelTargetParent` of all 51 targets, and each of those asks 15 groups × 2 masks:
**~211 000 strcmps for the root scan alone, per ImGui frame**, repeated for every
open node's children. The tables are `static const` and `GroupMembers()` had
already resolved the names, so the masks are now built **from it** — which also
makes "the rects a target contains" and "the rects `PaintPanelTarget` paints" one
answer instead of two that agree.

⚠ **The skips are stated ONCE, in `FxRectGated`.** The Displays switch and the
three electrical gates are the part of `FxRectFactor` that **does not depend on
the layer**, so they were split out and both callers share them: the per-quad
factor, and `FxTargetGateMask`, which asks them of a whole target before the first
texture bind. A second copy beside the skip would drift, and it drifts in the
worse direction — **a skip stricter than the paint is a target that silently stops
drawing.** `test_the_gates_are_stated_once` pins it.

⚠ **The skip must not fold in anything per-layer.** A layer with `follow` off
draws at full strength through a knob at zero (§8f), so testing the brightness
*factor* there would skip exactly the layers that setting exists for. Gates only.
⚠ And `gFxDrawStack` has to be set before it is asked — a stack may override
`power`.

⚠ **`FxSampleAmbient` and `FxExpireDriveOverride` stay ABOVE the early-out.** Both
are per-frame bookkeeping that goes wrong by *not* running: a watchdog behind an
early-out is not a watchdog (§8l), and an ambient blend that stops sampling
resumes with a fade — the artifact "the first sample snaps" exists to remove
(§8e), arriving whenever a user switches the effects back on at noon. Three
dataref reads and an `exp()` between them.

⚠ **The winding hold is opt-in and scoped.** A stale `GL_FRONT_FACE` is
**silently culled** and looks exactly like the compositor not running, so
`PanelQuadBatch` still queries by default and only skips it inside a
`PanelQuadWindingHold` — a scope in which nothing but this file draws. The
probe's own batch is outside it and is unchanged.

**What was deliberately NOT done, again:** hoisting `follows`, the layer's shape
and `hasRamp` out of `FxRectFactor` into per-layer globals. They are a handful of
branches per quad, and the Dev panes call `FxRectFactor` outside a pass — globals
they could read stale is the shape of bug this file already pays a value cache to
avoid (§8j).

#### 8j.3 The third pass: asking each question once (2026-08-10)

§8j made each thing cheaper and §8j.2 stopped doing the things that were not
needed. What was left is **repetition**: four quantities that cannot change inside
one pass, re-derived per layer or per quad. The shipped look is still 24 layers
over 70 quads.

| Was | Per pass | Now |
|---|---|---|
| `FxRectGated` per rect **per layer** | 80 evaluations, each up to 3 driver lookups | 29 — one per rect per stack, held in `gFxDrawGateMask` |
| `SetFxBlend` setting the state already set | 48 bridge calls | 29 (14 equation + 15 func) |
| `PanelRects()` per `PaintPanelTarget` call | 24 reads of a ToLiss dataref | 1, pinned in `gFxPassRects` |
| `PanelTargetBreadth` popcounts in the sort | ~60 popcounts of a 36-bit mask | a resolved table |

⚠ **The gate memo is not a second statement of the rules.** The mask is built by
calling `FxRectGated` itself, with the memo off, so the skip and the paint are
literally the same bits — the invariant §8j.2 wanted, tightened. What makes it
sound inside a pass is that everything the gates read is *already* frozen: the
Displays switches are user state, and every electrical read comes out of the
value cache (§8j). ⚠ It is armed around **one stack's layers**, because a stack
may override `power`, and dropped with `gFxDrawStack`.

⚠ **`FxTargetCanDraw` became `FxTargetGateMask`** — it returns the SET rather than
"any?", which is the whole trick: the walk the skip was already doing is what
fills the memo, so nothing pays for the gates twice. Zero still means skip.

⚠ **The blend memo records what WE set, so it cannot outlive one pass.** It is
dropped at both ends (`FxForgetBlendState`, after `PushBlendFunc` and after
`PopBlendFunc`) because X-Plane owns that state in between: `XPLMSetGraphicsState`
sets a func of its own when it enables blending, and in a dev build the probe's
multiply runs after us in the same frame. A memo that survived a pass would skip
a call the GL state no longer matches — the whole cockpit blended with someone
else's operator, i.e. the failure `PopBlendFunc` exists to prevent, arriving from
the other end. ⚠ The equation and the func are tracked **separately** (five of the
eight modes are plain `GL_FUNC_ADD`; Add, Burn and Subtract all want
`GL_ONE, GL_ONE`), and ⚠ the refusal path — an equation-based mode on a driver
with no `glBlendEquation` — records **nothing**, because it set nothing.

⚠ **Pinning the rect set buys a correctness property as well as 23 reads.** With
the resolution flipping mid-pass, the six DUs would be painted from the full-res
table by the layers drawn before the flip and from the low-res one by those after
it — a half-composited look on exactly the six rects whose faults are hardest to
attribute (§6, `gDisplayFallbackRef`). ⚠ Null means "not in a pass", **not**
"full-res": the Dev panes and the probe's log line read `PanelRects()` outside the
pass and must keep resolving live.

**What was deliberately NOT done:** dropping the per-vertex `glTexCoord2f` on
solid layers (48 of the 70 quads, ~190 immediate-mode calls) by setting the
texture coordinate once per batch. It works — a 1×1 white texture samples the same
texel at any coordinate — but it re-creates the `tile 0` failure exactly (§8c): a
textured layer that lost its per-vertex coordinates draws **one texel stretched
over the rect**, which reads as a flat color rather than as a bug. Immediate-mode
vertex calls are the cheapest thing in this pass and the mis-set flag would be the
most expensive kind of mistake here. Client-side vertex arrays instead of
immediate mode would remove the same calls more thoroughly, and were rejected for
the same reason plus a second: `GL_VERTEX_ARRAY`/`GL_COLOR_ARRAY` client state is
not tracked by `XPLMSetGraphicsState`, so leaking it corrupts the sim's own
drawing, and every failure mode here is invisible rather than loud.

### 8k. A layer's color ramp (2026-08-07)

A layer can carry a **start color** for a dark knob: `FxLayer::color` becomes **the
color at a full knob** and `FxLayer::rampColor` **the color at a dark one**, with
the compositor interpolating between them.

⚠ **On the RAW dataref position, not on the effect intensity** (corrected
2026-08-07). The value is `FxDriverNorm`'s — 0..1 after the driver's `lo`/`hi` and
**before** the response curve and the floor. It was the *factor* for one day, and
that is wrong three ways, all of them quiet:

| fed the factor | the symptom |
|---|---|
| the curve is in it | editing a target's curve silently moves every ramp on it |
| the curve's shape is in it | the ramp's midpoint sits wherever that curve crosses 0.5, not at half a knob |
| the floor is in it | a driver with a floor of 0.2 never reaches the start color at all |

A ramp is a statement about the **screen** — this display reads warmer at a
quarter knob than at full — so it follows where the knob *is*, not how strongly we
have decided to paint there. The factor is a statement about the **effect**, and
those are different quantities that happen to share a range.

**One read produces both.** `FxDriverNorm` normalizes, `FxShapeFactor` applies the
shape and the floor, and `FxRectFactor` calls them in sequence — so the color and
the opacity of one quad can never describe two different frames of a knob being
turned.

That asymmetry is the whole design. A look is authored at the top of the knob —
that is where it is judged and where the screenshot is taken — so the ramp says
where the color *came from* rather than where it goes. Ticking the box seeds the
start from the end, which makes a fresh ramp a no-op and means the control cannot
change what is on screen the moment it is enabled (same rule as the daylight box,
§8e).

**Why opacity cannot already do this.** A dim LCD is not the authored color more
faintly: it goes warm, its backlight reddens, its black stops being lifted, and
its cast shifts away from the blue-white it has at full. Fading toward the blend
mode's identity — all `follow` can produce — never gets there. This is the same
argument §8e makes about noon versus midnight, one axis over.

- ⚠ **It is NOT gated on `follow`.** `follow` answers "does the OPACITY track the
  knob"; the ramp answers "does the COLOR". They are different questions and
  tying them together would break the primary case: a **color correction** is
  the layer that most wants a ramp and is by definition the layer set to stay at
  full strength while powered (§8f). `FxRectFactor` therefore computes the drive
  value when **either** question wants it (`wantsDrive`), and a layer that wants
  neither still pays for no read.
- ⚠ **The master switch outranks it**, like everything else here: with *Follow
  display brightness* off, every rect resolves to drive 1 and every layer draws
  the color it was authored as. That is what keeps the master a "show me the raw
  colors" hatch rather than a switch with an exception.
- ⚠ **The gates still come first.** A dim knob cannot un-gate a dead screen, and a
  gated rect draws nothing at all, so `driveOut` on a gated rect is moot — but it
  is still initialized at the top of `FxRectFactor`, because "moot" and
  "uninitialized" are not the same value.
- ⚠ **The fast path is now "nothing to recompute", not `f >= 0.999`.** A layer
  with a ramp and `follow` off sits at `f == 1` with the knob at a quarter; taking
  the precomputed quad there would draw the full-brightness color and the ramp
  would appear to do nothing **in exactly its own primary case**.
- ⚠ **`FxLayerColorAtDrive` takes a COLOR, not an `FxLayer`** — the same
  signature rule `FxQuadColorFor` has (§8e) and for the same reason. It
  interpolates toward the **ambient-resolved** color, so the day/night blend
  still moves the top of the ramp; reaching into `l.color` would compile and would
  pin every ramped layer's daylight half to its night color.
- **The start color has no daylight half.** The same deliberate limit the blend
  mode and the image have: two eras of ramp is two layers with opposite day
  opacities, not four colors on one layer. A fourth control multiplying into one
  number is how a dim layer becomes unattributable.
- **No opacity of its own either**, for that reason exactly — `lfollow` and the
  layer's curve already answer "how much of this at a dim knob", and a third
  answer would leave nothing in the cockpit to say which of the three produced
  what you are looking at.

**Wire: `ramp <r> <g> <b>`**, own line under the layer, written only when set —
so a file with no ramps stays byte-identical to what the previous version wrote.
⚠ **Read into a local and adopted whole**, the rule the `tile` field learned the
hard way: `operator>>` zeroes its target on failure, and a short line read
straight into the layer leaves a **black** start color with `hasRamp` true, which
is a plausible enough look to survive review.

The editor row shows the start swatch and, beside it, **the color being painted
right now** with the drive value that produced it — taken from `FxRectFactor`
itself via `FxLayerLiveDrive`, never recomputed. A ramp that looks like it does
nothing and a knob sitting at the top are the same picture without that number.

`ColorRampTests` in `tests/test_panel_fx.py`.

### 8l. The held brightness pin (2026-08-07)

Authoring the dim end of a look means putting the knob there, and the knob is a
rheostat somewhere in the cockpit: find it, hold it, let go to reach the editor,
and it has moved by the time you are looking at the control you wanted to compare
against. **Panel FX ▸ Brightness knob override** is that knob, in the editor —
a slider that pins every brightness driver **while the mouse is down inside it**.

**It does TWO things while held, and neither is redundant:**

| half | what it does | why it alone is not enough |
|---|---|---|
| **the read pin** (`gFxDriveManual`, in `FxDriverNorm`) | every driver *reports* the slider's value | the display underneath stays at full brightness, so the tint is judged against the wrong background |
| **the write** (`FxDriveTheAircraft`) | the value goes back to the datarefs the drivers read | it silently does nothing on a **read-only** source, and those are disproportionately the faces whose `kPanelSources` row is a guess |

The first version of this was the read pin alone (2026-08-07). It is not enough:
a tint evaluated over a screen still at full brightness is a tint evaluated
against a picture it will never sit on, which is the specific mistake the whole
driver mechanism exists to prevent. The write is what makes the result worth
looking at; the read pin is what keeps the tint sweeping on the faces the write
cannot reach. **The pane reports both counts** and names the refusals — a refusal
is the answer to "the tint swept but that screen stayed lit", and without the
names that is a bug hunt through the rect table.

- ⚠ **NOTHING IS RESTORED, deliberately** (2026-08-07, user). This is a bench
  instrument for a **parked** aircraft. The knobs stay where the slider left them
  and the way back is the cockpit's own rheostats or an aircraft reload. A restore
  would have to survive whatever interrupts a drag *and* fight ToLiss for any
  dataref it rewrites per frame — two hard problems bought for a case that does
  not arise. The tooltip says so; the checkbox beside the slider turns the writes
  off for the pass where the aircraft's state must not move.
- ⚠ **The write is in the DATAREF's units** — the pinned 0..1 goes back through
  `lo`/`hi`, which is exactly the mapping those two numbers describe. Writing the
  normalized value puts 0.4 into a knob that counts 0..100 and reads as the write
  not working.
- ⚠ **And it writes the RAW position, never the curved one.** The curve is
  Photon's model of how a screen responds to its knob; the knob is where the pilot
  put it. Writing the curved value bends the aircraft's own dimming through our
  shape and then reads it back through that shape a second time.
- ⚠ **Widest type first, mirroring `FxReadScalar`.** A dataref with both an int
  and a float writer, written through the int one, quantizes a 0..1 knob to 0 or
  1 — the mirror of the read-side truncation trap, and just as quiet: the screen
  snaps between off and full and looks like a dataref that does not mean what its
  name says.
- ⚠ **Every array write gets its own length probe.** A read past the end returned
  nonsense; a *write* past the end is undefined behaviour in ToLiss's memory.
- **Deduped on the resolved `(ref, index)`**, the value cache's key and for the
  same reason: six DUs on one `DUBrightness` array are six `FxDriver` objects and
  a group's six rects are six more.
- **Every driver, not the selected target's** — that is what "and the child
  targets too" means for a group, and it keeps the two halves consistent with each
  other. A global read pin with a per-target write would dim one screen while
  every other target's tint swept without its screen following, which reads as the
  write being broken.
- **Rewritten every frame of the drag**, not once on mouse-down: ToLiss owns these
  datarefs and may write its own value back between two of our frames, exactly as
  it does with the beacon ratios.

- ⚠ **It pins the NORMALIZED value** — inside `FxDriverNorm`, after `lo`/`hi` and
  **before** the curve and the floor. Pinning the factor would draw a straight
  line through whatever shape is authored, and the curve editor plots that shape
  beside the slider: the one control for sweeping a curve would be the one control
  that bypasses it. It is also what makes the curve editor's live marker walk
  along the shape as the slider moves.
- ⚠ **Brightness only.** `FxDriverPowered` — power, bus, breaker — is untouched.
  Those are facts about the aircraft, and a slider able to fake them would let a
  look be authored against an electrical state the cockpit is not in.
- ⚠ **Held, not latched, which needs a WATCHDOG rather than an else branch.** The
  pane that would release it is not drawn when the Dev window is closed or another
  tab is selected, so a drag interrupted by anything that hides the window would
  leave every driver pinned for the session with the control that did it out of
  sight. `FxExpireDriveOverride` drops it half a second after the last hold, from
  `DrawPanelFx` — the only per-frame path the pin affects, so a stuck pin cannot
  outlive anything it could be visible in.
- **Never saved**, and the value is dev-only in the same shape `gFxAmbientManual`
  is: the float is declared outside the guard (the compositor reads it), every
  *write* is inside it, so a release holds −1 for its whole life and pays one
  compare per driver read. ⚠ **`FxWriteDriverValue` and `FxDriveTheAircraft` are
  wholly inside the guard** — a shipping build has no path that writes one of the
  aircraft's datarefs, and `ShippingBuildTests` pins that.
- **The pane names what it cannot reach.** A rect with no brightness source
  resolves to drive 1 whatever is pinned, so the selected target's undriven faces
  are counted out loud — a slider that visibly does nothing on one face of a group
  is otherwise indistinguishable from a pin that is not working.

`BrightnessPinTests` in `tests/test_panel_fx.py`, plus the two ordering tests in
`tests/test_brightness_curve.py`.

### 8a. Dear ImGui

The editor is the first thing in Photon built on **Dear ImGui** (v1.92.9,
vendored at `src/native/third_party/imgui/`, see its README). It is compiled
into *every* build, not gated behind the probe: it is the UI framework for
tooling and for whatever gets converted later, so its availability should not
depend on which experiment is switched on. A Release link drops what is
unreferenced, so a shipping plugin that opens no ImGui window pays almost
nothing — the shipping `.xpl` is 192 KB with it compiled in.

**`opengl32` is now linked unconditionally**, which §4 flagged as the easy-to-miss
build change. That item is done, and done for a better reason than the tint.

The X-Plane backend is ours, at `src/native/src/imgui_xplm.{h,cpp}` — the stock
backends assume they own the window, the GL context and the event loop, and a
plugin owns none of those. Its header comment carries the reasoning; the three
that matter are that textures must come from `XPLMGenerateTextureNumbers` rather
than `glGenTextures`, that the projection is *already* set up in global boxels,
and that clip rectangles therefore have to be mapped through the live
modelview/projection/viewport rather than through an assumed DPI scale.

**The existing windows are deliberately not converted.** The Exterior/Cockpit
Custom windows, About and the Panel Probe window still draw with
`XPLMDrawTranslucentDarkBox` + `XPLMDrawString`. That is a separate change with
its own risk, and none of it is in the way.

**Smoke test: Dev window ▸ ImGui tab.** If the stock demo draws, scrolls,
responds to the mouse and accepts typing, the backend is correct — a much better
first question than "why does my tool window look wrong".

#### Two house rules, both pinned by `tests/test_ui.py` (2026-08-02)

⚠ **Never call `ImGui::SetTooltip` — use `UiTooltip` / `UiItemTooltip`.** A tooltip
window auto-sizes to its content, and a paragraph with no wrap position is **one
line however long it is**. ImGui then *clamps* that window to the X-Plane window
rather than re-flowing it, so the sentence is **cut off** — and only near an edge,
so the same tooltip reads fine two hundred pixels to the left and nothing anywhere
reports a problem. Every explanatory string in this plugin is a paragraph, so this
is the normal case, not an occasional one. The helper pushes
`PushTextWrapPos(GetFontSize() * kUiTooltipEm)` inside the tooltip, so it sizes
itself to the wrap width. ⚠ **The width is part of the fix, not just the
wrapping**: it was 35 em (ImGui's demo value), which is wider than the settings
window and wide enough to run off the screen from a control near its right edge —
at which point the clamp cuts the ends off again despite the wrap, reported
2026-08-03. **24 em** is narrower than the shipping window, so a tooltip fits
wherever its control is. ⚠ `BeginTooltip` returns a bool in 1.92 and `EndTooltip`
may only be called when it returned true.

⚠ **An item's ID is its LABEL TEXT hashed with the ID stack.** Two visible items
that hash the same raise *"2 visible items with conflicting ID"* and then share
state — dragging one slider moves the other. Three shapes cause it, and the third
is the one that keeps recurring:

1. the same literal twice in one scope — caught by review;
2. a literal-labelled widget in a **loop** with no `PushID` in the body;
3. a **row helper called more than once from one pane** (`DisplayToggleRow`,
   `DbgOptionRow`, `FxDriverEditor`, `BuildFxLayerExtrasRow`). Its widgets land in
   the *caller's* scope, so identical labels across two calls collide. Either the
   helper pushes an ID of its own, or every label it emits comes from a parameter
   — which is what makes `FxTriCombo` safe without one.

The rule adopted: **scope the row, not the widget.** `PushID(label)` around a
helper costs one line and removes the whole class, including the cases that only
collide after someone adds a table row years later. `DbgOptionRow`'s three option
tables and the Build tab's command buttons carry a `PushID` they do not strictly
need *today* for exactly that reason.

---

## Where the detail lives

| Topic | File |
|---|---|
| The probe itself, datarefs, how to read it | `src/native/src/plugin.cpp`, `#if PHOTON_DEV` |
| The Debug submenu + probe window | same block — `DrawDebugWindow`, `AppendDebugMenu` |
| The layer compositor + its editor | same block — `DrawPanelFx`, `BuildFxUi` |
| Dear ImGui backend for X-Plane | `src/native/src/imgui_xplm.{h,cpp}` |
| Vendored ImGui, version and update steps | `src/native/third_party/imgui/README.md` |
| Building the probe | `src/native/deploy.ps1 -Dev` |
| Rect derivation, and the `--all` survey | `build/derive_panel_rects.py` |
| Display spill lighting (the other screen feature) | `docs/screens_plan.md` |
| In-sim tuning tools generally | `docs/dev-tools.md` |
