"""The agent window built for real (tkinter), walked tab by tab against a fake agent.

Opt-in: set EMFDTD_GUI_TESTS=1 (it needs tkinter and a display, and opens a window for a few seconds). With
the variable set, a missing tkinter, a missing display or any failure to build the window FAILS the test:
nothing is skipped silently. The window runs in tests/gui_smoke.py under a Python that has tkinter:
EMFDTD_GUI_PYTHON, else this Python, else (macOS) KiCad's bundled Python, else a python3 on PATH.
EMFDTD_GUI_SHOTS=<folder> also saves a screenshot of each tab (macOS).
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

OPT_IN = bool(os.environ.get("EMFDTD_GUI_TESTS"))
SMOKE = Path(__file__).with_name("gui_smoke.py")
KICAD_PYTHON = "/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3.9"


def _has_tkinter(py):
    try:
        r = subprocess.run([py, "-c", "import tkinter"], capture_output=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0


def find_python():
    candidates = [os.environ.get("EMFDTD_GUI_PYTHON"), sys.executable, KICAD_PYTHON]
    candidates += [
        shutil.which(n) for n in ("python3.13", "python3.12", "python3.11", "python3.10", "python3")
    ]
    for py in candidates:
        if py and Path(py).exists() and _has_tkinter(py):
            return py
    return None


@pytest.mark.skipif(
    not OPT_IN, reason="the window tests are opt-in: set EMFDTD_GUI_TESTS=1 (needs tkinter and a display)"
)
def test_the_agent_window_builds_and_every_tab_works():
    py = find_python()
    assert py, (
        "no Python with tkinter was found: set EMFDTD_GUI_PYTHON to one (this one has none: "
        f"{sys.executable}; on macOS KiCad's bundled Python has it)"
    )
    r = subprocess.run([py, str(SMOKE)], capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, f"the window smoke test failed under {py}:\n{r.stdout}\n{r.stderr}"
    assert "gui smoke: ok" in r.stdout
