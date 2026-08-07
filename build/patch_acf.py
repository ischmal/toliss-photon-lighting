#!/usr/bin/env python3
"""Dev CLI for the `.acf` cockpit-spot patcher (interior mod).

⚠ THE IMPLEMENTATION LIVES IN `installer/patch_acf.py`, not here — see the note in
`build/patch_acf_screens.py` for why the appliers moved into the installer package.
The ⚠ format trap (XP12 `-70.000000000` vs XP11 `-70.0`) is documented at the top
of that module.

This file stays so the dev CLI keeps working:

    python build/patch_acf.py <aircraft_dir> [--reverse] [--dry-run]
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from installer.patch_acf import *              # noqa: E402,F401,F403
from installer.patch_acf import _set_props, main  # noqa: E402,F401

if __name__ == "__main__":
    main()
