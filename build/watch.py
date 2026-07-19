#!/usr/bin/env python3
"""
Rebuild the OBJ files whenever the light config (or its mount / raw snippets)
change. Add --write so each rebuild also lands in the live X-Plane install —
just reload the aircraft in the sim after each rebuild.

Stdlib only: a lightweight mtime poll (no watchdog dependency).

Usage:
    python build/watch.py                     # watch; rebuild all 3 airframes (stock)
                                              #   into dist/
    python build/watch.py --write             # ...and install into X-Plane on
                                              #   every save (the live-iteration loop)
    python build/watch.py --wing durantula    # build a specific wing variant
    python build/watch.py --out .scratch/out  # build to a folder instead of dist/
    python build/watch.py --airframe a320     # only one airframe
    python build/watch.py --once              # build once and exit (no watching)
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CONFIGS = [REPO / "src" / "lights" / "lights.style.phdsl",
           REPO / "src" / "lights" / "lights.layout.phdsl"]
BUILDER = REPO / "build" / "build_objs.py"
PARSER = REPO / "build" / "photon_dsl.py"
WATCH_FILES = CONFIGS + [BUILDER, PARSER, REPO / "src" / "plugin" / "PI_ToLissPhoton.py"]
WATCH_DIRS = [REPO / "src" / "lights" / "mounts", REPO / "src" / "lights" / "raw"]


def watched_paths():
    paths = [p for p in WATCH_FILES if p.exists()]
    for d in WATCH_DIRS:
        if d.exists():
            paths += [p for p in d.rglob("*") if p.is_file()]
    return paths


def signature():
    sig = {}
    for p in watched_paths():
        try:
            sig[str(p)] = p.stat().st_mtime_ns
        except OSError:
            pass
    return sig


def build(args) -> bool:
    cmd = [sys.executable, str(BUILDER), "build", "--wing", args.wing]
    if args.airframe:
        cmd += ["--airframe", args.airframe]
    if args.out:
        cmd += ["--out", args.out]
    if args.write:
        cmd += ["--write"]
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    ts = time.strftime("%H:%M:%S")
    if r.returncode == 0:
        print(f"[{ts}] built ({args.wing}):")
        for line in r.stdout.strip().splitlines():
            print("    " + line)
    else:
        print(f"[{ts}] BUILD FAILED:")
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser(description="Rebuild OBJs on config save")
    ap.add_argument("--wing", choices=["stock", "durantula", "realwings"], default="stock")
    ap.add_argument("--airframe", choices=["a319", "a320", "a321"])
    ap.add_argument("--out", help="build to this dir instead of dist/")
    ap.add_argument("--write", action="store_true",
                    help="also install each rebuild into the live X-Plane aircraft "
                         "folders (reload the aircraft in-sim to see it)")
    ap.add_argument("--interval", type=float, default=1.0, help="poll seconds (default 1)")
    ap.add_argument("--once", action="store_true", help="build once and exit")
    args = ap.parse_args()

    dest = args.out or "dist/"
    if args.write:
        dest += " + live X-Plane install"
    scope = args.airframe or "all airframes"
    watched = " + ".join(c.name for c in CONFIGS if c.exists())
    print(f"watching {watched} + src/lights/{{mounts,raw}} + src/plugin")
    print(f"  -> {dest}  [{scope}, wing={args.wing}]")
    build(args)
    if args.once:
        return
    print("edit the config and save to rebuild;  Ctrl-C to stop.")
    last = signature()
    try:
        while True:
            time.sleep(args.interval)
            cur = signature()
            if cur != last:
                last = cur
                build(args)
    except KeyboardInterrupt:
        print("\nstopped.")


if __name__ == "__main__":
    main()
