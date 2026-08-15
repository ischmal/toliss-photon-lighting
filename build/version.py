"""Read THE version out of `src/native/src/core/version.h`.

⚠ THAT HEADER IS THE ONE DEFINITION (docs/installer_cpp_plan.md §8c). The plugin
reads it directly, `photon-installer` reads it directly, and the build tooling
reads it through here — so a release bundle's NAME, the README it ships, the
plugin's About tab and the OBJ version marker cannot disagree.

They did disagree once: `kPhotonVersion` in plugin.cpp and `VERSION` in
the installer's own VERSION drifted 0.6 against 0.7 through a whole feature, because
nothing in-sim looks wrong when they differ — the About tab simply reports an older
version than the files beside it, which is exactly what a half-finished install
looks like.

⚠ THERE IS NO SECOND COPY LEFT. `installer/constants.VERSION` was one until the
Python installer was deleted (2026-08-09); `build/constants.py` now RE-EXPORTS what
this module reads rather than restating it, and `tests/test_version.py` fails any
build script that hard-codes a version again.

⚠ COMMENT LINES ARE SKIPPED. version.h documents its own required shape, and the
first reader written against it matched that EXAMPLE — reporting the placeholder as
the release.
"""
from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VERSION_H = REPO / "src" / "native" / "src" / "core" / "version.h"

_DEF = re.compile(r'constexpr\s+char\s+kPhotonVersion\[\]\s*=\s*"([^"]*)"')


def read_core_version(path: Path | None = None) -> str:
    header = path or VERSION_H
    try:
        text = header.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        raise SystemExit(f"cannot read the version header {header}: {e}")
    for line in text.splitlines():
        if line.lstrip().startswith("//"):
            continue
        m = _DEF.search(line)
        if m:
            return m.group(1)
    raise SystemExit(
        f"kPhotonVersion is not defined in {header} — it must stay on one line as "
        f"a `constexpr char` array with a plain string literal, because the build "
        f"tooling reads it by regex")


VERSION = read_core_version()
