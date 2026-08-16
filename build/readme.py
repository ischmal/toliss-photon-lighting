#!/usr/bin/env python3
"""Shared README.txt generator for the release bundles.

One source of truth for the per-bundle READMEs so the three downloads
(Windows / macOS / Linux) stay consistent. Reuses the installer's own splash
ASCII art (lifted from `LogoArt()` in the installer's own screens.cpp) and the
project links from build/constants.py, so nothing drifts.

⚠ ONE INSTALLER, TWO BUILDS, AND THE INSTALL STEPS NAME EXACTLY ONE OF THEM
(2026-08-15). The bundle carries the GUI (`photon-installer`) and the console
build (`photon-installer-console`), because the GUI renders through OpenGL and on
a broken-GL machine (Remote Desktop, old drivers, VMs) it opens as a BLACK WINDOW
that can explain nothing. The old rule — "a README that has to explain which of
two installers to run is a README describing a choice the user should never have
been handed" — still shapes the text: HOW TO INSTALL names only the GUI, and the
console version appears under TROUBLESHOOTING, where the reader has already hit
the one problem it exists for. (The "Python" second executable stays gone;
docs/installer_cpp_plan.md §10 decision 1.)

Kept pure-ASCII on purpose: a README.txt gets opened in Notepad and every other
editor, so no smart quotes / arrows / dashes that can mojibake.

    render("Windows", exe_name="photon-installer.exe", arch="win_x64",
           console_name="photon-installer-console.exe")
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
if str(REPO / "build") not in sys.path:
    sys.path.insert(0, str(REPO / "build"))
from constants import GITHUB_URL, VERSION, XPLANE_ORG_URL  # noqa: E402

# Where the installer's own splash art lives, so the README opens with the banner
# the user sees on launch.
#
# ⚠ IT IS READ OUT OF THE INSTALLER'S SOURCE, NOT COPIED. This used to
# `import install` and read `install.LOGO_ART`; that file is gone, and pasting the
# art in here would leave two copies whose only symptom on drifting is a README
# whose banner is not the program's — invisible to every test.
SCREENS_CPP = REPO / "src" / "native" / "src" / "installer" / "screens.cpp"
_ART_LINE = re.compile(r'^\s*R"\((.*)\)",\s*$')

# platform.system() -> the pretty OS name used in filenames and READMEs.
OS_NAME = {"Windows": "Windows", "Darwin": "macOS", "Linux": "Linux"}
AIRCRAFT = "ToLiss A319, A320, A321, and A330-900"
AIRCRAFT_OR = "ToLiss A319, A320, A321, or A330-900"


def _logo() -> list[str]:
    """The installer's splash art, lifted from `LogoArt()` in screens.cpp so the
    README opens with the same banner the program does.

    The art is a run of raw string literals inside one `kArt` initializer; the
    trailing version line is built in C++ from `kPhotonVersion` and is appended
    here instead. A parse failure degrades to a plain title rather than raising —
    a bundle must not fail to build over its README's decoration."""
    try:
        text = SCREENS_CPP.read_text(encoding="utf-8", errors="replace")
        start = text.index("LogoArt()")
        block = text[start:text.index("return kArt;", start)]
        art = [m.group(1) for m in
               (_ART_LINE.match(l) for l in block.splitlines()) if m]
        if not art:
            raise ValueError("no raw-string art lines found")
        return art + ["", f" {'=' * 25}  v{VERSION}"]
    except Exception:
        return [f"ToLiss Photon Lighting  -  v{VERSION}"]


def _links() -> list[str]:
    return [
        "LINKS",
        f"  Project & source code :  {GITHUB_URL}",
        f"  X-Plane.org forum     :  {XPLANE_ORG_URL}",
    ]


def _install_steps(kind: str, exe_name: str) -> list[str]:
    if kind == "Windows":
        return [
            "HOW TO INSTALL",
            "  1. Extract this ENTIRE folder from the zip. Keep the installer and the",
            "     data/ folder together - the installer reads data/ from beside itself.",
            f"  2. Double-click  {exe_name}",
            "       If Windows SmartScreen appears (the app is not code-signed):",
            "       click  More info  >  Run anyway.",
            "  3. Follow the on-screen steps. It auto-detects X-Plane 12; pick",
            "     your aircraft and wing variant.",
        ]
    if kind == "macOS":
        return [
            "HOW TO INSTALL",
            "  1. Extract this ENTIRE folder from the zip. Keep the installer and the",
            "     data/ folder together - the installer reads data/ from beside itself.",
            f"  2. Open a Terminal in this folder and run:   ./{exe_name}",
            f"       (Double-clicking {exe_name} in Finder also works - it opens in",
            "        Terminal.) The app is not notarized, so if Gatekeeper blocks it:",
            f"        right-click > Open, or run:  xattr -d com.apple.quarantine {exe_name}",
            "  3. Follow the on-screen steps. It auto-detects X-Plane 12; pick",
            "     your aircraft and wing variant.",
        ]
    if kind == "Linux":
        return [
            "HOW TO INSTALL",
            "  1. Extract this ENTIRE folder from the archive. Keep the installer and",
            "     the data/ folder together - the installer reads data/ from beside it.",
            f"  2. Open a terminal in this folder and run:   ./{exe_name}",
            f"       (If it is not executable:  chmod +x {exe_name})",
            "  3. Follow the on-screen steps. It auto-detects X-Plane 12; pick",
            "     your aircraft and wing variant.",
        ]
    raise ValueError(f"unknown bundle kind {kind!r} - expected Windows/macOS/Linux")


