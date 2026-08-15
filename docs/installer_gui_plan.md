# The GUI installer (Slint)

A graphical front-end for `photon-installer`, built from a Figma mockup. Status as
of **2026-08-12**: **all seven screens are implemented and the GUI builds, runs and
detects aircraft correctly.** Verified on Windows — a 14.4 MB single executable,
Windows-GUI subsystem, no `slint_cpp.dll` and no VC++ redistributable among its
imports, so the one-executable bundle rule holds.

What has been seen working in the running app: the splash, the step rail, X-Plane
auto-detection filling the path field, and the aircraft list showing four real
ToLiss airframes with correct UPDATE badges and version transitions. What has NOT
been exercised is an actual install — Review → Run → Complete has never been run
against a real aircraft.

Companion documents: `docs/installer.md` (what the installer does),
`docs/installer_cpp_plan.md` (the C++ port that made this possible),
`src/native/ui/README.md` (how a Figma frame becomes a `.slint` component).

---

## 1. Why this is cheap to do

`photoncore` is a UI-free static library holding **all** the logic — detection, the
actions, the manifest, the payload, every file patcher — and it is forbidden from
linking X-Plane. The TUI (`screens.cpp` + `tui.cpp`) is a ~1,700-line presentation
layer over it and `cli.cpp` is a second one.

**The GUI is a third front-end over the same core. No logic moves.** That is the
whole design, and the rule that keeps it true:

⚠ **No install logic in `gui.cpp`, exactly as none is in the TUI.** The three
front-ends differ only in how they ask and how they report. A behavior in one and
not the others is a bug in that one, and `cli.cpp --json` is the contract the GUI
has to agree with.

---

## 2. Decisions taken

| Decision | Choice | Why |
|---|---|---|
| Toolkit | **Slint** | The mockup is a designed UI; Dear ImGui (already vendored, MIT) would have been free but cannot reproduce an arbitrary design faithfully. |
| License | **GPLv3, whole repo** | Slint's other option is royalty-free *with attribution conditions*. **Applied 2026-08-12** — `LICENSE` is the GPLv3 text and README §Usage says so. |
| Ship model | **The GUI replaces the TUI in the bundle** | `build_bundle` ships exactly one executable; a download must not hand the user two things to double-click. |
| The TUI | **Kept, reachable with `--tui`** | SSH and headless boxes still need it, and `photoncore_tests` covers its string engine. It is built either way; it is just not what a double-click gets. |
| Linkage | **Static Slint** | See the bundle rule — a `slint_cpp.dll` beside the binary has nowhere to go in `data/`. |
| Default | **`PHOTON_GUI=OFF`, and CI states `ON`** | The default keeps a contributor's checkout (and the core-only tree) buildable with no Rust. **CI installs Rust and passes `-DPHOTON_GUI=ON` since 2026-08-15**, so what ships is the GUI. |

### Licensing, precisely

The repository is **GPLv3** as of 2026-08-12 (was MIT). Third-party components
keep their own terms, all GPL-compatible: `reference/gus/` is Gus Rodrigues's work
vendored with permission, `src/native/third_party/` is Dear ImGui / nlohmann-json /
stb, and `ui/assets/fonts/` is Roboto under Apache 2.0.

⚠ **GPLv3 does not restrict commercial use, and nothing here does.** It is
explicitly commercial-friendly: anyone may sell GPLv3 software provided they ship
the source and keep it GPLv3. What it prevents is someone *closing* the source or
folding this into a proprietary product — which is the usual intent behind "keep
it non-commercial". No OSI-approved license carries a non-commercial clause; such
a clause would make the project non-free. Recorded here because the decision was
taken with "the project just needs to be non-commercial" as the stated goal, and
the license delivers something adjacent to but not the same as that.

Only `photon-installer` *needs* GPLv3, since it is the only thing linking Slint;
relicensing the whole repo was a simplification, not a requirement.

---

## 3. The flow

The mockup's sidebar names six steps, with a splash ahead of them:

```
0 splash · 1 X-Plane Directory · 2 Select Aircraft · 3 Select Features
        · 4 Review · 5 Run · 6 Complete
```

⚠ **Step 5 is called "Run", not "Install"** — that is the rail's own label, and it
is the better word, because the same screen runs an uninstall.

**This is not the TUI's topology** — the TUI runs
`launch → root → aircraft → action → (guard) → perform → complete`. The
differences are deliberate:

- **"Select Features" is new.** It gathers what the TUI asks as separate prompts:
  the wing-mod variant (stock / RealWings / Durantula, a sub-radio under the
  exterior option), Gus's cockpit lighting (`--interior`), and the display
  effects.
- **"Review" is new** — a confirmation page listing root, aircraft folder,
  identified airframe, version transition, and a key/value list of the chosen
  options, before anything is written.
- **Uninstall lives in the bottom bar on aircraft selection** (decided
  2026-08-12), shown only when the selected aircraft has something installed — the
  action belongs next to the thing it acts on, and Features is where you choose
  what to ADD. The mockup's Features-screen button is deliberately not
  implemented.
- **The directory screen's status line says its state in COLOR** (2026-08-13):
  the palette's green when the root is usable, its red when it is not. ⚠ **That is
  why neither string carries a ✓ any more.** With the color doing the work the
  glyph was the same thing said twice — and since it only ever appeared on the two
  good sentences, it also made the two failures read as a different *kind* of
  message rather than as the same line reporting something else. ⚠ **The failing
  color is `status-broken`, not `accent`**, which is what it used to be: green
  against blue is not a pair anyone reads as good against bad. Both are the
  logo's own dots, so this is the same green an installed aircraft's badge
  carries — see §3.0.
- **The step rail is also navigation** (2026-08-14) — a completed step can be
  clicked to return to it, mouse only and backward only. The mockup draws the rail
  as an indicator; this makes it a control. Full rules and the two refusals: §10.
- **The version label in the bottom bar is a link to the X-Plane.org page**
  (decided 2026-08-12). The design underlines it on Run and Complete only; it is
  underlined everywhere here, because a control that is clickable only sometimes,
  with no other difference, is worse than one that always is. ⚠ The underline is a
  drawn `Rectangle` — Slint's `Text` has no text-decoration.

### 3.0 The aircraft row has THREE install states

⚠ **`stale` is not "an update is available" — it is a BROKEN install**, and
conflating them was a real bug in the first cut of `EntryFor`. `detect.cpp` sets
it when the manifest claims Photon is installed but the OBJ's version marker is
gone. So:

| photoncore says | Badge | Color | Status line |
|---|---|---|---|
| `photon` empty | — | — | Not installed |
| `stale` | **REPAIR** | `status-broken` | Install incomplete - reinstall |
| `photon != kPhotonVersion` | **UPDATE** | `accent` | `0.8.1 → 0.8.2` |
| otherwise | **INSTALLED** | `status-ok` | Up to date (0.8.2) |

⚠ **The two status colors are the Photon logo's own dots** — `#9fea7c` and
`#f26951`, lifted from `assets/logo-v0.svg` and `logo-v1.svg` — so they are
already part of the brand and already sit beside the accent on every screen.
They are not in the Figma file as variants; the *intent* was specified and the
colors were taken from the artwork rather than invented.

⚠ **The REPAIR wording states the observation, not a diagnosis.** photoncore
cannot tell WHY the marker is missing. For a user it is almost always a ToLiss
update reverting the OBJ — but **on a dev machine it is the normal state after
`deploy.ps1`**, which writes OBJs straight from the DSL and never stamps a marker.
The lighting is present and working in that case, so "the install was reverted"
would be flatly wrong. Expect most aircraft on this machine to show REPAIR; that
is not a bug in the GUI.

⚠ **The badge INVERTS on a selected card** — white fill, colored text. Colored-on-
colored made it vanish exactly when you clicked the row it was on; the design was
updated to fix that. Its width is a *minimum* rather than the design's fixed 63 px,
because "INSTALLED" does not fit and the design's `text-ellipsis` would clip it to
"INSTALL…", which reads as a verb — the opposite of what a green badge means.

Two more screens carry contracts worth stating:

- **Run** gives 288 px to the log and 26 px to the progress bar — a deliberate
  inversion of the usual installer, because a run that fails halfway has to say
  WHERE without sending the user to find a log file. Its Next button is present
  from the start (per the design) but **dead until the run finishes**.
- **Complete** has no Back — the run it reports on already happened — and its
  primary button says **"Exit"**. Its actions are *Modify another aircraft* (the
  full-width primary), *Open X-Plane.org Page…* and *Open GitHub Project…* in the
  links well, and **View log** in the bottom bar. ⚠ Back WAS added here on
  2026-08-12 and removed again on 2026-08-14; the reasoning both times is in §11.4.

### 3.1 ⚠ "Cockpit Display Effects" does NOT gate the install

This checkbox looks like the interior's opt-in and is not one. The display-glow
OBJ and its `.acf` attachment are **part of the base install** on A3xx and A339
(`SCREENS_AIRFRAMES`) and stay that way — they work on a stock cockpit, so they do
not belong behind the interior's opt-in.

**The checkbox presets the runtime default instead**, which is why its description
ends "Can be enabled or disabled later." It exists because a tester read the
display effects as part of Gus's cockpit mod; the row is there to say they are
separate things, not to make them optional at install time.

Mechanically that means seeding `"$displays"` in
`<root>/Output/preferences/ToLissPhoton_profiles.json` — the `enabled` flag, which
`DisplaySettings` defaults to `true`. **The installer writes no user preferences
today, so this is new capability**, and it carries two hazards:

- ⚠ **It must MERGE, never overwrite.** That file holds every per-livery profile
  the user has, keyed `"<aircraft path>#<index>"`. Writing `{"$displays":{…}}`
  over it would erase all of them. The plugin's own `SaveProfilesFile` rewrites
  the whole file from memory, so the installer cannot borrow it.
- ⚠ **On an upgrade it must not clobber a user's existing choice** — write the key
  only when it is absent. This is the same trap as `panelfx.txt`, which a shipping
  `.xpl` is forbidden from writing for exactly this reason (`core/constants.h`).

---

## 4. Building it

**One command**, the counterpart to `deploy.ps1`:

```powershell
src\native\run-installer.ps1 -DryRun            # the GUI
src\native\run-installer.ps1 -Tui -DryRun       # the terminal installer
src\native\run-installer.ps1 -Cli detect --json # a headless subcommand
```

⚠ **THE SCRIPT IS THE DOCUMENTATION, and it is deliberately the only place these
steps are written down.** They change as the installer is reshaped and prose about
them goes stale silently. `src/native/README.md` §Running lists what the script
handles for you and why each one bites; the script's own header is the reference.
**When the steps change, change the script.**

Two facts worth having here anyway, because they explain failures that look like
something else:

- **Rust ≥ 1.92 is a hard prerequisite.** Slint's C++ API is a Rust library with a
  C++ binding; there is no pure-C++ path. ⚠ On this machine Rust is installed but
  **not on `PATH`** (`%USERPROFILE%\.cargo\bin`) — the same shape as the
  CMake-inside-VS-18 quirk `deploy.ps1` works around. "rustc: command not found"
  therefore reads as *not installed*.
- **The first Slint build compiles from source** — minutes, then cached in
  `build-gui/`. That tree is separate from `build-core/` so flipping `-Tui` is not
  a full recompile, mirroring the `build/` ÷ `build-dev/` split.

---

## 4.0 Where the design lives

```
File   G2EQDGYDXvXnKfedLJw0ma   ("ToLiss Photon")
212:1641   "Installer Mockup"   the seven screens, laid out left to right
168:408    "Controls"           THE COMPONENT LIBRARY - every variant
```

⚠ **`168:408` is the one to read before styling anything.** The screens are
instances; the states are only in the library, and there is no way to reach it
from a screen node. Notably it holds `Button` (Style x State = Primary/Normal/
Danger x Default/Hover/Down), `Badge` (Normal/Red/Green/Blue), `Aircraft Info`
(Badge x Selected), `Bottom Controls` (BackButton x Danger), plus Step, Radio
Button, Checkbox Radio, Large Checkbox, Progress Bar, Textbox, Attribute,
Attributes List, Option Checkbox, Property Label, Sidebar and Installer Page.

