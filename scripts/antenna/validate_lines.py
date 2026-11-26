#!/usr/bin/env python3
"""Check lines.py (closed-form Z0) against this project's FDTD solver on a microstrip and a grounded coplanar line.

  python3 scripts/antenna/validate_lines.py [--case microstrip|narrow|cpwg|all] [--lock "PREFIX CMD"] [--dry-run]
      [--thickness-um UM --copper-model sheet|slab --tag _NAME]   (copper thickness: run sheet and slab, compare)

Method: a line on a 1.6 mm board (er 4.4, lossless) with a lumped vertical port at one end and an open far end; the
solver gives S11, so Z_in(f) = 50 (1+S11)/(1-S11), and an open line of length l has Z_in = -j Z0 cot(k f), k = 2 pi
sqrt(er_eff) l_eff / c. Z0 and k are fitted to the phase of S11 (the loss from the open end's radiation changes the
magnitude, not the phase). The fitted Z0 is compared with lines.py; the fitted k gives the end extension l_eff - l
implied by lines.py's effective permittivity (about 0.4 h for a microstrip open end: a sanity check). The staircase
mesh, the port and the open end are errors of this method, not of the formulas: it shows how far apart the two are
on the stackups used here, nothing more; a run is minutes of GPU time.
Needs a release build of emfdtd (cmake --preset release && cmake --build --preset release). --lock is the prefix that
takes your machine's shared GPU lock, if you have one (this project's Mac: its with_gpu_lock.sh followed by nice -n 10).
Writes snapshots and results under out/lines-check/ (git-ignored).
"""

import argparse
import cmath
import json
import math
import os
import pathlib
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lines  # noqa: E402
import touchstone as ts  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]
H, ER = 1.6, 4.4
LINE_LEN = 30.0  # mm from the port to the open end
BEHIND = 0.5  # mm of strip behind the port
BOARD = (60.0, 30.0)


def rect(x0, y0, x1, y1):
    return [[x0 * 1e-3, y0 * 1e-3], [x1 * 1e-3, y0 * 1e-3], [x1 * 1e-3, y1 * 1e-3], [x0 * 1e-3, y1 * 1e-3]]


def snapshot(case, thickness_um=0.0):
    kind, w, gap = case["kind"], case["w"], case.get("gap")
    bw, bh = BOARD
    yc = bh / 2
    x_port, x_end = 8.0, 8.0 + LINE_LEN
    if kind == "bend":
        yc = case["y0"]
    copper = [{"layer": "B.Cu", "owner": "", "net": "GND", "polygon": rect(0.3, 0.3, bw - 0.3, bh - 0.3)}]
    strip = rect(x_port - BEHIND, yc - w / 2, x_end, yc + w / 2)
    if kind == "bend":  # a right-angle corner after leg1 mm, then leg2 mm along +y; the outer corner cut by a = miter * w along each edge (usual mitre fraction a / 2w)
        y0, leg1, leg2, m = case["y0"], case["leg1"], case["leg2"], case["miter"]
        xb = x_port + leg1  # the corner's centre lines cross here
        a = m * w
        outer = [(xb + w / 2, y0 - w / 2)] if a <= 0 else [(xb + w / 2 - a, y0 - w / 2), (xb + w / 2, y0 - w / 2 + a)]
        pts = [(x_port - BEHIND, y0 - w / 2)] + outer + [(xb + w / 2, y0 + leg2), (xb - w / 2, y0 + leg2),
                                                      (xb - w / 2, y0 + w / 2), (x_port - BEHIND, y0 + w / 2)]
        strip = [[x * 1e-3, y * 1e-3] for x, y in pts]
    copper.append({"layer": "F.Cu", "owner": "", "net": "SIG", "polygon": strip})
    vias = []
    if kind == "cpwg":
        y_in = yc - w / 2 - gap
        y_out = yc + w / 2 + gap
        for y0, y1 in ((0.3, y_in), (y_out, bh - 0.3)):
            copper.append(
                {"layer": "F.Cu", "owner": "", "net": "GND", "polygon": rect(0.3, y0, bw - 0.3, y1)}
            )
        for x in [3 + 3.0 * k for k in range(int((bw - 6) / 3.0) + 1)]:
            for y in (y_in - 1.5, y_out + 1.5):
                vias.append(
                    {
                        "x": x * 1e-3,
                        "y": y * 1e-3,
                        "diameter": 0.6e-3,
                        "drill": 0.3e-3,
                        "net": "GND",
                        "owner": "",
                    }
                )
    return {
        "format": "kicad-em-fdtd.snapshot",
        "version": 1,
        "board": {"name": "lines-check", "kicad_version": "10"},
        "outline": rect(0, 0, bw, bh),
        "stackup": [
            {"name": "F.Cu", "kind": "copper", **({"thickness": thickness_um * 1e-6} if thickness_um else {})},
            {
                "name": "core",
                "kind": "dielectric",
                "thickness": H * 1e-3,
                "epsilon_r": ER,
                "loss_tangent": 0.0,
            },
            {"name": "B.Cu", "kind": "copper", **({"thickness": thickness_um * 1e-6} if thickness_um else {})},
        ],
        "copper": copper,
        "vias": vias,
        "components": [],
        "ports": [
            {
                "name": "P1",
                "layer": "F.Cu",
                "layer_b": "B.Cu",
                "a": [x_port * 1e-3, (case["y0"] if kind == "bend" else yc) * 1e-3],
                "width": w / 3 * 1e-3,
                "impedance": 50,
            }
        ],
        "notes": [],
    }


