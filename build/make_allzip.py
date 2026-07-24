#!/usr/bin/env python3
"""Bundle the finished per-platform archives into ONE all-platforms zip.

The maintainer-convenience download: grab this single file from the workflow,
extract it, and you have all four release assets ready to attach to a GitHub
Release / forum post in one go.

    ToLissPhoton-Installer-v<VER>-All-Platforms.zip
      ToLissPhoton-Installer-v<VER>-Windows.zip
      ToLissPhoton-Installer-v<VER>-macOS.zip
      ToLissPhoton-Installer-v<VER>-Linux.tar.gz
      ToLissPhoton-Installer-v<VER>-Python-Universal.zip

Only the archives actually present in --archives are included, so it still runs
locally with just your own platform's bundle (+ the universal one).

Usage:
    python build/make_allzip.py --archives DIR [--out release]
"""
from __future__ import annotations

import argparse
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from installer.constants import VERSION  # noqa: E402

ALL_NAME = f"ToLissPhoton-Installer-v{VERSION}-All-Platforms"
PREFIX = f"ToLissPhoton-Installer-v{VERSION}-"


def main():
    ap = argparse.ArgumentParser(description="Bundle the per-platform archives into one zip")
    ap.add_argument("--archives", required=True,
                    help="folder holding the per-platform release archives")
    ap.add_argument("--out", default="release", help="output dir (default: release/)")
    args = ap.parse_args()

    src = Path(args.archives)
    if not src.is_absolute():
        src = REPO / src
    out = Path(args.out)
    if not out.is_absolute():
        out = REPO / out
    out.mkdir(parents=True, exist_ok=True)

    # The four release archives (any subset present), never the all-platforms
    # zip itself (so re-running into the same dir can't nest it recursively).
    seen: dict[str, Path] = {}
    for p in sorted(src.rglob("*")):
        if (p.is_file() and p.name.startswith(PREFIX)
                and p.name != f"{ALL_NAME}.zip"
                and (p.suffix == ".zip" or p.name.endswith(".tar.gz"))):
            seen.setdefault(p.name, p)
    files = [seen[n] for n in sorted(seen)]
    if not files:
        sys.exit(f"no per-platform archives ({PREFIX}*.zip / .tar.gz) found in {src}")

    zip_path = out / f"{ALL_NAME}.zip"
    if zip_path.exists():
        zip_path.unlink()
    # Contents are already compressed (zip/gz) -> store, don't re-deflate.
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_STORED) as zf:
        for f in files:
            zf.write(f, f.name)
            print(f"  + {f.name} ({f.stat().st_size / 1e6:.1f} MB)")
    print(f"\nall-platforms zip ready: {zip_path} ({zip_path.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