⚠ **`get_metadata` on the page (`0:1`) does NOT list these frames** — it returns
two unrelated ones and stops. There is no way to discover the library by walking
down from the document, which is why it went unread for a whole pass and six
button fills were guessed. Address `168:408` directly.

---

## 4.1 The design-to-code tooling

Three separate things, often confused:

| Tool | What it is | Status here |
|---|---|---|
| **Figma MCP server** | Reads the design out of Figma into the agent | **In use** — how every screen here was built |
| **"Figma to Slint"** | A *Figma plugin* that exports Figma **Variables** into `.slint` token files | **Not usable yet** — see below |
| **Slint MCP** | An MCP server compiled *into* a running Slint app; lists windows, inspects the element tree, dispatches clicks, screenshots (works headless via `SLINT_BACKEND=headless`) | **Not wired** — needs a build first |

⚠ **The Figma file defines no Variables.** `get_variable_defs` on the mockup
returns `{}`, so the "Figma to Slint" plugin would export an empty token file
today. It only pays off if the palette is first promoted to Figma Variables.
`ui/tokens.slint` is therefore hand-written — deliberately in the shape that
plugin emits (one global of `out property`s), so a generated file can later drop
in without touching a call site.

The Slint MCP is the obvious next step for verification: it would let the agent
see the real layout tree and screenshot the running installer rather than
reasoning about it. It requires the app to build, which is why it is a follow-up
rather than part of this scaffold.

---

## 5. Tripwires

**Build and linkage**

- ⚠ **`PHOTON_GUI` STAYS OFF BY DEFAULT — AND CI PASSES `-DPHOTON_GUI=ON`**
  (2026-08-15). The default is for the core-only tree and a contributor's checkout,
  which must keep building `photon-installer` with no Rust present; flipping the
  *default* would turn those red instead. CI wants the opposite answer and says so
  explicitly. ⚠ **The gap between the two is silent in the direction that matters**:
  an omitted flag does not fall back to the previous behavior, it packs the
  **terminal** installer into a bundle named for the GUI, with a green run and a
  well-formed download to show for it. That is what the workflow's `grep` on the
  configure log is for — CMakeLists prints `GUI build - photon-installer links
  Slint` only from inside `if(PHOTON_GUI)`, so matching it is the only proof that
  survives a typo'd `-D` or a stale cache. Changing that `message(STATUS)` string
  breaks the check; change both together.
- ⚠ **Rust defaults to the dynamic CRT (`/MD`) and this project is globally
  `/MT`.** Without `-C target-feature=+crt-static` the Rust staticlib drags in
  msvcrt against our libcmt: duplicate symbols at best, two heaps at worst, and a
  bad free across that boundary crashes with a stack pointing nowhere near the
  cause. The static CRT is not a preference here — a missing-VC++-redistributable
  dialog on a lighting mod reads as malware, and there is no
  installer-for-the-installer to fix it with.
- ⚠ **The MSVC runtime is pinned to the NON-debug static CRT in every config**,
  overriding `CMakeLists.txt`'s global `MultiThreaded$<$<CONFIG:Debug>:Debug>`. A
  Rust *debug* build still links the *release* CRT even under `+crt-static`, so a
  CMake Debug build would pair our `/MTd` objects with Slint's `/MT` ones and fail
  to link. `photoncore_tests` does not link Slint and keeps the global setting.
- ⚠ **`BUILD_SHARED_LIBS` is saved and restored around `FetchContent_MakeAvailable`,
  not just set.** That call is an `add_subdirectory`, so a bare `set` would
  silently retarget anything declared after it too.
- ⚠ **`GIT_TAG` IS PINNED TO `v1.17.1`** (2026-08-15, was the moving `release/1` —
  fine while writing the UI, wrong for a reproducible bundle, because two CI runs a
  month apart would compile different Slint into the same Photon version). v1.17.1
  is the version the GUI was developed and verified against. ⚠ **Slint's own
  `rust-version` is where the toolchain floor comes from** — v1.17.1 declares
  `1.92`, which is the number in `run-installer.ps1` and in the workflow's Rust
  step. Bump the tag and re-read that field before assuming the runners still
  satisfy it.
- ⚠ **Slint needs `opengl32` and `imm32` on Windows and Corrosion does NOT
  propagate them.** Its `Required static libs` line reports only
  kernel32/ntdll/userenv/ws2_32/dbghelp; the winit backend additionally pulls in
  WGL (glutin) and the IME API. Missing them is ~17 unresolved externals with
  Rust-mangled names, which reads as a broken Slint build rather than a missing
  `-l`.
- ⚠ **`SLINT_EMBED_RESOURCES` must be `embed-files`.** The default is
  `as-absolute-path`, which bakes the BUILD MACHINE's path to `ui/assets/*.svg`
  into the executable: perfect artwork on the dev machine, silently missing
  artwork — no error — on every user's. It is the same rule as the payload
  (resolve from the executable, never the working directory), applied to the UI's
  own assets.

**The Windows subsystem split**

- ⚠ **A `PHOTON_GUI` build is `WIN32_EXECUTABLE ON`**, so a double-click does not
  flash a console behind the window — and therefore starts with **no stdout at
  all**. `main` calls `platform::AttachParentConsole()` before the mode split;
  without it `photon-installer detect --json` from a terminal writes nowhere and
  exits 0, which reads as the subcommand being broken.
- ⚠ **That re-attach must not touch a redirected stream.** When stdout is a pipe
  or a file the handle is inherited and already correct; reopening it onto
  `CONOUT$` would send `--json` to the terminal instead of into the caller's pipe.
  Each standard stream is re-pointed only when it has no handle of its own.
- ⚠ **The GUI subsystem looks for `WinMain`, and we have `main`** — hence
  `/ENTRY:mainCRTStartup`. Without it the link fails on an unresolved `WinMain`
  out of libcmt's `exe_winmain.obj`, naming the CRT rather than the subsystem
  switch that caused it. The redirect keeps ONE `main()` serving the GUI, the TUI
  and the CLI, which is what stops the three modes drifting apart at startup.
- **A shell does not WAIT for a GUI-subsystem process.** Exit codes propagate
  correctly through a pipe or `Start-Process -Wait` (verified: 0 on success, 1 on
  a missing `--aircraft`), but a bare `photon-installer install …` returns to the
  prompt immediately. Anything scripting the CLI has to capture output or wait
  explicitly.
- **`--tui` is accepted and ignored by a non-GUI build.** Otherwise the two builds
  disagree about which arguments exist, and a support instruction saying "run it
  with `--tui`" would fail on exactly the build where it is redundant.

**The design**

- ⚠ **Figma asset URLs expire after about 7 days.** `src/native/ui/assets/` is
  committed for that reason; a link in a design-context response is not a source
  of truth.
- ⚠ **`get_design_context` SILENTLY TRUNCATES at ~25k tokens.** Asking for the
  whole mockup board returned the first five frames and stopped; the Run and
  Complete screens looked as though they did not exist, and were reimplemented
  from scratch against a design that was sitting there. **Call `get_metadata`
  first to enumerate the frames, then fetch them individually.**
- ⚠ **Roboto is embedded, and must stay embedded.** It is not installed by default
  on Windows or macOS, and Slint falls back to a system font SILENTLY when a
  family is missing — the UI lays out correctly and simply is not the design, with
  no warning anywhere. The six static instances reach the binary through
  side-effect `import "…ttf";` lines at the top of `app.slint`; see
  `ui/assets/fonts/README.md`, including why they are static rather than the
  variable font and why they must not be pulled from `github.com/google/fonts`.
- ⚠ **BUTTON STATES ARE NOT A UNIFORM LIGHTEN/DARKEN.** Deriving them with
  `brighter()`/`darker()` got three of six wrong. Primary hover jumps hue to a
  brighter blue; Normal hover moves from grey toward blue; Primary and Danger both
  **keep their base fill when pressed** and darken only via an inset shadow, while
  Normal genuinely changes fill. All of it is in `168:408 > Button`; the fills live
  in `tokens.slint`.
  ⚠ **NO STYLE HAS A RESTING BORDER any more (2026-08-12).** Primary used to carry
  a permanent 2 px white keyline and thin it to 1 px on hover. The design removed
  it everywhere and added a **Focus** variant to all three styles that is the base
  fill plus that same 2 px white border, so the keyline now means focus and
  nothing else — see §5.1.
- ⚠ **THE LARGE CHECKBOX'S HOVER AND DOWN STATES BELONG TO THE ROW, NOT THE BOX**
  (2026-08-13). The control on the Features screen is the whole 442 px
  `OptionRow` — the caption toggles it, deliberately — so the 20 px box has to
  light up for a pointer it can never see itself. `LargeCheckbox` takes
  `hovered`/`down` as in properties and `OptionRow` feeds them from its own
  TouchArea; drop those two lines and both of the design's new variants become
  unreachable everywhere except over the glyph, with the box still drawing a
  perfectly good-looking Normal. Pinned by `CheckboxStateTests`, which also pins
  that a press beats a hover — `has-hover` stays true for the whole of one, so
  without the exclusion Down never renders either.
  Its fills are the BUTTONS' fills and are not restated: unchecked walks Normal's
  states and checked walks Primary's, including Primary's habit of keeping its
  fill under the press and darkening with the inset alone. Only the two resting
  fills differ (unchecked rests on `muted`, an inactive mark, not `neutral`).
  Its **unchecked press was softened from `.37` to `.10` the same week** — the
  geometry did not move, so only the three gradient colors changed, and the top
  edge went from 26 levels below the fill to 7. That also retires the one real
  objection to drawing it as a top band: the sides carry Φ(0), half the source,
  which at `.37` was 47/255 and is now 13/255.
- ⚠ **THE WING-MOD "RADIO-CHECK" IS NEARLY THE CHECKBOX AND MUST NOT BE MERGED
  WITH IT** (`195:703`, 2026-08-13). Same six fills, and then four real
  differences: **no white hover lip at all**; a different pressed-unchecked inset
  (`0 1px 4px` against the checkbox's `0 2px 3px` — the two now share an alpha but
  not a geometry, so the radio's top edge is .069 against .091 and reaches 31% of
  its height rather than 25%); a hover lift on its own alpha *and its own geometry
  per state* (checked `0 1px` blur 4, unchecked `0 2px` blur 2 — both box-shadows,
  so neither blur is the σ trap); and a **label that goes white on HOVER, not only
  when checked** — five of its six variants carry white ink and only Unchecked
  Normal is muted, so keying that on `checked` alone leaves a hovered row with a
  lit box and a grey label.
  ⚠ **Its pressed-checked shadow is byte-identical to the checkbox's and still
  cannot share the brush.** The stops are fractions of the box's own height, and
  the three heights are 32 / 20 / 16 px. Same shadow, three sets of percentages.
  ⚠ **This is NOT the aircraft screen's radio** (`193:616`), which is round, drawn
  from `assets/radio-{on,off}.svg`, and has no hover or down variant.
- ⚠ **A SIZED CHILD WITH NO STATED POSITION IS CENTERED, NOT PLACED AT 0.** In a
  non-layout parent Slint centers anything given an explicit size and no explicit
  `x`/`y`. On a `Rectangle` that is at least visible; on a **`TouchArea` it is
  invisible and total** — the sized-but-unplaced hit zone slid to the middle of
  the wing-radio's 412 px row, so the box and its label were dead and a band of
  empty space to their right was live. Nothing warns, and the offending line
  reads as the most ordinary in the file. It is why `box`, `caption` and every
  zero-width `FocusScope` here state `x: 0` explicitly. `SizedItemTests` pins it
  for TouchAreas, exempting one sized to exactly `parent.width` (where centering
  is identity).
  ⚠ **`preferred-width` is a LAYOUT property and reads 0 outside a layout**, so
  measuring a `Text` with it silently yields nothing. `SmallRadio.hot-w` uses
  `caption.width` — which means the caption must NOT be given an explicit width,
  since self-sizing from the implicit text extent is the whole measurement.
