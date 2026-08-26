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
                     AircraftCard, PropertyRow, OptionRow, PathField,
                     PhotonContextMenu
  screens/
    splash.slint     0 — welcome (no rail)
    directory.slint  1 — where is X-Plane installed?
    aircraft.slint   2 — which aircraft are you modifying?
    features.slint   3 — which features do you want installed?
    review.slint     4 — does everything look correct?
    run.slint        5 — the log and the progress bar
    complete.slint   6 — the outcome glyph, the summary and the follow-up actions
  app.slint          the window, the step routing, and the C++ interface
  assets/            SVGs exported from Figma (see below)
```

⚠ **The step is called "Run", not "Install"** — that is the rail's own label, and
it is the better word because the same screen runs an uninstall.

⚠ **`app.slint` decides nothing.** Every `next()`, `back()` and `go-to-step()` goes
to `../src/installer/gui.cpp`, which works out where to go and writes `step` back.
What the file does decide is presentation: which screen a step shows, how the
bottom bar is configured for it, and which rail rows *look* clickable.

⚠ **The root `FocusScope` is a FALLBACK, not the handler**, and everything the
keyboard does globally rests on that: Slint gives a key to the focused item first
and only then walks up. Enter is the primary button; **Escape and Backspace are
Back**. Backspace is safe there *because* an editable `TextInput` accepts it
unconditionally — a `read-only` one would decline and let it navigate while the user
is trying to edit, which is why there are none and a test says so.

⚠ **The step rail is navigation as well as an indicator** (2026-08-14): a completed
step returns you to it, **mouse only and backward only**. There is deliberately no
high-water mark and no forward jump — walking forward is what rebuilds each screen
(`GoNext` re-detects aircraft, publishes availability, assembles the review list),
so a jump past a screen would arrive with the previous run's answers. Going *back*
needs no such guard: it restores answers and commits nothing, and pressing Next from
wherever you land walks the wizard forward again from there. ⚠ **Two things are still
refused** — a jump out of a run in progress (`run-finished`, not `!running`, so the
Run screen "View log" returns to is not a dead end) and a jump *to* Run from anywhere,
which is the `blocked` row. Full rules: `docs/installer_gui_plan.md` §10.

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

### Right-click menus

`PhotonContextMenu` is a `PopupWindow` we draw ourselves — **not** Slint's
built-in `ContextMenuArea`, which renders fluent's menu and follows the user's OS
light/dark setting, the same reason `PhotonScrollView` exists. Four ship: the path
field (Cut/Copy/Paste/Select all), the installer log (Copy log), an aircraft card
(Open aircraft folder...) and the Complete screen's outcome row (Copy result).
Mouse and arrow keys both drive it, it grows to its longest label from a 128 px
floor, and it flips to a top-right anchor near the window's right edge.

⚠ **A highlighted row has an OWNER**: taking the pointer out of the menu drops a
highlight the *pointer* put there, and keeps one the *arrows* did
(`active-by-key`). The detector is a `TouchArea` **wrapping** the rows — an
ancestor, so Slint still reports it hovered while a row's own TouchArea has the
event, which a sibling behind them would not.

Two traps worth knowing before you touch it:

- ⚠ **A menu's TouchArea goes BEFORE the TextInput it serves.** The last child is
  hit-tested first, so declared after one it swallows every *left* click and the
  field stops being a text field — the menu still works, which is what makes it
  hard to spot.
- ⚠ **The menu root's `clip: true` is load-bearing.** The width is measured with a
  hidden `VerticalLayout` of the same labels (Slint has no fold over a repeater,
  but a vertical layout's preferred width *is* the widest child). Slint does not
  clip by default, so without it those labels are painted onto the screen behind
  the menu.

Full contracts and the rest: `docs/installer_gui_plan.md` §8, enforced by
`ContextMenuTests` in `tests/test_installer_ui.py`.

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
| `glyph-success.svg`, `glyph-warning.svg`, `glyph-failure.svg` | the outcome tick, triangle and cross |

⚠ **The three outcome glyphs are used in TWO places and that is the point**: at 32 px
on the Complete screen, and at 12 px as the installer log's per-line marker. The
same three shapes mean the same three things throughout the installer. They also
carry their own fills — the palette's green, amber and red — so nothing tints them.

⚠ **The triangle arrived third (2026-08-24), and it exists because two shapes were
one too few.** `LogLine` carried a bool `bad` that was ERROR *or* WARN, so both drew
the red cross — and the loudest warning the installer has is the wing-mod mismatch,
which is a note about an install that **completed**. A red cross beside it reads as
an aborted run, and did. It is the Figma **Warn Glyph** (`371:522`) and its amber is
`log-ink-warn`, the glyph's own fill.

⚠ **It is 32×30, not 32×32 like the cross**, so `image-fit: contain` and a per-state
box carry it on both screens. ⚠ **Those box numbers are the SVG's own `width`/
`height`** and `GlyphGeometryTests` re-reads the three files to check it — a
re-export at a different size otherwise leaves the box behind, and `contain`
letterboxes the difference instead of failing. It shipped for one day at **32×29**
(2026-08-24), which is where `complete.slint`'s floored `y` comes from; the redraw
that put it on the pixel grid made every height even again, and the floor stays as a
guard.

⚠ **The log's marker is an SVG because NEITHER EMBEDDED FONT HAS U+2713 `✓`**
(checked against both `cmap` tables). A checkmark character there is drawn by a
system fallback font, which is exactly what embedding the fonts exists to prevent.
See `assets/fonts/README.md`.

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
