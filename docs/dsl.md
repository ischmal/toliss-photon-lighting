# Photon Light DSL — syntax & semantics

The light config is a CSS-like DSL split HTML/CSS-style across two files:

| File | Holds | Analogy |
|---|---|---|
| [src/lights/lights.style](../src/lights/lights.style) | axes, palettes, categories, reusable **light types** | the stylesheet |
| [src/lights/lights.layout](../src/lights/lights.layout) | mounts, per-airframe **fixtures** (positions, gating) | the document |

`build/photon_dsl.py` parses both into the exact same dict the old
`reference/legacy/lights.poc.jsonc` decoded to, so `build/build_objs.py` (Emitter,
`check`, `bootstrap`) is unchanged downstream. The migration was verified by
**byte-identical** Emitter output across all 3 airframes × 3 wing variants.

Builds land in `dist/` (generated; not committed). `check` compares a build
against the frozen hand-authored goldens in `reference/photon/` — never against
`dist/`, which would compare generated output with itself and always pass.

```
python build/build_objs.py build [--airframe a320] [--wing durantula] [--out DIR]
python build/build_objs.py check --airframe a320
python build/watch.py            # rebuild on save
```

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

## 3. lights.style

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

## 4. lights.layout

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
  `pos`, `raw`.
- **Position fallback** per light: light `pos` → group `pos` → fixture `pos`.
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
  `raw` files (`src/lights/raw/`). The DSL only models the frequently-edited
  leaf lights + gates.
- `reference/legacy/lights.poc.jsonc` is **superseded** but kept as a frozen
  record of the migration source; `load_config()` only falls back to it if the
  DSL pair is missing. Don't edit it.

## 6. Where everything lives

```
src/lights/      lights.style, lights.layout, mounts/, raw/   ← you edit this
src/plugin/      PI_ToLissPhoton.py                           ← you edit this
build/           the generator (code only)
dist/            generated installable X-Plane tree  (gitignored)
reference/       frozen goldens for `check` + superseded configs
```

The rule: **everything under `src/` is hand-authored, everything under `dist/`
is generated.** No exceptions — the plugin is edited at `src/plugin/` and staged
into `dist/` by the build, so `dist/` is a complete drop-in.