- **The hand cursor is per-TouchArea and three of the eight deliberately go
  without it** (2026-08-13). `PhotonButton`, `OptionRow`, the aircraft card and
  `SmallRadio` all state
  `mouse-cursor: root.enabled ? MouseCursor.pointer : MouseCursor.default`. The
  three that do not are right not to: the dead-space catcher behind every screen
  (a hand over the whole window), the scrollbar thumb (a scrollbar is an arrow),
  and `LargeCheckbox`'s own — `OptionRow`'s TouchArea is declared after it and
  Slint hit-tests the last child first, so the box's never sees a pointer. That
  ratio is why there is **no test** for this one: the exemption list would be
  longer than the rule, and unlike the state bugs above a missing hand cursor is
  visible the moment anyone moves the mouse. ⚠ On `SmallRadio` the cursor follows
  `hot-w` like the hit zone and the focus ring — a third reason to keep those one
  number, since a hand over 300 px of empty row promises a click that does not
  land.
- ⚠ **SLINT'S INNER SHADOW EXISTS AND THIS BUILD CANNOT DRAW IT.** `Rectangle`
  has `inner-shadow-{color,blur,offset-x,offset-y,spread}` in 1.17, and they are
  **Skia-only**: FemtoVG's item renderer opens `draw_box_shadow` with
  `if box_shadow.inset() { return; }`. We render with FemtoVG
  (`SLINT_FEATURE_RENDERER_SKIA=OFF`), so writing the real property **compiles,
  reviews clean, and draws nothing** — no warning at any stage. Eleven of the
  design's thirteen inset shadows are therefore hand-approximated: `WellRecess`
  for the two panel wells, `Tokens.press-*` for the three pressed buttons,
  `Tokens.check-press-*` and `Tokens.radio-press-*` for the two pressed
  checkboxes and the two pressed radios, and `InsetGlow` for the unselected
  aircraft card's white edge glow at its two strengths.
  ⚠ **The other two need no approximation, and spotting that is worth a look
  before reaching for a gradient.** The hovered checkbox's `inset 0 1px 0` has a
  blur of ZERO, and a zero-blur inset shadow offset 1 px down is exactly the 1 px
  strip the shifted shape leaves uncovered — so it is a 1 px `Rectangle`, correct
  rather than close (`Tokens.check-lip-*`).
- ⚠ **DO NOT "JUST TURN SKIA ON" — IT DOES NOT LINK, AND NOT FOR A FIXABLE
  REASON.** Measured 2026-08-12, both walls in order:
  1. Corrosion propagates neither skia-bindings' six native archives nor its 16
     `cargo:rustc-link-lib` directives (d3d12, dxgi, d3dcompiler, fontsub, usp10,
     …) — 1095 unresolved externals. Supply them by hand and you reach:
  2. `LNK2038: mismatch detected for 'RuntimeLibrary': 'MD_DynamicRelease'
     doesn't match 'MT_StaticRelease'`. **skia-bindings ships a PREBUILT Skia
     built against the dynamic CRT**, and this binary is `/MT` everywhere on
     purpose — the same constraint the RUSTFLAGS note in `CMakeLists.txt` exists
     for, except this time it is someone else's compiled artifact, so
     `+crt-static` has nothing to act on.
  Getting past it means building Skia from source with `/MT` (Python + GN +
  ninja on all three CI runners) and linking 42 MB of static archive into a
  14.8 MB binary — for eleven shadows, against a download whose whole design is
  "one file you double-click". Not worth it; revisit only if something else
  needs Skia.
- ⚠ **The approximations are DERIVED, not eyeballed**, and the derivation is the
  part to preserve: for a Gaussian of radius `B` (σ = B/2) the alpha at an edge is
  the source alpha × Φ(offset/σ), falling to zero about `B` inside. That is why
  the wells' top band is stronger than their bottom one (offset +1) and why the
  pressed buttons get a top band ONLY (offset +4 against blur 4 puts Φ(2) ≈ .98
  at the top and nothing at the bottom). Change a design value and redo the
  arithmetic rather than nudging hex until it looks close. Measured against the
  running window afterwards: wells .115/.098 top/side against .130/.107 predicted,
  buttons .15/.092/.157 against .159/.090/.170.
- ⚠ **The three pressed buttons do NOT share one gradient.** Figma gives them
  .20 (Primary), .19 (Danger) and .10 (Normal) — Normal's is half the others, so
  the single shared gradient this started with was wrong on two of three.
- ⚠ **A well that hosts a `WellRecess` needs `clip: true`.** The bands are square
  Rectangles; without the clip they paint over the 6 px radius and the well grows
  four dark notches at the corners.
- ⚠ **The recess is the LAST child and must stay input-transparent.** An inset
  shadow darkens the inside edges of the container's CONTENT, so it paints over
  it — Figma has it last for the same reason. Plain Rectangles take no hits in
  Slint (only `TouchArea` does), which is what keeps it off the aircraft cards'
  clicks and, on the Run screen, out of the log's text selection.
- ⚠ **ONE FIGMA BLUR CAN COME BACK AS TWO DIFFERENT CSS NUMBERS.** The hover lift
  is `0 4px blur 4` on all three button styles, but the component set renders
  Primary and Danger as a CSS *filter* `drop-shadow(0 4px 2px)` and Normal as a
  *box-shadow* `0 4px 4px` — a filter's third value is a standard deviation and a
  box-shadow's is a radius, twice it. Slint's `drop-shadow-blur` is the
  box-shadow kind (its own docs equate the two), so reading the filter number
  literally made Primary's shadow half its intended size. Alphas differ for real,
  though: .15 Primary, .11 Normal and Danger.
- ⚠ **The Green badge takes DARK ink — `#495057`, the screen background** — not
  the near-black used on the light text field. White on that green fails to read
  at 12 px. Blue and Red carry white.
- ⚠ **A FIXED-WIDTH CHILD SETS ITS COLUMN'S WIDTH.** A `VerticalLayout` takes its
  preferred width from its widest child, so one hard-sized element collapses
  everything stacked with it: the Review screen's 260 px attribute rows squeezed
  the whole column to ~270 px and the X-Plane path above them elided at half the
  space it had, and the Complete screen's 215 px buttons did the same to its
  summary sentence. Both read as a text bug in the element that got squeezed.
  **Wrap the fixed-width block in a `start`-aligned `HorizontalLayout`** — it
  absorbs the fixed width and reports "as wide as you like" upward. (Complete is
  positioned rather than laid out now, for the reason two bullets down; Review
  still uses the wrapper.)
- ⚠ **A LAYOUT DOES NOT STRETCH A CHILD THAT STATED ITS OWN WIDTH.** `PhotonButton`
  binds `width: Tokens.button-w`, and to a Slint layout an explicit width is a
  FIXED size, not a default — so a 215 px column handed all four Complete buttons
  96 px each and the sentences ran out of their fill onto the pane. Every caller
  that wants a different size has to say so on the button. This is the same
  property as the bullet above seen from the other end: stating a width opts an
  element out of the layout in both directions.
- ⚠ **A TEXT'S NATURAL HEIGHT IS A FONT METRIC, SO IT IS FRACTIONAL** — 14.0625 px
  for Roboto at 12 px. That fraction propagates: it went into the Complete
  screen's block height, `alignment: center` halved the leftover, and all four
  buttons landed on a y like 166.03, where a rectangle is resampled across two
  pixel rows. **Symptom is a soft edge on the elements BELOW the text, not on the
  text** — the same class as the logo's fractional rect, one axis over. The fix is
  to take the fraction out where it enters, not to round the thing that looks
  wrong: `complete.slint` rounds the summary UP TO WHOLE 16 px ROWS (the design's
  own Label row height — rounding to the pixel gives 15 and shifts everything up
  one against the mockup) and positions the block at a `floor`ed y.
- ⚠ **A Text's own `preferred-height` is its UNWRAPPED size.** `Text` carries
  `default_size_binding: implicit_size`, so it reports one line however many it
  draws, and measuring a wrapping paragraph with it silently under-reads. **Put it
  in a layout and read the LAYOUT's preferred height** — that accounts for the
  wrap. `OptionRow` and `CompleteScreen` both do this.
- ⚠ **Setting a child's `width` inside a layout can be a compile error.**
  `width: parent.width - 30px` makes the child's preferred width depend on the
  parent's actual width while the parent's preferred width is computed from its
  children — Slint rejects it as "layoutinfo-h is part of a binding loop". Use the
  layout's `padding-*` to express an indent instead.
- ⚠ **`MutedLabel` wraps by default** (it is a paragraph style elsewhere), so a
  long value in a fixed-height row grows to two lines and runs through whatever is
  below it. `PropertyRow` pins `height` and sets `wrap: no-wrap; overflow: elide;`
  for exactly that reason.
- ⚠ **Slint's ScrollView draws its bar OVER the viewport**, not beside it, and
  there is no "always reserve space" mode. Full-width children run underneath it —
  the bar sat over the UPDATE badges. Reserve `Tokens.scrollbar-gutter` as
  `padding-right` on the scrolled content.
- ⚠ **THE GUTTER IS THE CONTENT'S PADDING, NEVER THE SCROLLVIEW'S GEOMETRY.** The
  bar is pinned to the VIEW's right edge (fluent: 14 px wide, thumb 4 px inside
  it), so insetting the view to make room insets the bar by the same amount. Both
  wells were built that way and the thumb ended up floating 16 px in from the
  panel border with an empty lane outside it — the "equal padding on all sides"
  look that a scrollbar, alone among the things in a container, should not have.
  **The view fills the well (1 px in, to clear the border) and every inset that
  used to be its geometry is `padding-*` on the scrolled content.**
- ⚠ **Slint's `Text` CANNOT BE SELECTED.** The Run screen's log is therefore a
  single **read-only `TextInput`** — `read-only` keeps selection, the drag and
  Ctrl+C while disabling editing, and it also suppresses the caret, so the log
  does not sit there blinking. It has to be **one string**: a column of per-line
  `Text` could not be selected across lines even if each line could be, and
  selecting across lines is the entire point for someone reporting a failed
  install. The C++ side keeps one `std::string` for the same reason — a line model
  beside it would be a second representation of one log.
- ⚠ **The badge's SELECTED ink is the accent**, not its status color. The inverted
  badge is a hole punched in the card, so its ink is whatever the card is filled
  with — the same rule the green badge's dark ink follows (that one is the SCREEN
  background). A green "INSTALLED" left on an all-accent card was the one word on
  the screen fighting the selection instead of belonging to it.
- ⚠ **Two Texts of the same size do NOT share a baseline across font families.**
  `vertical-alignment: bottom` aligns layout BOXES, and Roboto Mono's metrics
  differ from Roboto's, so the aircraft version chip sat visibly low beside the
  name. Give both the same explicit height and align within it.
- ⚠ **A percentage-positioned SVG lands on fractional pixels and renders soft.**
  The logo is four vectors at percentage insets; rasterizing into a fractional
  rect resamples across two pixel columns. `Logo` snaps POSITION AND SIZE BOTH to
  whole pixels — rounding only the position moves the blur rather than removing it.
- ⚠ **Do not port the reference code's absolute positioning.** `get_design_context`
  returns React + Tailwind that positions nearly everything with `absolute` and
  percentage insets, because that is how Figma describes a frame. Reproducing it
  gives a window that is pixel-correct at exactly one size. Read the intent — row,
  column, gap — and write the layout. Details in `src/native/ui/README.md`.
  ⚠ **The Complete screen is the ONE exception and stays positioned**, for two
  reasons that both bite: a child width that reads `root.width` inside a layout is
  a compile-time binding loop (a layout sizes itself FROM its children, so the
  child sizing itself from the parent closes the circle — the same trap written up
  on `OptionRow`, which escapes it with padding), and that screen needs three real
  widths taken from the pane. Second, the design's two gaps are `flex: 1 0 0`, an
  even split of a remainder that depends on the summary's height — and a summary
  height is a FONT METRIC, so halving it lands every button below on a fractional
  y and resamples it across two pixel rows. `spacer-h` floors the half once. This
  screen has had the soft-edge bug before; do not "tidy" it into a VerticalLayout.

