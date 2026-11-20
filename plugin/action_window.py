"""KiCAD action "EM FDTD window": open (or bring forward) the solver window for this project without selecting
anything; the previous port is kept, or two selected pads define a new one."""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "src"))

from emfdtd_plugin import actions  # noqa: E402

if __name__ == "__main__":
    actions.run(HERE, require_port=False)
