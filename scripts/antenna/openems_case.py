#!/usr/bin/env python3
"""Cross-check a snapshot's one-port S11 with openEMS (a second FDTD solver), for validation only.

  out/openems/venv/bin/python -I scripts/antenna/openems_case.py SNAPSHOT.json OUTDIR
      [--cell 0.2] [--box X0 Y0 X1 Y1] [--fine X0 Y0 X1 Y1 CELL] [--coarse 2.0] [--margin 20]
      [--threads 6] [--setup-only] [--eps 4.5] [--tand 0.02]

Builds the same model the solver reads from the snapshot (board outline, each copper layer as a PEC slab of
its stackup thickness, holes in copper, vias as solid PEC columns, dielectric layers with a constant
conductivity equal to omega*eps0*er*tan(d) at the band centre, one vertical lumped port) and runs openEMS.
openEMS is GPL: it is run here as a black box through its Python API and none of its source is used.
Writes OUTDIR/s11.s1p in the format scripts/antenna/match.py reads. Needs the openEMS Python venv.
Lengths in the snapshot are metres, the case is in millimetres.
"""
import argparse
import json
import math
import os
import sys

import numpy as np
from CSXCAD import ContinuousStructure
from CSXCAD.SmoothMeshLines import SmoothMeshLines
from openEMS import openEMS

EPS0 = 8.854187817e-12


def cluster(values, tol):
    out = []
    for v in sorted(values):
        if out and v - out[-1][-1] < tol:
            out[-1].append(v)
        else:
            out.append([v])
    return [sum(c) / len(c) for c in out]


def edge_lines(shapes, box, cell, min_len):
    """x of vertical and y of horizontal long edges of copper inside box (mm), clustered."""
    xs, ys = [], []
    x0, y0, x1, y1 = box
    for poly in shapes:
        n = len(poly)
        for i in range(n):
            (ax, ay), (bx, by) = poly[i], poly[(i + 1) % n]
            if abs(ax - bx) < 1e-6 and abs(ay - by) >= min_len and x0 <= ax <= x1 and min(ay, by) < y1 and max(ay, by) > y0:
                xs.append(ax)
            if abs(ay - by) < 1e-6 and abs(ax - bx) >= min_len and y0 <= ay <= y1 and min(ax, bx) < x1 and max(ax, bx) > x0:
                ys.append(ay)
    return cluster(xs, 0.5 * cell), cluster(ys, 0.5 * cell)


def axis_lines(lo, hi, boxes, edges, fixed):
    """Lines along one axis: fixed ones, uniform refinement in each (a, b, cell) box with the snapped edges."""
    lines = set(fixed) | {lo, hi}
    for a, b, cell in boxes:
        n = max(1, int(round((b - a) / cell)))
        uniform = [a + (b - a) * i / n for i in range(n + 1)]
        snapped = [e for e in edges if a - 1e-9 <= e <= b + 1e-9]
        uniform = [u for u in uniform if all(abs(u - e) > 0.4 * cell for e in snapped)]
        lines |= set(uniform) | set(snapped)
    return sorted(lines)


