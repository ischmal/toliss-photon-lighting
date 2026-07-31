# Dear ImGui (vendored)

Upstream: https://github.com/ocornut/imgui — MIT, see `LICENSE.txt`.

| | |
|---|---|
| Version | **v1.92.9** |
| Commit | `01380c579715e62fb9a8d6ec0502c4ea83bfde6e` |
| Vendored | 2026-07-31 |

Committed rather than fetched, so a clean checkout builds with no network and a
given commit of this repo always builds the same UI code. `src/native/SDK/` is
gitignored because it is Laminar's redistributable; this is not.

## What is here

Core only — `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`,
their headers, and the three `imstb_*.h`. **No `backends/`**: X-Plane needs its
own, and it lives at `src/native/src/imgui_xplm.{h,cpp}`. See that file's header
for why the stock `imgui_impl_opengl2` is not usable as-is.

`imgui_demo.cpp` is included deliberately. It is the smoke test — if
`ImGui::ShowDemoWindow()` renders and responds to the mouse, the backend is
correct — and it is the fastest reference for "which widget call does X".

## Updating

Copy the same file list from a release tag, update the table above, rebuild both
configurations, and re-check the demo window. Two things to look at in the
changelog before bumping:

- **The font atlas.** 1.92 reworked fonts around `ImTextureData`, but kept the
  classic single-atlas path for backends that leave
  `ImGuiBackendFlags_RendererHasTextures` unset — which ours does, because
  X-Plane wants texture IDs from `XPLMGenerateTextureNumbers`, not
  `glGenTextures`. If a future release drops `GetTexDataAsRGBA32`, the backend
  needs the incremental-texture path before the bump can land.
- **`ImGuiKey` values**, which the key mapping in `imgui_xplm.cpp` translates to.
- **Anything newly obsoleted on `ImDrawData` / `ImDrawList`.** These deprecate by going
  quiet, not by failing: 1.92.9 stopped maintaining `ImDrawData::CmdListsCount` while
  leaving the field in place, so a backend reading it drew nothing at all and reported
  no error. Skim the changelog's obsolete list against what `imgui_xplm.cpp` touches.
