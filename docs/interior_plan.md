# Interior Lighting ("Gus Mod") integration — implementation plan

Status: **IMPLEMENTED (Phases 0, II–V), NOT YET IN-SIM VERIFIED (Phase VI).**
Revision 3 was the handoff revision; this header is the only part rewritten since.
Everything below is kept as the original specification — read it as the *intent*,
and the notes in this box as what actually happened.

**What was built** — `reference/gus/` vendored (3 OBJs + the 11-file canonical texture
set, ~57 MB); the N-way emitter with the exterior provably byte-identical; the interior
DSL (axis, palettes, 6 light types, 7 fixtures, 4 map-lamp mounts); multi-OBJ plumbing
through `build_objs` / `make_release` / `payload`; `build/patch_acf.py`; the plugin's
4 interior datarefs, ternary values, own profile enum, nested Exterior/Interior submenus,
13-row × 2-or-3-column Custom window, additive prefs, and the map-spill array; the
installer's opt-in step, texture install, livery `.dds` scan, four-`.acf` patch and full
uninstall. 60 new tests (192 total, all green), and the `.xpl` compiles clean.

**Phase I (the dev tuning harness) was deliberately skipped** — Gus's values are already
tuned and transcribed verbatim, so it would have paid off mainly in Phase VI, which needs
the sim anyway. Add it if VI proves painful.

**Deviations from this document, all deliberate:**

- §3.4's re-added spots needed a coordinate conversion the document didn't supply. It is
  *derived*, not guessed: the `.acf` gives spot positions in feet against a per-airframe
  datum, and subtracting the interior OBJ's own attach point
  (`_obja/11/_v10_att_{x,y,z}_acf_prt_ref` = 0 / 0.4 / 26.00 · 20.75 · 6.78 ft) yields the
  same offset on all three airframes — which is what keeps §3.2's one-shared-OBJ claim
  true. Cross-check: spot 1 (the `panel[0]` dome rheostat) lands at y 1.097, z −3.432
  against Gus's dome lights at y 1.09, z −3.35. **Direction, cone and size are still
  seeds** and are the main Phase VI tuning target — see the comment block on
  `int_spot_dome` in `lights.layout.phdsl` for the specific ambiguity and the one-line fix.
- §1.2's `0.9063077` vs `0.906307` note is settled by the emitter's project-wide 3 dp
  rounding: both become `0.906`. Decided, not drifted.
- §4.2's enum gained `INT_PROFILE_CUSTOM = 4`, which the document's list omitted but its
  own §4.4 Custom window and prefs schema require.
- Uninstall needed a manifest list the document didn't name: `interior.added[]`, for the
  two textures with no stock counterpart, which must be *deleted* rather than restored.

**Phase 0's in-sim dataref-type check was not done** — it needs the sim. `ckpt/lights/map`
is read through the existing type-detecting `ReadDataRefDouble`, so a wrong guess is
already defended against, but it remains unconfirmed.