def cases():
    w50 = lines.solve("microstrip", 50, H, ER)
    return {
        "microstrip": {"kind": "microstrip", "w": round(w50, 2), "cell": 0.25},
        "narrow": {"kind": "microstrip", "w": 1.0, "cell": 0.1},
        "cpwg": {"kind": "cpwg", "w": 0.8, "gap": 0.3, "cell": 0.1},
        "cpwg_wide": {"kind": "cpwg", "w": 0.8, "gap": 0.5, "cell": 0.1},
        "bend": {"kind": "bend", "w": 2.0, "cell": 0.1, "y0": 6.0, "leg1": 10.0, "leg2": 14.0, "miter": 0.0},
    }


def predicted(case):
    if case["kind"] == "bend":
        return lines.microstrip(case["w"], H, ER)
    if case["kind"] == "microstrip":
        return lines.microstrip(case["w"], H, ER)
    return lines.cpwg(case["w"], case["gap"], H, ER)


def fit(freqs, gammas):
    """Fit Z0, k and the port's series inductance to the phase of S11 of an open line.

    Z_in = j w Lp - j Z0 cot(k f). A vertical port is a short wire: its inductance (about a nanohenry per
    millimetre of height) is not part of the line and shifts the fit if left out."""
    sel = [(f, g) for f, g in zip(freqs, gammas) if 0.5e9 <= f <= 3.0e9]

    def cost(z0, k, lp):
        c = 0.0
        for f, g in sel:
            t = math.tan(k * f)
            z = complex(0, 2 * math.pi * f * lp - (z0 / t if abs(t) > 1e-9 else 1e9))
            gm = (z - 50) / (z + 50)
            c += abs(cmath.phase(g / gm)) ** 2
        return c

    best = (1e99, 0, 0, 0)
    for z0 in [20 + 4.0 * i for i in range(30)]:
        for esq in [1.0 + 0.05 * j for j in range(40)]:  # sqrt(er_eff)
            k = 2 * math.pi * esq * (LINE_LEN + 1.0) * 1e-3 / lines.C0
            for lp in (0.0, 0.5e-9, 1.0e-9, 1.5e-9, 2.0e-9):
                c = cost(z0, k, lp)
                if c < best[0]:
                    best = (c, z0, k, lp)
    _, z0, k, lp = best
    steps = [1.0, k * 0.01, 0.1e-9]
    for _ in range(80):
        moved = False
        for i in range(3):
            for sgn in (1, -1):
                trial = [z0, k, lp]
                trial[i] += sgn * steps[i]
                c = cost(*trial)
                if c < best[0]:
                    best = (c, *trial)
                    z0, k, lp = trial
                    moved = True
        if not moved:
            steps = [x / 2 for x in steps]
    return z0, k, lp, best[0] / len(sel)


