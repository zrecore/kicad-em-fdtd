"""The GPU job farm command line:  .venv/bin/python tools/emfdtd_farm.py status|enable ROLE|disable ROLE

Off by default; see docs/PLAN.md, GPU farm.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

from emfdtd_plugin.farm.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
