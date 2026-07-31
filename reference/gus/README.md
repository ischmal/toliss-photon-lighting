# `reference/gus/` — vendored interior-lighting source data

Frozen upstream source for ToLiss Photon Lighting's **interior** ("Gus Mod")
support. Everything here was authored by **Gus**, not by this project.
`gus-readme.md` is his own installation guide, kept verbatim.

> "Feel free to put this mod into your textures. Just remember to put my name,
> but, tbh, I don't care much about it." — Gus

Credit is carried through to the repo `README.md`, the generated per-bundle
`README.txt`, the installer's interior opt-in screen, and a header comment in
the generated `lights_inn.obj`.

## Why this folder exists

The build **must not** depend on `.scratch/` — it is gitignored and will not
reach a fresh checkout. The light table can be re-derived from `docs/interior_plan.md`
§1.2, but the **textures cannot be reproduced from any document**, so they are
vendored rather than referenced.

## Contents

### The three light variants

| File | Look | Photon profile value |
|---|---|---|
| `lights_inn_current.obj` | old halogen | `0` |
| `lights_inn_new.obj` | new halogen | `1` |
| `lights_inn_led.obj` | LED | `2` |

Byte-identical except for RGB triplets — one geometry, three palettes. Taken
from the A320 package, which is canonical for all three airframes (decision #2);
the A320 and A321 variants are byte-identical and A319's `new`/`led` match too.

These are the **input** to `src/lights/lights.{style,layout}.phdsl`, not build
output. The generated `dist/A3xx/objects/lights_inn.obj` carries all three as
`ANIM_show` branches on `ToLissPhoton/interior/<category>`.

**One deliberate deviation** (decision #1): in `lights_inn_led.obj` the CA/FO
tablet lights carry old-halogen orange `1.00 0.37 0.16`, while every comparable
small directed lamp goes warm white. Treated as an oversight and normalised to
`1.00 0.89 0.81` in the DSL. Everything else is transcribed verbatim.

### `textures/` — the canonical set (11 files, ~57 MB)

Resolves the per-package packaging drift (decision #4) into one union set:
A320's `objects/`, plus `chairs_LIT.png` / `kitchens_LIT.png` (which ship only
under his `liveries/objects/`), plus `pedals_details_{1,2}.png` from A319/A321
(absent from the A320 package). A319's differing `knobs.png` is discarded in
favour of A320's. `extra-needed.png` is documentation, not an asset.

These are **variant-independent** — one set, correct for all three looks — so
they are purely an install-time concern and never switch at runtime.

**Nine of the eleven replace a stock ToLiss file. Two do not:**
`pedals_details_1.png` and `pedals_details_2.png` have no stock counterpart, so
uninstall must **delete** them rather than restore a backup. `installer/actions.py`
tracks this via the manifest's `interior.added[]` list, separate from `backed_up[]`.

## What is *not* vendored

- `lights_out3xx_XP12.obj` — each package also ships a plain modified-stock
  exterior OBJ that **collides head-on** with Photon's generated one. Discarded
  (§1.6). User-facing docs warn that installing Gus's zip manually *after*
  Photon silently reverts the exterior mod.
- `extra-needed.png` — a screenshot documenting the Plane Maker steps.

## Not implemented, deliberately

`gus-readme.md` Steps 2–4 describe adding `NORMAL_METALNESS` / `GLOBAL_specular 1`
/ `ATTR_shiny_rat` / `BLEND_GLASS` to ~26 cabin mesh OBJs, to unlock roughness,
reflectance and translucency. That is a separate mod from the light-color work
and is **out of scope** for `docs/interior_plan.md` — noted here so it is not
lost, not because it is planned.
