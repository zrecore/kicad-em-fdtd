"""Write a snapshot of a 2.4 GHz rectangular microstrip patch on FR-4 (a solver validation case).

Sizes follow the standard cavity-model design equations (Balanis, Antenna Theory, ch. 14), so the expected
resonance is known: design frequency, er and h below. Probe-fed through a vertical port (patch to ground).
  python tools/make_patch_snapshot.py out.json [--f0 2.45e9] [--er 4.5] [--h 1.6e-3]
"""

import argparse
import json
import math

C = 299_792_458.0


def design(f0, er, h):
    w = C / (2 * f0) * math.sqrt(2 / (er + 1))
    eeff = (er + 1) / 2 + (er - 1) / 2 / math.sqrt(1 + 12 * h / w)
    dl = 0.412 * h * (eeff + 0.3) * (w / h + 0.264) / ((eeff - 0.258) * (w / h + 0.8))
    length = C / (2 * f0 * math.sqrt(eeff)) - 2 * dl
    return w, length, eeff


def rect(x0, y0, x1, y1):
    return [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--f0", type=float, default=2.45e9)
    ap.add_argument("--er", type=float, default=4.5)
    ap.add_argument("--h", type=float, default=1.6e-3)
    ap.add_argument("--tand", type=float, default=0.02)
    args = ap.parse_args()
    w, length, eeff = design(args.f0, args.er, args.h)
    board = 0.060
    eps = 1e-6  # keeps patch edges off the grid lines (no ambiguous inside/outside tests)
    px0, py0 = (board - w) / 2 + eps, (board - length) / 2 + eps
    # 50 ohm probe point on the length axis: input resistance falls as cos^2 from the radiating edge
    feed_y = py0 + length * math.acos(math.sqrt(50 / 250)) / math.pi
    snap = {
        "format": "kicad-em-fdtd.snapshot",
        "version": 1,
        "board": {"name": f"patch {args.f0 / 1e9:.2f} GHz", "kicad_version": "none"},
        "outline": rect(0, 0, board, board),
        "stackup": [
            {"name": "F.Cu", "kind": "copper"},
            {
                "name": "core",
                "kind": "dielectric",
                "thickness": args.h,
                "epsilon_r": args.er,
                "loss_tangent": args.tand,
            },
            {"name": "B.Cu", "kind": "copper"},
        ],
        "copper": [
            {
                "layer": "B.Cu",
                "owner": "",
                "net": "GND",
                "polygon": rect(-1e-3, -1e-3, board + 1e-3, board + 1e-3),
            },
            {"layer": "F.Cu", "owner": "", "net": "RF", "polygon": rect(px0, py0, px0 + w, py0 + length)},
        ],
        "vias": [],
        "components": [],
        "ports": [
            {
                "name": "probe",
                "layer": "F.Cu",
                "layer_b": "B.Cu",
                "a": [board / 2, feed_y],
                "width": 0.0,
                "impedance": 50,
            }
        ],
        "notes": [
            f"design: W={w * 1e3:.2f} mm, L={length * 1e3:.2f} mm, eeff={eeff:.3f}, "
            f"feed {feed_y * 1e3:.2f} mm"
        ],
    }
    with open(args.out, "w") as f:
        json.dump(snap, f)
    print(snap["notes"][0])


if __name__ == "__main__":
    main()
