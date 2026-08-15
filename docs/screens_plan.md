# Screen Glow — display spill lighting

Six spill lights, one per main display unit, throwing each screen's own bluish-gray
wash into the cockpit. On a dark flight deck the DUs are a real light source: they
put a cool cast on the glareshield, the yoke area and the pilots' faces that no
lamp in the cockpit reproduces.

**Status: SHIPPED (2026-08-01).** Part of the base install on A3xx — the OBJ is
written and the `.acf` attachment made by `actions.cpp`'s screens step, and
the user turns the glow on or off (and sets its strength) on the plugin's
**Displays** tab. It is no longer an experiment and no longer opt-in at install
time. `docs/installer.md` ▸ Display-glow attachment is the installer-side contract;
what follows is the lighting side.

⚠ **Much of this document was written while it was an experiment.** Where a
paragraph says "dev-only" or "not wired into the installer", read it as history —
the mechanism it describes is still accurate, its *status* is not.

---

## 0. What makes it optional

`lights_screens.obj` is an OBJ ToLiss does not ship and does not reference. X-Plane
draws an aircraft's OBJs from the `_obja/*` **attachment table** in its `.acf`, so
until that table names our file, the OBJ is inert on disk — it does not load, does
not draw, and costs nothing.

**This is still the design, and it is why the install is reversible**: an uninstall
detaches the row and deletes the file, leaving no trace. What changed in 2026-08-01
is only who pulls the lever.

| Layer | Default | Turned on by |
|---|---|---|
| The OBJ is not built into `dist/` | off | `build --target screens` (or `--target all`) |
| The OBJ is not attached to the aircraft | off | the installer, or `photon-installer screens --aircraft <dir>` for the attachment alone |
| The glow is drawn at all | **on** | the Displays tab's "backlight illumination" — under the tab's master "Enable display effects" (2026-08-03), and both are read through `DisplayFeatureOn` |

`build --target both` — what a bare `build` does — **excludes `screens`**
(`DEFAULT_TARGETS` in `build/build_objs.py`). Asking for it is the only way to get
it into `dist/`: either by name, or with **`--target all`** (= exterior + interior
+ screens), the convenience form for a dev tuning pass where the screens OBJ would
otherwise go stale while the other two rebuild. `all` is a separate word rather
than a widened `both` precisely so that nothing which builds the *default* set
starts emitting screens output. `test_a_bare_build_writes_no_screens_obj` is the
guard.

⚠ **`make_release.py` no longer packages `dist/` for this.** It emits the screens
OBJ straight from the Emitter (`build_screens_payload`), because `dist/` may hold a
`--debug` build — every light bound to `ToLissPhoton/debug/light/*`, ignoring every
DU knob, and not installable. `test_the_obj_is_not_a_debug_build` is the guard, and
it matters most on a dev machine, where a debug OBJ in the sim is the normal state.

`build/watch.py` takes the same `--target`, so `watch.py --target all --write`
keeps the display glow rebuilding on every save.

The plugin side is not gated, and does not need to be: six data accessors that
nothing reads cost nothing, and the alternative — a detection flag — would turn a
false negative into six dark lights. Same reasoning as the interior's map-rheostat
read (see `CLAUDE.md`, "Don't gate the 1 Hz interior Auto resolution").

---

## 1. The six lights

| Slot | Fixture | Display | `DUBrightness` | Position |
|---|---|---|---|---|
| 0 | `screen_capt_pfd` | Captain PFD | `[0]` | −0.59, 0.08, −4.67 |
| 1 | `screen_capt_nd` | Captain ND | `[1]` | −0.37, 0.08, −4.67 |
| 2 | `screen_ecam_upper` | Upper ECAM (E/WD) | `[4]` | 0.012, 0.08, −4.67 |
| 3 | `screen_ecam_lower` | Lower ECAM (SD) | `[5]` | 0.012, 0.10, −4.59 |
| 4 | `screen_fo_nd` | First Officer ND | `[3]` | 0.37, 0.08, −4.67 |
| 5 | `screen_fo_pfd` | First Officer PFD | `[2]` | 0.59, 0.08, −4.67 |

MCDUs, ISIS and the DCDUs are out of scope — "the six main LCD screens".