- ⚠ **NOTHING IMPORTS std-widgets, AND THAT IS NOW A RULE** (2026-08-13).
  `ScrollView` was the last one; it is `PhotonScrollView`, a `Flickable` plus our
  own thumb. It went for the reason `PathField` never used `LineEdit` — the
  widget's own theming fights every color here — but with two specifics worth
  keeping:
  - **fluent's colors are not reachable.** `alternate-background` (the dark
    rounded track it paints behind the thumb on hover) and `border` (the thumb)
    are literals inside `fluent/styling.slint`, not derived from the public
    `Palette` global. There was no re-theming option; replacing the widget was
    the only one.
  - ⚠ **and they follow the USER'S OS THEME.** Both are written
    `dark-color-scheme ? <dark> : <light>`, so one build drew a `#2C2C2C` track
    with an 8%-white thumb on a dark-mode machine and an `#f0f0f0` track with a
    45%-BLACK thumb on a light-mode one. Nothing else in this UI is
    theme-reactive, so it was one control disagreeing with the whole window on
    half of all machines — and invisible on whichever machine wrote the code.
  The replacement keeps fluent's geometry (14 px lane, thumb 4 px in from its
  right edge, 2 px growing to 6 px) so `scrollbar-gutter` still describes the
  truth, and keeps the resting thumb at fluent's own 8% white. What changed is
  the hover: **transparent track, opaque white thumb**. ⚠ There is deliberately
  no track color token — inventing one is the moment to stop.

### 5.1 Focus and keyboard navigation (2026-08-12)

Everything here fails **silently** — Slint compiles all of it, logs nothing, and
the window still screenshots correctly. `tests/test_installer_ui.py` is the guard
and each rule below has a named case in it.

- ⚠ **A `FocusScope` SWALLOWS CLICKS OVER ITS AREA, and it defaults to its
  parent's geometry.** `FocusScope::input_event` returns `EventAccepted` for a
  press whenever `enabled` and `focus-on-click` are both true — it does not merely
  take focus. Written plainly it therefore covers the control it was added to
  serve and kills every TouchArea beneath it: the button stops clicking, with no
  error and no visual change. Slint's own Button pins its scope to `x: 0;
  width: 0;` for exactly this reason and so does every one here. The App's
  window-sized fallback scope takes the other way out, `focus-on-click: false`.
  Those two escapes are what the test accepts.
- ⚠ **A `key-pressed` handler MUST `return reject` for what it does not handle,
  Tab above all.** Slint runs Tab/Backtab traversal in the window's key handler
  only *after* the focused item and all its parents have rejected the event, so a
  handler that falls off the end traps focus on its own control permanently.
- ⚠ **The white 2 px keyline means FOCUS and only focus.** It used to be Primary's
  permanent identity (2 px at rest, thinned to 1 px on hover); the design dropped
  it from every resting state and made it the Focus variant of all three styles.
  Putting `primary` back into that binding does not restore a decoration — it
  makes the focus ring invisible on the button every screen leads with. A
  decoration that is always on cannot also be a state.
- ⚠ **The aircraft list moves the SELECTION with the arrows, not a separate
  cursor.** It is one tab stop. This is the radio-group convention, but here it is
  also forced: a selected card and a focused button are both "2 px white border",
  so a focus cursor able to sit on an *unselected* card would have to invent a
  third look or claim two rows were chosen. Tying them together keeps the border
  meaning one thing per element — which is why `AircraftCard` takes `highlighted`
  (render as Hover) rather than `focused`.
- ⚠ **Reveal on the PROPERTY, not in the key handler.** `select()` goes out to
  C++, which decides whether the selection actually moves and writes `selected`
  back. Scrolling from the handler would chase the row we *asked* for and would
  not scroll at all when C++ moves the selection itself. `changed selected` is the
  hook.
- ⚠ **Focus must be put back when the step changes.** Every screen lives in an
  `if`, so advancing destroys the focused control and focus becomes nothing —
  after which Enter does nothing at all until the user thinks to press Tab.
  `changed step => { rootKeys.focus(); }`. It is also what lets repeated Enter
  walk the whole wizard.
- ⚠ **A text field swallows Enter, so it needs its own way out.** `TextInput`
  raises `accepted` and consumes the key, which the App's fallback scope never
  sees. `PathField` re-raises it and app.slint gates it on the same
  `can-advance` the Next button uses — hoisted to one property so the key and the
  button cannot disagree about whether the step is legal.
- **Clicking does not focus, deliberately.** Nothing here calls `focus()` from a
  TouchArea except the aircraft list (so the arrows work straight after a click),
  which means a ring only ever appears from Tab — `:focus-visible` behavior, and
  the reason clicking Next does not leave a keyline behind.
- ⚠ **The dead-space TouchArea must be declared BEFORE the content, and it hands
  focus BACK rather than dropping it.** Clicking the background dismisses the
  ring; that is one window-sized TouchArea in `app.slint`. Slint hit-tests the
  LAST child first, so declared first it sits behind every screen and sees only
  what nothing else took — moved after the layout it covers the whole UI and
  swallows every click, which is the FocusScope trap above reached by a different
  route and just as invisible. And it calls `rootKeys.focus()`, not
  `clear-focus()`: the second hides the ring and takes the Enter key with it,
  leaving a window whose keyboard silently stops working until someone presses
  Tab.
- **`accent`, not white, on the path field.** Every other ring is the design's
  white keyline; that field is a light box (`#e4e8ed`) on a dark screen and white
  on it is invisible.

---

## 6. Open threads

- **The folder picker is Windows-only.** `platform::PickFolder` uses
  `IFileOpenDialog`; it returns "" elsewhere, which degrades to typing the path
  into the field. That is why the field is editable rather than a read-only
  display of whatever the dialog returned.
