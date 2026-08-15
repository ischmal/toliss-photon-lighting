# A330-900 (a339) Support — Implementation Plan

Status: **analysis done, implementation not started.** This document was produced by an
up-front analysis pass over the installed ToLiss A339 (`Aircraft/ToLissA339_V1p0p4`,
v1.0.4) and the Photon codebase. Everything in "Findings" was verified against the real
files; everything in "Open unknowns" genuinely requires the sim. The implementing agent
should read `CLAUDE.md` and `docs/dsl.md` first, then follow the steps in order.

---

## 1. Findings (verified — do not re-derive)

### 1.1 File layout

- The A339's exterior lights live in **`objects/ExternalLights_XP12.obj`** — the A330
  equivalent of the A3xx `lights_out3xx_XP12.obj`. It is a light-only OBJ
  (`POINT_COUNTS 0 0 0 0`, no geometry), ~375 lines, all `LIGHT_PARAM`.
- Both XP12 `.acf` variants (`A330-900.acf` HD and `A330-900_StdDef.acf` SD) attach
  `ExternalLights_XP12.obj`. The XP11 acfs attach `ExternalLights.obj` — **out of
  scope**, same XP12-only policy as the A3xx.
- Replacing that ONE file covers all billboards/spills. No other exterior OBJ contains
  `LIGHT_PARAM` (only `CockpitLighting*.obj`, which is interior — leave alone).
- The aircraft folder has a `skunkcrafts_updater.cfg`, so `detect.detect_aircraft`
  works as soon as the `AIRFRAMES` row exists.
- ToLiss's original file header includes `TEXTURE ExtLights.png` — vestigial in a
  zero-geometry OBJ. The Photon `HEADER` constant in `build_objs.py` (no TEXTURE line)
  is fine as-is; the A3xx installs already prove that.

### 1.2 Light inventory of the original `ExternalLights_XP12.obj`

All lights use the XP12 `_pm` (photometric spill) + `_bb` (billboard) pairs, same
convention as the A3xx. Positions below are ToLiss's values (aircraft coords for
unmounted lights, mount-local otherwise).

| ToLiss section | Photon category | Class / index | Notes |
|---|---|---|---|
| L/R landing, inside WingFlex chain level 1 | `landing` | `airplane_landing` idx 0 (L) / 1 (R) | wing-root mounted, local pos ±0.3566 0.6737 −2.2119, dir 0 −0.05 −0.9987. **Not gear-mounted, no `LandingLightLocation` gating** (unlike A320) |
| Wingtip nav, innermost WingFlex level | `nav` | `airplane_nav` idx 0 | **Two bulb positions per tip** gated `ANIM_hide 1.5 2.5` / `−0.5 1.5` on `AirbusFBW/OHPLightSwitches[2]` (NAV1 vs NAV2 switch position — same pattern as A3xx tail_nav). L/R positions are NOT mirror images (e.g. L x=−1.4505 z=0.3359 vs R x=+1.4572 z=0.3221) |
| Wingtip strobe, innermost WingFlex level | `strobe` | `airplane_strobe` **idx 0 both sides** | L −1.5643 0.2603 0.5148, R +1.5742 0.2606 0.5093 |
| Tail nav (top level, static) | `nav` | `airplane_nav` idx 0 | dual NAV1/NAV2 gating again, pos ∓0.06 1.78 32.03, dir 0 0 1 |
| Tail strobe (top level, static) | `strobe` | `airplane_strobe` **idx 2** | 0 1.78 32.03, dir 0 0 1 |
| Beacons (top level, static) | `beacon` | `airplane_beacon` **idx 0 both** | bottom 0 −3.13 −9.3, top 0 2.85 −4.35 |
| Taxi (in nose-gear chain) | `taxi` | `airplane_taxi` | left bulb, local −0.358 −0.460 −0.242 |
| T.O. light (in nose-gear chain) | `to` | `airplane_generic` **idx 0** | right bulb +0.358, inside a static `ANIM_rotate −1 0 −0 2.5°` wrapper |
| Rwy turnoff L/R (in nose-gear chain) | `rwyturnoff` | `airplane_generic` idx 3 / 4 | each inside its own static trans+rotate wrapper (±45° about −Y) |
| Wing/engine scan lights | `wing` | `airplane_generic` idx 1 (L) / 2 (R) | **Two stations per side** (z ≈ −15.06 and −16.68), each station has TWO bulbs (local y −0.01 and +0.05), inside static trans+rotate wrappers (±145° / ±150° about −Y) |
| Logo L/R | `logo` | `airplane_generic` idx 7 / 8 | on the horizontal stab, nested inside an `AirbusFBW/PitchTrimPosition` rotate chain, then per-side static wrappers |

