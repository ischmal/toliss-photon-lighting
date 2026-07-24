#!/usr/bin/env python3
"""Shared README.txt generator for the release bundles.

One source of truth for the per-bundle READMEs so the four downloads
(Windows / macOS / Linux frozen bundles + the Python-Universal bundle) stay
consistent. Reuses the installer's own splash ASCII art (install.LOGO_ART) and
the project links from installer.constants, so nothing drifts.

Kept pure-ASCII on purpose: a README.txt gets opened in Notepad and every other
editor, so no smart quotes / arrows / dashes that can mojibake.

    render("Windows", exe_name="...-Windows.exe", arch="win_x64")
    render("Python")
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))
from installer.constants import GITHUB_URL, VERSION, XPLANE_ORG_URL  # noqa: E402

# platform.system() -> the pretty OS name used in filenames and READMEs.
OS_NAME = {"Windows": "Windows", "Darwin": "macOS", "Linux": "Linux"}
AIRCRAFT = "ToLiss A319, A320, A321, and A330-900"
AIRCRAFT_OR = "ToLiss A319, A320, A321, or A330-900"


def _logo() -> list[str]:
    """The installer's splash art, so the README opens with the same banner the
    user sees on launch. Imported (not duplicated) to stay in sync."""
    try:
        import install  # repo-root install.py
        return list(install.LOGO_ART)
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
            "  3. Follow the on-screen steps (arrow keys / ENTER; ESC goes back).",
            "     It auto-detects X-Plane 12; pick your aircraft and wing variant.",
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
            "  3. Follow the on-screen steps (arrow keys / ENTER; ESC goes back).",
            "     It auto-detects X-Plane 12; pick your aircraft and wing variant.",
        ]
    if kind == "Linux":
        return [
            "HOW TO INSTALL",
            "  1. Extract this ENTIRE folder from the archive. Keep the installer and",
            "     the data/ folder together - the installer reads data/ from beside it.",
            f"  2. Open a terminal in this folder and run:   ./{exe_name}",
            f"       (If it is not executable:  chmod +x {exe_name})",
            "  3. Follow the on-screen steps (arrow keys / ENTER; ESC goes back).",
            "     It auto-detects X-Plane 12; pick your aircraft and wing variant.",
        ]
    # Python-Universal
    return [
        "HOW TO INSTALL  (requires Python 3.8+ - the only prerequisite)",
        "  1. Extract this ENTIRE folder. Keep install.py, the installer/ folder,",
        "     and the payload/ folder together.",
        "  2. Open a terminal in this folder and run:   python install.py",
        "       (use  python3 install.py  on macOS/Linux if 'python' is Python 2)",
        "  3. Follow the on-screen steps (arrow keys / ENTER; ESC goes back).",
        "     It auto-detects X-Plane 12; pick your aircraft and wing variant.",
    ]


def _contents(kind: str, exe_name: str, arch: str | None) -> list[str]:
    if kind == "Python":
        return [
            "WHAT'S INCLUDED",
            "  install.py, installer/   the installer itself - plain, readable Python",
            "  payload/                 the files it installs:",
            "    objs/                  pre-built light objects (wing variants x airframes)",
            "    plugin/                the native X-Plane plugin (all OSes: win/mac/linux)",
            "    dsl/                   small toolchain the RealWings live-patch needs",
        ]
    return [
        "WHAT'S INCLUDED",
        f"  {exe_name}   the installer (self-contained - no Python needed)",
        "  data/        the files it installs:",
        "    objs/      pre-built light objects (wing variants x airframes)",
        f"    plugin/    the native X-Plane plugin (this build: {arch or 'this platform'})",
        "    dsl/       small toolchain the RealWings live-patch needs at install",
    ]


def render(kind: str, *, exe_name: str | None = None, arch: str | None = None) -> str:
    """Full README.txt text for one bundle. `kind` in {Windows, macOS, Linux, Python};
    exe_name/arch are used only for the three frozen (non-Python) bundles."""
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
    ]
    if kind == "Python":
        lines += [
            "  This is the PYTHON-UNIVERSAL version: the installer is plain Python you",
            "  can read and inspect before running. It works on Windows, macOS, and",
            "  Linux, and needs no compiled binary.",
            "",
        ]
    lines += _install_steps(kind, exe_name)
    lines += [
        "",
        "  Close X-Plane before installing.",
        "",
    ]
    lines += _contents(kind, exe_name, arch)
    lines += [
        "",
        "REQUIREMENTS",
        f"  X-Plane 12 with a {AIRCRAFT_OR} aircraft already installed.",
        "",
        "UNINSTALL",
        "  Re-run the installer and choose Uninstall for the aircraft; it restores",
        "  the original ToLiss files byte-for-byte.",
        "",
    ]
    lines += _links()
    lines += [""]
    return "\n".join(lines) + "\n"


if __name__ == "__main__":  # quick manual preview: python build/readme.py [kind]
    k = sys.argv[1] if len(sys.argv) > 1 else "Windows"
    txt = render(k, exe_name=f"ToLissPhoton-Installer-v{VERSION}-{k}"
                 + (".exe" if k == "Windows" else ""), arch="win_x64")
    sys.stdout.buffer.write(txt.encode("utf-8"))