- ⚠ **The accent blue does not match the plugin.** The mockup uses `#509ac8`; the
  in-sim UI is `kUiBlue` `#1678b5` (X-Plane's own UI blue), with every other
  accent derived from it through `UiLighten`/`UiFade`. Installer and plugin would
  read as different products. **Deferred, not decided** — it wants a deliberate
  answer, not a default.
- ⚠ **macOS universal binaries are untested.** `CMAKE_OSX_ARCHITECTURES` is
  `arm64;x86_64`; cargo builds one architecture at a time, and Corrosion drives it
  per-arch and lipos the results — which only works if rustc has BOTH standard
  libraries installed. CMake now checks for them and fails with the exact
  `rustup target add …` command rather than letting it surface as a cargo error
  deep in a Slint log. **The check is written but has never run on a Mac**, and
  neither has the resulting universal binary. Windows is the overwhelming majority
  of users, so this is deferred rather than blocking.
- **CI needs Rust** (`.github/workflows/release.yml`, three runners) before
  `PHOTON_GUI` can default ON.
- **Review → Run → Complete has now been watched** (2026-08-13), both as a dry run
  against the real X-Plane install and as a REAL install against a scratch root
  built from copies of one aircraft's `.acf` files and cockpit textures. The worker
  thread, the progress callback, the log marshalling and the Complete summary all
  behave. What is still unwatched is a real install into a real X-Plane — the
  difference is only which directory it writes to, but it has not been done.
- **`make_release.py` is untouched.** It stages `photon-installer` by name, so a
  GUI build drops into the bundle with no change — but that has not been
  exercised, and the `package-release` skill's smoke tests have not been run
  against a GUI binary.
- **The relicense to GPLv3 has not been applied.** `LICENSE` still says MIT.

---

## 7. The progress bar's cost model (2026-08-13)

A reference section, placed after the open threads because it is looked up rather
than read. The code is `src/native/src/core/progress.{h,cpp}`; the contracts are
pinned by the `progress:` cases in `src/native/tests/install_tests.cpp`.

### 7.1 What was wrong

`ProgressFn` was added for the ~57 MiB cockpit texture copy and **nothing else ever
called it**. So:

- An install **without** the cockpit mod — the common case, and the only case
  available on the A330-900 — reported *zero times*. The bar sat at 0% until the run
  ended and `gui.cpp` set it to 1.0 itself. That is the 0 → 100 jump.
- An install **with** it was worse, not better: the bar ran 0 → 100% during a copy
  that is step 2 of 5 of the *last* phase, then sat at 100% while the livery scan and
  the four-`.acf` patch ran on.

Nothing about either reads as broken. A bar that goes 0 → 100 looks like a fast
install, and one that reaches 100% and pauses looks like a slow final flush.

### 7.2 Why a fixed weight table cannot work

The dominant cost is reading the **aircraft's** files, not writing ours, and aircraft
differ enormously. Measured, dry-run, per airframe:

| airframe | `objects/*.obj` | dry-run install |
|---|---|---|
| A319 | 233 MB | 0.95 s |
| A320 | 263 MB | 0.50 s |
| A321 | 278 MB | 0.76 s |
| A330-900 | **1,597 MB** | **11.5 s** |

⚠ **The A330-900 is 23x the A320 because it is the ONLY airframe in
`GlowRedirect()`.** The skin-glow redirect reads every `.obj` in `objects/` to find
its targets, so that one phase is ~97% of that install and 0% of every other. Any
shared weight table is wrong by a factor of twenty on one airframe or the other.

So **the plan is built per run, from file sizes** — `stat` only, no reads, which is
what makes pricing an eleven-second phase cost microseconds.

### 7.3 The measured rates

Each is (measured ms) / (bytes that phase actually touches), on an NVMe machine with
a warm cache:

| kind | evidence | ms/MiB |
|---|---|---|
| `.obj` token scan | A339 glow: 1,597 MiB x2 passes in 11,131 ms | 3.5 |
| `.obj` patch | A320 RealWings: 140.6 MiB in 1,479 ms | 10.5 |
| `.acf` attach | A320 9.21 MiB / 346 ms; A339 4.52 MiB / 176 ms | 38 |
| `.acf` spot patch | A320 9.21 MiB in 258 ms | 28 |
| byte copy | 59 MiB to the same volume in 118 ms | 2.0 |

⚠ **The absolute numbers do not matter and the ratios do.** The bar is
`done / total`, so scaling every rate by a constant gives the identical bar — a
slower disk changes how long it takes, not where it sits. What *would* skew it is the
ratio between an I/O-bound phase and a CPU-bound one, and those move differently
across machines. That is the known, accepted imprecision. It is a progress bar.

⚠ **The `.acf` parse is the dearest thing here per byte, by an order of magnitude** —
19x a raw copy — because it reads every number and re-renders it in that file's own
float style. The glow scan, which reads a gigabyte and a half, is the *cheapest* per
byte. Both were counter-intuitive; neither would have been guessed.

⚠ **A dry run is priced differently.** Every write is
`if (!dryRun) WriteOrThrow(dest, CopyBytes(src))` — one guard covering the read as
well as the write — so a dry run skips *both halves* of a copy and pays only for the
scans and parses. Unmodelled, the cockpit texture copy is more than half of an
interior plan that never happens.

### 7.4 How to re-measure

Do not adjust these by intuition. The recipe that produced them:

1. Add a `Mark(const char*)` to `actions.cpp` that prints
   `PHASE|<name>|<ms since last mark>` to stderr, and call it at each phase boundary
   in `Install`, `Uninstall`, `InstallScreens` and `InstallInterior`.
2. Build, then run `photon-installer install --aircraft … --dry-run` for each
   airframe x wing x cockpit-mod combination, collecting stderr.
3. Divide each phase's ms by the bytes it touches — `find <dir> -name '*.obj'
   -printf '%s\n'` for the scans, the four `.acf` for the parses.
4. Put the result in the table in `progress.h` **with its evidence**, and remove the
   instrumentation.

### 7.5 Two things this found

- ⚠ **`patch_glow::Run` re-scanned the whole aircraft.** The installer calls
  `TargetFiles` to know what to back up, then called `Run`, which called `TargetFiles`
  again — reading every `.obj` in the aircraft twice. On the A330-900 that was 11.1 s
  of an 11.5 s install. `Run` now takes the list; the A339 install went **11.5 s →
  6.0 s**. The self-scanning path stays for callers with no list.
- ⚠ **A step the run enters that the plan never priced is INVISIBLE in the numbers.**
  It is missing from the denominator as well as the numerator, so the run still ends
  on exactly 1.000 and every arithmetic check passes. This is not a guess: the test
  written to catch it passed against a mutation that deleted a `plan.Add`. Hence
  `progress::SetUnknownStepHook`, null in production and armed by the test suite —
  which found a real one on its first run, the uninstall's screens step being priced
  on a narrower condition than the run enters it with.

### 7.6 Verified

- A339 dry run, 6 s: the bar sweeps 6.9 → 80.5% at a near-constant 15%/s, then 100%.
- A320 real install with the cockpit mod, ~1 s: nine distinct positions, monotonic.
- Every guard mutation-tested. ⚠ A missing `Finish` is deliberately *not* tested for
  and is not a bug — `Plan::Begin` banks the step it is leaving, so only a run's last
  step needs its own, and it has one.

---

## 8. The context menus (2026-08-14)

Right-click menus, from the design's `Radio Button/Context Menu` (`291:562`) and
its one row (`291:553` Default, `291:555` Hover). Three of them ship:

| Where | Rows |
|---|---|
| The X-Plane path field (`PathField`) | Cut · Copy · Paste · Select all |
| The installer log (`RunScreen`) | Copy · Select all |
| An aircraft card (`AircraftScreen`) | Open aircraft folder... |

The component is `PhotonContextMenu` in `ui/widgets.slint`; every color and metric
is in `tokens.slint` under *the context menu*. The contracts are pinned by
`ContextMenuTests` in `tests/test_installer_ui.py`.

### 8.1 Why it is a `PopupWindow` and not `ContextMenuArea`

Slint 1.10 grew a built-in `ContextMenuArea`/`Menu`/`MenuItem` trio, and on this
build it draws **fluent's** menu — the same unreachable literals and the same
`dark-color-scheme ? … : …` branch that got std-widgets' `ScrollView` replaced by
`PhotonScrollView` (§5). A floating menu is the worst place to accept that: it has
nothing beside it to be compared against, so on a light-mode machine it does not
read as one control disagreeing with the window, it reads as a different program's
menu. The three built-in elements are banned by a test.

### 8.2 Tripwires

- ⚠ **A menu's TouchArea must be declared BEFORE the TextInput it serves, and
  getting that backwards kills the text field, not the menu.** Slint hit-tests the
  LAST child first, so a TouchArea declared after a `TextInput` takes every
  **left** click too — no caret, no drag-selection, no editing — while the screen
  still draws perfectly. Declared before it, the TextInput gets first refusal and
  returns `EventIgnored` for a right press (its `input_event` handles Left and
  Middle and ignores the rest), so exactly one kind of click ever reaches the
  menu. Same rule as the App's dead-space TouchArea (§5), reached from the other
  direction.
- ⚠ **`pointer-event`, never `clicked`.** `TouchArea::clicked` is left-button only
  by construction; the right button is reachable through nothing else. And
  `pointer-event` fires for **every** button, so a handler that does not test
  `PointerEventButton.right` opens the menu on an ordinary click too.
- ⚠ **The menu element is zero-sized and states `x: 0; y: 0`.** A popup's x/y are
  measured from the element that holds it, and a sized child with no stated
  position is CENTERED by Slint (the trap `SizedItemTests` guards on TouchAreas) —
  so dropping either line offsets every menu by half its parent, which looks like
  the *caller* computing bad coordinates.
- ⚠ **Declare it in a NON-LAYOUT parent.** A layout overrides its children's
  x/y/width/height, and the origin those coordinates depend on becomes wherever the
  layout put the row.
- ⚠ **Both dimensions of the popup are stated, and the width is MEASURED HERE
  rather than left to the popup** (2026-08-14). Slint's popup layout would in fact
  size it — the generated `layout_info_h` for a popup does aggregate its children's,
  straight through the chrome `Rectangle`, which was checked in the generated C++
  rather than assumed. But the width would then exist only inside the runtime, and
  the right-edge flip below has to *know* it: a popup's `x` is evaluated **before**
  its size is solved, so reading `popup.width` there decides the flip from the
  previous menu's width.
- ⚠ **The measuring stack is the only way to take a maximum over `entries`.** Slint
  has no fold over a repeater — but a `VerticalLayout`'s preferred **width** is
  exactly the widest of its children, which is the same thing said as geometry. So
  `PhotonContextMenu` holds a hidden `VerticalLayout` of the same labels and reads
  `measure.preferred-width`. Two things about it:
  - it reads a **layout's** preferred width, never a `Text`'s — a Text's is a layout
    property and reads 0 outside one (the trap written up on `SmallRadio`);
  - ⚠ **the menu root needs `clip: true`, and that is load-bearing, not tidiness.**
    Slint does not clip by default and the stack is a real column of Texts sitting
    at the element's origin: unclipped, the menu's own labels are painted onto the
    screen behind it, permanently and nowhere near a menu.
- ⚠ **The measuring copy and the drawn copy must be the same type**, which is what
  `MenuLabel` is for. Written twice they drift and the menu comes out a few pixels
  short of its own longest label — visible only as an elided row, with nothing
  saying the two fonts disagree. A test forbids a second `font-family`.
- ⚠ **128 px is a FLOOR, not the width.** The design's 200 px is a mockup width, not
  a rule — its two sample rows say "Normal option" and "Hovered option", which draws
  the look and measures nothing that ships.
- ⚠ **Near the right edge the menu anchors its top-RIGHT corner at the pointer.**
  Slint already clamps a popup into the window, so nothing was ever drawn
  off-screen — it slid left until it fit, which left the pointer in the middle of
  the menu, resting on a row it did not open. ⚠ **The edge is `Tokens.window-w`
  because this window is a fixed size**: `App` binds its width to that token and
  Slint makes a Window with an explicit size non-resizable, so the two cannot
  disagree. A resizable installer would have to pass the real width down — there is
  no global for it — and the clamp would be all that kept a menu on screen. ⚠ **The
  flip lives in the `x` binding, not in `show-at`**: a popup re-evaluates its
  position while open, so a one-shot decision would be re-derived from the
  un-flipped coordinate on the next property change and the menu would jump.
  Vertically nothing flips; the clamp still slides a menu up off the bottom edge.
- ⚠ **The arrow keys reuse the design's Hover look, and that is safe only because
  there is exactly ONE highlighted row.** The design has just Default and Hover —
  the same position `AircraftCard.highlighted` is in, and the same resolution. The
  menu owns a single `active` index; hover *writes* it rather than the row reading
  its own TouchArea, or an arrowed-to row and a hovered row would both light up and
  the menu would appear to have two selections. `show-at` resets it to −1, or the
  second menu opens with the first one's row lit. Up/Down wrap (unlike the aircraft
  list, which clamps — a menu is one to four rows and Up meaning "the last one" is
  what every menu does); Home/End jump; Enter and Space activate.
- ⚠ **Enter is ACCEPTED even with no row highlighted.** Rejecting it sends the key
  up to the App's root FocusScope, which is what advances the wizard — so opening a
  menu and pressing Enter would step to the next screen with the menu still on top
  of it. Escape is the opposite case and must be rejected: closing on Escape is
  Slint's own popup handling and only runs if the focused chain declines it.
- ⚠ **The row list and the `activated` index ladder are ONE list written twice.**
  Adding a row without its branch is a menu row that does nothing; removing one
  shifts every action below it onto the wrong label. A test compares the counts.
- ⚠ **Cut and Paste need no extra wiring to keep C++ in step.** Slint's `cut()` and
  `paste()` both raise the TextInput's own `edited` callback (they route through
  `delete_selection`/`insert` with `TriggerCallbacks`), so `PathField`'s existing
  binding forwards a pasted path to `path-edited` exactly as typing does. Were that
  not so the failure would be the quiet one: paste a path, press Next, and the
  installer validates the one you replaced.
- ⚠ **The field is focused before the menu opens.** Copy needs a selection and
  Paste needs a caret, and a field never clicked into has neither — the menu would
  open, look right, and do nothing. Showing the popup takes focus straight back off
  it, which is safe *only* because Slint's `FocusOut(PopupActivation)` deliberately
  preserves the selection anchor.
- ⚠ **No disabled rows, anywhere.** The design's option has exactly two variants,
  Default and Hover. So the log's menu has no greyed-out "Paste" (it is
  `read-only`, and pasting would be refused silently) and its "Copy" is live even
  with nothing selected — Slint does expose the selection offsets, but only as
  properties its own headers mark internal and undocumented.
- ⚠ **A right-click on an aircraft card does NOT select it**, so the row the menu is
  about and the row that is about to be installed to are allowed to differ.
  `AircraftScreen` captures the index at right-click and hands it to C++;
  `on_open_aircraft_folder` must use that and never `Selected()`, or it opens the
  wrong folder exactly when the two differ — and is right often enough not to be
  noticed. A test forbids `Selected()` in that handler.
- ⚠ **One menu for the whole aircraft list, held by the SCREEN, not one per card.**
  A card lives inside a repeater inside a Flickable: a popup anchored to a card
  would be re-placed against it on every property change, so scrolling would drag
  an open menu along with the row — and a card recycled out from under an open
  popup is worse than a slightly longer callback. That is why `AircraftCard` raises
  `context-menu` in **window** coordinates (`absolute-position` is a reserved output
  on every element): the screen cannot convert from a coordinate space belonging to
  a row whose position it does not track.

### 8.3 The restyle, and the two deliberate deviations (2026-08-14)

The design was restyled after the first implementation. Two things moved, and both
are straight transcriptions:

- **The panel is OPAQUE `#2b333a`.** It was `rgba(13,21,28,.82)` over a
  `backdrop-blur(3.55px)` — an effect Slint has no way to draw on any renderer, and
  the one thing this component could not reproduce. The design has replaced both
  with a flat fill, so the gap is closed rather than papered over. ⚠ The hex is
  `bar-bg`'s and is deliberately **not** an alias of it: that is the bottom bar, a
  fixed piece of the frame, and this is a floating surface. Same
  duplication-on-purpose rule as `scroll-thumb-active`.
- **The row's type is 12 px Medium**, down from 14. ⚠ That is `label-size` at a
  weight nothing else pairs it with — every other 12 px line in this UI is semibold
  — which is why `MenuLabel` states it rather than inheriting `MutedLabel` with a
  color override. It also **changes the measured width automatically**, which is
  the whole reason the measuring stack and the drawn row share one component.

Two things depart from the design on purpose:

- ⚠ **The inner keyline is 1 px, where Figma says 2** — asked for, and right. A 2 px
  white keyline means FOCUS in this UI and nothing else (§5.1, and the note on
  `focus-ring-w`): a decoration that is always on cannot also be a state. A menu
  wearing one permanently would spend that signal on a panel that is never focused
  and leave every button's Focus variant looking like a menu border. At 1 px the
  edge highlight still reads and cannot be mistaken for the ring. It is otherwise
  **exact, not an approximation** — an inset shadow of zero blur and a spread is
  precisely a band inside the border box, i.e. a border, and it needs a second
  `Rectangle` only because a Slint Rectangle carries one and the black one is spent.
  Same case as `check-lip-*`; the genuinely approximated inset shadows are in §5.
- ⚠ **The menu draws no focus ring at all**, and is exempt from the convention on
  purpose. A menu is modal for as long as it is open — nothing else on screen can
  hold the keyboard — so a ring saying "this has focus" would be a true statement
  that tells the reader nothing. What the keyboard is *on* is the highlighted row,
  and the row's accent fill already says so.

The one effect still dropped is **`text-shadow: 0 1px 2px rgba(0,0,0,.33)`** on the
resting row. Slint's `Text` has no text-shadow, and the honest stand-in — the same
string drawn again a pixel lower — cannot carry the 2 px blur and shows as a hard
doubling, which at 12 px is worse than the omission. It is worth nothing either
way: white on `#2b333a` is already at full contrast, which is why Figma drops it on
the **hovered** row, where the fill is lighter and it would actually have said
something.

### 8.4 Not done

- **Not yet watched running.** It compiles and every source-level contract is
  pinned, but nothing has confirmed a menu opens where the pointer is. Four things
  to look at, in this order, because each can fail while the ones before it pass:
  1. **The aircraft list's position** — the only menu whose coordinates go through
     an `absolute-position` conversion, so it is the one that can be offset by a
     constant while the other two are perfect.
  2. **The arrow keys the moment a menu opens**, with no click first. That rests on
     `forward-focus` reaching a *zero-width* FocusScope through the popup's
     `call_focus_on_init`; Slint's own ComboBox does the same thing with a
     full-size one, and focus is not geometric, but that is reasoning rather than a
     measurement.
  3. **The right-edge flip**, by right-clicking within a menu's width of the window
     edge. Getting it wrong is invisible in the common case and the clamp still
     keeps the menu on screen, so this needs looking for.
  4. **The measured width**, which shows as the menu being exactly as wide as its
     longest label plus 16 px — a wrong `MenuLabel` would elide instead.
- **`platform::OpenFolder` is untested on macOS and Linux.** It is `open`/`xdg-open`
  through the same `Spawn` helper `OpenUrl` and `OpenInEditor` use, so it is as
  tested as they are — which is to say, on Windows only.

---

## 9. Run advances itself on a clean run (2026-08-14)

The Run screen steps to Complete on its own when the run finished **and had nothing
to say**. Pressing Next to leave a log with nothing in it is a step that exists only
because the screen does. `gui.cpp`, in the worker's completion lambda; guarded by
`AutoAdvanceTests` in `tests/test_installer_ui.py`.

⚠ **"Clean" counts a WARN, not only an ERROR**, and that is the whole design of the
gate. The Run screen is 288 px of log against a 26 px bar precisely so a run that
went wrong can be read where it happened, so the auto-advance has to be off for
exactly the runs worth reading. A warning is a problem the user is *meant* to read —
the wing-mod mismatch is the live example, where the installer proceeds with what
was asked and says so — and skipping the only screen that shows it would make
warning pointless. `RunLog::Clean()` is what tracks it; "succeeded" and "had nothing
to say" are different questions and the bar answers the second.

⚠ **It is not a one-way door**, which is what makes advancing on the user's behalf
acceptable rather than merely convenient. Complete's Back returns to Run with the
log still on it (`GoBack` is a plain `step - 1` and Run keeps its state) — see §5's
note on why that pair is a closed loop.

⚠ **The delay (500 ms) is not comfort, it is the progress bar.** `set_progress(1.0f)`
and the step change arrive in the same event-loop callback, so stepping inline means
the last frame the Run screen ever paints is the one *before* the bar filled: the
install appears to end on a part-full bar and a screen that vanished. Hence
`kAutoAdvanceMs` and a `slint::Timer::single_shot`.

⚠ **The timer re-checks the step before it fires.** The user can press Next — or
Escape, which is Back — inside the delay, and dragging someone off a screen they
just chose is worse than the one click this exists to save.

Both an install and an uninstall are covered (it is one screen and one rule), and a
dry run is **not** excepted: it is still a run that finished with nothing to report,
and its log is one Back away like any other.

---

## 10. The step rail is navigation (2026-08-14)

A completed rail step can be clicked to return to it. The mockup draws the rail as
an indicator only; this makes it a control, because people try it without thinking
about it. `StepRail` in `ui/widgets.slint`, `GoTo` in `gui.cpp`, guarded by
`RailNavigationTests`.

**The rule, entire:** a row is clickable when the screen allows the rail to be used
at all, *and* the row is **strictly before** the current step, *and* it is not the
**Run** row.

### 10.1 Why there is no high-water mark

The obvious design is to remember the furthest step reached and let the rail jump
anywhere within it. That is not what this does, and the reason is that **walking
forward is what builds each screen**: `GoNext` validates the root, re-detects
aircraft, publishes per-airframe availability and assembles the review list. A jump
*past* a screen arrives with the previous run's answers — a Review page built for
the aircraft that was selected before the X-Plane root changed, rendering perfectly
and describing an install that is not the one about to happen.

So the rail is backward-only, and "pressing Next locks you back in" falls out for
free rather than needing state to reset: there is never anything ahead of you to
reach. Backward is safe for the mirror-image reason — `GoTo` is a plain assignment,
exactly like `GoBack`, and every screen behind you already holds its answers.

### 10.2 The three refusals

⚠ **Not forward, ever** — §10.1.

⚠ **Never *to* Run, from anywhere.** Every other screen is a set of *answers*, and
re-showing one costs nothing. Run is not a set of answers; it is an action that has
already happened. A rail row landing on a finished log with an enabled Next
underneath gives no sign of which run it belongs to, and no sign that pressing that
Next will not run anything. Complete's **View log** button goes to that screen and
says what it is for (§11.4).

⚠ **Never *out of a run in progress*.** The test is `run_finished`, deliberately not
`step != kRun` — the Run screen is reachable after the fact via View log, and a rail
dead on it would make that a trap whose only exit is Next, back where you came from.

⚠ **The rule lives in `gui.cpp`, and app.slint's copy is presentation.** Same split
as `next()` and `back()`. It is not belt-and-braces: the two are written separately,
so a UI that offers a row it should not must still be unable to move the wizard.

### 10.2a ⚠ The rail was dead on Complete for two days, and that was wrong

The original rule refused the rail from Run **and Complete**, on the argument that
Complete ⇄ Run is a closed loop: no finished install may be walked back into Review
and started a second time, and Review would in any case be showing a version
transition assembled *before* the run — "Not installed → 0.8.4" about an aircraft
that now has 0.8.4 on it.

**That argument was aimed at the wrong control.** Going *back to look* at a screen
restores a set of answers and commits nothing. What commits something is pressing
**Next**, and Next from a revisited screen walks the wizard forward from there,
rebuilding everything after it exactly as a first pass would (`GoNext` re-detects,
re-publishes, re-assembles). The lock therefore already sits on the forward move,
which is where it belongs; adding a second one to the backward move bought nothing
and cost the thing people reach for without thinking.

What survives of the old argument is the one row above: **Run**.

⚠ **The residue, named rather than fixed:** Review reached after a run still shows
the pre-run version transition. It is a caption, not a decision — nothing acts on
it, and the Install button under it re-runs an install that is idempotent by
construction (the backup-once rule consults `added[]` precisely so a second pass
cannot back up our own copy as the stock original). Re-detecting on the way back
would cost `GoTo`'s plain-assignment invariant, which is worth more: a jump that
quietly re-derived things would mean going back to check what you chose showed you
something else.

### 10.3 Smaller things that are still decisions

- ⚠ **Mouse only.** Six rail rows in the tab order would sit *ahead* of every
  screen's content, so Tab would walk the sidebar before reaching the thing the user
  came to fill in. The keyboard already has this: Escape is Back.
- **The hover look is the design's own `Label/Highlight`.** The step component has
  no Hover variant, but its label already has exactly two looks — muted and white —
  and white is what `current` uses. Borrowing it costs no new color and cannot drift
  from the palette. Plus a pointer cursor, on reachable rows only.
- ⚠ **`reachable` is restated in the label's color even though a disabled TouchArea
  reports no hover.** Slint clears that flag from the touch area's event *filter*,
  so it only happens on the next mouse event — and the pointer can be sitting on a
  rail row at the moment the row stops being reachable, which is exactly what
  pressing Enter on the Review screen does. Without the extra term one rail label
  stays white on the Run screen until the mouse is moved.
- **The click target is the design's own 130x16 step**, no invented geometry. It is
  a small target between 18 px gaps; growing it would mean inventing a hit area the
  mockup does not describe, and the rows would start swallowing the gaps between
  them.

### 10.4 Not done

- **Not watched running.** The thing to check first is that a rail row on the
  *current* step does nothing and shows no pointer — a `<=` where the code says `<`
  would make it a no-op button that highlights, which reads as the rail being broken
  rather than as an off-by-one.

---

## 11. The 2026-08-14 restyle and QoL batch

Seven changes landed together. The ones that are pure transcription are listed and
left; the ones that cost something, or that found something, are written up.

| | Where |
|---|---|
| Primary button focused on arrival (4 screens) | `apply-focus` in `app.slint` |
| Review attributes: 22 px row, 24 px pitch, no indent | `screens/review.slint` |
| Log in Roboto Mono, green markers, red failure lines | `screens/run.slint`, `gui.cpp` |
| Progress bar restyled + a red **Fail** state | `screens/run.slint`, `tokens.slint` |
| Complete redesigned around a success/failure glyph | `screens/complete.slint` |
| Auto-advance delay 700 → 500 ms | `kAutoAdvanceMs` |

### 11.1 ⚠ Neither embedded font contains `✓`

Checked against `Roboto-Medium.ttf` and `RobotoMono-Medium.ttf`'s own `cmap`
tables rather than assumed: **U+2713 is absent from both**, and so is **U+2192
`→`**. The installer log has prefixed `" ✓  "` since it was written, which means
that character has been drawn by a **system fallback font** the whole time — the
precise failure the fonts are embedded to prevent (`assets/fonts/README.md`): it
looks correct on this machine and is a missing-glyph box on one whose fallback does
not cover it.

The log's marker is therefore an **SVG**, not a character — and it is the Complete
screen's own success/failure glyph, so the same two shapes mean the same two things
throughout the installer. Verified in the generated C++: the log row and the
Complete screen resolve to the *same two embedded resources*.

⚠ **`→` is still a character** and still falls back — in `EntryFor`'s version
transition and `BuildReview`'s. Benign on Windows, and now known about.
`LogRenderingTests` bans U+2713 from every `.slint` and from `gui.cpp`, and carries
a second test that re-reads the fonts, so the ban expires by itself if a future
font drop ever includes the glyph.

### 11.2 ⚠ The colored log cost drag-selection, and what replaced it

The log was **one read-only `TextInput`** precisely so it could be selected and
dragged across lines — someone reporting a failed install has to copy it. A Slint
`TextInput` has **one color**, and the log now needs three: a green marker, a white
message, and a whole line in red when it reported something. No arrangement gets
both, so the well is a column of rows and the selection is gone.

**`copy-log()` replaced it** — one context-menu row that puts the entire log on the
clipboard, which is what the dragging was for. Two consequences:

- ⚠ **`platform::SetClipboardText` is ours, not Slint's.** Slint exposes the
  clipboard only on its `Platform` interface — for code that *implements* a
  backend — and the one place the toolkit uses it (`TextInput::copy`) is exactly
  what we just stopped having. Win32 `CF_UNICODETEXT` (⚠ never `CF_TEXT`: aircraft
  folders and X-Plane paths are routinely non-ASCII, and the codepage conversion
  would mangle the very lines being copied), `pbcopy`/`xclip` elsewhere.
- ⚠ **The menu has no "Select all"**, deliberately. Offering one that selects
  nothing visible is worse than not offering it.

⚠ **C++ keeps the line vector and the Slint model is built from it**, so `copy-log`
joins exactly what the screen is drawing rather than a second rendering of the same
log. The `log-text` string that used to be the single representation is gone.

⚠ **The red line and the stalled auto-advance are ONE rule.** `bad` is ERROR *or*
WARN — the same predicate `RunLog::Clean()` uses — so a line painted red and a line
that stops the Run screen advancing itself (§9) are the same set by construction.
Two spellings drift the first time a level is added.

### 11.3 The outcome has to be sayable without reading

`run-failed` drives the progress bar's red fill and the Complete screen's glyph, and
both must agree with the headline and summary C++ renders — so ⚠ **all three read
the same `ok`**. A picture that disagreed with the sentence beside it would be worse
than no picture.

⚠ **A failed run is NOT forced to 100%.** Only success sets the bar full; a failure
stops where it stopped, so the red bar also reports how *far* it got — the first
thing anyone reading the log wants to know, and something a full bar reading "100%"
would flatly contradict. ⚠ `StartRun` clears `run-failed`, or a second run after a
failed one opens on a red bar and a failure glyph before doing anything.

⚠ **The green fill and the dark percentage are WITHDRAWN** — see §12.2. Both lasted
one afternoon. The running fill is the accent again and the ink is white on both
states.

### 11.4 ⚠ Complete's Back became "View log", and what that button does changed twice

Back was added to Complete (2026-08-12) because the installer log vanished the moment
a run finished, and stepping back to Run was the only way to read it again (`GoBack`
is a plain `step - 1`, so the round trip was free). The redesign puts **View log** in
that exact slot.

It shipped for a few hours **opening the log file** in the OS text viewer, on the
reading that a scrollable, searchable window is strictly more than Back reached. It
is not: the Run screen is *already* showing that log, with its markers, its colors
and its red failure lines, and swapping a designed screen for a wall of timestamps is
a worse view of the same information. So **View log now goes back to the Run
screen** — the same movement Back made, by the same plain `step` assignment, with a
label that says what the movement is for on a screen where "Back" begged the question
of back to what.

⚠ **The file is still written**, flushed per line, unchanged. It is the record for
afterwards — the only artifact that outlives the process and the only copy carrying
levels and timestamps at all — and nothing in the GUI opens it any more. `log_available`
went with the file-opening version: it gated a button on a log file existing, and the
button now goes to a screen that exists whenever Complete does.

⚠ **This is also what makes the Run screen's auto-advance acceptable** (§9). The
wizard skips past that screen on the user's behalf, so there has to be a way back to
it that is not a re-run — and after §10.2a there is now exactly one, which is why Run
is the one rail row that stays blocked.

`BackButtonTests` encodes all of it, including that the two buttons share one slot and
must never both appear — the bar has room for two.

### 11.4a Right-click the outcome to copy it (2026-08-14)

A fourth context menu, on the Complete screen's outcome row: one row, **Copy result**,
putting the summary sentence on the clipboard through the same
`platform::SetClipboardText` the log's *Copy log* uses (§11.2).

- ⚠ **The hit area is the whole outcome block, not the Header 2 alone.** The sentence
  is one 26 px line in a 442 px pane and the glyph beside it is part of the same
  statement. Same reasoning as the log menu covering its well rather than its lines: a
  menu you can only raise over a thin run of text is one nobody finds.
- ⚠ **It copies the summary only, not the headline above it.** The summary is already
  a whole sentence naming the version, the components and the aircraft — and
  `"Failed: ..."` on the other branch — while the headline is a two-word label for it.
  Pasting "Install complete" ahead of a line that says the same thing is noise in a bug
  report.
- ⚠ **The text is read back out of the UI property, not re-rendered.** `SummaryLine`
  runs on the worker against a snapshot that is gone by then; rebuilding it here would
  let the copy disagree with the sentence the user right-clicked. The property *is*
  the sentence.

### 11.5 Focus on arrival

Four screens open with their primary control focused: Splash, Directory (⚠ **only
when the path is usable** — if it is not, the user has work to do here and the
keyboard has no business skipping it), Features, and Complete's *Modify another
aircraft*.

⚠ **Aircraft and Review are deliberately excluded.** Aircraft *requires* a choice —
Next is disabled until one is made — and Review exists to be read. Pre-arming the
button that writes to someone's X-Plane install is the one place this would be
actively wrong.

Notes on the mechanism:

- **Enter already did this**; the root scope's fallback handler *is* "press the
  primary button". What the focus buys is the **ring** — the screen now says which
  control Enter operates instead of leaving it to be guessed. ⚠ That makes the ring
  appear without a Tab, a deliberate break from the `:focus-visible` behaviour every
  button otherwise has.
- ⚠ **`BottomBar`'s Next is no longer inside an `if`.** Slint refuses to reach an
  element inside a conditional from outside one, and `focus-next()` is outside. It
  cost nothing — `next-visible` was bound to a literal `true` at its only call site.
- ⚠ **Complete focuses itself**, for the same Slint rule: every screen lives in an
  `if`, so `app.slint` cannot call `.focus()` into one. It takes an `autofocus` flag
  and calls `.focus()` in its own `init`, and `apply-focus` **stands back** when the
  destination owns its focus — without that exclusion the two race for the keyboard
  on arrival, and which wins depends on when Slint instantiates the `if`.
- ⚠ **`init` as well as `changed step`.** `changed step` does not fire for the step
  the window opens on, so without the `init` the splash — the one screen never
  arrived at — would be the only one that did not get its focus.

**Features is currently unconditional**, and that is an assumption worth naming: the
screen has no automated check that can fail today. Everything on it is prechecked
and any row the airframe cannot use is hidden, so the defaults are already an
answer. If a check ever belongs there — a wing-mod mismatch is the obvious candidate,
since `core/wingmod` already detects one and the GUI does not surface it — this is
where it would gate.

### 11.6 ⚠ The taller attribute rows overflowed Review, and it pushed the bottom bar off

Found in-sim, reported as *"too much padding between the Features header and the
first attribute line"* — which is precisely where the extra space was, and is not
where the bug was.

**The Review page has no vertical slack.** It is the tallest screen in the installer
and the design fills the content area to the pixel:

```
24 pad + 40 head + 4 x (10 + 40 property rows) + 10 + [24 head + 94 list] + 24 pad = 416
```

416 is 480 minus the bar. The restyle grew the attribute rows from 18 px to 22 (+16),
and the "Selected Options" head was a **sibling of the attribute list** in the
`content-gap` column rather than grouped with it (+10) — 426 in a 416 px box.

⚠ **A Slint layout resolves that by GROWING, not by clipping or scrolling.** The
screen and the bottom bar are children of one `VerticalLayout`, so ten pixels too
many do not truncate the list — they slide the bar, a fixed piece of the frame on
every other screen, down out of the window. The symptom is therefore at the *bottom*
of a screen whose problem is in the *middle*.

The fix is the design's own structure: Figma's `Features` frame (`212:1642`) holds the
head and the `Attributes` frame with the list flush at y = 24, so they are one block
with zero spacing and only the block takes a `content-gap`. That is exactly the 10 px.

⚠ **`ReviewLayoutTests` now does this arithmetic end to end** — tokens, the screen's
literals, `PropertyRow`'s height and the row count `BuildReview` can emit — and
asserts it fits. It is an *equality* in the design and the test asserts `≤`, so there
is room to take something out of that page and none at all to put anything in.
⚠ **The row count is the MAXIMUM, not the usual one**: two attribute rows are gated
per airframe, and the page has to fit the airframe that shows all four.

### 11.7 Not done

- **None of this has been watched running.** In rough order of "can fail while
  everything before it passes": the focus ring appearing on the right control; the
  log's SVG markers sitting on the first line of a *wrapped* message; the red bar
  stopping at its true fraction; the Complete glyph's 83 px column against a
  four-line failure summary.
- **`SetClipboardText` is Windows-tested only**, like every other platform call.

---

## 12. The log tail, the problem summary and Complete's two buttons (2026-08-14)

Five changes. The middle three are one feature seen from three sides.

| | Where |
|---|---|
| A menu's highlight is dropped when the pointer leaves it | `PhotonContextMenu` |
| The progress bar is blue again, ink white | `tokens.slint` |
| The log follows its own bottom | `PhotonScrollView`, `screens/run.slint` |
| Problems repeated under a rule at the end of the log | `screens/run.slint`, `gui.cpp` |
| Complete's blue moves between Exit and *Modify another* | `gui.cpp`, `app.slint` |

### 12.1 ⚠ A highlighted row has an OWNER, not just a value

Leaving the menu with the pointer must drop the highlight — a lit row under no
cursor is a menu claiming a choice nobody is making, and the next Enter takes it. A
row the **arrows** chose must survive exactly that, because the pointer is not what
is driving the menu and moving it out of the way is not a decision.

So `active` gained `active-by-key` beside it, written at the only two places that
can set it (`step()` and a row's `entered`), and reset by `show-at`.

Three things about the detector, all of them checked against Slint's own source
rather than assumed:

- ⚠ **It is an ANCESTOR `TouchArea`, wrapping the rows — not a sibling behind
  them.** Slint sets `has-hover` in `input_event_filter_before_children`
  (`items/input_items.rs`: `let hovering = !matches!(event, MouseEvent::Exit)`),
  which runs on every ancestor of the item under the pointer. A sibling would go
  un-hovered the moment the pointer touched a row, and the highlight would clear on
  the way **in**.
- ⚠ **It does not swallow the rows' clicks.** That filter returns
  `ForwardAndInterceptGrab`: children get the press first and a row that returns
  `GrabMouse` keeps it. This is the *opposite* of the sibling case written up on
  `PathField` (§8.2), where declaration order is the whole contract — the two are
  easy to confuse and the difference is ancestry, not order.
- ⚠ **A popup does get told when the pointer leaves it.** `WindowInner::
  process_mouse_input` resolves the item tree to the popup only while the position
  is inside its geometry; outside, `root` is `None` and it calls
  `input::send_exit_events` on the previous stack. That `MouseEvent::Exit` is what
  clears the wrapper's `has-hover`. Without that branch the whole approach would be
  inert, and it would look like the handler never firing.

⚠ **The chrome is inside the wrapper too**, deliberately: the 3 px of padding and
border is still "in the menu", and clearing the highlight while crossing it between
two rows would flicker.

### 12.2 ⚠ The green progress bar is withdrawn, and so is the dark ink

It shipped green (`status-ok`) with the well's own color as the percentage, on the
reasoning in §11.3: both fills are light, and the green badge already carries dark
ink for that reason.

**Green is a verdict, and this bar should not be making one.** It spends almost all
of its life somewhere between 3% and 40% — a green sliver reporting a run that has
barely started reads as a claim it is going well, when what the bar measures is only
how far it got. The accent is the neutral statement; the **red failed state is the
only verdict the bar makes**, and it is worth more with nothing else competing.

⚠ **The ink is white on both states, and that is the pairing the file already had.**
`badge-ink-dark` exists for the *green* badge alone (`AircraftCard`, kind 2) because
that one fill is too light for white; every other badge, the red included, is white.
Dark ink here was a second rule invented for one control.

⚠ **One ink for BOTH states, not one per fill.** A percentage that changed color with
the fill would be the fill's news said twice, and it is the one thing on the bar that
has to stay legible while the fill slides out from under it.

### 12.3 The log follows its own bottom

`PhotonScrollView` gained `stick-to-bottom` (off by default — the aircraft list is
built once and jumping it to the end would be wrong) and a `scroll-to-bottom()`.

⚠ **IT KEYS OFF THE CONTENT'S HEIGHT, NOT OFF THE MODEL, and the difference is one
line every time.** A `changed` on `log-lines` fires before the repeater has
instantiated the new row, so `viewport-height` is still the old height and the view
scrolls to the *previous* bottom — which reads as an off-by-one in the scroll
arithmetic rather than as a sequencing bug. `viewport-height` changes only once the
row has been measured, which is exactly the moment the new bottom exists.

⚠ **It also cannot fight the user, for the same reason.** It fires when the content
changes and never otherwise, so once the run is over the log stops growing and
scrolling back through it is undisturbed. There is no mode to leave, no "am I near
the bottom" heuristic, and nothing to detect a manual scroll against — all of which
the obvious implementation needs.

⚠ `scroll-to-bottom()` uses `-maximum`, never a large negative number: Flickable
clamps its own dragging and wheel handling, but a direct write to `viewport-y` is
taken literally and would scroll the log into empty space.

Verified in the generated C++: a `ChangeTracker` on the Flickable's `viewport_height`
calling `scroll_to_bottom()`, and — because `stick-to-bottom` is a compile-time
constant at both call sites — **no tracker at all on the aircraft list**.

### 12.4 The problems are repeated at the end of the log

When a run stops without advancing itself, its ERROR/WARN lines are repeated under a
1 px rule with 16 px either side, at the bottom of the log well.

⚠ **THE SUMMARY'S CONDITION AND THE AUTO-ADVANCE'S ARE THE SAME ONE, by
construction.** The wizard advances on `ok && RunLog::Clean()`; `problems` is exactly
the lines `Clean()` counts, and a failed run always writes an ERROR (the catch does
it) — so *"finished with a non-empty summary"* and *"finished and did not advance
itself"* are the same state. That is what the section answers: **why am I still
here.** It is gated on `finished` as well, or a WARN two seconds into a run would
open a section that grows while it is being read.

⚠ **It is at the bottom because that is where §12.3 has just put the reader.** The
two arrived together and work as one thing: the run stops, the view is already parked
on the end of the log, and what is under the rule is why it stopped. A banner above
the log or a line in the heading would be somewhere the user is not looking.

⚠ **C++ filters, Slint does not.** `PublishLog` builds both models off the one vector
on the same `bad` flag the rows are colored by, so a line cannot be red in the log and
absent from the summary, or worded differently in the two places. Slint has no
filtered model, and the alternative — a repeater over every line with the good ones
hidden — is worse than it looks: ⚠ **an invisible element still occupies its cell in
a Slint layout**, so that leaves the log ending in a column of blank rows with a
scrollbar for them. One `if` around the whole group instead.

⚠ **The rule's PADDING is the point, not its color.** The summary repeats lines that
are a few rows further up, so what has to read at a glance is that a *section* has
started; a hairline sitting in the log's own 2 px line gap would be one more row.

### 12.5 Complete's blue moves to the likelier action

If every supported ToLiss aircraft under this root is now current there is nothing
left to modify and the likely next move is to leave, so **Exit** takes Primary and
the focus ring; otherwise **Modify another aircraft** takes both.

⚠ **THE FILL AND THE RING ARE ONE DECISION.** A blue button that Enter does not press
is worse than no recommendation at all, so one flag drives `next-primary`,
`CompleteScreen.primary`, `focus-bar-next` and `screen-owns-focus`. ⚠ The last two
must stay **mutually exclusive**: `apply-focus` reads them in order, so a state where
both were true would focus the bar *and* leave the screen calling `.focus()` on its
own button a frame later — a race whose winner depends on when Slint instantiates
the `if`.

⚠ **ONLY THE FILL MOVES.** *Modify another aircraft* keeps the full pane either way:
the design's layout is not a statement about which action is likelier, and resizing
it would move every row under it between two runs.

⚠ **The answer is RE-DETECTED on the way in** (`EnterComplete`), because the run just
changed it. Reading the pre-run list is wrong in exactly the case the feature exists
for: install the last out-of-date aircraft and the screen would still push "Modify
another".

Three consequences, each of which was a bug before it was a line of code:

- ⚠ **The auto-advance now presses Next instead of assigning the step**, so both
  routes into Complete go through `GoNext` and cannot diverge. A clean install is
  *exactly* the run that reaches Complete without anyone clicking anything, so a
  timer that set the step directly would skip the re-detection in the one case that
  matters most. `set_step(kComplete)` now appears once in the whole file.
- ⚠ **The refresh KEEPS THE SELECTION**, matched by folder — the only stable identity
  here, since the index is a position in a list that was just rebuilt. Plain
  `RefreshAircraft` clears it, and since §10.2a the rail can walk from Complete back
  to Review, where Install would then find `Selected()` null and *return*. A button
  that silently does nothing is the worst of the ways this could go wrong.
- ⚠ **"Green" is read through `EntryFor`**, the same function the aircraft list badges
  with, rather than re-derived from `photon`/`stale`. A second spelling would let the
  Complete screen call an aircraft current that the previous screen badged REPAIR.

⚠ **An empty list is NOT "all up to date."** It is unreachable from Complete — you
cannot install without an aircraft — but the sentence is false about nothing, and of
the two defaults the safe one offers to go back to the list.

### 12.6 Not done

- **None of it has been watched running.** In order of what can fail while everything
  before it passes: the menu highlight clearing on the way *out* and not on the way
  *in*; the log's tail landing on the new row rather than the one before it; the
  problem summary's rule sitting under a log that has just scrolled; and Complete
  opening with the ring on the blue button rather than beside it.
- **The problem summary has no heading**, only the rule. A repeat of two red lines
  under a divider may read as the log having printed them twice rather than as a
  summary. Deliberately left as asked; worth a look in-sim before deciding.

---

## 13. The wing mod preselects itself, and Backspace is Back (2026-08-14)

### 13.1 The Features screen opens on the wing mod that is actually drawing

`SeedWing` runs `wingmod::Detect` when the wizard walks from Aircraft to Features and
sets `g.wing` from the verdict, so the radio is already correct on arrival.

⚠ **THIS IS NOT THE SAME ACT AS `actions::Install`'s WING CHECK, and it does not
break that one's rule.** The install-time check is emphatic that it *reports and that
is all* — a detector that silently substituted the user's choice would be
unexplainable from the screen they clicked through, and would be wrong exactly when
detection is wrong. That check runs **after** the user has chosen. This one runs
**before anyone has chosen anything**: it fills in a default, on screen, where it can
be seen and changed. Not overriding an answer and not offering one are different
rules, and only the first is worth keeping.

The install-time WARN therefore **stays exactly as it was**, and is now also the
backstop for this: a wrong seed produces the same warning a wrong manual choice
would. `WingSeedTests` pins that `actions.cpp` still never assigns `opts.wing`.

Four things that are decisions rather than plumbing:

- ⚠ **Once per aircraft, not once per visit** (`wingSeededFor`, keyed on the
  **folder** — the index is a position in a list detection rebuilds).
  `ApplyAvailability` runs on every walk from Aircraft to Features, so without this
  a user who corrected the radio, stepped back to check the aircraft and came
  forward again would find their correction silently undone. That is the one
  behavior a preselection must never have: the reason to touch that radio at all is
  believing the detector is wrong.
- ⚠ **`WingIndexFor` is `WingName` backwards, and the two are one table written
  twice.** Off by one it preselects RealWings on a Durantula aircraft — which
  installs cleanly, says so, and leaves the wingtip lights holding still while the
  wing flexes, needing the aircraft loaded and airborne to notice. The test parses
  both tables and compares them.
- ⚠ **Clamped to `WingsFor`.** `wingmod::Detect` answers about the aeroplane;
  `WingsFor` answers about what Photon can build. `resolve_mount()` falls back to
  whatever mount exists, so seeding a variant with no build is how a stock OBJ ships
  mislabeled as a mod build.
- ⚠ **It is written to the file log**, because a checked box explains itself to
  nobody. The log is the only place that can say why — and the only place a wrong
  detection leaves a trace worth reading.

**Cost:** it reads the `.acf` attachment tables and, for Durantula, both wing OBJs.
Same read `Install` does per run, once per aircraft — and the reason this is not done
for every row of the aircraft list. It ran **on the UI thread** for the first few
hours of this feature's life; §13.1a is how that was fixed.

### 13.1a The read moved to its own thread, and moved earlier (2026-08-14)

`SeedWing` reads nothing now. `WingProbe` — one worker thread for the session, one
pending slot, results kept for the life of the window — runs `wingmod::Detect`, and
`SeedWing` is handed the verdict.

⚠ **THE COST IS I/O, NOT ARITHMETIC, WHICH IS WHY THREADING ALONE WOULD NOT HAVE
FIXED IT.** The check reads all four `.acf` variants (9.2 MB on the A320 and A321
here) and both wing OBJs (3.8–4.4 MB more). Measured on this machine: the whole of
`status --json` around it is **17 ms warm and 40 ms on the first read of an
aircraft**, and a cold cache, a slow library drive or a scanner in the path make that
worse without limit. It was being paid inside the Next click on the Aircraft screen,
where a GUI-subsystem window does not repaint — so it read as the button sticking.

⚠ **IT IS STARTED FROM THE *SELECTION*, AND THAT IS THE ACTUAL OPTIMIZATION.** A
probe first asked for at Next is a probe that is waited on: Features cannot open on
the right radio until the answer exists. Starting it when the card is clicked
overlaps the read with the user reading the screen and travelling to the button —
the one interval in this wizard where nobody is waiting on anything — so by the time
Next is pressed the answer is normally already in hand and the seed is a map lookup.

⚠ **ARROWING THROUGH THE LIST RE-SELECTS ON EVERY KEYPRESS** (aircraft.slint has no
separate focus cursor — the arrows move the selection itself). Hence one worker and
one *pending slot* rather than a thread per request: at most one probe runs and one
waits, and an aircraft already answered is never asked again.

⚠ **NEXT NEVER WAITS.** Selecting a row and pressing Enter is one keystroke apart and
beats any read, so the miss path opens Features on `stock` — the safe default, and
what the screen showed before there was a detector at all — and a `slint::Timer`
polls the probe every 40 ms until the verdict lands. A bounded wait was considered
and rejected: it is the same freeze with a shorter fuse.

⚠ **THE HAND-OFF IS A POLL, NOT `slint::invoke_from_event_loop`, AND THAT IS NOT A
STYLE CHOICE.** The C++ wrapper is `slint_post_event`, which **`.unwrap()`s** the
Rust result — posting after the loop has terminated **aborts the process** — and a
worker cannot check "is the loop still running" without racing the answer. A timer
runs on the UI thread by construction and cannot outlive the loop. (The install
worker in `StartRun` *does* post, and has the same latent hazard if the window is
closed mid-run; it is pre-existing and untouched here.)

Everything a late verdict must not do, each one its own guard in `WingProbeTests`:

- ⚠ **It cannot overrule a hand-made choice.** `on_set_wing` clears
  `wingSeedPending`, which is `wingSeededFor`'s rule extended across the wait: the
  reason to touch that radio at all is believing the detector is wrong.
- ⚠ **It cannot land on another aeroplane.** `wingSeedPending` is a *folder*, not a
  flag, and choosing a different aircraft abandons it. ⚠ Re-selecting the aircraft
  already being waited on must **not** abandon it — `wingSeededFor` refuses a second
  seed for the same folder, so that would strand it on `stock` with the answer
  sitting unread.
- ⚠ **It cannot move the radio after Review.** Past Features the answer is no longer
  a default being offered: the user has already been shown what will be installed,
  and moving it under them is the silent substitution this whole feature is written
  not to be.

Nothing is logged when a verdict is dropped. `actions::Install` runs the same check
per run and writes both the summary and the mismatch WARN, so a run that happened is
described either way — and a wizard the user walked away from has nothing to
describe.

**Not measured:** the double parse inside `wingmod`. `ReadAttachments` calls
`DeclaredCount` and `AttachmentRows`, and each does its own full `acf::ReadProps` over
the same 2.3 MB of text — so every `.acf` is parsed twice, four times per aircraft.
Worth fixing (it would halve `Install`'s copy of this read too), but it is **not** what
the lag was: warm, the whole check is a few ms.

⚠ **Measured against the three real aircraft here (2026-08-14): A319 stock, A320
DURANTULA, A321 stock** — all `determined`, all `acf_variants_agree`, all `healthy`.
(The A320 has since had RealWings installed over it, which is what surfaced the XP11
false alarm — `docs/installer.md` §*Which `.acf` variants Photon touches*.)
Note that this contradicts the example in `CLAUDE.md` §*Which wing mod*, which uses
this same A320 as the illustration of "both mods' folders are present and it is
running stock wings". The folder half is still true (`RealWings320/` is present and
unattached on all three); the stock half is not, on today's disk. The detector reads
7 converted blocks per wing, symmetric — Durantula is drawing.

⚠ That 7 is itself worth knowing: `kDurantulaBlocksPerWing` is 3 (V1.3's manual), and
the mismatch is reported as a **Note, never a verdict** — exactly as designed, since
a later version may add cases and an installer that called that "wrong" would cry
wolf on a correct install. **Asymmetry** is the hard failure, and there is none.

### 13.2 Backspace is Back

Beside Escape, in the App's root `FocusScope`, on the same `can-go-back` guard.

⚠ **IT IS SAFE ONLY BECAUSE THAT SCOPE IS A FALLBACK.** Slint offers a key to the
focused item first, and an editable `TextInput` accepts Backspace **unconditionally**
— even on empty text, since only `read-only` makes it decline (checked in Slint's
`TextInput::key_event`: the `DeleteBackward` branch returns `EventIgnored` for
`read_only()` and `EventAccepted` otherwise). So the path field keeps deleting
characters and never navigates.

⚠ **A READ-ONLY `TextInput` WOULD LEAK IT.** There are none left — the installer log
stopped being one on 2026-08-14 (§11.2) — and `BackspaceTests` fails if one comes
back, because it would pass Backspace to the wizard while the user is trying to use
it on text.

⚠ **An open context menu now swallows it**, which is the rule Enter already had
(§8.2): a menu is modal for as long as it is open, so a key that moves the wizard has
to stop at it. Without that, right-clicking and pressing Backspace steps the wizard
*backward* with the menu still on screen. Escape remains the deliberate exception —
rejecting it is how Slint's own popup handling gets to close the menu.

⚠ **On Complete it goes to Run**, and that is not a hole. `can-go-back` is true there
(the guard is `!running`, and Complete is not Run); what the bar hides on that screen
is the *Back button*, because "View log" occupies the slot and makes the same move
(§11.4). The key doing what the visible button does is consistent.

### 13.3 Not done

- **Neither has been watched running.** For the seed, the thing to check is the A320:
  it should arrive at Features with **Durantula** already checked, and the file log
  should carry a `features: … - preselecting --wing durantula` line. For Backspace,
  check it inside the path field first — that is the one place it must NOT navigate.
- **The probe has not been watched running either** (§13.1a). Next off the Aircraft
  screen should now be instant. The one path worth deliberately provoking is the miss:
  arrow to the A320 and press **Enter immediately** — Features should open at once
  with nothing checked and tick over to **Durantula** a moment later. Then the same
  again, but click a wing radio inside that moment: the choice must **stick**.