def report(name, case, res):
    freqs, s11, _ = ts.read(os.path.join(res, "s11.s1p"))
    z0_fit, k_fit, lp_fit, resid = fit(freqs, s11)
    z0_f, eeff = predicted(case)
    l_eff = k_fit * lines.C0 / (2 * math.pi * math.sqrt(eeff)) * 1e3
    eeff_fit = (
        k_fit * lines.C0 / (2 * math.pi * (LINE_LEN + 0.4 * H) * 1e-3)
    ) ** 2  # assuming a 0.4 h end extension
    print(
        f"    lines.py: Z0 {z0_f:.1f} ohm, eff. permittivity {eeff:.2f}   solver fit: Z0 {z0_fit:.1f} ohm "
        f"({100 * (z0_fit - z0_f) / z0_f:+.1f} %), eff. permittivity {eeff_fit:.2f} (if the end extension is 0.4 h), "
        f"port inductance {lp_fit * 1e9:.2f} nH, phase residual {math.degrees(math.sqrt(resid)):.1f} deg rms; "
        f"extension implied by lines.py's permittivity {l_eff - LINE_LEN:+.2f} mm"
    )


def run_case(name, case, args):
    out = REPO / "out" / "lines-check"
    out.mkdir(parents=True, exist_ok=True)
    tag = name + args.tag
    last = out / "last.json"
    saved = json.loads(last.read_text()) if last.exists() else {}
    print(f"[{tag}] w {case['w']} mm" + (f", gap {case['gap']} mm" if case.get("gap") else ""))
    if args.refit:
        report(name, case, saved[tag])
        return
    snap = out / f"{tag}.json"  # one file per run: runs started together must not read each other's snapshot
    snap.write_text(json.dumps(snapshot(case, args.thickness_um)))
    cell = case["cell"] * args.cell_scale
    ybox = case["w"] + 2 * (case.get("gap") or 0) + 6
    ylo, yhi = BOARD[1] / 2 - ybox / 2, BOARD[1] / 2 + ybox / 2
    if case["kind"] == "bend":
        ylo, yhi = case["y0"] - 3, case["y0"] + case["leg2"] + 3
    cmd = [
        str(REPO / "build/release/apps/emfdtd/emfdtd"),
        str(snap),
        "--profile", "custom",
        "--set", "band_min_ghz=1.0", "--set", "band_max_ghz=2.0", "--set", "sweep_margin_fraction=0.5",
        "--set", "sweep_points=201", "--set", f"dielectric_cells_min={args.diel_cells}", "--set", "decay_threshold=0.001",
        "--refine", "5", str(ylo), "42", str(yhi), str(cell),
    ]  # fmt: skip
    if args.full_refine:  # one uniform cell over the whole board, so no grading touches the line
        cmd[cmd.index("--refine") : cmd.index("--refine") + 6] = ["--refine", "0", "0", str(BOARD[0]), str(BOARD[1]), str(cell)]
    for kv in args.set:
        cmd += ["--set", kv]
    if args.copper_model:
        cmd += ["--set", f"copper_model={args.copper_model}"]
    if args.air_near:
        cmd += [
            "--set",
            f"air_near_cell_mm={args.air_near}",
            "--set",
            f"air_near_height_mm={args.air_height}",
        ]
    if args.dry_run:
        cmd.append("--dry-run")
    if args.lock:
        cmd = args.lock.split() + cmd
    print(
        f"    lateral cell {cell} mm, {args.diel_cells} cells through the board, air cell near the board {args.air_near or 'default'}"
    )
    p = subprocess.run(cmd, capture_output=True, text=True)
    text = p.stdout + p.stderr
    for line in text.splitlines():
        if (
            line.startswith(("grid ", "estimate", "note: port", "note: copper", "results:"))
            or "joined" in line
        ):
            print("   ", line[:200])
    if args.dry_run or p.returncode != 0:
        if p.returncode != 0:
            print("    emfdtd failed:", text[-400:])
        return
    res = [ln.split("results:")[1].strip() for ln in text.splitlines() if ln.startswith("results:")][-1]
    saved = json.loads(last.read_text()) if last.exists() else {}  # re-read: another run may have added keys
    saved[tag] = res
    last.write_text(json.dumps(saved, indent=1))
    if case["kind"] != "bend":
        report(name, case, res)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", default="all", choices=["all", *cases()])
    ap.add_argument(
        "--lock", default=os.environ.get("GPU_LOCK", ""), help="command prefix that takes the GPU lock"
    )
    ap.add_argument("--tag", default="", help="suffix naming this mesh variant in last.json")
    ap.add_argument(
        "--cell-scale", type=float, default=1.0, help="multiply the lateral cell (0.5 = twice as fine)"
    )
    ap.add_argument("--diel-cells", type=int, default=8, help="cells through the board thickness")
    ap.add_argument("--air-near", type=float, default=0.0, help="vertical air cell next to the board, mm")
    ap.add_argument("--air-height", type=float, default=2.0, help="thickness of that layer, mm")
    ap.add_argument(
        "--refit", action="store_true", help="refit the last saved results of each case, no solve"
    )
    ap.add_argument(
        "--thickness-um", type=float, default=0.0, help="copper thickness written into the snapshot (um)"
    )
    ap.add_argument("--line-len", type=float, default=30.0, help="line length from the port to the open end, mm")
    ap.add_argument("--miter", type=float, default=0.0, help="bend case: the cut leg along each outer strip edge in strip widths (a = miter w). The usual mitre fraction (cut length along the corner square's diagonal over the diagonal w sqrt(2)) is a / (2 w)")
    ap.add_argument("--board", type=float, nargs=2, metavar=("W", "H"), help="board size, mm (default 60 30)")
    ap.add_argument("--full-refine", action="store_true", help="one uniform lateral cell over the whole board (no grading near the line)")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="an extra emfdtd setting (repeatable)")
    ap.add_argument("--er", type=float, default=4.4, help="substrate relative permittivity (1.0 is an air-filled line: eeff must read 1)")
    ap.add_argument("--behind", type=float, default=0.5, help="mm of strip behind the port (0 puts the strip's end at the port)")
    ap.add_argument("--leg1", type=float, default=10.0, help="bend case: length before the corner, mm")
    ap.add_argument("--leg2", type=float, default=14.0, help="bend case: length after the corner, mm")
    ap.add_argument("--width", type=float, default=0.0, help="replace the case's strip width (mm), for the 'narrow' case")
    ap.add_argument(
        "--copper-model", default="", choices=["", "sheet", "slab"], help="the copper_model setting to run with"
    )
    ap.add_argument("--dry-run", action="store_true", help="make the snapshots and print the plans, no solve")
    args = ap.parse_args()
    global LINE_LEN, BEHIND, ER, BOARD
    BEHIND = args.behind
    if args.board:
        BOARD = tuple(args.board)
    ER = args.er
    LINE_LEN = args.line_len
    for name, case in cases().items():
        if name == "bend":
            case = dict(case, miter=args.miter, leg1=args.leg1, leg2=args.leg2)
        if args.width and name == "narrow":
            case = dict(case, w=args.width, cell=min(case["cell"], round(args.width / 8, 3)) if args.width < 1.5 else case["cell"])
        if args.case in ("all", name):
            run_case(name, case, args)


if __name__ == "__main__":
    main()
