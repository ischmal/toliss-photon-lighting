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
import threading
import time
import webbrowser
from pathlib import Path

from installer import actions, config, detect, payload
from installer import tui
from installer.constants import (
    AIRFRAMES, GITHUB_URL, INTERIOR_AIRFRAMES, INTERIOR_ORG_URL, VERSION,
    WING_ACTION_LABEL, WING_LABEL, WINGS_FOR, XPLANE_ORG_URL,
)
from installer.log import Log
from installer.tui import C, Back, flash, menu, render, text_prompt

STAGE_ROOT, STAGE_AIRCRAFT, STAGE_CONFIGURE, STAGE_RUN, STAGE_COMPLETE = range(5)

LOG: Log | None = None
DRY_RUN = False

# Cosmetic per-step reveal delay on the Perform screen (see screen_perform).
STEP_DELAY = 0.1
# Cadence of the animated "Working…" ellipsis while the (blocking) filesystem
# work runs on a worker thread — short enough to read as live, not frozen.
WORKING_ANIM_INTERVAL = 0.3


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


def _clean_manual_path(raw: str) -> Path:
    """Undo the quoting a drag-and-drop of a folder onto the console window
    often adds (Windows/macOS both wrap a dropped path that contains spaces in
    double quotes; some shells use single quotes instead) before turning it
    into a Path — otherwise the literal quote characters end up part of the
    path and it never validates."""
    s = raw.strip()
    if len(s) >= 2 and s[0] == s[-1] and s[0] in ("\"", "'"):
        s = s[1:-1].strip()
    return Path(s)


