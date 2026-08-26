# `reference/toliss/` — ToLiss stock textures, harvested locally

`photon-lit-studio` tunes the integral-lighting palette against each airframe's **stock**
`objects/text.png` and `objects/text_LIT.png`. Browsing for those by hand is four file
dialogs per comparison, so they are harvested once into this folder and the tool loads
them from an aircraft dropdown by itself.

## ⚠ Everything here except this file is gitignored, deliberately

These are **ToLiss's payware textures**. We have no right to redistribute them, and this
repository is public and GPLv3.

That is the difference between this folder and its neighbor: `reference/gus/` is vendored
**with Gus's explicit permission** and says so in its own README. Nobody has given
permission for ToLiss's art, so nothing here is committed — only this README, and the
harvester that regenerates the folder from a machine that already owns the aircraft.

The build does not depend on this folder. Nothing in `dist/`, `release/` or the installer
reads it; it exists solely so a tuning session does not begin with four file dialogs.

## Regenerating it

```powershell
photon-lit-studio harvest --xplane "C:\...\X-Plane 12"
```

It scans the install for ToLiss A3xx airframes, copies each one's stock pair, and
**de-duplicates byte-identical airframes** — as of 2026-08-24 the A319 and A320 ship the
same two files, so they share one set and the folder holds two sets rather than three:

```
index.json          airframe -> set, read by the tool's dropdown
a319-a320/          text.png + text_LIT.png
a321/               text.png + text_LIT.png
```

Harvest also re-checks the property the shared profiles rest on — that every preserve
rect and the promote region hold **identical** artwork across all harvested sets, which
is what lets one Halogen (or LED) palette serve all three airframes. If that ever stops
being true it says so, and the rect table has to become per-airframe.

## ⚠ Two traps it protects you from

- **It harvests from `objects/`, never from `Photon Backup Files/`.** A backup holds
  whatever was stock when Photon last installed, and ToLiss ships updates. On 2026-08-24
  the A319/A320 backup was a whole revision behind: it still carried ToLiss's old,
  misaligned trim-scale artwork, which their update had since **fixed**. Harvesting the
  backup would have frozen the very bug this subsystem exists to stop propagating.
- **It decides "is this already recolored?" from the PIXELS, not from the install
  marker.** An aircraft can carry a Photon manifest while its textures have been restored
  to stock by hand or replaced by a ToLiss update — which is exactly the state this
  machine was in when the folder was first harvested. The test is the one thing the
  recolor always does: it drives the placard amber's blue channel to zero, where stock
  sits near 89. Refusing on the marker instead would make the tool unusable in the state
  a tuning session normally starts from; trusting the marker the other way would feed a
  recolored file back through the curve and freeze it in as "stock".

Full background: `docs/lit_recolor.md`.
