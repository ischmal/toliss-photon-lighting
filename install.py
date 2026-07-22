#!/usr/bin/env python3
"""
ToLiss Photon Lighting — Installer.

A zero-dependency, cross-platform interactive installer. Walks: launch ->
X-Plane directory -> aircraft selection -> action -> perform -> complete, with
real keyboard navigation (arrows / ENTER / ESC / number keys / typing) via
installer/tui.py, real detection via installer/detect.py, and real filesystem
writes via installer/actions.py. The plugin it installs is the compiled native
`.xpl` — no XPPython3/Python prerequisite.

Run from anywhere — payload/ is resolved relative to this file, not CWD:
    python install.py [--dry-run] [--xplane-root PATH]

`--dry-run` logs every action but performs no filesystem writes (mirrors
build_objs' --dry-run convention); useful for verifying detection without
touching a real X-Plane install.
"""
from __future__ import annotations

import argparse
import time
import webbrowser
from pathlib import Path

from installer import actions, detect, payload
from installer import tui
from installer.constants import (
    AIRFRAMES, PHOTON_URL, VERSION, WING_ACTION_LABEL, WING_LABEL, WINGS_FOR,
)
from installer.log import Log
from installer.tui import C, Back, flash, menu, render, text_prompt

STAGE_ROOT, STAGE_AIRCRAFT, STAGE_CONFIGURE, STAGE_RUN, STAGE_COMPLETE = range(5)

LOG: Log | None = None
DRY_RUN = False

# Cosmetic per-step reveal delay on the Perform screen (see screen_perform).
STEP_DELAY = 0.1


# ─── screen 1: launch ─────────────────────────────────────────────────────────
LOGO_ART = [
    "    T o L i s s  " + "=" * 20,
    "    ____  __          __",
    "   / __ \\/ /_  ____  / /_____  ____",
    "  / /_/ / __ \\/ __ \\/ __/ __ \\/ __ \\",
    " / ____/ / / / /_/ / /_/ /_/ / / / /",
    "/_/   /_/ /_/\\____/\\__/\\____/_/ /_/",
    "",
    f" {'=' * 25}  v{VERSION}",
]


