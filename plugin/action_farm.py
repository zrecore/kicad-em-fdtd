"""KiCAD action "EM FDTD farm": open (or bring forward) the solver window on its Farm tab, where the GPU farm
is switched on or off and its devices and jobs are managed. Starts no run."""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "src"))

from emfdtd_plugin import actions  # noqa: E402

if __name__ == "__main__":
    actions.run(HERE, require_port=False, request="farm")
