#!/usr/bin/env python3
"""Dev CLI for the skin-glow redirect patcher.

⚠ THE IMPLEMENTATION LIVES IN `installer/patch_glow.py`, not here — see the note
in `build/patch_acf_screens.py` for why the appliers moved into the installer
package. `REDIRECT` (which must stay in sync with `GlowMapForIcao` in
`src/native/src/plugin.cpp`) went with it.

This file stays because the dev CLI is documented:

    python build/patch_glow.py --root <objects dir> --airframe a339 [--reverse]
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from installer.patch_glow import *          # noqa: E402,F401,F403
from installer.patch_glow import main       # noqa: E402,F401

if __name__ == "__main__":
    main()
