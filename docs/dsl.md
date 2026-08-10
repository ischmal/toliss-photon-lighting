# Photon Light DSL — syntax & semantics

The light config is a CSS-like DSL split HTML/CSS-style across two files:

| File | Holds | Analogy |
|---|---|---|
| [src/lights/lights.style.phdsl](../src/lights/lights.style.phdsl) | axes, palettes, categories, reusable **light types** | the stylesheet |
| [src/lights/lights.layout.phdsl](../src/lights/lights.layout.phdsl) | mounts, per-airframe **fixtures** (positions, gating) | the document |

Both carry the `.phdsl` extension so editors can identify the language; see
[§7 Editor support](#7-editor-support).

`build/photon_dsl.py` parses both into the exact same dict the old
`reference/legacy/lights.poc.jsonc` decoded to, so `build/build_objs.py` (Emitter,
`check`, `bootstrap`) is unchanged downstream. The migration was verified by
**byte-identical** Emitter output across all 3 airframes × 3 wing variants.

Builds land in `dist/` (generated; not committed). `check` compares a build
against the frozen hand-authored goldens in `reference/photon/` — never against
`dist/`, which would compare generated output with itself and always pass.

```
python build/build_objs.py build [--airframe a320] [--wing durantula] [--out DIR] [--write]
python build/build_objs.py check --airframe a320
python build/build_objs.py patch-realwings --airframe a320 [--dry-run] [--reverse]
python build/watch.py [--write]  # rebuild on save
```

`--write` also installs each built OBJ into the live X-Plane install, auto-locating
`<X-Plane 12>/Aircraft/ToLissA3XX*/objects/`. `watch.py --write` is the live-iteration
loop: save the config, the OBJ lands in the sim, reload the aircraft. (This replaced
`tools/Link-Objects.ps1`, now removed; `--write` clears any leftover symlink.)

`build_objs.py` is the only entry point; `patch_realwings.py` is a module it
drives, not a script. **`patch-realwings` writes to the live X-Plane install**
(it auto-locates the mod folder) — use `--dry-run` to preview and `--reverse` to
restore from the `.bak` files it leaves. Under `--wing realwings` the wingtip
lights are patched into the mod in place rather than emitted into `lights_out`,
but they still take their parameters from the DSL — see
[design.md §10.5](design.md#10-sequencing--status).

---

## 1. Lexical basics

- `//` line comments and `/* */` block comments anywhere.
- Statements are one of three shapes:
  - **declaration** — `name: value ...;` (values are space-separated; numbers,
    words, `true`/`false`/`none`)
  - **block** — `word word "optional string" { statements }`
  - **reference** — `name;` (a bare light-type reference inside a fixture/group)
- Words may contain `/`, `[`, `]`, `#`, `.`, `-` — datarefs
  (`AirbusFBW/OHPLightSwitches[2]`), paths, and labels need no quoting.
  Quoted strings are only used for human-readable comments on fixtures/groups.

## 2. Axes and condition blocks (the one variant mechanism)

Variant axes are **declared as data**, not baked into the parser:

```
axis profile: halogen xenon led;
axis side:    port starboard;
axis swatch:  bb sp;            // billboard vs spill, derived from the light class
axis wing:    stock durantula realwings;   // wing mod, fixed per build by --wing
axis wingtip: fence sharklet;   // RealWings CEO vs NEO mod OBJ — used ONLY on the
                                // @realwings wingtip offset (see §offset below)
```

Anywhere a declaration can appear, `@<axis-value> { ... }` scopes the
declarations inside it to matching contexts; **nesting means AND**:

```
intensity: 5000;                                   // base: all contexts
@starboard { intensity: 4000; }                    // side = starboard
@led { intensity: 6000;  @starboard { intensity: 5000; } }   // led AND starboard
```

**Resolution rule** (deliberately *not* the CSS cascade): for a concrete
context, the applicable declaration with the **most conditions wins**. Two
*different* values tied at the winning specificity are a **build error** — no
source-order tiebreak, no specificity wars. A later declaration with the
*identical* condition set replaces an earlier one (this is what `extends`
relies on).

Practical corollary: if you condition the same property on two different axes
(e.g. a top-level `@starboard` and an `@led`), also write the combined
`@led { @starboard { ... } }` value so the overlapping context has a unique
winner.

### Free axes vs. bound axes

`profile`, `side` and `swatch` are **free**: one built OBJ contains every
profile branch and both sides, so a conditioned value expands into the nested
dicts the Emitter resolves at emit time.

`wing` is **bound**: `--wing` picks one variant per build, so it is supplied as
a fixed context when the config is loaded. `@realwings { ... }` therefore acts
as a *filter* — it adds specificity and either applies or doesn't — and never
becomes a key in the expanded value. Everything collapses to a scalar before
the Emitter sees it, which is why nothing in the Emitter knows about `wing`.

Two consequences worth knowing. A config loaded for one variant is **specific to
it** and is tagged `wing`; pairing it with an `Emitter` built for another is an
error rather than silently wrong output, so call `load_config(wing=...)` with
the variant you're emitting. And because a bound condition can filter *every*
declaration out, a property conditioned only on `@realwings` simply has no value
under `--wing stock` — that's the "no applicable value for any context" build
error. Write an unconditioned base alongside it:

```
offset: 0 0 0;                      // stock
@realwings { offset: 0.05 0 0.11; }
```

## 3. lights.style.phdsl

```
palette xenon {
  red:   1 0.03 0.05;
  white: 0.75 0.75 1;  @bb { white: 0.45 0.45 1; }   // billboard runs bluer
}

categories {            // which palette the dataref=0 (original) branch uses
  nav: halogen;
  strobe: xenon;
  ...
}

light nav_base {        // mixin: no class, never referenced directly
  color: red;
  @starboard { color: green; }
}

light nav_beam extends nav_base {      // parent declarations are mixed in first
  class: airplane_nav_bb;
  intensity: 5000;  @starboard { intensity: 4000; }
  @led { intensity: 6000;  @starboard { intensity: 5000; } }
  cone: 0.2;
}
```

Light-type properties: `class` (required for anything a fixture references),
`color`, `intensity`, `cone`, `dir`, `index` — every one of them may sit inside
condition blocks. The bb/sp palette swatch is picked automatically from the
`class` suffix (`*_bb` → bb, else sp).

#### `aim:` and `spread:` — angles instead of a vector and a cosine

Two properties are **sugar, resolved at load**, before the Emitter sees anything:

| Write | Instead of | Meaning |
|---|---|---|
| `aim: <pitch> <yaw>;` | `dir: <x> <y> <z>;` | degrees |
| `spread: <degrees>;` | `cone: <cosine>;` | half-angle |

```
light screen_glow {
  spread: 26;      // == cone: 0.899
  aim: 0 180;      // == dir: 0 0 1  (straight aft)
}
```

**Convention**, in OBJ axes (+X right, +Y up, +Z aft):

- `pitch` — degrees above horizontal. `0` level, `+90` straight up, `-90` straight down.
- `yaw` — aircraft heading, clockwise seen from above. `0` **forward** (−Z), `90` right
  (+X), `180` aft (+Z), `-90` left (−X).

There is deliberately **no roll**. A cone is rotationally symmetric about its own axis,
so two angles exhaust the degrees of freedom; a roll knob would be a control that
provably does nothing — the same reasoning that keeps dead rows out of the menus.

Why they exist, beyond readability:

- **`cone` runs backwards.** It is a cosine, so widening a beam *lowers* it. That is how
  "reduce the cone" became an edit that raises the number. `spread` reads the right way.
- **An `aim:` cannot express a non-unit vector.** That is not a style preference — see
  below. Angles make the whole class of bug unrepresentable.
- The dev light tuner edits **pitch/yaw/spread**, not components, and "Log DSL" prints
  `aim:`/`spread:` ready to paste (`docs/dev-tools.md`).

Both forms are accepted on a light **type** and on a per-light override, so whichever
reads better at that spot wins. Declaring both forms of the same property
(`dir:` *and* `aim:`) is an **error**, not a precedence rule: there is no reading of it
that is not a mistake, and silently preferring one would be a value the author never
sees.

#### `dir` should be unit length, and nothing enforces it

`cone` is cos(half-spread), and X-Plane compares it against a **dot product** with
`dir`. So a `dir` that is not unit length makes cone and aim *interact*: the beam
narrows or widens as a side effect of its own direction, and past a point the cone
control stops appearing to do anything at all. Two standing in-sim reports fit that
shape (`docs/dev-tools.md`).

`build` reports every non-unit `dir:` with its unit form ready to paste:

```
note: 6 light type(s) have a non-unit `dir:`. ...
    int_panelflood   dir 0 -0.15 -0.1   |d| 0.180   unit: 0 -0.832 -0.555
```

**It reports rather than rewrites, deliberately.** Normalizing should be a visible,
reviewable edit to the `.phdsl`, not something a build does behind your back.
`dir: 0 0 0` is omnidirectional and is never reported.

The **whole interior was normalized on 2026-07-29** (interior deviation #5) after the
maths turned out to explain two standing bugs — Gus's outer panel floods are 0.180 and
0.258 long against cones of 0.819 and 0.906, i.e. effective thresholds of 4.5 and 3.5,
which nothing can satisfy. `GusFidelityTests` compares aims with length canonicalized
out, so a real change of *direction* still fails loudly.

The **exterior is deliberately left alone**. `landing_mid` (1.031) and `landing_wide`
(1.099) are within 10% of unit, are ToLiss's own transcribed values, have no in-sim
complaint against them, and — decisively — the exterior's byte-identical output
against the frozen `reference/photon/` goldens is the project's only exterior
correctness net. Changing them to chase a ≤9% cone shift would spend that net for
nothing. They stay in the build's report as a standing note, not as a to-do.

### N-way categories

A `categories` entry with **one** value is the two-way form and still means
`<palette> led` — branch 0 uses that palette, branch 1 uses `led`. With **two or
more**, branch *i* uses profile *i* and is gated on the category dataref reading
*i*:

```
categories {
  nav: halogen;                       // 2-way -> [halogen, led]
  dome: int_old int_new int_led;      // 3-way -> branch i == dataref value i
}
```

The two forms emit **different gate encodings**, and this is load-bearing:

| branches | gate emitted for branch `i` of `n` |
|---|---|
| 2 | `ANIM_hide 1 1 <dref>` / `ANIM_hide 0 0 <dref>` |
| 3+ | `ANIM_hide -0.5 <i-0.5> <dref>` (if `i > 0`) **and** `ANIM_hide <i+0.5> <n-0.5> <dref>` (if `i < n-1`) |

Both forms are **hide**, never `ANIM_show`. An OBJ8 animation block starts
*visible*, and a show/hide only acts while its range **matches** the dataref — so
a lone `ANIM_show -0.5 0.5` leaves the branch visible at `2` as well, every branch
draws at once, and switching the dataref does nothing. (That bug shipped in the
interior and was caught in-sim on 2026-07-28.) With hides, "no match" is exactly
the visible state, so a branch is on for its own value and off everywhere else.

A three-way category therefore hides the open-ended range on each side of its own
value; the middle branch emits two hide lines and the end branches one each.
Multiple `ANIM_hide` in a single block AND together, so no nesting and no
per-profile boolean datarefs are needed — `branch_gate` returns a list of lines.

**Keep every exterior category one-valued**: that is what pins exterior output to
the original one-line encoding and keeps the frozen `reference/photon/` goldens
passing `check`.

### `size:` vs `intensity:`

Slot 8 of a `LIGHT_PARAM` is class-dependent. The exterior `*_bb` / `*_pm`
classes read it as a photometric intensity and X-Plane wants the `cd` suffix;
the interior's `airplane_panel_sp` / `airplane_inst_sp` read the same slot as a
bare fractional **size**. Declaring `size:` selects the bare rendering,
`intensity:` the `…cd` one. Never both on one light.

### `spill_dref:` — custom lights

A light type carrying `spill_dref` emits a `LIGHT_SPILL_CUSTOM` instead of a
`LIGHT_PARAM`, and needs no `class`:

```
LIGHT_SPILL_CUSTOM  x y z  r g b a  s  dx dy dz  semi  <dref>
```

`alpha:` supplies the `a` slot. X-Plane runs the **baked** parameters through
that dataref — a plugin may modify them in place — and draws them unmodified if
the dataref isn't found, so bake `alpha` at the value you want in the
plugin-absent case. Used for exactly one light (the interior map spot, whose
brightness source is a ToLiss dataref rather than a rheostat `index`).

### `optimize:` — a cheaper alternative for a stack of lights

A light may declare how it behaves when the user asks for fewer lights:

```
int_panelflood#a { cone: 0.819;  optimize: boost 1.5; }   // the reduced set
int_panelflood#b { cone: 0.906;  optimize: drop; }        // stack only
```

`drop` means "not in the reduced set"; `boost <f>` means "this light IS the
reduced set, at `<f>`× its slot-8 magnitude" (`size:` here, `intensity:` on an
exterior class) to stand in for the ones that went away. A light declaring
**neither** is emitted once and unconditioned, so a group with no `optimize:`
anywhere — which is everything but the four main panel floods — is byte-identical
to what it was before the feature existed.

Where both appear, the emitter writes the group twice, gated against each other
on `ToLissPhoton/<target>/optimized`:

```
ANIM_begin
  ANIM_hide 1 1 ToLissPhoton/interior/optimized     # the authored stack
  ... every drop + boost light, at its authored size ...
ANIM_end
ANIM_begin
  ANIM_hide 0 0 ToLissPhoton/interior/optimized     # the reduced set
  ... the boost lights only, scaled ...
ANIM_end
```

⚠ **0 is the authored look, and that is not arbitrary.** An `ANIM_hide` whose
dataref does not exist reads 0 forever, so with the plugin absent — or older than
the OBJ — you get what the DSL authored, never a reduced set nobody chose.

⚠ **Exterior lights may not declare it.** No `ToLissPhoton/exterior/optimized`
exists, so it would fail open and silently while breaking the frozen goldens;
`build` refuses it instead. A malformed value is refused too, rather than falling
through as "no optimize" — which would put the whole stack in the reduced set and
read in-sim as the switch doing nothing.

**Which light to keep** is a judgement, not a rule the builder can apply: for the
panel floods it is the widest cone of each stack (cone is a cosine, so the
*smallest* number), because that is the one that sets where the pool reaches.
`tests/test_interior.py::OptimizedLightCountTests` pins that per fixture.

### The interior profile axis

`axis intprofile: int_old int_new int_led;` is the interior's own profile
dimension, deliberately separate from `profile`. No light carries both, so
keeping them apart means an `@int_led` block never expands into a
halogen/xenon/led cross product — and exterior resolution stays untouched.

## 4. lights.layout.phdsl

```
mounts a320 {
  gear_left {
    stock:     src/lights/mounts/a320/gear_left.stock.obj;
    durantula: src/lights/mounts/a320/gear_left.durantula.obj;
    realwings: src/lights/mounts/a320/gear_left.realwings.obj;
  }
}

airframe a320 {
  fixture wing_nav "Wing Nav" {
    category: nav;                     // -> ToLissPhoton/exterior/nav gating
    mirror: true;                      // emit port + X-mirrored starboard copy
    omit: realwings;                   // skip under this wing variant
    mount: wing_left;  @starboard { mount: wing_right; }
    group fence {                      // named group with its own sub-gate
      hide: 1 1 anim/SHARK;            // ANIM_hide <lo> <hi> <dataref> order
      pos: -0.194 0.021 -1.205;        // group default position
      nav_marker#up   { dir: -0.354  0.866 -0.354; }
      nav_marker#down { dir: -0.354 -0.866 -0.354; }
      nav_beam        { dir: -0.574  0 -0.819; }
      nav_spill       { pos: -0.175 0.021 -1.181;  dir: -0.865 -0.16 -0.476; }
    }
    group sharklet { ... }
  }

  fixture tail_strobe "Tail Strobe" {  // no groups: lights sit directly here
    category: strobe;
    pos: 0 0.7 31.32;
    strobe_tail_bb;                    // bare reference = type defaults only
    strobe_tail_sp;
  }

  fixture landing_spill { raw: src/lights/raw/a320/landing_spill.obj; }  // verbatim
}
```

- **Fixture properties:** exactly one of `category` / `profile` (unless `raw`),
  plus `mirror: true`, `omit: <variant> ...`, `mount` (per-side via `@starboard`;
  `none` detaches), `pos`, `offset`, `raw`.
- **Position fallback** per light: light `pos` → group `pos` → fixture `pos`.

### `category:` vs `profile:` — gated or not

A fixture is rendered one of two ways, and must say which:

| | Emits | Switchable at runtime |
|---|---|---|
| `category: nav;` | one `ANIM_hide`-gated branch per profile the category declares | yes, via `ToLissPhoton/<target>/nav` |
| `profile: screen;` | **one un-gated block** in palette `screen` | no — there is nothing to switch |

`profile:` exists for a target with a **single look**, which so far means only the
screen glow. Writing that as a category would force either a two-branch category
whose branches render identically — **a dead menu row**, which has shipped twice on
the interior and both times looked exactly like the mod not being installed — or a
fake second palette existing only to satisfy the one-value shorthand.

An un-gated fixture is not a static one: the screen lights vary per frame through
their spill dataref's **alpha**, which is a brightness channel, not a gate. Reach
for `profile:` when a fixture has one appearance, never to skip writing a category
that genuinely has two.

Declaring both, or neither, is a parse error.

### `offset` — moving a whole fixture per wing mod

`offset: <x> <y> <z>` translates the entire fixture — mount rigging included —
in **aircraft coordinates** (+x right, +y up, +z aft), emitted as a static
`ANIM_trans` wrapping it:

```
fixture landing_left "Retractable Landing Light #1" {
  category: landing;
  mount: gear_left;
  pos: 0.149 -0.03 -0.06;
  offset: 0 0 0;                          // stock matches ToLiss
  @realwings { offset: 0.05 -0.02 0.11; } // the mod's housing sits elsewhere
}
```

This is the knob for "the mod's lights are in the wrong place." A mount's own
`ANIM_trans` is first in its chain and shares this frame, so an `offset` adds to
it linearly — it is numerically identical to hand-editing the mount snippet's
placement, but it's data, it's per-variant, and it leaves the snippet shared.
A zero offset emits nothing, so stock output is untouched.

Note the difference from `pos`: light positions are **mount-local** and are the
same across wing mods; `offset` moves the mount itself. Editing the fixture `pos`
would also only move lights that don't declare their own.

Deliberate limits: `offset` is translation-only, may vary by `side`, `wing`, and
`wingtip` (the last only on the RealWings-omitted wingtip fixtures — the emitter
rejects a `@fence`/`@sharklet` offset on a fixture it actually emits) but not by
`profile`/`swatch` (one fixture spans every profile branch — the build rejects
it), and is unsupported on a `raw` fixture, whose block is copied verbatim. Rotations and animation keyframes stay in the mount snippet: they are
frozen rigging lifted from ToLiss and are not what differs between wing mods.
A mod needing a different *rotation* is what the per-variant mount slot is for.

**`offset` on an `omit`ted fixture — the RealWings wingtip case.** `wing_nav` and
`wing_strobe` are `omit: realwings` (RealWings bakes its *own* wingtip lights into
its OBJs, so Photon emits none there), yet they still carry a `@realwings
{ offset }`. That offset is **not** emitted as an `ANIM_trans` — instead
`build/patch_realwings.py` reads it (via `load_config(wing="realwings")`, which
resolves `@realwings` onto the fixture) and **adds it to RealWings' baked X/Y/Z**,
mirroring X per side exactly as the emitter would. This is the *same* "mod lights
in the wrong place" knob, just applied by the patcher because RealWings owns the
geometry. Frame difference: the emitter's `ANIM_trans` is aircraft-coordinate,
but the patcher adds into the baked light's **mount-local** position (deep inside
RealWings' wing-flex animation), which is what keeps the light glued to the
wingtip through flex. A zero offset is a byte-for-byte pass-through.

CEO and NEO ship as **separate** RealWings OBJs (`Main.obj` vs `MainNEO.obj`) and
can need different corrections, so the offset is split by the **`wingtip` axis**
(`axis wingtip: fence sharklet;`, `fence` = CEO, `sharklet` = NEO):

```
// on wing_nav / wing_strobe (omit: realwings)
offset: 0 0 0;                          // stock/durantula: no shift
@realwings {
  offset: 0 0 0;                        // NEO / sharklet OBJ: correct as baked
  @fence { offset: 0.08 -0.15 0; }      // CEO OBJ: the correction
}
```

`wingtip` is a **free** axis but appears *only* here: the emitter never resolves
these fixtures (they're omitted for RealWings) and no other property conditions on
`@fence`/`@sharklet`, so nothing else expands over it. `patch_realwings.py` picks
the value by the mod OBJ's filename (`…NEO.obj → sharklet`, else `fence`) and, per
airframe, honors a319/a321 `extends` overrides of the offset (appearance is
inherited unchanged, so only placement varies). Don't confuse the `@fence`/
`@sharklet` *conditions* with the `group fence`/`group sharklet` blocks in the same
fixtures — same words, unrelated: the groups are the (omitted) stock lights, the
conditions select the CEO/NEO correction.
- **Labels** (`type#label`) name a light within its group so selectors can
  target it; required only when the same type appears twice in one group
  (`#up/#down`, `#left/#right`).
- Light references may override any type property inline, conditions included
  (see `bottom_beacon`'s per-profile intensities).

### Airframe overrides (selectors, not positional merge)

```
airframe a319 extends a320 {
  tail_nav {                                   // selector: fixture
    g1 nav_tail_bb { pos: -0.042 0.706 29.14; }   // selector: group light
    g1 nav_tail_sp { pos: -0.029 0.706 29.14; }
  }
  top_beacon { mount: none; }                  // fixture-level property override
  wing_nav { sharklet nav_spill { pos: -0.098 -0.003 -1.302; } }
  landing_spill { raw: src/lights/raw/a319/landing_spill.obj; }
}
```

`extends` deep-copies the base airframe, then each override block walks a
**name path** — `fixture [group] [light]` — and applies its declarations to
that node. Selector paths may be written flat (`tail_nav g1 nav_tail_bb { ... }`)
or nested as above. A light selector must match exactly one light (use
`type#label` or bare `#label` to disambiguate). A full `fixture name { ... }`
block inside an extends airframe *replaces* the inherited fixture.

## 5. What lives where (unchanged from the jsonc design)

- Transform rigging stays in verbatim **mount** snippet files with a
  `{{lights}}` slot (`src/lights/mounts/`), and opaque decorative blocks in
  `raw` files (`src/lights/raw/`). The DSL models the frequently-edited leaf
  lights + gates — plus, since a mount's *placement* turned out to need tweaking
  per wing mod, the `offset` above. The boundary is empirical: what you have to
  tune lives in the DSL, what you copy from ToLiss and freeze lives in a
  snippet. A per-variant snippet whose only difference from stock is its
  placement should be deleted in favor of an `offset`.
- `reference/legacy/lights.poc.jsonc` is **superseded** but kept as a frozen
  record of the migration source; `load_config()` only falls back to it if the
  DSL pair is missing. Don't edit it.

## 6. Where everything lives

```
src/lights/      lights.style.phdsl, lights.layout.phdsl, mounts/, raw/  ← you edit this
src/native/      the shipping plugin (C++ .xpl)                ← you edit this
build/           the generator (code only)
dist/            generated installable X-Plane tree  (gitignored)
reference/photon/  frozen goldens for `check`
reference/gus/     vendored interior source data (Gus's OBJs + textures)
reference/legacy/  superseded configs
```

The rule: **everything under `src/` is hand-authored, everything under `dist/`
is generated.** `reference/` is a third category: frozen inputs and goldens that
are neither edited nor generated.

### Targets

Each airframe builds one OBJ **per target** — `exterior` (`lights_out3xx_XP12.obj`),
`interior` (`lights_inn.obj`, the cockpit lights) and `screens`
(`lights_screens.obj`, the EXPERIMENTAL display glow). A fixture declares
`target: interior;` or `target: screens;` to route into those; the default is
`exterior`, so every pre-existing fixture is unaffected.
`build --target {exterior,interior,screens,both}` selects.

⚠ **`both` means exterior + interior — it never builds `screens`**
(`DEFAULT_TARGETS` in `build/build_objs.py`). `dist/` is what `make_release.py`
packages, so an experiment must be asked for by name rather than arriving in a
release bundle by default. See `docs/screens_plan.md`.

An airframe with no entry for a target in `OBJ_TARGETS` doesn't build it at all —
that's how the A330-900 is kept out of the interior mod, mirroring how
`SUPPORTED_WINGS` gates its absent wing mods. The interior fixtures live in the
`airframe a320` block so a319/a321 inherit them verbatim via `extends`, and a339
(which doesn't extend a320) inherits nothing.

## 7. Editor support

`.vscode/settings.json` maps `*.phdsl` to VS Code's **SCSS** grammar. This is a
borrowed grammar, not a real one for this language, but the overlap is close:
`//` and `/* */` comments, `name: value;` declarations, `{ }` blocks with
matching/folding, numbers, and `@led { ... }` condition blocks (SCSS reads them
as at-rules, which lands about where you'd want the emphasis anyway).

What it doesn't do: `axis` / `palette` / `light` / `fixture` / `airframe` get no
keyword coloring, and bare light references (`strobe_tail_bb;`) look to SCSS
like a property with a missing value — hence `scss.validate: false` alongside
the mapping, since those squiggles are false positives here.

The real fix is a TextMate grammar in a small VS Code extension, worth doing once
the syntax stops moving. Note that *no* grammar gives diagnostics (unknown light
type, undefined axis value, equal-specificity conflict) — that needs a language
server, or more cheaply a task wrapping `build/build_objs.py check`, which
already finds those errors.