Shared appearance, retuned in-sim **2026-08-10** against the new size-driven knob:
`glow: 0.354 0.649 1`, `alpha: 1`, `size: 0.498`, `spread: 89.5`, `aim: 13 180`
(the aim alone is unchanged). Was `0.55 0.66 1` / `0.15` / `0.9` / `80`: with the DU
knob moved off alpha, the old 0.15 — chosen as a *degraded-state* value — became the
intensity at every knob position and left the glow ~85% dimmer than the look that had
been signed off, so alpha went to full and `size` took over the dimming.
⚠ **The DSL is the authority** — this line has gone stale once already (it read
`size 0.83`, `spread 26`, `aim 0 180` long after `lights.style.phdsl` had moved on).
⚠ `spread: 89.5` is the dev tuner's slider CEILING rather than a value converged on
from both sides; if the wash still wants to be wider, the slider is what has to move.
All six carry one color; the separate slightly-warmer `ecam` palette entry the ECAM pair used to have
was dropped rather than left dead. These are the first fixtures written in the
angle form — see `docs/dsl.md`.

### The DU index order — SETTLED 2026-07-29, and it is not left to right

The seed assumed `DUBrightness` ran left to right across the panel. In-sim, dimming
one display at a time, **four of the six were wrong**: the upper ECAM knob moved the
F/O ND's glow, the lower ECAM knob the F/O PFD's, and vice versa. The real ordering
is the Airbus DU numbering, pilot-side pairs first:

```
DUBrightness[0] CA PFD   [1] CA ND   [2] FO PFD
DUBrightness[3] FO ND    [4] E/WD    [5] SD
```

Two numbers per fixture must agree with the plugin and they are **different numbers**:
`spill_dref: .../spill/<n>` is the light's own slot (fixture order), `index: <du>` is
which `DUBrightness` element drives it. `index` must equal `kScreenDU[n]` in
`src/native/src/plugin.cpp`; `DUMappingTests.test_dsl_and_plugin_agree_on_every_slot`
fails if only one side moves.

`kScreenGain` is **1.0** (was 0.85). The dev tool drives the same multiplier that
gain is, so a look approved in-sim at full needs gain 1 to reproduce — 0.85 would
have shipped the approved tuning 15% short. Tone it down with `size`, `alpha` or
the palette, not with gain.

**The user's own multiplier is a third one** (2026-08-01): the Displays tab's
"backlight illumination" switch and strength, `gDisplays.spill` / `spillGain` — read
through `DisplayFeatureOn`, so the tab's master switch covers it too. It is
applied **after** the curve, and last of the three — it scales the finished look
rather than reshaping the response, so turning it down pulls the glow in without
moving where the knob's dead zone ends. Off writes **0** into `gScreenSizeFactor`
rather than skipping the loop: a stale factor left in the array would keep the
lights lit at whatever they last were. It clamps to 0..1 and is deliberately not a
boost — the authored `size:` is already the reach at a screen turned fully up.

**The DC bus is a hard gate, not a fourth multiplier** (2026-08-01). `DcBusLive()` —
`AirbusFBW/DCBusVoltages[0] > 24` — zeroes all six size factors outright. A dead bus means
the screens are *off*, not dim, so there is nothing for the knob or the user's gain
to scale: "80% of nothing" is still a lit cockpit. It is read **once for all six**
rather than per light (same bus, and this is the per-frame path).

⚠ **ToLiss's `DUBrightness` does NOT fall to zero with the bus.** It is the KNOB, and
a knob keeps its setting through a power loss — so nothing already in this path
noticed, and no reshaping of `BrightnessResponse` could have produced the effect.
⚠ **Unreadable means LIVE**: no dataref, or an index past the end of the array,
leaves the lights lit. Same rule as the FX drivers, same reason.

⚠ **The threshold constant is shared with the panel-FX tint** (`kDcBusLiveVolts`,
`kPanelBuses`). A screen that stops glowing into the cockpit while its tint stays
painted on the glass — or the reverse — is worse than neither being gated.
`ElectricalGateTests` pins the two together.

**The knob does not reach the driven slot linearly** (2026-08-01). `DUBrightness` goes through
`BrightnessResponse` first — the shared `kBrightnessCurve`, knots
`(0,0) (0.2,0) (0.5,0.2) (1,1)`, so the bottom fifth of the knob is dead and half a
turn is still dim. It is the **same curve the panel-FX tint uses**, which is the
whole reason it is shared: a tint and the spill under it come up together. Here it
is unconditional — the FX tab's `Curve` switch is a tuning knob on a dev-only
feature and deliberately does not reach these lights. Full rationale, including why
the interpolation is monotone cubic and not Catmull-Rom, is in
`docs/fcu_tint_plan.md` §8d; `tests/test_brightness_curve.py` pins it.

---

## 2. How a light is wired