def thin(lines, tol, keep=()):
    """Merge lines closer than tol, the first in sorted order surviving, except that a line in `keep` (the port,
    the board edges) always survives: it replaces a neighbor that is not kept, and two kept lines both stay.
    (Dropping the port's own line left the port off the mesh: openEMS ran 200 000 steps with no excitation.)"""
    protected = set(keep)
    out = []
    for v in sorted(lines):
        if not out or v - out[-1] > tol or (v in protected and out[-1] in protected):
            out.append(v)
        elif v in protected:
            out[-1] = v
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("snapshot")
    ap.add_argument("outdir")
    ap.add_argument("--cell", type=float, default=0.2, help="cell inside the antenna box (mm)")
    ap.add_argument("--box", type=float, nargs=4, default=[43.5, 121, 71, 132.5], help="antenna box x0 y0 x1 y1 (mm)")
    ap.add_argument("--fine", type=float, nargs=5, default=[53.5, 119.0, 56.5, 124.0, 0.06], help="feed box and cell")
    ap.add_argument("--coarse", type=float, default=2.0)
    ap.add_argument("--margin", type=float, default=32.0, help="air between the board and the absorbing layer (mm)")
    ap.add_argument("--threads", type=int, default=6)
    ap.add_argument("--setup-only", action="store_true")
    ap.add_argument("--post-only", action="store_true", help="skip the run and read the port data already in OUTDIR/sim (a stopped run)")
    ap.add_argument("--eps", type=float, default=None, help="override every dielectric er")
    ap.add_argument("--tand", type=float, default=None)
    ap.add_argument("--nts", type=int, default=600000)
    ap.add_argument("--end", type=float, default=5e-4, help="energy decay stop criterion")
    ap.add_argument("--growth", type=float, default=1.1)
    ap.add_argument("--zstep", type=float, default=0.16, help="largest vertical cell inside the board stack (mm); the z axis is not refined by --cell or --fine")
    ap.add_argument("--band", type=float, nargs=2, default=[1.9, 2.9], metavar=("LO", "HI"), help="S11 band (GHz); the excitation spans 0.6 of its width around the centre")
    a = ap.parse_args()

    a.outdir = os.path.abspath(a.outdir)  # the engine changes directory while it runs
    snap = json.load(open(a.snapshot))
    os.makedirs(a.outdir, exist_ok=True)
    f0, fc = 0.5e9 * (a.band[0] + a.band[1]), 0.6e9 * (a.band[1] - a.band[0])  # default 1.9..2.9: 2.4 GHz, 0.6 GHz
    mm = lambda p: [[x * 1e3, y * 1e3] for x, y in p]  # noqa: E731

    # Stack, bottom to top: z positions in mm.
    stack = list(reversed(snap["stackup"]))
    z, layers = 0.0, {}
    diel = []
    for e in stack:
        t = e.get("thickness", 0.0) * 1e3  # a copper entry without a thickness is a PEC sheet (like the solver)
        if e["kind"] == "copper":
            layers[e["name"]] = (z, z + t)
        else:
            diel.append((z, z + t, e.get("epsilon_r", 4.5), e.get("loss_tangent", 0.0)))
        z += t
    top = z
    names = [e["name"] for e in stack if e["kind"] == "copper"]

    xs0, ys0 = [p[0] * 1e3 for p in snap["outline"]], [p[1] * 1e3 for p in snap["outline"]]
    bx0, bx1, by0, by1 = min(xs0), max(xs0), min(ys0), max(ys0)

    CSX = ContinuousStructure()
    FDTD = openEMS(NrTS=a.nts, EndCriteria=a.end)
    FDTD.SetGaussExcite(f0, fc)
    FDTD.SetBoundaryCond(["PML_8"] * 6)
    FDTD.SetCSX(CSX)
    grid = CSX.GetGrid()
    grid.SetDeltaUnit(1e-3)

    # Dielectric between the outer copper slabs (a copper slot with no copper is filled like the layer below it).
    for i, (z0, z1, er, td) in enumerate(diel):
        er = a.eps if a.eps is not None else er
        td = a.tand if a.tand is not None else td
        kappa = 2 * math.pi * f0 * EPS0 * er * td
        m = CSX.AddMaterial("diel%d" % i, epsilon=er, kappa=kappa)
        hi = diel[i + 1][0] if i + 1 < len(diel) else z1
        m.AddBox([bx0, by0, z0], [bx1, by1, hi], priority=1)

    cu = CSX.AddMetal("copper")
    airhole = CSX.AddMaterial("airhole", epsilon=1.0)
    polys_by_layer = {}
    for c in snap["copper"]:
        lay = c["layer"]
        if lay not in layers:
            continue
        z0, z1 = layers[lay]
        pts = np.array(mm(c["polygon"])).T
        cu.AddLinPoly(pts, "z", z0, z1 - z0, priority=10)
        polys_by_layer.setdefault(lay, []).append(c["polygon"])
        for h in c.get("holes", []):
            hp = np.array(mm(h)).T
            if lay not in (names[0], names[-1]):
                # inner layer: a hole shows the dielectric
                hm = CSX.AddMaterial("hole_in_" + lay.replace(".", "_"), epsilon=diel[0][2])
                hm.AddLinPoly(hp, "z", z0, z1 - z0, priority=20)
            else:
                airhole.AddLinPoly(hp, "z", z0, z1 - z0, priority=20)
    for v in snap.get("vias", []):
        cu.AddCylinder([v["x"] * 1e3, v["y"] * 1e3, 0.0], [v["x"] * 1e3, v["y"] * 1e3, top], v["diameter"] * 5e2, priority=10)

    port = snap["ports"][0]
    px, py = port["a"][0] * 1e3, port["a"][1] * 1e3
    pw = port["width"] * 1e3
    zA, zB = layers[port["layer_b"]][1], layers[port["layer"]][0]  # the gap between the two copper slabs
    p1 = FDTD.AddLumpedPort(1, port["impedance"], [px - pw / 2, py, zA], [px + pw / 2, py, zB], "z", 1.0, priority=30)

    # Mesh.
    allshapes = [p for ps in polys_by_layer.values() for p in ps]
    shapes_mm = [mm(p) for p in allshapes]
    ex, ey = edge_lines(shapes_mm, a.box, a.cell, max(2 * a.cell, 0.15))
    fx0, fy0, fx1, fy1, fcell = a.fine
    fex, fey = edge_lines(shapes_mm, (fx0, fy0, fx1, fy1), fcell, max(2 * fcell, 0.05))
    m = a.margin + 8 * a.coarse
    X = axis_lines(bx0 - m, bx1 + m, [(a.box[0], a.box[2], a.cell), (fx0, fx1, fcell)], ex + fex, [bx0, bx1, px - pw / 2, px + pw / 2])
    Y = axis_lines(by0 - m, by1 + m, [(a.box[1], a.box[3], a.cell), (fy0, fy1, fcell)], ey + fey, [by0, by1, py])
    # A snapped edge a few microns from another line makes a micro-cell that sets the time step: merge lines
    # closer than half the finest cell (the first in sorted order is kept).
    mtol = 0.5 * min(a.cell, fcell)
    X = thin(X, mtol, [bx0, bx1, px - pw / 2, px + pw / 2])
    Y = thin(Y, mtol, [by0, by1, py])
    # z: every slab edge, at most --zstep (default 0.16 mm) in the dielectric.
    zl = {-a.margin - 8 * a.coarse, top + a.margin + 8 * a.coarse}
    for lo, hi in layers.values():
        zl |= {lo, hi}
    zl |= {0.0, top}
    zl = sorted(zl)
    fine_z = []
    for lo, hi in zip(zl, zl[1:]):
        if 0 <= lo and hi <= top:
            n = max(1, int(math.ceil((hi - lo) / a.zstep)))
            fine_z += [lo + (hi - lo) * i / n for i in range(n + 1)]
    zl = thin(sorted(set(zl) | set(fine_z)), 1e-4)
    for ax, lines in (("x", X), ("y", Y), ("z", zl)):
        lines = SmoothMeshLines(np.array(lines), a.coarse, ratio=a.growth)
        grid.AddLine(ax, lines)
        d = np.diff(lines)
        print("%s: %d cells below 0.04 mm, at %s" % (ax, int((d < 0.04).sum()), np.round(lines[:-1][d < 0.04], 4)[:12]), flush=True)
    nx, ny, nz = (len(grid.GetLines(i)) for i in range(3))
    print("mesh lines %d x %d x %d = %.2f M cells" % (nx, ny, nz, nx * ny * nz / 1e6), flush=True)
    for name, lines, want in (("x", grid.GetLines(0), px), ("y", grid.GetLines(1), py)):
        if float(np.abs(np.asarray(lines) - want).min()) > 1e-6:
            sys.exit("the port's %s line (%.4f mm) is not on the mesh: the port would never be excited" % (name, want))
    print("smallest cell (mm): x %.4f y %.4f z %.4f" % tuple(float(np.diff(grid.GetLines(i)).min()) for i in range(3)), flush=True)
    # Copper that spans no mesh line in x or y gets no cells (openEMS reports it as an unused primitive).
    gx, gy = grid.GetLines(0), grid.GetLines(1)
    dropped = 0
    for poly in shapes_mm:
        px_, py_ = [q[0] for q in poly], [q[1] for q in poly]
        nxl = int(((gx > min(px_) + 1e-6) & (gx < max(px_) - 1e-6)).sum())
        nyl = int(((gy > min(py_) + 1e-6) & (gy < max(py_) - 1e-6)).sum())
        if nxl == 0 or nyl == 0:
            dropped += 1
            inside = a.box[0] <= max(px_) and min(px_) <= a.box[2] and a.box[1] <= max(py_) and min(py_) <= a.box[3]
            print("copper shape with no mesh line inside: x %.3f..%.3f y %.3f..%.3f %s" % (
                min(px_), max(px_), min(py_), max(py_), "IN THE ANTENNA BOX" if inside else "outside"), flush=True)
    print("%d of %d copper shapes span no mesh line" % (dropped, len(shapes_mm)), flush=True)
    print("outline has %d points (the dielectric is its bounding box)" % len(snap["outline"]), flush=True)
    sim = os.path.join(a.outdir, "sim")
    if a.setup_only:
        CSX.Write2XML(os.path.join(a.outdir, "case.xml"))
        return
    if not a.post_only:
        FDTD.Run(sim, cleanup=True, numThreads=a.threads)
    freqs = np.linspace(a.band[0] * 1e9, a.band[1] * 1e9, 401)
    p1.CalcPort(sim, freqs, ref_impedance=port["impedance"])
    s11 = p1.uf_ref / p1.uf_inc
    zin = p1.uf_tot / p1.if_tot
    with open(os.path.join(a.outdir, "s11.s1p"), "w") as f:
        f.write("! openEMS cross-check of %s\n# Hz S RI R %g\n" % (os.path.basename(a.snapshot), port["impedance"]))
        for fr, s in zip(freqs, s11):
            f.write("%.9g %.9g %.9g\n" % (fr, s.real, s.imag))
    i = int(np.argmin(abs(s11)))
    print("best match %.2f dB at %.4f GHz, Z = %s" % (20 * math.log10(abs(s11[i])), freqs[i] / 1e9, zin[i]))


if __name__ == "__main__":
    sys.exit(main())
