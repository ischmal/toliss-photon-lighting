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

- **Fixture properties:** `category` (required unless `raw`), `mirror: true`,
  `omit: <variant> ...`, `mount` (per-side via `@starboard`; `none` detaches),
  `pos`, `offset`, `raw`.
- **Position fallback** per light: light `pos` → group `pos` → fixture `pos`.

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
airframe, honours a319/a321 `extends` overrides of the offset (appearance is
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
  placement should be deleted in favour of an `offset`.
- `reference/legacy/lights.poc.jsonc` is **superseded** but kept as a frozen
  record of the migration source; `load_config()` only falls back to it if the
  DSL pair is missing. Don't edit it.

## 6. Where everything lives

```
src/lights/      lights.style.phdsl, lights.layout.phdsl, mounts/, raw/  ← you edit this
src/plugin/      PI_ToLissPhoton.py                           ← you edit this
build/           the generator (code only)
dist/            generated installable X-Plane tree  (gitignored)
reference/       frozen goldens for `check` + superseded configs
```

The rule: **everything under `src/` is hand-authored, everything under `dist/`
is generated.** No exceptions — the plugin is edited at `src/plugin/` and staged
into `dist/` by the build, so `dist/` is a complete drop-in.

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