Each is a `LIGHT_SPILL_CUSTOM` bound to `ToLissPhoton/screens/spill/<n>`, for
exactly the reason `int_spot3` (the map spot) is: brightness comes from a
**per-screen dataref**, and `airplane_panel_sp`'s `INDEX` slot can only name a
rheostat. The plugin writes **only the SIZE slot** (⚠ not alpha, since 2026-08-09 —
a screen is an area source, so turning it up spreads its wash rather than making a
fixed pool more opaque) and passes the other eight parameters through untouched, so
color, **alpha**, aim and cone all stay in the DSL and are never duplicated in C++.

```
LIGHT_SPILL_CUSTOM  x y z   r g b a   size   dx dy dz   cone   <dataref>
                                             ^
                                  the ONLY slot the plugin drives
```

The authored size is **captured** from the pre-filled buffer on the first read and
assigned thereafter, never multiplied in place — see the `screen-glow` skill.

**Three places must agree**, and a mismatch binds a light to the wrong screen with
no visible symptom until the dimming test above:

| Thing | Where |
|---|---|
| the six `spill_dref:` values, and each light's `index:` | `src/lights/lights.layout.phdsl` |
| `kScreenCount`, `kSpillScreenPrefix`, `kScreenDU` | `src/native/src/plugin.cpp` |
| `DebugDUBrightness`, `source: "du"` handling | `src/plugin/PI_PhotonDevReload.py` |

### Plugin-absent behavior

If the dataref is not found, X-Plane draws the **baked** parameters unmodified — so
the lights still appear, at full reach and their authored alpha, no longer following
the DU knobs. Since the plugin stopped writing alpha (2026-08-09) that authored
`alpha:` is simply the intensity in every case, loaded or not; what the plugin-absent
state loses is the knob, not the look.
`test_the_baked_alpha_is_the_shipping_intensity_and_is_never_zero` pins that it is
never 0 — invisible reads in-sim as a failed `.acf` attachment.

---

## 3. No gating of LOOKS, and why that is deliberate

The screens OBJ contains **no `ANIM_hide` selecting an appearance**. There is one
look, so there is nothing to switch, and no menu row of looks. (There is exactly
one `ANIM_hide` in the file — the master on/off gate in §3a — and it is not a
look.)

Writing it as a category would have forced one of two bad shapes:

* a two-branch category whose branches render identically — **a dead menu row**,
  which has already shipped twice on the interior (the dome, the console strips)
  and both times looked exactly like the mod not being installed; or
* a fake second palette existing only to satisfy the DSL's one-value shorthand.

So the DSL grew an honest way to say it instead: a fixture takes **exactly one** of

* `category: X` — N `ANIM_hide`-gated branches, switched at runtime, or
* `profile: X` — **one un-gated block** rendered in palette `X`.

The level still varies per frame; it rides the spill dataref's SIZE slot (⚠ not its
alpha, since 2026-08-09), not a gate.
`NoGatingTests` fails if a gate on a LOOK ever appears here, so re-introducing one
is a deliberate act with the reasoning above to answer.

---

## 3a. The master gate — one `ANIM_hide` around the whole OBJ

Added 2026-08-04. The entire body is wrapped in a single block:

```
ANIM_begin
  ANIM_hide 1 1 ToLissPhoton/screens/disabled
  ... all six lights ...
ANIM_end
```

**Why, when a zero driven slot already made them invisible:** invisible is not
free. Six `LIGHT_SPILL_CUSTOM`s are six lights the renderer sets up and six 9-float
accessor calls, whatever they draw — so "backlight illumination: off" was
paying for a feature it had switched off. One dataref read now answers for all
six.

**The dataref is derived, not stored.** `ReadScreensDisabledInt` returns
`DisplayFeatureOn(gDisplays.spill) ? 0 : 1` — the user's switch under the
Displays master, read through the same helper every other consumer uses. A cached
copy would be a second place to forget and would lag the checkbox by a frame.

⚠ **It is named for the OFF state on purpose.** An OBJ8 `ANIM_hide` whose dataref
does not exist reads **0 forever** (an OBJ binds dataref *names* at load time), so
0 has to mean *draw*. Called `.../enabled`, a missing or older plugin would make
the OBJ invisible — which in-sim reads as *"the `.acf` attachment never
happened"* and sends the next person after the wrong bug. Same reasoning that
bakes the alpha low rather than at 0 (§ Plugin-absent behavior).

