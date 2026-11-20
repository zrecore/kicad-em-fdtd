"""Write a snapshot of the board open in KiCAD:  .venv/bin/python tools/export_snapshot.py out.json

Read-only. KiCAD's IPC API server must be enabled and a board open. Ports are added with --port:
  --port NAME LAYER AX AY BX BY WIDTH [IMPEDANCE]   (millimetres, board coordinates)
"""

import argparse
import json

from kipy import KiCad

from emfdtd_plugin import kicad_reader
from emfdtd_plugin.units import mm_to_m


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out")
    ap.add_argument("--port", nargs="+", action="append", default=[], metavar="V")
    ap.add_argument(
        "--refill-zones", action="store_true", help="refill zones in the open board first (unsaved change)"
    )
    args = ap.parse_args()
    ports = []
    for p in args.port:
        name, layer, ax, ay, bx, by, width = p[:7]
        ports.append(
            {
                "name": name,
                "layer": layer,
                "a": [mm_to_m(float(ax)), mm_to_m(float(ay))],
                "b": [mm_to_m(float(bx)), mm_to_m(float(by))],
                "width": mm_to_m(float(width)),
                "impedance": float(p[7]) if len(p) > 7 else 50.0,
            }
        )
    snap = kicad_reader.read_board(KiCad(), ports, refill_zones=args.refill_zones)
    with open(args.out, "w") as f:
        json.dump(snap, f)
    print(f"wrote {args.out}: {len(snap['copper'])} copper polygons, {len(snap['vias'])} vias")
    for n in snap["notes"]:
        print("note:", n)


if __name__ == "__main__":
    main()
