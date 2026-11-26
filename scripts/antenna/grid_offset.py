#!/usr/bin/env python3
"""Grid-offset sweep on the TI board: the same model moved by a fraction of a cell, through the farm.

If the mesh were converged, moving the whole board by 0.1, 0.17 or 0.33 mm (0.2, 0.34 and 0.66 of a 0.5 mm cell)
would not change the series resonance; whatever it changes is the grid-placement error. The refinement boxes move
with the board. Snapshots go to out/validation/offset/, jobs to the farm (`emfdtd_farm.py submit`).

  python3 scripts/antenna/grid_offset.py submit --dx 0.1 0.17 0.33 [--box 0.5] [--cell 0.5] [--set KEY=VALUE ...]
  python3 scripts/antenna/grid_offset.py collect          (fetch finished runs, print the series resonance)
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from known_answer import translated  # noqa: E402

SNAP = os.path.join(ROOT, "out/antennas/swrr034c/ti_nol3_nodrop.json")
OUT = os.path.join(ROOT, "out/validation/offset")
FARM = [os.path.join(ROOT, "plugin/.venv/bin/python"), os.path.join(ROOT, "plugin/tools/emfdtd_farm.py")]
BASE_SETS = {
    "copper_model": "slab",
    "decay_threshold": "0.001",
    "lateral_growth_ratio": "1.1",
    "max_steps": "400000",
    "mesh_preset": "medium",
}
# The refinement boxes of the whole-board runs (mm): feed, antenna region, whole board at CELL.
BOXES = [(53.5, 119.0, 56.5, 124.0, 0.06), (43.5, 121.0, 71.0, 132.5, 0.1), (40.0, 32.0, 90.0, 133.0, None)]


def label_for(dx, box, extra):
    return "fb05_off%03d_bd%d%s" % (round(dx * 100), round(box * 100), extra)


def submit(args):
    os.makedirs(OUT, exist_ok=True)
    jobs = {}
    for dx in args.dx:
        path = os.path.join(OUT, "ti_dx%g.json" % dx)
        json.dump(translated(json.load(open(SNAP)), dx * 1e-3), open(path, "w"))
        sets = dict(BASE_SETS, boundary_distance_wavelengths=str(args.box))
        sets.update(kv.split("=", 1) for kv in args.set)
        cmd = FARM + ["submit", path, "--profile", "bluetooth", "--label", label_for(dx, args.box, args.suffix)]
        for k, v in sets.items():
            cmd += ["--set", "%s=%s" % (k, v)]
        for x0, y0, x1, y1, cell in BOXES:
            cmd += ["--refine"] + ["%g" % (v + (dx if i in (0, 2) else 0)) for i, v in enumerate((x0, y0, x1, y1))]
            cmd += ["%g" % (cell or args.cell)]
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        jobs[label_for(dx, args.box, args.suffix)] = out.stdout.strip().splitlines()[-1] if out.stdout.strip() else out.stderr
        print(label_for(dx, args.box, args.suffix), jobs[label_for(dx, args.box, args.suffix)])
    prev = json.load(open(os.path.join(OUT, "jobs.json"))) if os.path.exists(os.path.join(OUT, "jobs.json")) else {}
    prev.update(jobs)
    json.dump(prev, open(os.path.join(OUT, "jobs.json"), "w"), indent=1, sort_keys=True)


def collect(_args):
    jobs = json.load(open(os.path.join(OUT, "jobs.json")))
    for label, job in sorted(jobs.items()):
        dest = os.path.join(OUT, label)
        if not os.path.exists(os.path.join(dest, "s11.s1p")):
            r = subprocess.run(FARM + ["results", job, "--dest", dest], capture_output=True, text=True)
            if r.returncode:
                print(label, "not done yet")
                continue
        print(subprocess.run([sys.executable, os.path.join(HERE, "subbin.py"), dest], capture_output=True, text=True).stdout.strip())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("submit")
    s.add_argument("--dx", type=float, nargs="+", required=True, help="shift in x (mm)")
    s.add_argument("--box", type=float, default=0.5, help="boundary_distance_wavelengths")
    s.add_argument("--cell", type=float, default=0.5, help="whole-board refinement cell (mm)")
    s.add_argument("--set", action="append", default=[], help="extra KEY=VALUE (overrides the defaults)")
    s.add_argument("--suffix", default="", help="added to the job label")
    s.set_defaults(fn=submit)
    c = sub.add_parser("collect")
    c.set_defaults(fn=collect)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
