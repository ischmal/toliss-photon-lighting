# Embedded fonts

Roboto and Roboto Mono, the families the Figma design specifies.

⚠ **These are committed and embedded in the binary on purpose.** Roboto is *not*
installed by default on Windows or macOS, and Slint silently falls back to a system
font when a `font-family` is missing — the UI lays out correctly and simply is not
the design, with no warning anywhere. Embedding is the only way a downloaded
single-file installer looks the same on every machine.

They reach the binary through side-effect imports at the top of `../../app.slint`:

```slint
import "assets/fonts/Roboto-Regular.ttf";
```

That instructs the Slint compiler to include the font and makes the family
available to every `font-family` in the project. Nothing else references these
paths, so a renamed file fails at compile time rather than at runtime.

| File | Weight | Used for |
|---|---|---|
| `Roboto-Regular.ttf` | 400 | Header 1 |
| `Roboto-Medium.ttf` | 500 | Header 2, card titles, log lines |
| `Roboto-SemiBold.ttf` | 600 | buttons, labels, step names |
| `Roboto-Bold.ttf` | 700 | the progress percentage, attribute values |
| `Roboto-Black.ttf` | 900 | the status badges |
| `RobotoMono-Light.ttf` | 300 | the aircraft version chip |
| `RobotoMono-Medium.ttf` | 500 | the installer log |

⚠ **NEITHER FAMILY CONTAINS U+2713 `✓`, NOR U+2192 `→`** — checked against both
files' own `cmap` tables, not assumed. Any such character in a UI string is being
drawn by a **system fallback font**, which is the exact failure these embedded
files exist to prevent: it looks fine here and is a missing-glyph box on a machine
whose fallback does not cover it. The installer log used to prefix `✓` and now
draws `assets/glyph-success.svg` instead. ⚠ **The `→` in the version transitions
(`EntryFor`, `BuildReview`) is still a character and still falls back** — it is
benign on Windows and worth knowing about.

**Static instances, not the variable font.** Google now ships Roboto as a variable
font, but weight selection from a variable family depends on the renderer setting
the `wght` axis, and Slint's behavior there is unconfirmed. Six static files are
~680 KB total and remove the question.

## License

Roboto and Roboto Mono are licensed under the **Apache License 2.0** by Google —
`https://fonts.google.com/specimen/Roboto`. Apache 2.0 is compatible with the
GPLv3 this project ships under.

Downloaded from the Google Fonts API (`fonts.gstatic.com`, Roboto v51 / Roboto
Mono v31). To refresh:

```bash
curl -H "User-Agent: Mozilla/4.0" \
  "https://fonts.googleapis.com/css2?family=Roboto:wght@400;500;600;700;900&family=Roboto+Mono:wght@300;500"
```

and download the `.ttf` URLs it returns. ⚠ Do **not** pull these from
`github.com/google/fonts` — Roboto's static instances are no longer at
`apache/roboto/static/` there, and a `raw` URL for a path that does not exist
returns a 302 KB HTML error page that `curl -o` will happily save with a `.ttf`
name.
