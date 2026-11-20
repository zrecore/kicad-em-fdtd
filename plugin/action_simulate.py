"""KiCAD action "Simulate (EM FDTD)": select two pads (the port), click. Opens or refreshes the solver window.
The board is only read: the plugin never modifies it."""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "src"))

from emfdtd_plugin import actions  # noqa: E402

if __name__ == "__main__":
    actions.run(HERE, require_port=True)
