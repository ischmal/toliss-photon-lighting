"""Verbose installer log — timestamped, leveled lines flushed to disk on every
write so a crash still leaves a trace. Opened in the OS text editor from the
Complete screen (`open_in_editor`)."""
from __future__ import annotations

import datetime as _dt
import os
import subprocess
import sys
import tempfile
from pathlib import Path


class Log:
    def __init__(self, version: str, dry_run: bool = False):
        ts = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        self.path = Path(tempfile.gettempdir()) / f"ToLissPhoton_install_{ts}.log"
        self.lines: list[str] = []
        self.write(f"ToLiss Photon {version} installer log — {_dt.datetime.now():%c}")
        self.write(f"platform: {sys.platform}  python: {sys.version.split()[0]}")
        if dry_run:
            self.write("MODE: --dry-run — no files will be modified")
        self.write("-" * 70)

    def write(self, msg: str, level: str = "INFO"):
        self.lines.append(f"{_dt.datetime.now():%H:%M:%S} [{level:5}] {msg}")
        try:
            self.path.write_text("\n".join(self.lines) + "\n", encoding="utf-8")
        except OSError:
            pass

    def open_in_editor(self):
        self.write("user opened installer log")
        try:
            if os.name == "nt":
                # NOT os.startfile(): a `.log` file frequently has no registered
                # file association, in which case os.startfile pops Windows'
                # "How do you want to open this file?" shell dialog (the log never
                # opens) and that shell interaction disturbs the console — the
                # reported "opening the log scrolls the header off the top" bug.
                # notepad.exe is always present, opens any text file regardless of
                # association, and is a GUI app so it never writes to the console.
                subprocess.Popen(["notepad.exe", str(self.path)])
            elif sys.platform == "darwin":
                os.system(f'open "{self.path}"')
            else:
                os.system(f'xdg-open "{self.path}" >/dev/null 2>&1')
        except Exception as e:
            return str(e)
        return None