**Question 2 for Gus is answered by the repo's own README**: he is
[GusRodrigues](https://forums.x-plane.org/profile/6767-gusrodrigues/) on the X-Plane.org
forums, and that is the name and link used everywhere credit appears.

---

**Verdict: feasible.** Interior light data has the same shape as the exterior data
Photon already generates — one geometry, three colour variants — so the DSL +
`ANIM_show` + per-category-dataref machinery transfers almost unchanged. The two things
that looked like blockers (the Plane Maker `.acf` step, and 2-way→3-way switching) are
solved in §3.4 and §3.1. What remains is plumbing and in-sim tuning.

**Read §9 first if you are picking this up cold** — it lists what this document cannot
give you.

---

## 0. Decisions taken — settled, do not re-litigate

| # | Decision |
|---|---|
| 1 | The LED variant's two old-halogen tablet lights are an **oversight** → normalise to warm white (§1.3). |
| 2 | **A320's files are canonical** for all three airframes; A319's `_current` is a stale revision, discarded. |
| 3 | Gus's `liveries/objects/` folder is guidance for livery authors, **not an X-Plane path**. All six files install to `objects/` (§1.5). |
| 4 | Per-package file differences are **packaging drift** → build one canonical union set (§1.4). |
| 5 | Texture recompression: **tabled**, not a blocker. |
| 6 | Gus gets **heavy credit** in docs and user-facing surfaces (§7). |
| 7 | Textures ship **bundled** in the four per-platform bundles. The All-Platforms zip is a maintainer convenience, not distributed, so its size is irrelevant. |
| 8 | `.acf`: patch the variant-independent geometry at install, move **colour** into the OBJ where it becomes runtime-switchable (§3.4). An `.acf` requirement is a problem to solve, not a hard block. |
| 9 | Category granularity: the **four rheostat-derived categories** of §2.2. |
| 10 | Interior selection persists **per livery**, same as exterior. |
| 11 | Patch **all four `.acf`** per airframe (XP12 HD + StdDef, XP11 HD + StdDef). |
| 12 | Install-state validation reads **whether our modified lines are present**, never a file hash — the `.acf` legitimately carries other mods' edits (§3.5). |
| 13 | The third `.acf` spot (`ckpt/lights/map`) is re-added as **`LIGHT_SPILL_CUSTOM`** (§3.4). |
| 14 | **A330-900 is out of scope** for interior lighting entirely. |
| 15 | Interior "Auto" mirrors the **resolved exterior era**: Classic→Old Halogen, Hybrid→New Halogen, Full LED→LED (§4.2). No new detection gate. |

Two corrections to earlier assumptions, both from the project owner:

- **The one-level menu limit is an XPPython3 4.7a1 bug, not an X-Plane one.** The
  shipping plugin is native, so nested submenus are available (§4.3). CLAUDE.md's
  level-2 warning still applies to `PI_PhotonDevReload.py`, which remains XPPython3.
- **Custom lights expose every parameter through a dataref** — confirmed (§3.3). Agreed
  usage is **bake every parameter, modulate brightness only**. A consequence worth
  stating: once brightness is the only dynamic parameter, `airplane_panel_sp` provides
  it natively, so custom lights are used in exactly two places (§3.3), not everywhere.

---

## 1. Source data

### 1.1 What the mod is

Three variants of one file — `lights_inn_current.obj` (old halogen),
`lights_inn_new.obj` (new halogen), `lights_inn_led.obj` (LED) — which the user renames
to `lights_inn.obj`. ToLiss attaches it from the `.acf`
(`P _obja/11/_v10_att_file_stl lights_inn.obj`), so swapping the file needs no `.acf`
change.

| Fact | Consequence |
|---|---|
| The three variants are **byte-identical except RGB triplets** (99 lines, 4070 bytes) | One geometry, three palettes — the exterior pattern exactly |
| A320 and A321 variants are **byte-identical**; A319's `new`/`led` match too | One shared config, no per-airframe placement work |
| Stock `lights_inn.obj` is **identical across A319/A320/A321** (md5 `c56be6a5fe6c90544617f7ebfb90146b`, 77 lines, 13 lights) | Confirms a single shared source file |
| Gus's file has **23 lights vs stock's 13** | Adds CA/FO table + tablet lights and extra lamps per map group — not just a recolour |
| Intensities are retuned (pedestal flood `5.0`→`2.0`, console `1.3`→`0.4`, map lamps `1.2`→`0.471`) | Variant-**independent** → base values in the DSL, not in profile branches |

### 1.2 The canonical light table (authoritative — transcribe from here)

From `.scratch/gus-lighting/A320/a20n-light_mod (2)/objects/lights_inn_{current,new,led}.obj`.
Line format is `LIGHT_PARAM <class> <px> <py> <pz> <R> <G> <B> <INDEX> <SIZE> <dx> <dy> <dz> <W>`.

Four colours appear across the whole mod:

| Key | RGB | Name |
|---|---|---|
| **A** | `1.00 0.37 0.16` | old-halogen orange |
| **B** | `0.80 0.55 0.30` | new-halogen amber |
| **C** | `0.82 0.82 1.00` | LED cool blue-white |
| **D** | `1.00 0.89 0.81` | warm white |

| # | Group | Class | Position | IDX | SIZE | Direction | W | Ramp |
|---:|---|---|---|---:|---:|---|---:|---|
| 0 | Dome Light L | `airplane_panel_sp` | `-0.472 1.090 -3.35` | 0 | 1.414 | `0.0 0.0 0.0` | 1.0 | **P1** |
| 1 | Dome Light R | `airplane_panel_sp` | `0.472 1.090 -3.35` | 0 | 1.414 | `0.0 0.0 0.0` | 1.0 | **P1** |
| 2 | Pedestal flood | `airplane_panel_sp` | `0.0 0.8 -4.1` | 3 | 2.0 | `0 -0.99499 0.1` | 0.9 | **P1** |
| 3 | CA table | `airplane_panel_sp` | `-0.5577 0.3333 -4.5` | 3 | 1.0 | `0.0 -1.0 0.42` | 0.701 | **P2** |
| 4 | CA tablet | `airplane_panel_sp` | `-0.8 0.7 -4.0` | 3 | 0.5 | `-0.35 -0.75 -0.40` | 0.93969 | **P3** |
| 5 | Console L | `airplane_inst_sp` | `-1.1471 0.0333 -4.2` | 22 | 0.4 | `0.0 -0.3 0.0` | 0.4 | **P2** |
| 6 | Console L | `airplane_inst_sp` | `-1.3463 0.0032 -3.65` | 22 | 0.4 | `0.0 -0.3 0.0` | 0.4 | **P2** |
| 7 | Console L | `airplane_inst_sp` | `-1.4369 -0.0101 -3.3` | 22 | 0.4 | `0.0 -0.3 0.0` | 0.4 | **P2** |
| 8 | FO table | `airplane_panel_sp` | `0.5577 0.3333 -4.5` | 3 | 1.0 | `0.0 -1.0 0.42` | 0.701 | **P2** |
| 9 | FO tablet | `airplane_panel_sp` | `0.8 0.7 -4.0` | 3 | 0.5 | `0.35 -0.75 -0.40` | 0.93969 | **P3** |
| 10 | Console R | `airplane_inst_sp` | `1.1471 0.0333 -4.2` | 23 | 0.4 | `0.0 -0.3 0.0` | 0.4 | **P2** |
| 11 | Console R | `airplane_inst_sp` | `1.3463 0.0032 -3.65` | 23 | 0.4 | `0.0 -0.3 0.0` | 0.4 | **P2** |
| 12 | Console R | `airplane_inst_sp` | `1.4369 -0.0101 -3.3` | 23 | 0.4 | `0.0 -0.3 0.0` | 0.4 | **P2** |
| 13 | map lamp 1 | `airplane_panel_sp` | `0 0 0` | 1 | 0.471 | `0 -0.15 -0.1` | 0.819152 | **P3** |
| 14 | map lamp 1 | `airplane_panel_sp` | `0 0 0` | 1 | 0.471 | `0 -0.21 -0.15` | 0.906307 | **P3** |
| 15 | map lamp 2 | `airplane_panel_sp` | `0 0 0` | 1 | 0.471 | `0 -1.50 0.40` | 0.906307 | **P3** |
| 16 | map lamp 2 | `airplane_panel_sp` | `0 0 0` | 1 | 0.471 | `0 -1.50 0.40` | 0.866025 | **P3** |
| 17 | map lamp 2 | `airplane_panel_sp` | `0 0 0` | 1 | 0.471 | `0 -1.50 0.40` | 0.642787 | **P3** |
| 18 | map lamp 3 | `airplane_panel_sp` | `0 0 0` | 1 | 0.471 | `0 -1.50 0.40` | 0.906307 | **P3** |
| 19 | map lamp 3 | `airplane_panel_sp` | `0 0 0` | 2 | 0.471 | `0 -1.50 0.40` | 0.866025 | **P3** |
| 20 | map lamp 3 | `airplane_panel_sp` | `0 0 0` | 2 | 0.471 | `0 -1.50 0.40` | 0.642787 | **P3** |
| 21 | map lamp 4 | `airplane_panel_sp` | `0 0 0` | 2 | 0.471 | `0 -0.15 -0.1` | 0.819152 | **P3** |
| 22 | map lamp 4 | `airplane_panel_sp` | `0 0 0` | 2 | 0.471 | `0 -0.21 -0.15` | 0.9063077 | **P3** |

Lights 13–22 sit inside four nested `ANIM_trans` groups keyed on
`ckpt/lights/{1,2,3,4}/{x,y,z}/dir` — ToLiss's movable map lamps. Transcribe those
`ANIM_trans` stacks verbatim from the source file into a **mount snippet**
(`src/lights/mounts/a3xx/maplamp{1,2,3,4}.stock.obj`), the same treatment
`beacon_fus` / `gear_left` already get. Note the `SIZE` on 21/22 differs in the last
digit from 13/14 (`0.9063077` vs `0.906307`) — Gus's own inconsistency; keep it or
normalise, it is visually irrelevant, but *decide* rather than drift.

**The colour ramps.** Gus's own data has three, after decision #1's normalisation of
#4 and #9's LED value from **A** to **D**:

| Ramp | old / new / led | Lights | Count |
|---|---|---|---|
| **P1** | A / B / **C** | dome L, dome R, pedestal flood | 3 |
| **P2** | D / D / D *(constant)* | CA+FO table, 6 console | 8 |
| **P3** | A / B / **D** | CA+FO tablet, 10 map lamps | 12 |

3 + 8 + 12 = 23 ✓. The LED variant is therefore 3 × cool + 20 × warm.

Photon reshuffles that into four. Decision #2 (§1.3b) takes the two dome lights off
**P1**, and decision #3 (§1.3c) puts the two table lamps onto it:

| Ramp | old / new / led | Lights | Count |
|---|---|---|---|
| **P1** | A / B / **C** | pedestal flood, CA+FO table | 3 |
| **P2** | D / D / D *(constant)* | 6 console | 6 |
| **P3** | A / B / **D** | CA+FO tablet, 10 map lamps | 12 |
| **P4** | **E** / **E** / **C** | dome L, dome R | 2 |

3 + 6 + 12 + 2 = 23 ✓, with a fifth colour **E** = `0.95 0.95 0.88`, fluorescent white.
P1 is now exactly "the pedestal rheostat's era-coloured lights" and P2 is the console
strips alone.

### 1.3 Decision #1 in detail

In Gus's `led` file, lights #4 and #9 (the CA/FO tablet lights) carry **A**
(`1.00 0.37 0.16`) — old-halogen orange in the LED variant, while every comparable
small directed lamp (the map lamps) goes **D**. Treated as an oversight: normalise both
to **D**, which collapses them into ramp P3.

### 1.3b Decision #2 — the dome lights leave the era ramp

Reported in-sim **2026-07-28**: the dome/storm light *changes colour temperature*
between the two halogen profiles, which reads wrong. It does — Gus puts it on **P1**,
so it swings **A** old-halogen orange → **B** amber → **C** LED blue-white, exactly like
the pedestal flood.

That is faithful to his files but not to the aeroplane. The A320's dome light is the
general cockpit floodlight — a **white fluorescent fitting**, physically unrelated to
the incandescent integral lighting behind the panel legends. It has no halogen era to
follow. Integral/flood lighting genuinely is era-coloured; the dome is not.

So the dome takes **P4**: one fluorescent white **E** `0.95 0.95 0.88` across *both*
halogen profiles, and Gus's **C** under LED (unchanged — the LED variant needed no
deviation, which is why the fidelity test only trips on `current` and `new`).

