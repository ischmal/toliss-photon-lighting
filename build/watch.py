#!/usr/bin/env python3
"""
Rebuild the OBJ files whenever the light config (or its mount / raw snippets)
change. Add --write so each rebuild also lands in the live X-Plane install AND
triggers PI_PhotonDevReload's quick-reload in the running sim (via X-Plane's
UDP command port — see send_reload below), so a save shows up in-sim hands-free.

Stdlib only: a lightweight mtime poll (no watchdog dependency).

Usage:
    python build/watch.py                     # watch; rebuild all 3 airframes (stock)
                                              #   into dist/
    python build/watch.py --write             # ...install into X-Plane AND reload the
                                              #   aircraft in-sim on every save
    python build/watch.py --write --no-reload # install only; reload by hand
    python build/watch.py --wing durantula    # build a specific wing variant
    python build/watch.py --out .scratch/out  # build to a folder instead of dist/
    python build/watch.py --airframe a320     # only one airframe
    python build/watch.py --once              # build once and exit (no watching)

Note the quick-reload sequence takes several seconds (ToLiss settle + camera
restore); if you save again mid-sequence the plugin ignores the busy trigger —
your files still land, just save again (or press your reload key) afterwards.

CRASH-SAFETY NOTE: sim/operation/reload_aircraft reads the installed OBJ (and
the RealWings mod's Main.obj, if patched) synchronously, so a save landing too
soon after a reload could rewrite that same file while X-Plane is still reading
it. Two independent layers guard against this: every live-file write goes
through build_objs.atomic_write_bytes (temp file + os.replace — a concurrent
reader always sees the complete old or complete new file, never a torn one),
and this loop won't even attempt a rebuild within --reload-guard-seconds of its
last reload (default 3s; raise it if you still see trouble on a slower
machine). See RELOAD_GUARD_SECONDS below and atomic_write_bytes's docstring.
"""
from __future__ import annotations

import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# With --write, each successful rebuild asks the running sim to reload the
# aircraft by firing PI_PhotonDevReload's quick-reload command over X-Plane's
# UDP command interface (a "CMND" packet to the sim's UDP port). X-Plane
# dispatches UDP commands through the same path as a key binding — i.e. in
# command-HANDLER context, which is the only context the dev-reload plugin may
# issue sim/operation/reload_aircraft from (see the crash lesson in that file:
# a reload initiated from flight-loop dispatch hard-crashes the sim, which is
# why the plugin does NOT watch a sentinel file itself). Fire-and-forget: no
# ack exists, and if the sim isn't running the packet just disappears.
RELOAD_COMMAND = "ToLissPhoton/dev/quick_reload"
XP_UDP_PORT = 49000  # X-Plane's default UDP command/data port

# CRASH LESSON (2026-07-20): sim/operation/reload_aircraft (fired by send_reload
# below) is SYNCHRONOUS in X-Plane and reads the installed OBJ (and, for
# --wing realwings, the RealWings mod's Main.obj) off disk while it runs. If a
# save lands close enough behind a previous one, this loop's NEXT rebuild can
# start rewriting that same file while X-Plane is still mid-read of it from the
# PREVIOUS reload — reported symptom: the auto-reload worked N times then
# crashed the sim, N varying with save cadence. build_objs.atomic_write_bytes
# (used by every live-file write in the build/patch path) makes that race safe
# against garbled/torn content — a concurrent reader always gets the complete
# old file or the complete new one. This constant is the second, complementary
# layer: it keeps the write from even being ATTEMPTED while a reload we just
# fired is likely still resolving, which is what actually avoids wasting the
# retry budget in atomic_write_bytes and keeps a rebuild from racing the read
# at all. It's a conservative fixed guess, not a real busy signal from the sim
# (nothing currently reports when reload_aircraft has actually finished) — if
# your machine is slow enough that crashes/corruption still occur, raise
# --reload-guard-seconds.
RELOAD_GUARD_SECONDS = 3.0


def send_reload(port: int = XP_UDP_PORT):
    msg = b"CMND\x00" + RELOAD_COMMAND.encode("ascii") + b"\x00"
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.sendto(msg, ("127.0.0.1", port))
        print(f"    -> sent {RELOAD_COMMAND} to the sim (udp:{port}); "
              f"busy/no-sim = silently ignored")
    except OSError as e:
        print(f"    -> reload send failed: {e}")


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


def should_defer_rebuild(reloading: bool, last_reload_at: float | None,
                        now: float, guard_seconds: float) -> bool:
    """True if a detected change should be deferred rather than rebuilt right
    now, per the reload-guard (see RELOAD_GUARD_SECONDS): only relevant when
    we actually issue reloads, and only within guard_seconds of the last one."""
    if not reloading or last_reload_at is None:
        return False
    return (now - last_reload_at) < guard_seconds


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
                         "folders and trigger the in-sim quick reload "
                         "(--no-reload to skip the reload)")
    ap.add_argument("--no-reload", action="store_true",
                    help="with --write: don't send the quick-reload command to "
                         "the running sim after each install")
    ap.add_argument("--reload-port", type=int, default=XP_UDP_PORT,
                    help=f"X-Plane UDP port for the reload command "
                         f"(default {XP_UDP_PORT})")
    ap.add_argument("--reload-guard-seconds", type=float, default=RELOAD_GUARD_SECONDS,
                    help="with --write: after sending a reload, wait at least "
                         "this long before writing the live files again — "
                         f"reload_aircraft reads them synchronously (default "
                         f"{RELOAD_GUARD_SECONDS}s; raise if crashes/corruption "
                         "persist on a slower machine, see the module docstring)")
    ap.add_argument("--interval", type=float, default=1.0, help="poll seconds (default 1)")
    ap.add_argument("--once", action="store_true", help="build once and exit")
    args = ap.parse_args()

    dest = args.out or "dist/"
    if args.write:
        dest += " + live X-Plane install"
        if not args.no_reload:
            dest += " + in-sim reload"
    scope = args.airframe or "all airframes"
    watched = " + ".join(c.name for c in CONFIGS if c.exists())
    print(f"watching {watched} + src/lights/{{mounts,raw}} + src/plugin")
    print(f"  -> {dest}  [{scope}, wing={args.wing}]")

    reloading = args.write and not args.no_reload
    last_reload_at = None  # monotonic time of the last send_reload (see the guard)

    def cycle():
        nonlocal last_reload_at
        if build(args) and reloading:
            send_reload(args.reload_port)
            last_reload_at = time.monotonic()

    cycle()
    if args.once:
        return
    print("edit the config and save to rebuild;  Ctrl-C to stop.")
    last = signature()
    deferred_notice_shown = False
    try:
        while True:
            time.sleep(args.interval)
            cur = signature()
            if cur == last:
                continue
            # Guard: don't even attempt a rewrite while X-Plane's reload from
            # our own last trigger is likely still reading the files it wrote
            # (see RELOAD_GUARD_SECONDS above). Leave `last` stale so this same
            # diff (plus anything else saved meanwhile) retries next poll —
            # rapid edits coalesce into one rebuild once the guard clears.
            if should_defer_rebuild(reloading, last_reload_at, time.monotonic(),
                                    args.reload_guard_seconds):
                if not deferred_notice_shown:
                    print(f"    ...change detected, deferring rebuild "
                          f"({args.reload_guard_seconds:.1f}s reload guard active)")
                    deferred_notice_shown = True
                continue
            deferred_notice_shown = False
            last = cur
            cycle()
    except KeyboardInterrupt:
        print("\nstopped.")


if __name__ == "__main__":
    main()
