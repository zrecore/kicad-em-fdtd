#!/usr/bin/env python3
"""Do nets that should be separate touch in a board snapshot? Rasterizes the copper of one layer and flood-fills it.

A generated snapshot can short two nets by a hair (a corner overlapping a pour edge by a few micrometres), and the
grid then connects them. Prints the connected pieces each net's copper lies in and where the nets touch.

Usage: net_separation.py SNAPSHOT.json LAYER X0 X1 Y0 Y1 CELL_MM [NET_A NET_B]   (window in mm; default nets RF vs GND
as named in the snapshot: pass the two net names to compare). Needs numpy.
"""
import json
import sys

import numpy as np


def inside(poly, pts):
    x, y = pts[:, 0], pts[:, 1]
    out = np.zeros(len(pts), bool)
    for i in range(len(poly)):
        (xa, ya), (xb, yb) = poly[i], poly[(i + 1) % len(poly)]
        if ya != yb:
            out ^= ((ya > y) != (yb > y)) & (x < (xb - xa) * (y - ya) / (yb - ya) + xa)
    return out


def main():
    snap, layer = sys.argv[1], sys.argv[2]
    x0, x1, y0, y1, res = (float(v) for v in sys.argv[3:8])
    net_a, net_b = (sys.argv[8], sys.argv[9]) if len(sys.argv) > 9 else ("RF", "GND")
    d = json.load(open(snap))
    xs = np.arange(x0, x1, res) + res / 2
    ys = np.arange(y0, y1, res) + res / 2
    X, Y = np.meshgrid(xs, ys)
    pts = np.c_[X.ravel(), Y.ravel()]
    nets, union = {}, np.zeros(X.shape, bool)
    for sh in d["copper"]:
        if sh["layer"] != layer:
            continue
        m = inside(np.array(sh["polygon"]) * 1e3, pts).reshape(X.shape)  # snapshot lengths are metres
        union |= m
        nets[sh["net"]] = nets.get(sh["net"], np.zeros(X.shape, bool)) | m
    cc = np.zeros(X.shape, int)
    n = 0
    for j in range(X.shape[0]):
        for i in range(X.shape[1]):
            if union[j, i] and not cc[j, i]:
                n += 1
                cc[j, i] = n
                stack = [(j, i)]
                while stack:
                    a, b = stack.pop()
                    for da, db in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        u, v = a + da, b + db
                        if 0 <= u < X.shape[0] and 0 <= v < X.shape[1] and union[u, v] and not cc[u, v]:
                            cc[u, v] = n
                            stack.append((u, v))
    print(f"{layer}: {n} connected piece(s) of copper in the window")
    for name, m in nets.items():
        print(f"  net {name or '(none)'}: in piece(s) {sorted(set(cc[m]) - {0})}")
    if net_a in nets and net_b in nets:
        grown = nets[net_a].copy()
        grown[1:] |= nets[net_a][:-1]
        grown[:-1] |= nets[net_a][1:]
        grown[:, 1:] |= nets[net_a][:, :-1]
        grown[:, :-1] |= nets[net_a][:, 1:]
        jj, ii = np.nonzero(grown & nets[net_b])
        if len(jj):
            print(f"TOUCH: {net_a} and {net_b} meet in {len(jj)} cell(s), x {xs[ii].min():.3f}..{xs[ii].max():.3f}, "
                  f"y {ys[jj].min():.3f}..{ys[jj].max():.3f} mm")
            sys.exit(1)
        print(f"{net_a} and {net_b} are separate in this window")


if __name__ == "__main__":
    main()