def screen_xplane():
    roots = detect.xplane_candidates(LOG)

    remembered = config.load_saved_root()
    if remembered is not None:
        try:
            remembered = remembered.resolve()
        except OSError:
            pass
        if detect._valid_root(remembered):
            LOG.write(f"remembered X-Plane root still valid: {remembered}")
            if remembered not in roots:
                roots = [remembered] + roots
        else:
            LOG.write(f"remembered X-Plane root no longer valid, discarding: "
                      f"{remembered}", "WARN")
            config.clear_saved_root()
            remembered = None

    if not roots:
        LOG.write("no X-Plane install auto-detected", "WARN")
    default_idx = roots.index(remembered) if remembered in roots else 0

    while True:
        labels = [str(r) + (f"  {C.DIM}(last used){C.RESET}" if r == remembered else "")
                 for r in roots]
        options = labels + ["Somewhere else…"]
        choice = menu(STAGE_ROOT, STEP_XPLANE,
                     "Please select your root X-Plane 12 folder:", options,
                     index=default_idx)
        if choice == len(roots):  # Somewhere else…
            manual = ""
            error = None
            while True:
                try:
                    manual = text_prompt(
                        STAGE_ROOT, STEP_XPLANE,
                        "Manually specify your X-Plane 12 folder below:",
                        initial=manual, error=error)
                except Back:
                    break  # back to the root menu, same options
                if not manual:
                    error = None
                    continue
                p = _clean_manual_path(manual)
                if not detect._valid_root(p):
                    LOG.write(f"manual X-Plane path rejected: {manual}", "WARN")
                    error = (f"'{p}' doesn't look like an X-Plane 12 folder — "
                            f"expected an Aircraft/ and a Resources/ folder inside it.")
                    continue
                LOG.write(f"selected X-Plane (manual): {p}")
                config.save_root(p)
                return p
            continue
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
        wing_note = (f" ({WING_LABEL.get(ac['wing'], ac['wing'])})"
                    if ac["wing"] else "")
        if ac["photon"] != VERSION:
            c3 = (f"Update available (v{ac['photon']} → v{VERSION}){wing_note}",
                  C.BR_CYAN)
        else:
            c3 = (f"v{ac['photon']}{wing_note}", C.BR_GREEN)
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
    while True:
        aircraft = detect.detect_aircraft(xplane_root, LOG)
        if not aircraft:
            flash(STAGE_AIRCRAFT, [
                f"{C.RED}No compatible ToLiss aircraft found under{C.RESET}",
                f"{C.DIM}{xplane_root / 'Aircraft'}{C.RESET}"])
            raise Back
        cols = _aircraft_columns(aircraft)
        options = [_aircraft_option(ac, cols) for ac in aircraft]
        options.append(f"{C.BR_BLACK}Refresh list…{C.RESET}")
        choice = menu(STAGE_AIRCRAFT, "Step 2.  Which aircraft do you wish to modify?",
                      "Select a supported aircraft:", options,
                      header_lines=_aircraft_header(cols))
        if choice == len(aircraft):  # Refresh list… -> redo detection from scratch
            LOG.write("user requested aircraft list refresh")
            continue
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
    # Render feedback BEFORE the (occasionally slow, e.g. on a network drive)
    # byte-for-byte plugin comparison below — otherwise the previous screen
    # sits frozen with no feedback while it runs, and looks like a hang.
    render(STAGE_RUN, ["", tui._step("Step 3b.  Checking installed plugin…"), "",
                       f"{C.DIM}Comparing the installed plugin against this release…{C.RESET}"])
    if action != "uninstall" and actions.plugin_is_current(xplane_root):
        # No process check here on purpose: it would only refine a log message,
        # and spawning tasklist/ps costs ~0.5-3 s — the bulk of what made this
        # screen feel slow. Whether or not X-Plane is up, this run only rewrites
        # OBJs (safe while loaded), so the wait is skipped either way.
        LOG.write("plugin already current — this run only rewrites OBJs (safe "
                  "even while X-Plane is running); skipping the close-X-Plane wait")
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
def screen_interior(ac: dict) -> bool:
    """Opt in to the interior ("Gus Mod") cockpit lighting — a separate axis from
    the exterior mod, so a user who wants only the exterior can decline (§5.1).

    Skipped entirely when the airframe is out of scope (the A330-900) or this
    build has no interior payload, rather than offering a step that would fail.
    Installing is the recommended choice; the third option opens Gus's own .org
    page for this airframe and returns here rather than answering."""
    if ac["airframe"] not in INTERIOR_AIRFRAMES or not payload.interior_available():
        LOG.write(f"interior mod not offered for {ac['airframe']} "
                  f"(unsupported airframe or no payload)")
        return False
    options = [
        "Yes (recommended)",
        "No, install exterior lighting only",
        "Open X-Plane.org page…",
    ]
    body = (
        "ToLiss Photon includes Light Mod by Gus Rodrigues, offering improved "
        "cockpit lighting textures and three interior light profiles: "
        "Halogen (Old), Halogen (New), and LED."
    )
    idx, note = 0, None
    while True:
        choice = menu(STAGE_CONFIGURE,
                      "Step 3d.  (Optional) Install enhanced cockpit lighting?",
                      body, options, index=idx,
                      footer_lines=(["", note] if note else None))
        if choice == 2:
            url = INTERIOR_ORG_URL[ac["airframe"]]
            webbrowser.open(url)
            LOG.write(f"opened the Light Mod page for {ac['airframe']}: {url}")
            idx, note = 2, f"{C.BR_BLACK}Opened {url} in your browser.{C.RESET}"
            continue
        LOG.write(f"interior opt-in: {choice == 0}")
        return choice == 0


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

    # Any interactive prompt must run on the main thread BEFORE the spinner owns
    # it (the animation loop below reads no keys). screen_plugin_disposition may
    # raise Back — let it propagate to the flow, unchanged.
    remove_plugin = False
    want_interior = False
    if action == "uninstall":
        remove_plugin = screen_plugin_disposition(ac, xplane_root)
    else:
        want_interior = screen_interior(ac)

    # The real filesystem work is synchronous and can take a beat (backing up
    # OBJs, RealWings/glow patching, plugin copy), during which a static
    # "Working…" looks hung. Run it on a worker thread and animate the ellipsis
    # on the main thread so the screen visibly stays alive.
    result: dict = {}
    # Written by the worker, read by the render loop below. A plain dict
    # assignment is atomic enough for this — the reader only ever displays the
    # latest value and never acts on it, so a torn read is impossible and a
    # stale one is invisible.
    progress: dict = {}

    def _on_progress(done, total, name):
        progress["text"] = f"copying textures {done}/{total} — {name}"

    def _work():
        try:
            if action == "uninstall":
                result["steps"] = actions.uninstall(
                    ac, xplane_root, LOG, dry_run=DRY_RUN, remove_plugin=remove_plugin)
            else:
                result["steps"] = actions.install(
                    ac, action, xplane_root, LOG, dry_run=DRY_RUN,
                    interior=want_interior, progress=_on_progress)
        except actions.ActionError as e:
            result["error"] = str(e)
            LOG.write(f"ACTION FAILED: {e}", "ERROR")
        except OSError as e:
            result["error"] = f"unexpected filesystem error: {e}"
            LOG.write(f"ACTION FAILED: {e}", "ERROR")
        except BaseException as e:  # never let the worker vanish silently — that
            result["exc"] = e       # would render as a (false) success; re-raised below

    worker = threading.Thread(target=_work, daemon=True)
    worker.start()
    tick = 0
    while True:  # renders at least once, then animates until the work finishes
        dots = ("." * (1 + tick % 3)).ljust(3)
        # The interior step copies ~57 MiB of textures; naming the current file
        # is what keeps that from reading as a hang (interior_plan.md §5.3).
        note = progress.get("text")
        body = ["", title, "", f"  {C.DIM}Working{dots}{C.RESET}"]
        if note:
            body += ["", f"  {C.BR_BLACK}{note}{C.RESET}"]
        render(STAGE_RUN, body)
        worker.join(WORKING_ANIM_INTERVAL)  # wakes early the instant work finishes
        if not worker.is_alive():
            break
        tick += 1

    if "exc" in result:
        raise result["exc"]  # unexpected error: propagate as before, no false success
    steps: list[str] = result.get("steps", [])
    error = result.get("error")

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
                   f"Install complete — {ac['name']} now uses ToLiss Photon"
                   f" v{VERSION} ({_variant_label(action, want_interior)}).")
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
    return error is None, want_interior