def _contents(kind: str, exe_name: str, arch: str | None,
              console_name: str | None) -> list[str]:
    """⚠ THIS LIST IS A CONTRACT WITH THE BUNDLE, not prose. It described a
    `dsl/` folder for one release after that folder stopped being staged (the
    installer no longer needs the toolchain at install time), which reads to a
    user as a download missing a piece. It then described a second installer for
    the length of the C++ transition. Change it in the same commit as
    make_release.build_bundle whenever the layout moves."""
    out = [
        "WHAT'S INCLUDED",
        f"  {exe_name}   the installer (self-contained - no Python needed)",
    ]
    if console_name:
        out.append(f"  {console_name}   the same installer as a console")
        out.append("               (text-mode) program - see TROUBLESHOOTING")
    out += [
        "  data/        the files it installs:",
        "    objs/      pre-built light objects (wing variants x airframes)",
        "    textures/  cockpit textures for the interior mod (by Gus)",
        f"    plugin/    the native X-Plane plugin (this build: {arch or 'this platform'})",
        "    plugindata/  the plugin's own runtime data (panel effects)",
    ]
    return out


def _troubleshooting(kind: str, exe_name: str, console_name: str | None) -> list[str]:
    """The black-window section — the reason the console build ships at all.

    ⚠ IT NAMES THE SYMPTOM FIRST. A user staring at a black window searches the
    README for the word "black", not for "renderer" or "console"."""
    run = console_name if kind == "Windows" else f"./{console_name}"
    sw = exe_name if kind == "Windows" else f"./{exe_name}"
    out = [
        "TROUBLESHOOTING",
        "  If the installer opens as a BLACK OR BLANK WINDOW (this can happen over",
        "  Remote Desktop, in a VM, or with old graphics drivers - the window is",
        "  drawn by the GPU and some setups cannot do that):",
    ]
    if console_name:
        out += [
            f"    * Run  {run}",
            "      instead. It is the SAME installer as a text-mode program in a",
            "      console window - same steps, same result.",
        ]
    out += [
        f"    * Or run  {sw} --software",
        "      from a terminal, which draws the normal installer window on the",
        "      CPU instead of the GPU.",
    ]
    return out


def render(kind: str, *, exe_name: str | None = None, arch: str | None = None,
           console_name: str | None = None) -> str:
    """Full README.txt text for one bundle. `kind` in {Windows, macOS, Linux}."""
    exe_name = exe_name or ""
    lines: list[str] = []
    lines += _logo()
    lines += [
        "",
        "=" * 74,
        "",
        "WHAT THIS IS",
        f"  A lighting modification for {AIRCRAFT} aircraft in X-Plane 12.",
        "  It reshapes the exterior beacon/strobe flash (crisp LED vs. halogen/xenon)",
        "  and swaps the exterior light colors between an LED and the original look,",
        "  per light category. You pick a profile from the plugin's menu in-sim; the",
        "  choice is saved per livery.",
        "",
        "  It can also, optionally, re-light the COCKPIT - dome, map, pedestal, table",
        "  and console lights - switchable between old halogen, new halogen and LED",
        "  from the same menu. The interior lighting is the work of GUS, used with",
        "  permission; see CREDIT below. The installer asks before installing it, and",
        "  it is easy to decline if you only want the exterior mod.",
        "  (A330-900: exterior only.)",
        "",
    ]
    lines += _install_steps(kind, exe_name)
    lines += [
        "",
        "  Close X-Plane before installing.",
        "",
    ]
    lines += _troubleshooting(kind, exe_name, console_name)
    lines += [""]
    lines += _contents(kind, exe_name, arch, console_name)
    lines += [
        "",
        "REQUIREMENTS",
        f"  X-Plane 12 with a {AIRCRAFT_OR} aircraft already installed.",
        "",
        "UNINSTALL",
        "  Re-run the installer and choose Uninstall for the aircraft; it restores",
        "  the original ToLiss files byte-for-byte - including the aircraft's .acf",
        "  files and any livery textures the interior install had to move aside.",
        "",
        "CREDIT",
        "  The interior (cockpit) lighting - its design, placement, intensities and",
        "  the three color looks - is the work of GUS, used with permission. Photon",
        "  only folds his three separate variant files into one that can be switched",
        "  in-sim. All credit for how the cockpit looks belongs to him.",
        "",
        "  NOTE: if you also install Gus's original zip BY HAND, do it BEFORE running",
        "  this installer, not after. His package includes its own exterior light",
        "  object which would overwrite Photon's and silently revert the exterior mod.",
        "",
    ]
    lines += _links()
    lines += [""]
    return "\n".join(lines) + "\n"


if __name__ == "__main__":  # quick manual preview: python build/readme.py [kind]
    k = sys.argv[1] if len(sys.argv) > 1 else "Windows"
    ext = ".exe" if k == "Windows" else ""
    txt = render(k, exe_name="photon-installer" + ext, arch="win_x64",
                 console_name="photon-installer-console" + ext)
    sys.stdout.buffer.write(txt.encode("utf-8"))