def _launch_body() -> list[str]:
    """Two columns, aligned row-for-row: the cyan ASCII logo on the left and the
    welcome blurb on the right."""
    left_w = 48
    art = [f"{C.BR_CYAN}{l}{C.RESET}" if l else "" for l in LOGO_ART]
    blurb = [
        f"{C.BR_BLACK}ToLiss Photon v{VERSION}{C.RESET}",
        "",
        f"{C.BR_WHITE}Welcome to the ToLiss Photon Installer!{C.RESET}",
        "",
        f"{C.WHITE}This is a lighting modification program for{C.RESET}",
        f"{C.WHITE}ToLiss aircraft in X-Plane 12.{C.RESET}",
        "",
        tui._press_hint("any key", "continue…"),
    ]
    out = []
    for l, r in zip(art, blurb):
        pad = " " * max(1, left_w - tui._vis_len(l))
        out.append((l + pad + r) if r else l)
    # Horizontally center the two-column block: keep its own content width but
    # offset every row by a common left pad so the whole thing sits mid-terminal.
    # The columns aren't equal, so the midpoint is taken from the widest row.
    content_w = tui._dims()[0] - tui.BODY_INDENT
    block_w = max((tui._vis_len(l) for l in out), default=0)
    off = max(0, (content_w - block_w) // 2)
    if off:
        out = [(" " * off + l) if l else l for l in out]
    return out


def screen_launch():
    LOG.write("launch screen shown")
    render(None, _launch_body(), back="exit", cont="continue",
           cont_key="ANY KEY", center=True)
    if tui.read_key() == tui.KEY_ESC:  # called via the module, not imported by
        raise SystemExit(_quit())      # name, so tests can swap tui.read_key


# ─── screen 2: X-Plane directory ──────────────────────────────────────────────
STEP_XPLANE = "Step 1.  Where is X-Plane installed?"


def screen_xplane():
    roots = detect.xplane_candidates(LOG)
    if not roots:
        LOG.write("no X-Plane install auto-detected", "WARN")
    while True:
        options = [str(r) for r in roots] + ["Somewhere else…"]
        choice = menu(STAGE_ROOT, STEP_XPLANE,
                     "Please select your root X-Plane 12 folder:", options)
        if choice == len(roots):  # Somewhere else…
            try:
                manual = text_prompt(
                    STAGE_ROOT, STEP_XPLANE,
                    "Manually specify your X-Plane 12 folder below:")
            except Back:
                continue
            if not manual:
                continue
            p = Path(manual)
            if not detect._valid_root(p):
                flash(STAGE_ROOT, [
                    f"{C.RED}That doesn't look like an X-Plane 12 folder:{C.RESET} {manual}",
                    f"{C.DIM}Expected an Aircraft/ and a Resources/ folder inside it.{C.RESET}",
                ])
                LOG.write(f"manual X-Plane path rejected: {manual}", "WARN")
                continue
            LOG.write(f"selected X-Plane (manual): {p}")
            return p
        LOG.write(f"selected X-Plane: {roots[choice]}")
        return roots[choice]


# ─── screen 3: aircraft selection (table) ─────────────────────────────────────
_AC_TITLES = ("Folder Name", "Aircraft", "Photon Version")
_AC_GAP = 4  # spaces between columns


def _aircraft_cells(ac: dict) -> tuple[str, str, tuple[str, str]]:
    """Plain-text cell values for one row: (folder, aircraft, (photon_text,
    photon_color)). The Photon column is kept split so it can be measured by its
    visible text yet rendered in its status color."""
    c1 = ac["folder"]
    c2 = f"{ac['name']} (v{ac['ac_ver']})"
    if ac.get("stale"):
        c3 = (f"Needs reinstall (v{ac['photon']})", C.BR_YELLOW)
    elif ac["photon"]:
        tag = f"v{ac['photon']}" + (f" ({WING_LABEL.get(ac['wing'], ac['wing'])})"
                                    if ac["wing"] else "")
        c3 = (tag, C.BR_GREEN)
    else:
        c3 = ("Not installed", C.DIM)
    return c1, c2, c3


def _aircraft_columns(aircraft: list[dict]) -> tuple[int, int, int]:
    """Choose the three column widths. Preferred: every column equal to the
    widest column, with _AC_GAP spaces between them. If that overflows the
    terminal, drop back to natural per-column widths; if even those don't fit,
    squeeze/truncate the middle Aircraft column so the row still fits."""
    cells = [_aircraft_cells(ac) for ac in aircraft]
    w1 = max([len(_AC_TITLES[0])] + [len(c[0]) for c in cells])
    w2 = max([len(_AC_TITLES[1])] + [len(c[1]) for c in cells])
    w3 = max([len(_AC_TITLES[2])] + [len(c[2][0]) for c in cells])
    avail = tui._dims()[0] - tui.BODY_INDENT - tui.PREFIX
    colw = max(w1, w2, w3)
    if 3 * colw + 2 * _AC_GAP <= avail:
        return colw, colw, colw
    if w1 + w2 + w3 + 2 * _AC_GAP <= avail:
        return w1, w2, w3
    return w1, max(6, avail - w1 - w3 - 2 * _AC_GAP), w3


def _fit(text: str, width: int) -> str:
    """Left-justify to `width`, or truncate with an ellipsis if longer."""
    if len(text) <= width:
        return text.ljust(width)
    return text[:width - 1] + "…" if width >= 1 else ""


def _aircraft_header(cols: tuple[int, int, int]) -> list[str]:
    w1, w2, w3 = cols
    gap = " " * _AC_GAP
    titles = _fit(_AC_TITLES[0], w1) + gap + _fit(_AC_TITLES[1], w2) + gap + _AC_TITLES[2]
    rule_w = w1 + w2 + w3 + 2 * _AC_GAP
    pad = " " * tui.PREFIX
    return [f"{pad}{C.DIM}{titles}{C.RESET}",
            f"{pad}{C.BR_BLACK}{'▔' * rule_w}{C.RESET}"]


def _aircraft_option(ac: dict, cols: tuple[int, int, int]) -> str:
    w1, w2, _ = cols
    gap = " " * _AC_GAP
    c1, c2, (c3text, c3color) = _aircraft_cells(ac)
    return _fit(c1, w1) + gap + _fit(c2, w2) + gap + f"{c3color}{c3text}{C.RESET}"


def screen_aircraft(xplane_root: Path):
    aircraft = detect.detect_aircraft(xplane_root, LOG)
    if not aircraft:
        flash(STAGE_AIRCRAFT, [
            f"{C.RED}No compatible ToLiss aircraft found under{C.RESET}",
            f"{C.DIM}{xplane_root / 'Aircraft'}{C.RESET}"])
        raise Back
    cols = _aircraft_columns(aircraft)
    options = [_aircraft_option(ac, cols) for ac in aircraft]
    choice = menu(STAGE_AIRCRAFT, "Step 2.  Which aircraft do you wish to modify?",
                  "Select a supported aircraft:", options,
                  header_lines=_aircraft_header(cols))
    LOG.write(f"selected aircraft: {aircraft[choice]['folder']}")
    return aircraft[choice]


# ─── screen 4: aircraft action ────────────────────────────────────────────────
def _paren(label: str, note: str) -> str:
    """An option label with a dark-gray parenthetical note appended, so the
    incidental detail reads as secondary to the action itself."""
    return f"{label}  {C.BR_BLACK}({note}){C.RESET}"


def _action_label(ac: dict, wing: str) -> str:
    base = WING_ACTION_LABEL[wing]
    if not ac["photon"]:
        return base
    same_wing = ac["wing"] == wing or (wing == "stock" and ac["wing"] is None)
    if ac.get("stale"):
        # OBJ marker gone (an aircraft update reverted our edit) though the
        # manifest survived — the on-disk OBJ is stock, so this is a fresh redo.
        if same_wing:
            return _paren(base, "reinstall — update reverted it")
        return _paren(base, f"switch from {WING_LABEL[ac['wing'] or 'stock']}")
    if same_wing:
        if ac["photon"] != VERSION:
            return _paren("Reinstall / Upgrade", f"v{ac['photon']} → v{VERSION}")
        return _paren("Reinstall", f"already v{VERSION}")
    return _paren(base, f"switch from {WING_LABEL[ac['wing'] or 'stock']}")


def screen_action(ac: dict):
    step = (f"Step 3.  What would you like to do to {ac['folder']} "
           f"({ac['name']} v{ac['ac_ver']})?")
    wings = WINGS_FOR[ac["airframe"]]
    keys = list(wings) + ["uninstall"]
    options, disabled = [], set()
    for i, key in enumerate(keys):
        if key == "uninstall":
            label = "Uninstall"
            if not ac["photon"]:
                options.append(_paren(label, "nothing installed"))
                disabled.add(i)
                continue
            options.append(label)
        else:
            options.append(_action_label(ac, key))
    # When Photon is already installed, default the highlight to the currently-
    # installed wing variant (e.g. Durantula → its own option) so a reinstall
    # lands on the obvious choice; a fresh install defaults to the first option.
    default_idx = 0
    if ac["photon"]:
        w = ac["wing"] or "stock"
        if w in wings:
            default_idx = wings.index(w)
    choice = menu(STAGE_CONFIGURE, step,
                  f"Select an action for ToLiss Photon v{VERSION}:", options,
                  disabled=disabled, index=default_idx)
    action = keys[choice]
    LOG.write(f"action chosen: {action} on {ac['folder']}")
    return action


# ─── X-Plane-running guard ─────────────────────────────────────────────────────
POLL_INTERVAL_SECONDS = 2.0  # how often the wait screen rechecks for X-Plane


def screen_check_not_running(ac: dict, action: str, xplane_root: Path):
    """Hold before performing the run until X-Plane is closed, then proceed on
    its own — but only when this run needs to replace the loaded plugin binary.
    Overwriting the aircraft OBJs mid-session is safe (X-Plane loads them into
    memory and releases the files); a loaded `.xpl` is memory-mapped and locked,
    so it's the one artifact we can't swap while the sim is up. If the installed
    plugin is already byte-current (an OBJ-only reinstall/upgrade), install()
    won't touch it at all, so this waits for nothing and returns immediately.

    While X-Plane is up the screen rechecks every POLL_INTERVAL_SECONDS and, the
    instant it closes, returns automatically. ESC goes back a step (re-choose the
    action); ENTER is an explicit 'continue anyway' escape hatch."""
    if action != "uninstall" and actions.plugin_is_current(xplane_root):
        if detect.xplane_running():
            LOG.write("X-Plane running, but the plugin is already current — this "
                      "run only rewrites OBJs (safe while loaded); skipping wait")
        return

    warned = False
    tick = 0
    while detect.xplane_running():
        if not warned:
            LOG.write("X-Plane appears to be running — waiting for it to close", "WARN")
            warned = True
        dots = "." * (1 + tick % 3)
        body = [
            "",
            tui._step("Step 3b.  Please close X-Plane to continue"),
            "",
            f"{C.YELLOW}{C.BOLD}X-Plane appears to be running.{C.RESET}",
            "",
            f"{C.BR_WHITE}This install adds or updates the ToLiss Photon plugin, which"
            f" X-Plane loads at startup — X-Plane keeps that file locked while it's"
            f" open, so the install can't complete until it's closed.{C.RESET}",
            "",
            f"{C.BR_WHITE}Close X-Plane and the installer will continue automatically."
            f"{C.RESET}",
            "",
            f"{C.DIM}Waiting for X-Plane to close{dots}{C.RESET}",
        ]
        render(STAGE_RUN, body, back="go back", cont="continue anyway",
               cont_key="ENTER", focus=len(body) - 1)
        k = tui.read_key_timeout(POLL_INTERVAL_SECONDS)
        if k == tui.KEY_ESC:
            LOG.write("user backed out of the X-Plane-running wait")
            raise Back
        if k == tui.KEY_ENTER:
            LOG.write("user chose to continue with X-Plane apparently running", "WARN")
            return
        tick += 1
    if warned:
        LOG.write("X-Plane closed — installer continuing automatically")


# ─── uninstall: shared-plugin disposition ─────────────────────────────────────
def screen_plugin_disposition(ac: dict, xplane_root: Path) -> bool:
    """Whether to also remove the shared plugin on uninstall. Left installed by
    default (other airframes may use it); only offered when this is the last
    airframe with Photon on it."""
    if detect.any_other_airframe_has_photon(xplane_root, ac["path"]):
        LOG.write("other airframes still have Photon — leaving shared plugin in place")
        return False
    options = ["Leave the plugin installed (recommended)", "Remove it too"]
    choice = menu(STAGE_CONFIGURE, "Step 3c.  No other aircraft use ToLiss Photon",
                 "The ToLiss Photon plugin is shared across aircraft. Remove it too?",
                 options)
    return choice == 1


# ─── screen 5: perform (real writes) ──────────────────────────────────────────
def screen_perform(ac: dict, action: str, xplane_root: Path):
    verb = "Uninstalling…" if action == "uninstall" else "Installing…"
    title = tui._step("Step 4.  " + verb)
    render(STAGE_RUN, ["", title, "", f"  {C.DIM}Working…{C.RESET}"])

    error = None
    steps: list[str] = []
    try:
        if action == "uninstall":
            remove_plugin = screen_plugin_disposition(ac, xplane_root)
            steps = actions.uninstall(ac, xplane_root, LOG, dry_run=DRY_RUN,
                                      remove_plugin=remove_plugin)
        else:
            steps = actions.install(ac, action, xplane_root, LOG, dry_run=DRY_RUN)
    except actions.ActionError as e:
        error = str(e)
        LOG.write(f"ACTION FAILED: {e}", "ERROR")
    except OSError as e:
        error = f"unexpected filesystem error: {e}"
        LOG.write(f"ACTION FAILED: {e}", "ERROR")

    # Reveal each completed step one at a time so the run reads as a deliberate,
    # step-by-step process instead of an instant flash. Purely cosmetic — the
    # filesystem work already finished above; this just paces the display.
    lines = [""]
    for s in steps:
        marker = f"{C.YELLOW}!{C.RESET}" if s.startswith("!") else f"{C.BR_GREEN}✓{C.RESET}"
        lines.append(f"  {marker} {s.lstrip('! ')}")
        render(STAGE_RUN, ["", title, *lines])
        if not DRY_RUN:
            time.sleep(STEP_DELAY)

    if error:
        lines += ["", f"  {C.RED}{C.BOLD}Failed: {error}{C.RESET}"]
    else:
        summary = ("Uninstall complete — original ToLiss lighting restored."
                   if action == "uninstall" else
                   f"Install complete — {ac['name']} now uses ToLiss Photon v{VERSION}.")
        lines += ["", f"  {C.BR_GREEN}{summary}{C.RESET}"]
        if action == "uninstall":
            lines += [
                "",
                f"  {C.DIM}Tip: if any lighting issues linger, reinstall the aircraft"
                f" with the{C.RESET}",
                f"  {C.DIM}SkunkCrafts updater — it will redownload and replace all"
                f" modified files.{C.RESET}",
            ]
    if DRY_RUN:
        lines.append(f"  {C.YELLOW}(--dry-run — no files were actually modified){C.RESET}")

    render(STAGE_RUN, ["", title, *lines], cont="continue", focus=len(lines) + 2)
    while tui.read_key() != tui.KEY_ENTER:
        pass
    return error is None


# ─── screen 6: complete ───────────────────────────────────────────────────────
def _complete_summary(ac: dict, action: str, success: bool) -> str:
    """One-line recap of what just happened, e.g. 'ToLiss Photon v0.4
    (Durantula) installed in ToLiss A321.'"""
    if not success:
        verb = "Uninstall" if action == "uninstall" else "Installation"
        return f"{C.YELLOW}{verb} did not complete — see the installer log for details.{C.RESET}"
    if action == "uninstall":
        return (f"{C.BR_WHITE}ToLiss Photon removed from {ac['name']} — original"
                f" lighting restored.{C.RESET}")
    return (f"{C.BR_WHITE}ToLiss Photon v{VERSION} ({WING_LABEL[action]}) installed in"
            f" {ac['name']}.{C.RESET}")


def screen_complete(ac: dict, action: str, success: bool):
    if not success:
        verb = "Uninstall" if action == "uninstall" else "Installation"
        title = f"Step 5.  {verb} failed"
    elif action == "uninstall":
        title = "Step 5.  Uninstall complete"
    else:
        title = "Step 5.  Installation complete"
    summary = _complete_summary(ac, action, success)
    options = ["Exit installer", "Modify another aircraft", "Open installer log",
              "Find ToLiss Photon online"]
    while True:
        try:
            choice = menu(STAGE_COMPLETE, title, "",
                         header_lines=[summary, "", "What would you like to do next:", ""],
                         options=options)
        except Back:
            return "restart"
        if choice == 0:
            return "exit"
        if choice == 1:
            return "again"
        if choice == 2:
            err = LOG.open_in_editor()
            flash(STAGE_COMPLETE, [
                f"{C.RED}Could not open log: {err}{C.RESET}" if err
                else f"{C.GREEN}Opened installer log:{C.RESET}",
                f"{C.DIM}{LOG.path}{C.RESET}"])
        elif choice == 3:
            webbrowser.open(PHOTON_URL)
            flash(STAGE_COMPLETE, [f"{C.GREEN}Opened ToLiss Photon's page in your browser.{C.RESET}"])


# ─── exit ─────────────────────────────────────────────────────────────────────
def _quit():
    tui.show_cursor()
    tui.leave_alt_screen()
    print(f"{C.DIM}ToLiss Photon installer closed.{C.RESET}")
    print(f"{C.DIM}Log saved to: {LOG.path}{C.RESET}")
    return 0


# ─── flow orchestration (state machine) ───────────────────────────────────────
def main():
    global LOG, DRY_RUN
    ap = argparse.ArgumentParser(description="ToLiss Photon Lighting installer")
    ap.add_argument("--dry-run", action="store_true",
                    help="log every action but perform no filesystem writes")
    ap.add_argument("--xplane-root", help="skip auto-detection and use this X-Plane 12 folder")
    args = ap.parse_args()
    DRY_RUN = args.dry_run
    LOG = Log(VERSION, dry_run=DRY_RUN)

    if payload.available():
        LOG.write(f"payload mode: {payload.mode()}"
                  + (f" (building from repo at {payload.ROOT})"
                     if payload.mode() == "dev" else ""))
    else:
        LOG.write(f"no installable payload found (looked in {payload.PAYLOAD_DIR}); "
                  f"install/uninstall will fail until a release is built", "ERROR")

    tui.enable_vt()
    tui.enter_alt_screen()
    tui.hide_cursor()
    try:
        screen_launch()
        xplane_root = Path(args.xplane_root) if args.xplane_root else None
        while True:
            try:
                if xplane_root is None:
                    xplane_root = screen_xplane()
                ac = screen_aircraft(xplane_root)
                while True:
                    action = screen_action(ac)
                    try:
                        screen_check_not_running(ac, action, xplane_root)
                    except Back:
                        continue  # backed out of the close-X-Plane wait -> reconfigure
                    break
                success = screen_perform(ac, action, xplane_root)
                nxt = screen_complete(ac, action, success)
                if nxt == "exit":
                    break
                if nxt == "restart":
                    xplane_root = None
                # "again" -> back to aircraft selection with the same root
            except Back:
                if xplane_root is not None:
                    xplane_root = None
                    continue
                break
        raise SystemExit(_quit())
    except KeyboardInterrupt:
        raise SystemExit(_quit())
    finally:
        tui.show_cursor()
        tui.leave_alt_screen()


if __name__ == "__main__":
    main()