Scope is deliberately narrow:

- **The two dome lamps and `int_spot1` only.** `int_spot1` is the re-added `.acf` dome
  spot: same physical fitting, so it must share the ramp or one overhead fixture draws
  two colours at once. Pinned by `test_dome_spot_tracks_the_dome_lamps_in_every_branch`.
- **The pedestal flood stays on P1.** It is integral lighting and *should* swing with
  the era. Restoring the white-dome-over-warm-panels contrast is the entire point of
  the change, so flattening it too would undo it. Pinned by
  `test_pedestal_flood_stays_era_coloured`.
- **The constant-warm P2 lights are untouched** — Gus's CA/FO table and console lamps
  keep **D** in all three profiles.

**E** is an aesthetic seed, not a derived value. Retune it at the `fluoro` entry in the
three `int_*` palettes in `lights.style.phdsl`; the dev tuner moves geometry only, never
colour.

### 1.3c Decision #3 — the table lamps join the pedestal flood

Reported in-sim **2026-07-28**, alongside §1.3b. Gus's `CA table lights` / `FO table
lights` (#3 and #8) are aimed down and aft at a ~45° spread from `y 0.333`, so what
they actually wash is the captain's and F/O's **footwell and seat**, not a table. They
carry `INDEX 3` — the *same* `panel_brightness_ratio[3]` rheostat as the pedestal
flood — yet sit on ramp **P2**, constant warm white.

That is what produced the second half of the report: one knob driving two lights that
brighten and dim together while disagreeing about the era. Under old halogen the flood
goes deep orange and the seat wash stays near-white, which reads as a mismatch rather
than as a design.

Moved to **P1**, so they carry the flood's colour in all three profiles. The 6 console
strips keep **P2** — they are on their own `inst[22]/[23]` bank, so nothing about them
contradicts anything.

**Geometry is untouched.** Decision #3 changes colour only; both lamps keep Gus's
`cone: 0.701` (45.5°) and stay a mirror pair. A 2026-07-28 request to narrow "the
captain's flood" was applied to `int_table#ca` first and was **aimed at the wrong
light** — the reporter meant the MAIN PNL flood, not the footwell wash — and has been
reverted. `test_table_lamps_are_a_symmetric_pair` holds the line.