⚠ **Both gates stay.** The block gate is the cheap answer to the user's switch;
the per-frame size factor is what follows the DU knobs and the DC bus, neither of
which belongs in an `ANIM_hide`. Dropping the per-frame gating because "the block is
hidden anyway" would leave the spill lit through a bus failure whenever the switch
is on.

⚠ **Not in a `--debug` build.** The dev tuner owns every light in that file, and a
master gate could only hide the one being tuned.

---

## 4. The `.acf` attachment

`core/patch_acf_screens.cpp` adds one `_obja/<n>` row and bumps `_obja/count`.

### The attachment row is COPIED from `lights_inn.obj`, never written

An attachment row carries the OBJ's origin **in feet** against a per-airframe
datum, and the three airframes genuinely differ:

| Airframe | `_v10_att_z_acf_prt_ref` |
|---|---|
| A319 | `26.000000000` |
| A320 | `20.750000000` |
| A321 | `6.780000210` |

Our light coordinates are authored in `lights_inn.obj`'s frame, so **the only
correct attachment is a copy of that OBJ's own row with the filename swapped**.
Hardcoding a datum would silently mis-place every screen light on two airframes out
of three. If `lights_inn.obj` is not in the table the patcher **refuses** rather
than guessing.

Copying also solves two problems for free:

* **The XP11/XP12 float trap.** Values are copied as *strings*, so each file keeps
  its own rendering (`20.750000000` vs `20.75`) and nothing is re-formatted. This
  is the trap that makes a literal string patch silently no-op on XP11 files —
  see `build/patch_acf.py`'s header.
* **Schema drift.** XP11 rows use a *different field set* (`_v10_att_part` instead
  of `_v10_att_body`/`_gear`/`_wing`), and the A319/A321 rows carry
  `_obj_hide_dataref AirbusFBW/ObjectKill` where the A320's does not. Copying
  whatever is there handles all of it; a hardcoded row would have broken XP11.

### ⚠ This touches `_obja/*`, which is Durantula's namespace

`patch_acf.py` deliberately stays inside `acf/_spot*` so it cannot collide with
Durantula's 312-line rewrite of the attachment table. This patcher **cannot** —
attaching an OBJ *is* an `_obja` edit. So:

* **Detection is by our FILENAME** in a `_v10_att_file_stl` slot — never by index
  (ours is whatever was free) and never by a hash (the table legitimately carries
  other mods' edits).
* `is_attached()` also requires the row to be **inside `_obja/count`**. A row at
  index ≥ count is in the file but never read by X-Plane: it looks installed to a
  grep and draws nothing in the sim.
* If Durantula's uninstall restores a pre-Photon `a320.acf.durantula.bak`, our row
  vanishes with it and `is_attached()` reports False — which is the truth. Re-run
  the patcher. Same documented hazard as the cockpit-spot patch, not a new one.

### Reversal

`--reverse` removes the row and decrements the count. Rows **above** ours are
renumbered down by one rather than left with a hole: X-Plane reads indices
`0..count-1`, so a gap would silently drop whatever the table says that index is —
another mod's OBJ. In the normal case ours is last and nothing is renumbered.

Verified byte-for-byte against all twelve real `.acf` files: attach → detach
restores the original bytes exactly, including the stray NUL in `a321_StdDef.acf`.
Both patcher and tests read/write with `newline=""` and
`errors="surrogateescape"` — these files are CRLF, and other mods back them up.

---

## 5. Tuning it in-sim

Color, alpha, size, aim, spread **and position** are all live knobs. Aim is
**Pitch/Yaw in degrees** and the cone is **Spread in degrees** — not vector
components and not a backwards-running cosine. Each row's `-`/`+` moves by the lit
**Step** button — `1`, `0.1`, `0.01`, `0.001`, in that row's own units.

`Marker: This` draws a green dot on the light, an amber arrow along its aim and a
blue ring on the cone rim, which is the only direct read on where a spill light is
and where it is pointing.

Position is the one to treat with suspicion. The interior's debug lights were reported
mis-placed on 2026-07-28 (`CLAUDE.md` open thread 1b) and the `ANIM_trans` wrappers were
briefly blamed and disabled; that was traced elsewhere and they are on by default again,
but the underlying "outer floods are wrong under all 48 permutations" thread is still
open. If a screen light will not go where the numbers say, rebuild `--no-debug-pos` and
edit the `.phdsl` directly to tell the two apart.

```bash
# 1. install the tunable build (six lights on ToLissPhoton/debug/light/0..5)
python build/build_objs.py build --target screens --debug --write

# 2. attach the OBJ, once per aircraft folder
python build/patch_acf_screens.py "<X-Plane>/Aircraft/ToLissA320_V1p3p2"

# 3. tune in the Live Light Debug window (Plugins > Photon Dev)
# 4. put the numbers back in src/lights/lights.{style,layout}.phdsl
# 5. rebuild WITHOUT --debug so the plugin drives the lights again
python build/build_objs.py build --target screens --write
```

Since 2026-08-09 the two debug targets live on **disjoint slot windows**
(`DEBUG_SLOT_BASE` in `build_objs.py`: interior from 0, screens at 48..53), so both
debug OBJs can be installed and driven at once — the native Dev window's Lights tab
merges both manifests, and its "Edit all screens together" checkbox edits the six
lights' shared attributes (color, brightness, size, aim, spread — everything but
position) as one. The Python tool still drives one manifest at a time, the most
recently written, naming it in the window (`screens: 6 lights`).