Category indices (generic 0=TO, 1/2=wing, 3/4=turnoff, 7/8=logo) are **identical to the
A3xx**, so the ToLiss switch plumbing is the same and existing light *types* in
`lights.style.phdsl` can be reused unchanged.

### 1.3 Structural differences vs the A3xx (why this isn't `extends a320`)

1. **WingFlex animation chains.** Wingtip nav/strobe sit inside 10 nested `ANIM_begin`
   blocks — a rotate/counter-rotate pair per `AirbusFBW/WingFlex[n]` level; left uses
   indices 0,2,4,6,8, right uses 1,3,5,7,9, and the two sides' translation values
   differ slightly (not mirrors). Landing lights sit after only the first pair.
2. **No fence/sharklet split** — no `anim/SHARK` gating anywhere; the A3xx
   fence/sharklet groups are meaningless here.
3. **No wing mods** — Durantula and RealWings do not exist for the A330. All
   `omit: realwings` / `@realwings` machinery must not apply.
4. Landing lights are **wing-mounted** (in the flex chain), not gear-mounted
   retractables; no `AirbusFBW/LandingLightLocation` hide.
5. Logo lights ride the **pitch-trim** rotation (they're on the moving stab).
6. Dual NAV1/NAV2 bulb positions at the **wingtips** too (A3xx only has that at the
   tail).
7. Doubled wing-scan stations (4 fixtures where the A3xx has 2 lights).

### 1.4 The texture fade-out effect (the user's observation) — root cause found

The glow you see **on the aircraft skin** (wingtip strobe flash lighting the wing
surface, etc.) is not a light at all: the wing/fuselage/glass meshes contain
`ATTR_light_level 0 1 AirbusFBW/ExternalLightBrightnesses[N] <nits>` regions — LIT
texture patches whose brightness tracks a **ToLiss custom float array**
(`AirbusFBW/ExternalLightBrightnesses`), animated by ToLiss's plugin with its own
strike-and-fade envelope. Photon overrides the sim's
`beacon/strobe_brightness_ratio` (billboards + spill) but nothing overrides this array,
so on the A339 the texture glow keeps ToLiss's xenon fade while Photon's billboards go
LED-square → the mismatch the user saw.

Why it's A339-specific in practice: the A320 only uses the array for landing-light wing
glow (`[0]`,`[1]` in wingL/R.obj) and two fuselage regions (`[19]`,`[20]` in
fuselage.obj); the A339 adds **strobe/nav/beacon-class glow regions**. Verified A339
usage:

| File | Indices (nits) | Hypothesis (needs in-sim confirmation) |
|---|---|---|
| `WingL.obj` (carries both sides' overlays) | `[0]`,`[1]` (5000) at wing root | L/R landing glow (matches A320's `[0]`/`[1]`) |
| `WingL.obj`, innermost flex level | `[6]`,`[9]` L / `[7]`,`[10]` R (3500) | wingtip NAV1/NAV2 bulb glows (two 540-tri meshes each side — matches the dual-bulb gating) |
| `WingL.obj`, innermost flex level | `[12]` L / `[13]` R (5000) | **wingtip strobe glows** |
| `Fuselage_Fwd.obj`, in PitchTrim chain | `[17]` (5000) | logo glow on the stab |
| `Fuselage_Fwd.obj` | `[8]`,`[11]`,`[14]` (5000) | plausibly tail NAV1 / tail NAV2 / **tail strobe** glows (pattern: 6,7,8 = NAV1 L/R/tail; 9,10,11 = NAV2; 12,13,14 = strobe L/R/tail) |
| `Fuselage_Fwd.obj` | `[19]`–`[22]` (5000) | four regions — likely beacon top/bottom + scan-light glows; **probe required** |
| `ExteriorGlass.obj` | `[15]`,`[16]` (3000) | glass glow regions; probe required |

The fix (Phase B below) is plugin-side: write Photon's waveform value into the
strobe/beacon glow indices from `EngineLoop`, after the ratio writes. The mesh OBJs
(WingL etc.) are huge, livery-textured files we must never edit.

### 1.5 Codebase touchpoints (all table-driven; verified)

- `build/build_objs.py`: `OBJ_PATH`, `AIRCRAFT_GLOB`, `REALWINGS_DIR` dicts;
  `resolve_mount` **falls back to the first variant file when the requested wing
  variant is missing** (`m.get(variant) or next(iter(m.values()))`) — so an a339
  "durantula" build would silently emit stock content. Wing support must be gated per
  airframe, not left to the fallback.
- `cmd_patch_realwings` iterates `list(OBJ_PATH)` and `_find_realwings_dir` does
  `REALWINGS_DIR[airframe]` — **KeyError for a339** unless skipped.
- `build/make_release.py` builds `WINGS × OBJ_PATH` (9 OBJs today) — must become
  "supported combos only" (9 + a339 stock = 10).
⚠ **FILE NAMES BELOW ARE PRE-PORT (this section is a 2026-07 record).** The Python
installer was deleted on 2026-08-09; `installer/constants.py` → `core/constants.cpp` (plus
the small `build/constants.py` mirror), `installer/detect.py` → `core/detect.cpp`,
`installer/actions.py` → `core/actions.cpp`, `install.py`'s screens →
`src/native/src/installer/screens.cpp`. The ANALYSIS is unaffected — only the paths moved.

- `installer/constants.py`: `AIRFRAMES` row + `REALWINGS` (no a339 entry) + the global
  `WINGS` list that `install.py screen_actions` (line ~237) offers for **every**
  aircraft — needs a per-airframe wings lookup.
- `installer/detect.py`: `_obj_marker` and `detect_aircraft` iterate `AIRFRAMES` —
  work automatically once the row exists (marker injection targets the row's
  filename).
- `src/native/src/plugin.cpp`: **no changes needed for basic support.** `IsToLiss()`
  matches "toliss" in the aircraft path (covers `ToLissA339_V1p0p4`); Auto already
  resolves ICAO `A339` → Hybrid (after the `kMfrlGate` check); the engine writes the
  sim-global ratio arrays. Phase B adds the glow-sync feature.
- `reference/photon/` goldens: none exists for a339 yet — `check` must not pretend
  one does.

---

## 2. Design decisions (already made — do not re-litigate)

1. **`airframe a339` is a standalone block, NOT `extends a320`.** The fixture sets
   barely overlap, the mounts all differ, and extending would drag in
   fence/sharklet groups and `omit: realwings` conditions that must not exist here.
   Light *types* (style file) are shared; that's where the reuse lives.
2. **No `mirror: true` on the flex-mounted fixtures.** ToLiss's left/right chains and
   light positions are deliberately asymmetric; author left and right as separate
   fixtures with ToLiss's exact per-side values. Use mirroring only if a fixture is
   verified byte-symmetric (the scan/turnoff wrappers are not — they carry per-side
   rotations anyway).
3. **Every ToLiss static wrapper (trans+rotate) stays verbatim inside a mount
   snippet** rather than being composed into pos/dir by hand — zero rotation math,
   exact fidelity, matches the existing mount philosophy. Fixtures place lights in
   mount-local coordinates. Duplicating a shared chain across several mounts (e.g.
   nose gear chain in three fixtures) is the established A3xx pattern — output
   structure need not match ToLiss's single-emission structure, only placement.
4. **Indices:** strobes L=0, R=1, tail=2 (diverging from ToLiss's L=R=0 so the
   engine's per-channel double-flash phasing applies per side — safe because the
   engine overwrites all four strobe ratio indices whenever the switch gate is on).
   **Beacons: keep BOTH on index 0 initially** (ToLiss stock). Unlike strobes, the
   beacon auto-gate reads ToLiss's own per-index activity, and we have not verified
   ToLiss's A339 drives `beacon_brightness_ratio[1]`; splitting top/bottom to 0/1
   (A3xx-style) is an optional in-sim experiment — keep only if the bottom beacon
   still flashes.
5. **XP12 only** (`ExternalLights_XP12.obj`); XP11 acfs keep stock — existing policy.
6. **Never commit ToLiss's original files.** Working copies go in `.scratch/a339/`
   (gitignored). Short verbatim mount excerpts are fine to commit (existing practice).
7. **Golden deferred.** `reference/photon/` gets an a339 golden only after the in-sim
   tuning pass is accepted; until then `check --airframe a339` should error clearly
   ("no golden yet") rather than fabricate one, and a339 is excluded from the
   default all-airframes `check` loop.
8. **Phase B (texture-glow sync) is separate and gated on in-sim probing.** Ship
   Phase A (correct lights, colors, waveforms on billboards) first.

---

## 3. Phase A — implementation steps

### Step 0 — snapshot the original
Copy `Aircraft/ToLissA339_V1p0p4/objects/ExternalLights_XP12.obj` to
`.scratch/a339/ExternalLights_XP12.obj` (reference for mounts extraction and position
checks; gitignored).

### Step 1 — build-system wiring (`build/build_objs.py`)
- `OBJ_PATH["a339"] = ("A339", "ExternalLights_XP12.obj")`
- `AIRCRAFT_GLOB["a339"] = "ToLissA339*"`
- No `REALWINGS_DIR` entry. Make `cmd_patch_realwings` (and anything else indexing
  `REALWINGS_DIR`) skip airframes without an entry, with a log line, instead of
  KeyError.
- Add a single source of truth for wing support, e.g.
  `SUPPORTED_WINGS = {"a319": ("stock","durantula","realwings"), ... "a339": ("stock",)}`
  and use it in `cmd_build` (skip unsupported combos when building all airframes for a
  given `--wing`; hard error if `--airframe a339 --wing durantula` is requested
  explicitly) so `resolve_mount`'s silent stock fallback can never ship a lie.
- `check`: exclude a339 from the default target list until a golden exists; explicit
  `--airframe a339` errors with "no golden yet — see docs/a339_plan.md".
- Optional but recommended authoring aid: `check --against PATH [--positions-only]` —
  run the existing `parse_records` normalized compare against an arbitrary file
  (the `.scratch` original), with `--positions-only` projecting records down to
  (class-family, pos, index, dir) so appearance re-authoring doesn't drown the diff.
  ~30 lines; makes "did I transcribe every light at the right place?" mechanical.

### Step 2 — mounts (`src/lights/mounts/a339/`)
Extract verbatim from the `.scratch` copy (keep ToLiss's numbers untouched, insert
`{{lights}}` at the innermost point, balance the `ANIM_end`s). `reindent` normalizes
indentation at emit time. Files:

| Mount file | Content (from the original file) |
|---|---|
| `wingroot_left.stock.obj` | first WingFlex[0] rotate/counter-rotate pair (2 nested blocks) — for the L landing light |
| `wingroot_right.stock.obj` | WingFlex[1] pair |
| `wingtip_left.stock.obj` | full left chain: WingFlex[0],[2],[4],[6],[8] pairs (10 nested blocks) |
| `wingtip_right.stock.obj` | full right chain: WingFlex[1],[3],[5],[7],[9] pairs |
| `nosegear.stock.obj` | `ANIM_trans 0 −2.34254 −24.36973` + the `acf_gear_deploy[0]` 13-key rotate |
| `nose_turnoff_left.stock.obj` | nosegear chain + the left turnoff static trans+rotate (−45°) wrapper |
| `nose_turnoff_right.stock.obj` | nosegear chain + right (+45°) wrapper |
| `nose_to.stock.obj` | nosegear chain + the static `ANIM_rotate −1 0 −0 2.5°` wrapper |
| `stab_logo_left.stock.obj` / `_right` | PitchTrimPosition chain + per-side static wrapper |
| `scan_left_fwd/aft`, `scan_right_fwd/aft` `.stock.obj` | the four wing-scan static trans+rotate wrappers (top-level, not in any chain) |

Only `stock` variants exist (no wing mods). The plain `nosegear` mount is for the taxi
fixture (its light has no inner wrapper).

### Step 3 — `airframe a339` in `src/lights/lights.layout.phdsl`
Standalone block. Fixtures (reusing existing light types; ToLiss local positions from
§1.2 as the starting point; Photon look = the A3xx type intensities, tuned in-sim
later):

- `landing_left` / `landing_right` — mount wingroot_*; Photon's landing_bb +
  landing_main/mid/wide fan (copy the A320 dir-fan pattern), index 0/1.
- `wingtip_nav_left` / `_right` — mount wingtip_*; two groups g1/g2 with the
  NAV1/NAV2 `hide:` gating (copy tail_nav's pattern), each group: nav_marker#up/#down,
  nav_beam, nav_spill at ToLiss's local pos per side. No fence/sharklet groups.
- `wingtip_strobe_left` / `_right` — mount wingtip_*; strobe_marker#a/#b, strobe_beam,
  strobe_spill; index 0 left / 1 right.
- `tail_nav` — no mount; g1/g2 gated groups at ∓0.06 1.78 32.03 (nav_tail_bb/_sp,
  dir 0 0 1 comes from the type).
- `tail_strobe` — no mount; strobe_tail_bb/_sp at 0 1.78 32.03 (type carries idx 2).
- `top_beacon` / `bottom_beacon` — no mount; beacon_bb+beacon_pm at 0 2.85 −4.35 and
  0 −3.13 −9.3; **index 0 on both** (decision #4).
- `nose_taxi` — mount nosegear; taxi_pm at ToLiss's local pos. (A3xx has no taxi_bb;
  keep parity with the a320 fixture.)
- `nose_to` — mount nose_to; to_bb/to_pm.
- `rwyturnoff_left` / `_right` — mounts nose_turnoff_*; rwy_bb+rwy_pm, index 3/4;
  lights sit near local origin (ToLiss uses 0 0 0.03).
- `wing_scan_left_fwd`, `wing_scan_left_aft`, `wing_scan_right_fwd`,
  `wing_scan_right_aft` — category `wing`, mounts scan_*; each two wing_bb+wing_pm
  bulbs at local y −0.01 / +0.05, index 1 left / 2 right.

Acceptance for steps 2–3: `python build/build_objs.py build --airframe a339` emits
`dist/A339/objects/ExternalLights_XP12.obj`; the `--against` compare (or a manual
`parse_records` diff) shows every original light has a Photon counterpart at the same
position/index under the same ToLiss gate, in both the halogen/xenon and LED
`ANIM_show` branches (positions equal; classes/colors/intensities intentionally
Photon's own).

### Step 4 — installer
- `installer/constants.py`: `AIRFRAMES["a339"] = ("ToLissA339*", "A339",
  "ExternalLights_XP12.obj", "ToLiss A330-900")`; add
  `WINGS_FOR = {airframe: [...]}` (a339: `["stock"]`) next to `WINGS`.
- `install.py screen_actions`: build the wing-action list from `WINGS_FOR[airframe]`
  instead of the global `WINGS` (a339 shows Install/Reinstall + Uninstall only).
  Audit the other `WINGS` uses (default-index selection ~line 254) for the same.
- `installer/actions.py` / `payload.py`: verify (don't assume) they're fully driven by
  the `AIRFRAMES` row + `objs/<wing>/<fname>` payload layout; the RealWings branches
  are already gated on `wing == "realwings"` which a339 can never select.
- `build/make_release.py`: build the payload from supported combos (10 OBJs), reusing
  Step 1's `SUPPORTED_WINGS`.

### Step 5 — tests (stdlib unittest; `python -m unittest discover -s tests -v`)
- `test_detect.py`: a339 fake tree (skunkcrafts cfg + `ExternalLights_XP12.obj`) —
  detection row, marker/status round-trip, stale case.
- `test_actions.py`: a339 install/uninstall round-trip against the real built payload;
  assert backup/restore of `ExternalLights_XP12.obj` and marker injection.
- `test_install_e2e.py`: scripted flow for an a339 aircraft; assert no
  Durantula/RealWings options are offered.
- `test_patch_realwings.py`: patch-realwings with no `--airframe` skips a339 without
  error.
- `test_payload.py`: payload obj enumeration includes `objs/stock/ExternalLights_XP12.obj`
  and no a339 file under durantula/realwings; dev-mode `obj_text("a339")` works.
- Update any assertion pinning "9 OBJs".

### Step 6 — docs
README supported-aircraft section; `docs/dsl.md` only if any construct was actually
added (the existing syntax covers everything above); CLAUDE.md handoff notes (A339
section: file name, no wing mods, glow-array facts).

### Step 7 — in-sim verification (author + Sonnet driving `watch.py`)
The dev loop works once Step 1 lands (`build --write` auto-locates via
`AIRCRAFT_GLOB`; `watch.py --write` + `PI_PhotonDevReload` quick-reload are
aircraft-agnostic; the dev plugin's path-scoped plugin-disable covers the A339's
AirbusFBW/SASL plugins). Checklist:
- [ ] All nine categories switch color sets via the menu (Custom window, per category).
- [ ] Beacon + strobe billboards follow the LED/Classic waveforms (ToLiss A339 holds
      `override_beacons_and_strobes` like the A3xx — the engine logs a NOTE if not).
- [ ] NAV switch positions 1 vs 2 move the nav bulbs (wingtip AND tail).
- [ ] Strobe L/R/tail all flash (confirms the 0/1/2 index choice); beacon top+bottom
      flash on index 0; optionally try the 0/1 beacon split (decision #4).
- [ ] Gear-swing: taxi/TO/turnoff lights track gear retraction; logo lights track
      pitch trim; wingtip lights track wing flex (load fuel / turbulence).
- [ ] Per-livery persistence + Auto profile (A339 → Hybrid unless `kMfrlGate` fires).
- [ ] Installer round-trip on the real aircraft incl. "Needs reinstall" staleness.
- [ ] Check whether the `.acf` bakes a Plane-Maker taxi/landing light (the A3xx
      "extra taxi billboard" gotcha #8) — note it if so, as on A3xx.
Then freeze the accepted build as `reference/photon/ExternalLights_XP12.obj` and lift
the Step 1 `check` exclusion.

---

## 4. Phase B — texture-glow sync (the fade-out fix) — IMPLEMENTED 2026-07-21

Goal: the `ATTR_light_level` skin glows follow Photon's beacon/strobe waveform instead
of ToLiss's baked fade, so LED profiles flash crisp on the skin too.

**Approach pivot: `AirbusFBW/ExternalLightBrightnesses` is READ-ONLY to plugins**
(confirmed in DataRefTool 2026-07-21). The original write-into-ToLiss's-array plan (below,
kept for history) was abandoned — X-Plane silently drops writes to a read-only dataref, and
claiming ToLiss's dataref name is a dead end. Instead we **redirect the OBJs to a dataref we
own**:

1. **Plugin owns the array.** `plugin.cpp` registers `ToLissPhoton/exterior/glow` (float
   array, read-only to the sim, backing store `gGlow[24]`) at `XPluginStart` — before any
   mesh OBJ binds it (gotcha #1). `GlowMapForIcao` maps engine channels → indices (A339:
   strobe ch0→[12], ch1→[13], ch2→[14]; beacon→[15],[16]). `EngineLoop` sets `gGlow[idx]` to
   the SAME value the billboard got — and to **0 when the light is off** (we own the array,
   nothing else drives it down). Confirmed in-sim indices, not the §1.4 hypothesis guesses.
2. **Installer redirects the OBJs.** `core/patch_glow.cpp` swaps the dataref token per region:
   `AirbusFBW/ExternalLightBrightnesses[N]` → `ToLissPhoton/exterior/glow[N]` (same N, trailing
   nits untouched — ToLiss already uses the `[index]` subscript form so the line parses
   identically). Only the 5 driven indices; every other glow region still reads ToLiss's
   array. Backed up before patching (existing `backed_up` list), reversed on uninstall; gated
   on `constants.GLOW_AIRFRAMES`. Idempotent; `--reverse` restores byte-for-byte. Dev CLI:
   run as part of `photon-installer install --aircraft <dir>` (the standalone
   `build/patch_glow.py` command was deleted with the Python installer, 2026-08-09).
3. **Coupling to remember:** the three index lists (`plugin.cpp GlowMapForIcao`,
   `GlowRedirect()` in `core/constants.cpp` — now ONE table all of them read) MUST agree; a redirected index the
   plugin never drives goes dark, and the mesh patch requires the new plugin deployed first.
4. **A3xx follow-up:** if A320's `[19]`/`[20]` (fuselage) turn out to be beacon glows, add
   them to all three lists; same latent mismatch, just fainter.

*Original (abandoned) write-based plan, for history:* probe each index by writing
`ExternalLightBrightnesses[i]=1`; race-check a 1 Hz square from `EngineLoop` after the ratio
writes; `XPLMSetDatavf` each mapped element. Dropped once the array proved read-only.

---

## 5. Do-NOT list

- Don't edit the GEOMETRY/textures of `WingL/WingR/Fuselage_*/ExteriorGlass.obj` — they're
  huge livery-bound files. The ONE allowed automated edit is `patch_glow`'s dataref-token
  redirect (a backed-up, reversible per-region string swap that adds no content) — see §4.
- Don't commit ToLiss originals; `.scratch/` only. Mount excerpts are the exception.
- Don't `extends a320` for a339; don't add fence/sharklet or realwings conditions to
  a339 fixtures.
- Don't let a339 build/install under durantula/realwings (the `resolve_mount` stock
  fallback makes this a *silent* failure today — that's why Step 1 gates it).
- Don't repoint `check` at `dist/` or generated output (CLAUDE.md's one trap).
- Don't hand-edit `dist/`; don't put positions in Python — placement lives in the DSL.