Worth recording for whichever light does eventually get tuned: `cone` is
**cos(half-spread)**, so it runs backwards. A *bigger* number is a *narrower* beam,
which makes "reduce the cone" an edit that *raises* the value.

| `cone` | spread |  | `cone` | spread |
|---|---|---|---|---|
| `0.701` | 45.5° | | `0.866` | 30° |
| `0.772` | 39.5° | | `0.906` | 25° |
| `0.809` | 36° | | `0.940` | 20° |

---

Decisions #1, #2 and #3 are the *only* deliberate deviations from Gus's data;
everything else is transcribed verbatim. `GusFidelityTests` pins all three — it asserts
the exact `(position, colour)` records that differ in each variant, so a fourth
deviation, or any of these spreading to more lights, fails the suite rather than
passing quietly.

### 1.4 Textures

29 PNGs, ~134 MB as shipped, **~61 MiB as one deduplicated canonical set**. All replace
stock ToLiss files. They are **variant-independent** — one set, correct for all three
looks — so they are purely an install-time concern and never switch at runtime.

| File | Stock | Gus |
|---|---|---|
| `knobs_LIT.png` | 148 KB | **16.8 MB** |
| `text_LIT.png` | 8.1 MB | 7.7 MB |
| `chairs_LIT.png` | 9.9 MB | 7.1 MB |
| `kitchens_LIT.png` | 8.5 MB | 4.3 MB |
| `pedals_details_{1,2}.png` | *(absent)* | 10.4 MB each |
| `walls_{top,outer,bottom}_LIT.png`, `panels_overhead_LIT.png`, `knobs.png` | 111–602 KB | 54–3858 KB |