# ─── screen 6: complete ───────────────────────────────────────────────────────
def _variant_label(action: str, interior: bool) -> str:
    """What was installed, in one parenthetical: the wing variant plus the cockpit
    axis when opted into — 'Default', 'Default + Cockpit', 'Durantula + Cockpit'.
    The two axes install independently, so the label names both; the exterior-only
    wording is unchanged."""
    return WING_LABEL[action] + (" + Cockpit" if interior else "")


def _complete_summary(ac: dict, action: str, success: bool,
                      interior: bool = False) -> str:
    """One-line recap of what just happened, e.g. 'ToLiss Photon v0.4
    (Durantula + Cockpit) installed in ToLiss A321.'"""
    if not success:
        verb = "Uninstall" if action == "uninstall" else "Installation"
        return f"{C.YELLOW}{verb} did not complete — see the installer log for details.{C.RESET}"
    if action == "uninstall":
        return (f"{C.BR_WHITE}ToLiss Photon removed from {ac['name']} — original"
                f" lighting restored.{C.RESET}")
    return (f"{C.BR_WHITE}ToLiss Photon v{VERSION} ({_variant_label(action, interior)})"
            f" installed in {ac['name']}.{C.RESET}")


def screen_complete(ac: dict, action: str, success: bool, interior: bool = False):
    if not success:
        verb = "Uninstall" if action == "uninstall" else "Installation"
        title = f"Step 5.  {verb} failed"
    elif action == "uninstall":
        title = "Step 5.  Uninstall complete"
    else:
        title = "Step 5.  Installation complete"
    summary = _complete_summary(ac, action, success, interior)
    options = ["Exit installer", "Modify another aircraft", "View installer log…",
              "Open X-Plane.org Page…", "Open GitHub Project…"]
    note = None
    while True:
        # The install/uninstall this screen reports on already happened — there
        # is no "previous step" to rewind to, so ESC/back is disabled here
        # rather than restarting the whole installer (unlike every other
        # screen, which only steps back one stage).
        choice = menu(STAGE_COMPLETE, title, "",
                     header_lines=[summary, "", "What would you like to do next:", ""],
                     footer_lines=(["", note] if note else None),
                     options=options, allow_back=False)
        note = None
        if choice == 0:
            return "exit"
        if choice == 1:
            return "again"
        if choice == 2:
            err = LOG.open_in_editor()
            note = (f"{C.RED}Could not view installer log: {err}{C.RESET}" if err
                   else f"{C.BR_BLACK}Viewed installer log: {LOG.path}{C.RESET}")
        elif choice == 3:
            webbrowser.open(XPLANE_ORG_URL)
            note = f"{C.BR_BLACK}Opened the X-Plane.org page in your browser.{C.RESET}"
        elif choice == 4:
            webbrowser.open(GITHUB_URL)
            note = f"{C.BR_BLACK}Opened the GitHub project in your browser.{C.RESET}"


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
            # ─ Step 1: X-Plane root ─ ESC here exits (there's no earlier real
            # step to fall back to — the launch screen already had its own
            # ESC-exits-immediately handling).
            if xplane_root is None:
                try:
                    xplane_root = screen_xplane()
                except Back:
                    break

            # ─ Step 2: aircraft selection ─ ESC here is the ONE place that
            # legitimately steps all the way back to Step 1 (a different root
            # may have different aircraft under it).
            try:
                ac = screen_aircraft(xplane_root)
            except Back:
                xplane_root = None
                continue

            # ─ Step 3+: configure / run / complete for this aircraft ─ every
            # ESC from here only steps back ONE stage, never back to Step 1.
            while True:
                try:
                    action = screen_action(ac)
                except Back:
                    break  # -> Step 2 (aircraft selection), same root
                try:
                    screen_check_not_running(ac, action, xplane_root)
                except Back:
                    continue  # -> re-choose the action, still Step 3
                try:
                    success, interior = screen_perform(ac, action, xplane_root)
                except Back:
                    continue  # backed out of a sub-prompt (e.g. uninstall
                              # plugin disposition) -> re-choose the action
                nxt = screen_complete(ac, action, success, interior)
                if nxt == "exit":
                    raise SystemExit(_quit())
                break  # "again" -> Step 2 (aircraft selection), same root
        raise SystemExit(_quit())
    except KeyboardInterrupt:
        raise SystemExit(_quit())
    finally:
        tui.show_cursor()
        tui.leave_alt_screen()


if __name__ == "__main__":
    main()
