#!/usr/bin/env python3
"""Dev CLI for the `.acf` screen-glow attachment patcher.

⚠ THE IMPLEMENTATION LIVES IN `installer/patch_acf_screens.py`, not here. It moved
there when install time was decoupled from the DSL toolchain
(docs/installer_cpp_plan.md §2): the installer used to reach the patchers by
putting a BUNDLED copy of `build/` on `sys.path`, which meant every applier had to
ride along in `payload/dsl/`. The appliers are stdlib-only and are installer code,
so they now live in the installer package and the bundle ships no toolchain at all.

This file stays because CLAUDE.md documents the command:

    python build/patch_acf_screens.py <aircraft_dir> [--reverse] [--dry-run]

Importing it also still works (`import patch_acf_screens` re-exports the real
module's public names), but new code should import `installer.patch_acf_screens`.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from installer.patch_acf_screens import *          # noqa: E402,F401,F403
from installer.patch_acf_screens import _ENC, main  # noqa: E402,F401

if __name__ == "__main__":
    main()
