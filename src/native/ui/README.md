# `ui/` — the Slint GUI front-end

The installer's graphical front-end. `.slint` files here are compiled to C++ headers
at build time by `slint_target_sources()` in `../CMakeLists.txt`, and consumed by
`../src/installer/gui.cpp`.

⚠ **Everything in here is behind `-DPHOTON_GUI=ON`, which is OFF by default.** A
plain build produces the TUI installer and needs no Rust. See
`docs/installer_gui_plan.md` for why, and for the plan this implements.

---

## Building and running it

```powershell
src\native\run-installer.ps1 -DryRun
```

That is the whole thing — it finds CMake and Rust, builds into `build-gui/`,
stages nothing you have not asked for, and launches the GUI pointed at a staged
payload. See `../README.md` §Running for what it handles and why, and the script's
header for every flag. ⚠ **Do not write build steps into this file**: they change,
and a second copy is a copy that goes stale.

**Rust ≥ 1.92** is the one hard prerequisite — Slint's C++ API is a Rust library
with a C++ binding. The first build compiles Slint from source (minutes), then
caches.

---

## Layout

```
ui/
  tokens.slint       every color, size and metric — nothing else names a literal
  widgets.slint      Logo, PhotonButton, checkboxes, StepRail, BottomBar,
                     AircraftCard, PropertyRow, OptionRow, PathField
  screens/
    splash.slint     0 — welcome (no rail)
    directory.slint  1 — where is X-Plane installed?
    aircraft.slint   2 — which aircraft are you modifying?
    features.slint   3 — which features do you want installed?
    review.slint     4 — does everything look correct?
    run.slint        5 — the log and the progress bar
    complete.slint   6 — the summary and four follow-up actions
  app.slint          the window, the step routing, and the C++ interface
  assets/            SVGs exported from Figma (see below)
```

⚠ **The step is called "Run", not "Install"** — that is the rail's own label, and
it is the better word because the same screen runs an uninstall.

⚠ **`app.slint` decides nothing.** Every `next()` and `back()` goes to
`../src/installer/gui.cpp`, which works out where to go and writes `step` back.
What the file does decide is presentation: which screen a step shows and how the
bottom bar is configured for it.

---

## How a Figma frame becomes a Slint component

The Figma MCP server returns **React + Tailwind** as a *reference*, never as
something to paste. The translation is manual and mechanical:

| Figma / Tailwind | Slint |
|---|---|
| `bg-[#495057]` | `background: #495057;` |
| `w-[640px] h-[480px]` | `width: 640px; height: 480px;` |
| `flex flex-col gap-[10px]` | `VerticalLayout { spacing: 10px; }` |
| `absolute inset-[0_0_64px_158px]` | a `Rectangle` in a layout, not absolute coords |
| `rounded-[4px]` | `border-radius: 4px;` |
| `border-2 border-white` | `border-width: 2px; border-color: white;` |
| `shadow-[0px_-1px_0px_#0d151c]` | a 1 px `Rectangle` — Slint has no box-shadow |
| text styles | a `Text` with explicit `font-size` / `font-weight` / `color` |

Two rules that are easy to get wrong:

- ⚠ **Do not port absolute positioning.** The reference code positions almost
  everything with `absolute` + percentage insets, because that is how Figma
  describes a frame. Reproducing that gives a window that is pixel-correct at
  exactly one size and wrong everywhere else. Read the *intent* — a row, a column,
  a gap — and write the layout.
- ⚠ **Take colors and sizes from the reference, not from a screenshot.** The
  numbers in the returned code are exact; eyedropping a PNG is not.

### Assets

`assets/` holds SVGs exported from the Figma file. They are committed because
**Figma's asset URLs expire after about 7 days** — a link in the design context is
not a source of truth.

Slint loads them with `@image-url("assets/name.svg")`, which is resolution
independent, so one file serves the logo at both the 235×124 splash size and the
105×56 sidebar size.

| File | What it is |
|---|---|
| `logo-v0.svg`, `logo-v1.svg` | the two dots of the Photon wordmark |
| `logo-v2.svg` | the "Photon" wordmark |
| `logo-v3.svg` | the "ToLiss" wordmark |
| `checkmark.svg` | the checkbox glyph |
| `radio-off.svg`, `radio-on.svg` | the aircraft-card radio indicator |
| `marker-todo.svg`, `marker-current.svg`, `marker-done.svg` | the sidebar step dots |
| `divider.svg` | the 1 px rule |

⚠ **The logo is four separate vectors, not one file**, composed at fixed insets
inside a 207×108 box. Whatever draws it has to reproduce that composition; the
pieces are meaningless alone.

The markers, the radios and the divider are single `<circle>`/`<line>` primitives.
Drawing them as native Slint `Rectangle`s with a `border-radius` would render
identically and drop nine files — but it also silently decouples them from the
design file, so the assets are kept as the source of truth.

---

## Regenerating from Figma

```
File: G2EQDGYDXvXnKfedLJw0ma  ("ToLiss Photon")
Node: 212:1641                ("Installer Mockup" — the board holding every frame)
```

Ask for the mockup by URL and the design context comes back per frame. Re-export
`assets/` whenever a vector changes; the rest is hand-translated.
