"""The agent window: `emfdtd-farm gui`. Standard library tkinter only; it opens no port and does nothing
in the background unless it is open. All logic is in controller.py (headless, tested); view.py only draws
it."""

import sys


def main(argv=None):
    """Open the window. Imported lazily: a machine without tkinter can still use every other command."""
    try:
        from .view import main as view_main
    except ImportError as e:
        print(
            f"emfdtd-farm gui needs tkinter, which this Python ({sys.executable}) does not have ({e}).\n"
            "Use a Python that includes tkinter (the python.org installers, and the system Python on macOS "
            "and Windows, do), or use the command line: emfdtd-farm agent status",
            file=sys.stderr,
        )
        return 1
    return view_main(argv)