### Where the positions came from

ToLiss draws the DUs from its own plugin rather than as `ATTR_cockpit` faces, so
**there is no screen geometry in the OBJs to measure** — unlike the `.acf` spots,
whose positions were *derived*. The seeds came from the panel around them, measured
from `cockpit_INN.obj` in the same frame, and this table is kept because it is the
only independent check on the tuned numbers:

| Feature | y | z | width |
|---|---|---|---|
| glareshield front bar | 0.379 | −4.408 | 1.20 m |
| main instrument panel | 0.139 | −4.591 | 2.19 m |
| flat panel strip | 0.093 | −4.604 | 1.44 m |

Tuned in-sim 2026-07-29. Five of the six landed on one plane — y 0.08, z −4.67, x
mirrored about the centerline — which is what a row of panel-mounted DUs should look
like, and which sits just aft of the measured panel face. ⚠ **The lower ECAM is the
odd one out**: y 0.10, z −4.59 puts it 2 cm *above* and 8 cm *aft* of the other
five, i.e. above the upper ECAM's glow. Taken from the tool's absolute readout as
reported and confirmed as intended; if that light ever looks like it is coming from
the wrong place, it is the first line to check.

**Use the markers, not the pool.** `Marker: This`/`All` in the dev window draws a
green dot on each light's origin and an amber arrow along its aim
(`docs/dev-tools.md`). A spill light's bright spot is not its origin, which is what
made these positions guesswork in the first place.

**If the cockpit looks lit from behind the seats, flip `dz`** on `screen_glow` in
`lights.style.phdsl` — the same ambiguity that bites the `.acf` spots.

**`cone` runs backwards**: it is cos(half-spread), so *widening* a beam means
*lowering* the number. Settled at `0.9` (≈26°).

**`dir` is written unit-length on purpose.** It tuned to `0 0 0.3`, which aims
identically but is 0.3 long, and X-Plane compares `cone` against a *dot product* with
that vector — so a short vector makes cone and aim interact, which is the likeliest
explanation for cone edits appearing to move the light. `build` warns about any
non-unit `dir:`.

---

## 6. Uninstalling

```bash
python build/patch_acf_screens.py "<X-Plane>/Aircraft/ToLissA320_V1p3p2" --reverse
```

That alone is enough — the OBJ stops being drawn. Delete
`objects/lights_screens.obj` and `objects/lights_screens.debug.json` to remove
every trace. The plugin needs no change: its six accessors go back to being read by
nobody.

---

## 7. Open questions

1. ~~Is the `kScreenDU` order right?~~ — **answered 2026-07-29**, and it was not the
   seed. See §1.
2. ~~Are the positions right?~~ — **tuned in-sim 2026-07-29** (§5). The lower ECAM's
   outlier position is the one part still worth a second look.
3. ~~Is `kScreenGain` (0.85) sensible?~~ — **1.0 since 2026-07-29**, to match the
   look approved at full in the tuner.
4. **Does it read as a screen or as a lamp?** Still the core question the experiment
   exists to answer, and the only one tuning cannot settle by itself. If it reads as
   decorative, `cone` and the palette's saturation are the first two knobs.
5. **Does a non-unit `dir:` make `cone` and aim interact?** Reported in-sim: changing
   the cone appeared to move the light. `dir` is now unit here, which removes the one
   known out-of-spec factor, but the same question is open across the interior — six
   light types are non-unit and `build` lists them.
6. **SkunkCrafts.** A ToLiss update that rewrites the `.acf` drops the attachment;
   detection reports it and re-running the patcher fixes it. Whether SkunkCrafts
   *refuses* to update a modified `.acf` is the same unknown the interior already
   carries.
