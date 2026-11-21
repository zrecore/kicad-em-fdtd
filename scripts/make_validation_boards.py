#!/usr/bin/env python3
"""Writes small synthetic boards (snapshot JSON, docs/snapshot.md) for validation runs against closed forms
into out/samples/ (git-ignored). Nothing here is copied from any real design.

    python3 scripts/make_validation_boards.py

microstrip_short.json: a 3 mm trace over a full ground plane of 120 x 60 mm (large enough not to radiate much near 2 GHz; a 50 x 30 mm board radiated and gave |S11| well below 1) on 1.6 mm FR-4 (epsilon_r 4.4), fed by a vertical
port at one end and shorted to the ground by a via at the other, 39.3 mm of line between them. A short-
circuited line reflects everything with a phase that falls linearly with frequency; the slope gives the
effective permittivity, which has a closed form (Hammerstad and Jensen, IEEE MTT-S 1980, quasi-static)."""
import json
import math
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "out", "samples")


def rect(x0, y0, x1, y1):
    return [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]


def microstrip_short(width=3.0e-3, length_between=39.3e-3, eps_r=4.4, h=1.6e-3, board=(0.120, 0.060)):
    port_x = 40.2e-3 / 4 * 0 + (board[0] - length_between) / 2 + 0.2e-3 # the line is centred on the board
    via_x = port_x + length_between
    board = {
        "format": "kicad-em-fdtd.snapshot",
        "version": 1,
        "board": {"name": "microstrip-short", "kicad_version": "synthetic"},
        "outline": rect(0, 0, board[0], board[1]),
        "stackup": [
            {"name": "F.Cu", "kind": "copper"},
            {"name": "core", "kind": "dielectric", "thickness": h, "epsilon_r": eps_r, "loss_tangent": 0.0},
            {"name": "B.Cu", "kind": "copper"},
        ],
        "copper": [
            {"layer": "B.Cu", "owner": "", "net": "GND", "polygon": rect(0, 0, board[0], board[1])},
            {"layer": "F.Cu", "owner": "", "net": "SIG", "polygon": rect(port_x - 0.2e-3, board[1] / 2 - width / 2, via_x + 1.25e-3, board[1] / 2 + width / 2)},
        ],
        "vias": [{"x": via_x, "y": board[1] / 2, "diameter": 2.5e-3, "drill": 1.0e-3, "net": "SIG", "owner": ""}],
        "components": [],
        "ports": [{"name": "P1", "layer": "F.Cu", "layer_b": "B.Cu", "a": [port_x, board[1] / 2], "width": width, "impedance": 50.0}],
    }
    return board


def hammerstad_jensen_eps_eff(w, h, eps_r):
    """Quasi-static effective permittivity of a microstrip (Hammerstad and Jensen 1980), w / h >= 1."""
    u = w / h
    a = 1 + math.log((u**4 + (u / 52) ** 2) / (u**4 + 0.432)) / 49 + math.log(1 + (u / 18.1) ** 3) / 18.7
    b = 0.564 * ((eps_r - 0.9) / (eps_r + 3)) ** 0.053
    return (eps_r + 1) / 2 + (eps_r - 1) / 2 * (1 + 10 / u) ** (-a * b)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "microstrip_short.json")
    with open(path, "w") as f:
        json.dump(microstrip_short(), f)
    print("wrote", path)
    print("quasi-static effective permittivity, w = 3 mm, h = 1.6 mm, er = 4.4: %.4f" % hammerstad_jensen_eps_eff(3e-3, 1.6e-3, 4.4))