**Canonical set** (resolving decision #4's drift): A320's `objects/` set, **plus**
`chairs_LIT.png` / `kitchens_LIT.png` (which appear only under his `liveries/objects/`),
**plus** `pedals_details_{1,2}.png` from A319/A321 (absent from the A320 package), with
A319's differing `knobs.png` discarded in favour of A320's. `extra-needed.png` is
documentation, not an asset — exclude it.

### 1.5 Liveries

Every filename in Gus's `liveries/objects/` folder exists in stock `objects/` on all
three airframes, byte-identical, and **nowhere else** in the aircraft tree — so all six
install to `objects/` and the folder split carries no installation meaning.

**DDS beats PNG.** Across all 27 liveries in all three local installs there is exactly
one interior override: `chairs_LIT.dds` in the A320 "JV - Frontier N369FR" livery.
X-Plane substitutes `.dds` for the `.png` an OBJ names (`chairs.obj` declares
`TEXTURE_LIT chairs_LIT.png`; that livery ships only the `.dds`, and it renders). So
dropping our `.png` into a livery folder does nothing — the fix is to back up and
**remove** the livery's `.dds` so the PNG in `objects/` resolves (§5.4).

### 1.6 The exterior collision

Each package also ships `lights_out3xx_XP12.obj` — plain modified-stock (300 lines, no
`ToLissPhoton` markers) that **collides head-on with Photon's generated exterior OBJ**.
Discarded. User-facing docs must warn that installing Gus's zip manually *after* Photon
silently reverts the exterior mod.

---

## 2. Target architecture

### 2.1 What transfers unchanged

| Photon mechanism | Applies? |
|---|---|
| DSL: one geometry + per-profile colour branches | **Yes, directly** |
| `ANIM_show`/`ANIM_hide` range gating | **Yes.** Stock `lights_inn.obj` already wraps `LIGHT_PARAM` in `ANIM_begin`; the exterior config already uses a 3-way range (`hide: 1.5 2.5 …` on `tail_nav`) |
| `XPluginStart` dataref registration (gotchas #1, #4) | **Yes**, same requirement and solution |
| Per-livery prefs JSON | **Yes** (decision #10) — absent keys default, so old prefs stay valid |
| Custom window (`NCAT` × 2 grid from `RowRects`) | **Yes structurally** — needs a 3rd column and section headers |
| Installer backup / manifest / stale detection | **Yes.** `_backup_once` + `backed_up[]` generalises to N files; `patch_glow` is the precedent for "install touches other files too" |
| Waveform engine | **Not used.** (Interior rheostats are writable, so a fluorescent warm-up is possible later — but ToLiss drives them, so it needs gotcha #5's write-race treatment. Out of scope.) |

### 2.2 Categories (decision #9)

`INDEX` in `LIGHT_PARAM airplane_panel_sp` / `airplane_inst_sp` selects the rheostat
driving brightness — `panel_brightness_ratio_{auto,manual}[INDEX]` and
`instrument_brightness_ratio_{auto,manual}[INDEX]` (`_auto` = photo-cell simulated,
`_manual` = pilot-set). Grouping by that index gives the category set:

| Category | Dataref | Index | Lights | Label |
|---|---|---|---|---|
| `dome` | `ToLissPhoton/interior/dome` | panel `[0]` | 0–1 (2) | Dome Lights |
| `map` | `ToLissPhoton/interior/map` | panel `[1]`,`[2]` | 13–22 (10) | Map Lights |
| `mainpnl` | `ToLissPhoton/interior/mainpnl` | `ckpt/lights/map` | *spot 3* (1) | Main Panel Flood |
| `pedestal` | `ToLissPhoton/interior/pedestal` | panel `[3]` | 2,3,4,8,9 (5) | Pedestal & Tables |
| `console` | `ToLissPhoton/interior/console` | inst `[22]`,`[23]` | 5–7,10–12 (6) | Console Lights |

Note `pedestal` spans several colour ramps (light 2 is P1, 3/8 are P1 after decision #3,
4/9 are P3). That is fine and matches how the exterior works — the *light type* carries
the colour ramp, the *fixture* carries the category.

**Value encoding:** `0` = old halogen, `1` = new halogen, `2` = LED. `ANIM_show` ranges
`-0.5 0.5`, `0.5 1.5`, `1.5 2.5`.

#### 2.2b `mainpnl` — why it is not part of `map` (2026-07-28)

**The rule is one category per brightness source**, because that is the only thing that
makes a category mean anything: the lights in one dim together on one knob, so they
should switch look together.

The re-added `.acf` spot 3 (§3.4) breaks the index scheme — its brightness comes from
ToLiss's `ckpt/lights/map` dataref, not a rheostat `INDEX`. It was originally filed under
`map` on the strength of that dataref's *name*. That was wrong: in the cockpit it is
driven by the **MAIN PNL** knob, while the four movable reading lamps on `panel[1]`/`[2]`
are driven by their own. One colour switch sat across two rheostats, so changing the
reading lamps also changed the main panel flood. Found in-sim by flipping single rows in
the Cockpit Custom window.

**Naming rule that falls out of this:** ToLiss's dataref name and the cockpit placard
disagree. The **placard wins for anything the user sees** (the category, its menu row,
the label) and the **dataref name wins for anything internal** (`kMapRheostatRef`, and
our own `ToLissPhoton/interior/spill/map`, which mirrors the thing it reads). Do not
"tidy" either into agreement — they are describing different things.

**Prefs migration:** a file written before the split has no `mainpnl` key, so the loader
gives it **`map`'s value** rather than the 0 default. Defaulting would silently reset an
existing Custom cockpit's flood to old halogen.

Pinned by `test_mainpnl_is_the_spill_light_and_map_is_the_reading_lamps` and by the
per-category light counts in `test_categories_carry_the_expected_lights`.

---

## 3. Build side

### 3.1 N-way branches (2-way → 3-way)

`Emitter.emit_branch` (`build/build_objs.py` ~L347) emits `ANIM_hide 1 1` for the
original branch and `ANIM_hide 0 0` for LED. **That encoding breaks at value 2** —
`ANIM_hide 1 1` does not hide when the dataref reads 2, so both branches would draw.

Replace with explicit per-value `ANIM_show lo hi`, driven by an ordered list of profile
values per category. **Keep the exterior pinned at 2 values** so its emission stays
byte-identical and `reference/photon/` + `check` stay green.

DSL change in `lights.style.phdsl`'s `categories` block — a space-separated list means
"branch *i* uses profile *i*", and the existing single-value form keeps meaning
`<value> led`:

```
categories {
  nav: halogen;                            // unchanged → [halogen, led]
  dome: int_old int_new int_led;           // new 3-way form
  map: int_old int_new int_led;
  pedestal: int_old int_new int_led;
  console: int_old int_new int_led;
}
```

New axis and palettes (interior-only, so exterior resolution is untouched):

```
axis intprofile: int_old int_new int_led;

palette int_old { white: 1 0.37 0.16;    warm: 1 0.89 0.81; }
palette int_new { white: 0.8 0.55 0.3;   warm: 1 0.89 0.81; }
palette int_led { white: 0.82 0.82 1;    warm: 1 0.89 0.81; }
```

The three ramps then fall out of two idioms:

- **P1** → `color: white;` (resolves A / B / C)
- **P2** → `color: warm;` (resolves D / D / D)
- **P3** → `color: white; @int_led { color: warm; }` (resolves A / B / D)

### 3.2 Multi-OBJ per airframe

**The simplification:** the interior OBJ has **no wing variants and no per-airframe
variants**. Wing mods don't touch the cockpit, and `lights_inn.obj` is byte-identical
across A319/A320/A321. All three looks live in the one file as `ANIM_show` branches. So
the interior artifact is **exactly one file, for everything** — wherever the exterior
loops `WINGS × AIRFRAMES`, interior is a single unconditional item.

#### `build/build_objs.py`

```python
OBJ_FOLDER  = {"a319": "A319", "a320": "A320", "a321": "A321", "a339": "A339"}
OBJ_TARGETS = {
    "a319": {"exterior": "lights_out319_XP12.obj", "interior": "lights_inn.obj"},
    "a320": {"exterior": "lights_out320_XP12.obj", "interior": "lights_inn.obj"},
    "a321": {"exterior": "lights_out321_XP12.obj", "interior": "lights_inn.obj"},
    "a339": {"exterior": "ExternalLights_XP12.obj"},          # decision #14
}
```

`a339` having no `"interior"` key is how decision #14 is enforced — same shape as
`SUPPORTED_WINGS` gating its absent wing mods.

| Site | Change |
|---|---|
| `OBJ_PATH` (L66) | delete; `list(OBJ_PATH)` at L684/710/801/814/832 → `list(OBJ_TARGETS)` |
| `HEADER` (L85) | **⚠** per-target — see §3.6 |
| `Emitter.__init__` | add `target="exterior"`; `emit()` filters fixtures to that target, picks the matching header |
| `cmd_build` (L675) | inner loop over `OBJ_TARGETS[af]`; add `--target {exterior,interior,both}`, default `both` |
| `get_reference` (L513) | take `target`; `reference/photon/<fname>` still works since filenames differ |
| `cmd_check` (L~740) | add `--target`, default `exterior`. **⚠** No interior golden exists until Phase VI freezes one, so `check --target interior` errors by design — the a339 situation, not a bug |
| `_write_live` (L~640) | **no change** — derives destination from `src.name` |
| `_patch_realwings_after_build` | **no change** — exterior-only by nature |

#### DSL (`src/lights/lights.layout.phdsl`, `build/photon_dsl.py`)

Add an optional `target:` key on `fixture`, defaulting to `exterior`. Put the interior
fixtures in the **`airframe a320` block**: `a319`/`a321` already `extends a320` so they
inherit byte-identically (correct per §1.1), and `a339` does not extend `a320` so it
inherits nothing — decision #14 for free.

#### `build/make_release.py`

`build_objs_payload` (L63) writes `payload/objs/<wing>/<fname>`. **⚠** Three airframes
writing the same `lights_inn.obj` collide on one path. Since interior is wing- and
airframe-independent, write it **once, outside the wing loop**, to
`payload/objs/interior/lights_inn.obj`.

#### `installer/`

| Site | Change |
|---|---|
| `constants.AIRFRAMES` | **⚠ leave the `fname` slot as the *exterior* file.** It feeds `detect._identify_airframe` and `_obj_marker`; `lights_inn.obj` is byte-identical across all three airframes, so adding it to the fingerprint set makes every A3xx match every other. Add a separate `OBJ_TARGETS` dict mirroring build_objs' |
| `payload.obj_text(wing, airframe)` | unchanged. Add `interior_obj_text()` taking **no arguments** |
| `payload.obj_path` | add `interior_obj_path()` → `PAYLOAD_DIR/objs/interior/lights_inn.obj` |
| `actions.install` (L226) | second `_backup_once` + `_atomic_write_bytes` + `_inject_marker` for `lights_inn.obj`, gated on opt-in. `_inject_marker` works as-is — it anchors on `POINT_COUNTS`, which `lights_inn.obj` has |
| `actions.uninstall` | **no change** — restores everything in `manifest["backed_up"]` |
| manifest | add an `interior` block (`installed`, `version`, `acf_patched`, `liveries_patched[]`). Exterior and interior are **independently installable** |
| `detect.photon_status` | **⚠** two-axis now. Handle "interior installed, exterior not" — today `_obj_marker` returns `(None, None)` when no *exterior* OBJ carries a marker, reporting the whole install as absent |

### 3.3 Custom lights — used in exactly two places

Spec-confirmed from the OBJ8 file format specification:

```
LIGHT_SPILL_CUSTOM  <x> <y> <z>  <r> <g> <b> <a>  <s>  <dx> <dy> <dz>  <semi>  <dref>
LIGHT_CUSTOM        <x> <y> <z>  <r> <g> <b> <a>  <s>  <s1> <t1> <s2> <t2>     <dref>
```

> "`<dref>` should be a **9-float array dataref that optionally modifies** the
> parameters (excluding x,y,z and dref)" — and "the parameters from the OBJ file are
> **run through** a dataref, which may modify or replace some or all of the parameters.
> **If the dataref is not found** … **then the 9 parameters are drawn unmodified.**"

Baked params are the *input*; the dataref is a filter; a missing dataref means "draw as
baked". Precedent in the local install: ToLiss's own `lights_out320_XP11.obj`
(`… AirbusFBW/LeftLogoSpill`), Laminar's `738_lights.obj` (baked rgb + brightness
dataref), Felis's `cockpit_lights.obj` (36 cockpit spill datarefs, all params zeroed —
the pattern we deliberately do **not** copy).

Because we modulate **brightness only**, `airplane_panel_sp` already does the job
natively via `INDEX` — so custom lights are used only where `INDEX` cannot express the
brightness source:

| Use | Mechanism |
|---|---|
| The 23 OBJ lights | `LIGHT_PARAM` + `ANIM_show`, 3 branches |
| `.acf` spots 1 and 2 | `LIGHT_PARAM airplane_panel_sp` + `ANIM_show`, `INDEX` 0 / 3 |
| `.acf` spot 3 (map) | **`LIGHT_SPILL_CUSTOM`** — its driver is a ToLiss dataref, not a rheostat index |
| Dev tuning harness | **`LIGHT_SPILL_CUSTOM`** everywhere, dev builds only (§3.7) |

### 3.4 The `.acf` plan (decisions #8, #13)

1. **Patch the geometry** — identical constants on all three airframes, all four files:
   `_spot1_3d_xyz/1 = 4.0`, `_spot_angle/0 = 50`, `_spot_size/{0,1,2} = 8/9/10`,
   `_spot_the/0 = -60`, `_spot_psi/0 = 180`. Variant-independent → no runtime state.
2. **Disable the three spots** via `_spot_name_3d/{0,1,2} = none` — the A339's own
   pattern (it ships `_spot_name_3d/2 none`), cleaner than relying on black-colour
   semantics.
3. **Re-add spots 1 and 2** as `LIGHT_PARAM airplane_panel_sp` in `lights_inn.obj`,
   three `ANIM_show` branches deep, `INDEX` = 0 / 3 — rheostat tie reproduced natively,
   no plugin involvement.
4. **Re-add spot 3 (map) as `LIGHT_SPILL_CUSTOM`.** Do **not** point its `<dref>` at
   `ckpt/lights/map`: the slot needs a 9-float array and that dataref is almost
   certainly scalar (it appears *only* in the four `.acf` files, never in an OBJ, so its
   type cannot be settled statically — confirm in-sim, Phase 0). Instead register
   `ToLissPhoton/interior/spill/map` as our own float[9]; the plugin reads
   `ckpt/lights/map` via the existing type-detecting `ReadDataRefDouble` helper and
   writes it into the `a` slot, passing the other eight params through unmodified.
   Colour still comes from `ANIM_show` branches.

This is the **only** per-frame interior work in the shipping plugin: one dataref read
plus one array write, for one light group. Degraded state: plugin absent ⇒ dref not
found ⇒ map light drawn at its baked brightness, ignoring the map rheostat. Bake `a`
conservatively so that failure is dim rather than glaring.

Stock values for reference (identical across A319/A320/A321, and across all four `.acf`
per airframe): `_spot_angle/* = 0 / 17 / 14`, `_spot_size/* = 20 / 28.01 / 38.01`,
`_spot_the/* = -70 / -66 / -53`, `_spot_psi/* = -20 / 0 / 14`,
`_spot_name_3d/* = panel_brightness_ratio[0] / panel_brightness_ratio[3] / ckpt/lights/map`.
The only per-airframe difference in the whole spot block is `_spot*_3d_xyz/2`
(longitudinal position: `14.75` / `9.49` / `-4.48`), which we do not touch.

### 3.5 `.acf` patching: scope, format, detection

**Scope (decision #11):** all four files per airframe — `a3xx.acf`, `a3xx_StdDef.acf`,
`a3xx_XP11.acf`, `a3xx_XP11_StdDef.acf`. All four carry identical spot values, so it is
one constant patch applied four times. (Photon's OBJ work stays XP12-only; the `.acf`
patch does not, because `lights_inn.obj` is shared by every variant — an XP11 user would
otherwise get patched OBJ lights alongside unpatched sim spots.)

**⚠ Format trap — XP12 and XP11 are written differently:**

| Property | XP12 `.acf` | XP11 `.acf` |
|---|---|---|
| `_spot_the/0` | `-70.000000000` | `-70.0` |
| `_spot_angle/0` | `0.000000000` | `0.0` |
| `_spot1_3d_xyz/1` | `1.000000000` | `1.0` |

Same values, different rendering. The patcher must **parse numerically and rewrite in
each file's own style**; detection must compare numerically. A literal string match for
`_spot_the/0 -60.000000000` works on XP12 and **silently no-ops on both XP11 files**.

**Detection (decision #12):** presence of our lines, never a hash — the `.acf`
legitimately carries other mods' edits. Sentinel is `_spot_name_3d/{0,1,2} == "none"`:
a *string* value, so it dodges the formatting problem entirely; unambiguous (stock is a
dataref path); and it is the property that actually means "our patch is live". Treat the
numeric geometry properties as corroboration, compared with a tolerance.

**Precedent and coexistence.** `.acf` patching by third-party ToLiss mods is already
normal: **Durantula patches 312 lines** of `a320.acf` (rewriting the `_obja/*`
attached-object table) and keeps an `a320.acf.durantula.bak`.

- We are safe from its *edits* — it renumbers `_obja/*`; we touch only `acf/_spot*`, a
  disjoint namespace, and never depend on `lights_inn.obj`'s attachment index.
- We are **not** safe from its *uninstall*: restoring that `.bak` wipes our spot patch.
  The line-presence check detects exactly this and reports "needs reinstall". Same
  ordering hazard as the existing RealWings interaction — document, don't prevent.

**Reversal:** uninstall restores the stock values listed in §3.4 rather than diffing,
so it composes with whatever else has touched the file since.

### 3.6 OBJ header

Stock `lights_inn.obj` header is **not** the exterior header. Emit exactly:

```
I
800
OBJ

TEXTURE	
POINT_COUNTS	0 0 1 0
```

Two differences from `build_objs.HEADER`: no `GLOBAL_cockpit_lit`, and `POINT_COUNTS`
is `0 0 1 0` not `0 0 0 0`. Both stock and Gus's files declare `0 0 1 0` while carrying
13 and 23 lights respectively, so the count is plainly ignored — match stock anyway
rather than introduce an untested difference. Note `TEXTURE` is followed by a tab and an
empty value.

### 3.7 Dev tuning harness (build this first)

A dev-only build emitting every interior light as `LIGHT_SPILL_CUSTOM` pointing at
`ToLissPhoton/dev/spill[N]`, plus an in-sim window with sliders for
r/g/b/a/size/direction/cone. Replaces the edit → build → reload loop (seconds per
iteration, plus `watch.py`'s documented reload-race risk) with instant feedback; tuned
values get baked back into the `.phdsl`.

The spec's modulation semantics make this pleasant: because baked OBJ params are the
*input*, sliders start at the real authored value and show a live delta rather than
reconstructing state from zeros.

Build it before the authoring work. It is the only consumer of custom lights besides the
map spot, so it carries no release risk — if it proves awkward we lose a tuning
convenience, not a shipping feature.

---

## 4. Plugin (`src/native/src/plugin.cpp`)

### 4.1 Datarefs

- `ToLissPhoton/interior/{dome,map,pedestal,console}` — Int **and** Float accessors,
  read-only to the sim, registered at `XPluginStart` alongside the exterior nine
  (gotchas #1 and #4 apply identically).
- `ToLissPhoton/interior/spill/map` — float[9], read-only, for the map spot (§3.4 step 4).
- No `is_led`-style alias for interior.

### 4.2 Profile model

`gValues[]` becomes per-category ranged rather than boolean. Interior gets its **own**
profile enum — the exterior profiles are meaningless for a cockpit:

```
INT_PROFILE_AUTO = 0, INT_PROFILE_OLD = 1, INT_PROFILE_NEW = 2, INT_PROFILE_LED = 3
```

**Auto (decision #15)** mirrors the *resolved exterior era*, so it needs no new
detection: exterior Classic → Old Halogen, Hybrid LED → New Halogen, Full LED → LED.
Re-resolves on the existing 1 Hz Auto flight loop. Never persisted (absence = Auto),
same as exterior.

Prefs schema — additive, so old files stay valid and absent keys mean Auto:

```json
{"<aircraft path>#<livery index>": {
  "profile": 4, "categories": {...},
  "interior_profile": 2, "interior_categories": {"dome": 1, "map": 1, "pedestal": 1, "console": 2}
}}
```

`ApplyProfileToValues` and the `Entry` struct gain interior counterparts. `IsHybridLed`
is exterior-only and unchanged.

### 4.3 Menu (nesting is available — decision correction)

```
Plugins ▸ ToLiss Photon ▸ Exterior ▸ Auto / Classic / Hybrid LED / Full LED
                        ▸ Interior ▸ Auto / Old Halogen / New Halogen / LED
                        ─────────
                        ▸ Custom…
                        ▸ Light Tuner…        (dev builds only)
```

CLAUDE.md's level-2 submenu warning is an **XPPython3 4.7a1** defect affecting
`PI_PhotonDevReload.py` — re-scope the note, don't delete it.

### 4.4 Custom window

One window, two sections with headers ("Exterior" / "Interior"): 2 buttons per exterior
row, 3 per interior row (`Old Halogen` / `New Halogen` / `LED`). `RowRects` already
computes geometry from `kCategories`, so this is a per-row column count plus a section
offset. Widen `gLabelColW` measurement to include interior labels.

---

## 5. Installer

1. **Interior is an opt-in step** — a user who wants only the exterior mod must be able
   to decline. Two independent install axes in the manifest.
2. Install `lights_inn.obj`, backed up via the existing `backed_up[]` list.
3. Install the canonical texture set (§1.4) into `<aircraft>/objects/`, backed up the
   same way. **⚠** ~61 MiB of copying — show progress; do not let it look hung.
4. **Livery scan**: for each `<aircraft>/liveries/*/objects/`, detect any interior `_LIT`
   override. Handle **both extensions** — a `.dds` wins over our `.png` (§1.5), so back
   up the livery's `.dds` and remove it, letting the PNG in `objects/` resolve.
   (PNG→DDS conversion is the alternative but needs an encoder we don't have and won't
   add for one file.) Record each in the manifest so uninstall restores it. Rare: 1
   override across 27 liveries locally. Liveries installed *later* aren't covered —
   surface a "re-run to cover new liveries" hint rather than silent partial coverage.
5. **`.acf` patch** (§3.4, §3.5) across all four files per airframe.
6. **Uninstall** restores everything in `backed_up[]`, re-removes the interior OBJ and
   textures, restores removed livery `.dds` files, and reverses the `.acf` patch by
   writing back the stock values in §3.4.

---

## 6. Distribution

Textures ship bundled (decision #7). Current `release/` is ~8.5 MB; the canonical set
adds ~61 MiB **once** (shared, not ×3). Four distributed bundles → roughly **280 MB
published per release**, up from ~35 MB. Within GitHub's limits; the costs are CI time
and download size.

`knobs_LIT.png` at 16.8 MB against stock's 148 KB is a 113× jump and looks like an
uncompressed export. Recompression is tabled (decision #5) but would likely cut the set
substantially at zero visual cost.

---

## 7. Credit (decision #6)

Gus is credited prominently, not in a footnote: repo `README.md`, the generated
per-bundle `README.txt` (`build/readme.py`), the X-Plane.org and GitHub release
descriptions, the installer's interior opt-in screen, and a header comment in the
generated `lights_inn.obj`. His licence note is *"Feel free to put this mod into your
textures. Just remember to put my name"* — exceed it, and get his preferred
name/handle/links before shipping (§9).

---

## 8. Phased task list

| Phase | Deliverable | Acceptance |
|---|---|---|
| **0** | Vendor the source data: copy the three canonical `lights_inn_*.obj` and the canonical texture set into `reference/gus/` and commit. `.scratch/` is gitignored — **the build must not depend on it** | `reference/gus/` holds the three OBJs; §1.2's table reproduces from them |
| **0** | Confirm `ckpt/lights/map`'s type in-sim (DataRefTool) | Type recorded in this doc |
| **I** | Dev tuning harness (§3.7) | Sliders move a light in-sim; values round-trip into `.phdsl` |
| **II** | N-way emitter (§3.1), exterior pinned at 2 | `build/build_objs.py check --airframe a320` still passes unchanged |
| **II** | Interior DSL: axis, palettes, 4 fixtures, 4 map-lamp mount snippets (§1.2, §3.1) | `build --target interior` emits 23 lights × 3 branches; a diff against Gus's three files shows only the decision-#1 normalisation |
| **III** | Multi-OBJ plumbing (§3.2) | Full `python -m unittest discover -s tests -v` green; the three ⚠ traps have named regression tests |
| **IV** | Plugin: datarefs, interior profile, ternary values, nested menu, 3-column Custom rows, prefs, map-spill array (§4) | Old prefs files load unchanged; menu switches all four categories; `.xpl` builds on all three arches |
| **V** | Installer: opt-in, textures, livery scan, four-`.acf` patch + line-presence detection, uninstall (§5) | Install → uninstall restores the aircraft byte-for-byte, `.acf` included; tests cover both `.acf` numeric formats |
| **VI** | In-sim validation, then freeze `reference/photon/lights_inn.obj` as the golden | 3-way switching works; spot replacement matches; plugin-absent degrades sanely; `check --target interior` passes |

Phase I pays for itself in Phase II and VI. Phases II and III are independent and can
be done in either order; III is the larger.

---

## 9. What this document cannot give you

- **Nothing here is in-sim verified.** Every claim comes from file analysis of the
  vendor packages, the stock ToLiss files, `DataRefs.txt`, and the OBJ8 spec.
- **`.scratch/gus-lighting/` is gitignored** and will not reach a fresh checkout. §1.2
  carries the full light table for exactly this reason, but the **textures cannot be
  reproduced from this document** — Phase 0's vendoring step is a hard prerequisite.
- **The local X-Plane install is the only source** for the stock comparison files
  (`lights_inn.obj`, the four `.acf`, the liveries). Paths are under
  `<X-Plane 12>/Aircraft/ToLissA3{19,20,21}_V*/`.
- **In-sim steps need the project owner** at the controls: Phase 0's dataref type check
  and all of Phase VI.

### Open risks

- **SkunkCrafts vs. the `.acf`** — the largest untested unknown. An update reverting the
  `.acf` is *handled* (detected as "needs reinstall"); untested is whether SkunkCrafts
  refuses to update, or ToLiss objects to, a modified `.acf`. Durantula's 312-line patch
  coexisting locally is encouraging but not proof.
- **Fidelity of the spot replacement** — a spill light is not guaranteed to look
  identical to a sim 3D spot at the same position/cone. Fallback: leave the spots
  enabled with an install-time colour and accept that they don't follow the in-sim
  switch.
- **`.acf` numeric-format divergence** (§3.5) — a naive patcher works on XP12 and
  silently no-ops on XP11. Test with both formats as fixtures.
- **Mod-ordering hazard** — a later Durantula uninstall wipes our spot patch. Detected,
  not preventable.
- **Maintenance obligation** — Photon becomes the distribution point for ~61 MiB of art
  we didn't author, and inherits tracking Gus's future updates.

### Questions still outstanding for Gus

1. Are the big textures recompressible, and do you have the sources? *(tabled)*
2. How would you like to be credited — name, handle, links?
