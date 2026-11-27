#!/usr/bin/env python3
"""Port and known-load controls on a 50 ohm microstrip stub, through the farm.

Why: our settled series resonance on the TI board sits 2.7 % above openEMS's. Before blaming the mesh, check that the
lumped port and the feed give the right answer on a structure whose S11 is known: a stub of 50 ohm microstrip on FR-4
(Hammerstad) fed by a vertical lumped port, ending in (a) nothing (open), (b) a wall of vias (short), (c) a lumped
two-terminal part across a gap (`--part R1=open|short|r25|r50|r100|r200`).
  Expected: open reads 0 degrees and short 180 degrees when the phase of S11 is extrapolated to f = 0; the slope grows
  with the length by 720 sqrt(eeff) / c degrees per GHz per metre (lengths are compared as differences, so port
  and junction offsets cancel); a load R on a 50 ohm line reads |S11| = |R - 50| / (R + 50) at every frequency.
  Port controls: the width of the vertical port across the strip, its place across the strip and its distance to
  the strip start, and the gap of the lumped part, each against the base case.

  port_controls.py gen                    write the snapshots in out/validation/ports
  port_controls.py submit [--only RE]     queue every case on the farm (coarse; minutes each)
  port_controls.py collect                fetch finished jobs
  port_controls.py table                  print the readings against the expectations

Standard library only; everything is written under out/validation/ports (git-ignored).
"""

import argparse
import cmath
import json
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "out", "validation", "ports")
JOBS = os.path.join(OUT, "jobs.json")
C = 299_792_458.0
ER, H, TAND = 4.5, 1.6e-3, 0.02
Z0 = 50.0
BAND = (1.5, 3.5)  # GHz
FIT = (1.8, 3.2)  # GHz, the phase fit window
GAP = 0.8e-3  # the lumped part's gap
BOARD_X, BOARD_Y = 24e-3, 44e-3
EPS = 1e-6  # keeps copper edges off the grid lines


def hammerstad(er, h, z0=Z0):
    """Strip width and effective permittivity of a microstrip of impedance z0 (Hammerstad, w/h >= 1)."""
    a = z0 / 60 * math.sqrt((er + 1) / 2) + (er - 1) / (er + 1) * (0.23 + 0.11 / er)
    wh = 8 * math.exp(a) / (math.exp(2 * a) - 2)
    eeff = (er + 1) / 2 + (er - 1) / 2 / math.sqrt(1 + 12 / wh)
    return wh * h, eeff


def open_end_extension(w, h, eeff):
    """Length added to an open microstrip by the fringing field (Hammerstad)."""
    wh = w / h
    return 0.412 * h * (eeff + 0.3) * (wh + 0.264) / ((eeff - 0.258) * (wh + 0.8))


def slope_per_metre(eeff):
    """Magnitude of the S11 phase slope of a line, degrees per GHz per metre of one-way length."""
    return 720.0 * math.sqrt(eeff) / (C / 1e9)


def gamma(r, z0=Z0):
    return (r - z0) / (r + z0)


def rect(x0, y0, x1, y1):
    return [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]


def stub_snapshot(end="open", lp=5e-3, port_w=0.0, port_dx=0.0, port_back=0.0, gap=GAP, er=ER):
    """A y-oriented 50 ohm stub. The vertical port is `lp` from the strip end (the end of the part's near pad for
    end='load'); `port_back` is strip behind the port, `port_dx` moves the port across the strip."""
    w, eeff = hammerstad(er, H)
    xc, yp = BOARD_X / 2 + EPS, 12e-3 + EPS
    ys, ye = yp - 0.5e-3 - port_back, yp + lp
    copper = [
        {"layer": "B.Cu", "owner": "", "net": "GND", "polygon": rect(-1e-3, -1e-3, BOARD_X + 1e-3, BOARD_Y + 1e-3)},
        {"layer": "F.Cu", "owner": "", "net": "RF", "polygon": rect(xc - w / 2, ys, xc + w / 2, ye)},
    ]
    vias, components = [], []
    if end == "via":
        for k in range(-3, 4):  # a solid wall: diameter 0.6 on a 0.4 pitch
            vias.append({"x": xc + 0.4e-3 * k, "y": ye - 0.3e-3, "diameter": 0.6e-3, "drill": 0.3e-3, "net": "GND", "owner": ""})
    if end == "load":
        a0, a1 = ye - 0.4e-3, ye
        b0, b1 = ye + gap, ye + gap + 1.2e-3
        copper.append({"layer": "F.Cu", "owner": "R1", "net": "RF", "polygon": rect(xc - w / 2, a0, xc + w / 2, a1)})
        copper.append({"layer": "F.Cu", "owner": "R1", "net": "GND", "polygon": rect(xc - w / 2, b0, xc + w / 2, b1)})
        for dx in (-1.0e-3, 0.0, 1.0e-3):
            vias.append({"x": xc + dx, "y": 0.5 * (b0 + b1), "diameter": 0.5e-3, "drill": 0.25e-3, "net": "GND", "owner": ""})
        components.append(
            {
                "ref": "R1",
                "value": "load",
                "layer": "F.Cu",
                "x": xc,
                "y": 0.5 * (a0 + b1),
                "pads": [
                    {"number": "1", "net": "RF", "x": xc, "y": 0.5 * (a0 + a1)},
                    {"number": "2", "net": "GND", "x": xc, "y": 0.5 * (b0 + b1)},
                ],
            }
        )
    return {
        "format": "kicad-em-fdtd.snapshot",
        "version": 1,
        "board": {"name": "stub " + end, "kicad_version": "none"},
        "outline": rect(0, 0, BOARD_X, BOARD_Y),
        "stackup": [
            {"name": "F.Cu", "kind": "copper"},
            {"name": "core", "kind": "dielectric", "thickness": H, "epsilon_r": er, "loss_tangent": TAND},
            {"name": "B.Cu", "kind": "copper"},
        ],
        "copper": copper,
        "vias": vias,
        "components": components,
        "ports": [
            {"name": "probe", "layer": "F.Cu", "layer_b": "B.Cu", "a": [xc + port_dx, yp], "width": port_w, "impedance": Z0}
        ],
        "notes": ["%s stub, port %.1f mm from the end, strip %.2f mm (eeff %.3f)" % (end, lp * 1e3, w * 1e3, eeff)],
    }


def cases():
    """name -> {snap: stub_snapshot kwargs, part: the --part flag or None, kind: how to read it}."""
    out = {}
    for lp in (4, 5, 6):
        out["open_l%d" % lp] = {"snap": dict(end="open", lp=lp * 1e-3), "part": None, "kind": "open", "lp": lp * 1e-3}
        out["via_l%d" % lp] = {"snap": dict(end="via", lp=lp * 1e-3), "part": None, "kind": "short", "lp": lp * 1e-3}
    for tag in ("open", "short", "r25", "r50", "r100", "r200"):
        out["load_" + tag] = {"snap": dict(end="load", lp=5e-3), "part": "R1=" + tag, "kind": tag, "lp": 5e-3}
    for g in (0.4, 1.6):  # the gap of the lumped part (the base case is 0.8 mm)
        out["load_r100_g%d" % round(g * 10)] = {
            "snap": dict(end="load", lp=5e-3, gap=g * 1e-3),
            "part": "R1=r100",
            "kind": "r100",
            "lp": 5e-3,
        }
    for pw in (0.5, 1.5, 2.5):  # port width across the 3 mm strip (a third of the copper is 1 mm)
        out["open_l5_pw%d" % round(pw * 10)] = {"snap": dict(end="open", lp=5e-3, port_w=pw * 1e-3), "part": None, "kind": "open", "lp": 5e-3}
    out["open_l5_dx07"] = {"snap": dict(end="open", lp=5e-3, port_dx=0.7e-3), "part": None, "kind": "open", "lp": 5e-3}
    out["open_l5_back2"] = {"snap": dict(end="open", lp=5e-3, port_back=2e-3), "part": None, "kind": "open", "lp": 5e-3}
    return out


def flags(spec):
    f = ["--profile", "wifi", "--set", "band_min_ghz=%g" % BAND[0], "--set", "band_max_ghz=%g" % BAND[1]]
    f += ["--set", "sweep_points=401", "--set", "decay_threshold=0.001", "--set", "boundary_distance_wavelengths=0.25"]
    f += ["--set", "max_steps=200000", "--set", "mesh_preset=medium"]
    ye = 12e-3 + spec["lp"] + 5e-3
    f += ["--refine", "4", "6", "20", "%g" % (ye * 1e3 + 4), "0.3"]
    if spec["part"]:
        f += ["--part", spec["part"]]
    return f


def snap_path(name):
    return os.path.join(OUT, name + ".json")


def gen():
    os.makedirs(OUT, exist_ok=True)
    for name, spec in cases().items():
        json.dump(stub_snapshot(**spec["snap"]), open(snap_path(name), "w"))


def load_jobs():
    try:
        return json.load(open(JOBS))
    except (OSError, ValueError):
        return {}


def farm(*args):
    tool = os.path.join(ROOT, "plugin")
    return subprocess.run(
        [os.path.join(tool, ".venv", "bin", "python"), os.path.join(tool, "tools", "emfdtd_farm.py"), *args],
        cwd=tool,
        capture_output=True,
        text=True,
        timeout=900,
    )


def submit(only):
    gen()
    jobs = load_jobs()
    for name, spec in cases().items():
        if name in jobs or (only and not re.search(only, name)):
            continue
        r = farm("submit", snap_path(name), *flags(spec), "--label", "pc_" + name)
        job = r.stdout.strip().splitlines()[-1] if r.returncode == 0 and r.stdout.strip() else ""
        if not job:
            print("FAILED", name, r.stderr.strip()[-200:])
            continue
        jobs[name] = job
        json.dump(jobs, open(JOBS, "w"), indent=1, sort_keys=True)
        print("submitted", name, job)


def read_s1p(path):
    out = []
    for line in open(path):
        if line[0] in "!#" or not line.strip():
            continue
        p = line.split()
        out.append((float(p[0]) / 1e9, complex(float(p[1]), float(p[2]))))
    return out


def phase_fit(data, lo=FIT[0], hi=FIT[1]):
    """(slope in degrees per GHz, phase at f = 0 in degrees [0, 360)) of the unwrapped phase of S11."""
    pts = [(f, cmath.phase(s)) for f, s in data if lo <= f <= hi]
    xs, un = [pts[0][0]], [pts[0][1]]
    for f, p in pts[1:]:
        while p - un[-1] > math.pi:
            p -= 2 * math.pi
        while p - un[-1] < -math.pi:
            p += 2 * math.pi
        xs.append(f)
        un.append(p)
    n = len(xs)
    mx, mp = sum(xs) / n, sum(un) / n
    slope = sum((x - mx) * (p - mp) for x, p in zip(xs, un)) / sum((x - mx) ** 2 for x in xs)
    return math.degrees(slope), math.degrees(mp - slope * mx) % 360


def mean_mag(data, lo=FIT[0], hi=FIT[1]):
    v = [abs(s) for f, s in data if lo <= f <= hi]
    return sum(v) / len(v)


def collect():
    for name, job in sorted(load_jobs().items()):
        dest = os.path.join(OUT, name)
        if os.path.exists(os.path.join(dest, "s11.s1p")):
            continue
        r = farm("results", job, "--dest", dest)
        print(name, "fetched" if r.returncode == 0 else "not done yet")


def table():
    w, eeff = hammerstad(ER, H)
    dl = open_end_extension(w, H, eeff)
    k = slope_per_metre(eeff)
    print("strip %.3f mm, eeff %.3f, open-end extension %.2f mm, slope %.2f deg/GHz per mm" % (w * 1e3, eeff, dl * 1e3, k / 1e3))
    res = {}
    for name, spec in cases().items():
        p = os.path.join(OUT, name, "s11.s1p")
        if not os.path.exists(p):
            continue
        data = read_s1p(p)
        slope, phi0 = phase_fit(data)
        res[name] = (slope, phi0, mean_mag(data), data)
    print("\nname            kind    slope deg/GHz  phase(f=0)  mean|S11|  expected")
    for name, spec in cases().items():
        if name not in res:
            continue
        slope, phi0, mag, _ = res[name]
        kind = spec["kind"]
        if kind in ("open", "short"):
            want = "phase %s" % ("0" if kind == "open" else "180")
        elif kind.startswith("r"):
            want = "|S11| %.3f" % abs(gamma(float(kind[1:])))
        else:
            want = ""
        print("%-15s %-6s %9.1f %11.1f %10.3f  %s" % (name, kind, slope, phi0, mag, want))
    for kind in ("open", "via"):
        a, b = "%s_l4" % kind, "%s_l6" % kind
        if a in res and b in res:
            got = abs(res[a][0]) - abs(res[b][0])
            print("slope difference L=6 minus L=4 (%s): %.2f deg/GHz, expected %.2f (2 mm, dispersion-free)" % (kind, -got, k * 2e-3))
    base = res.get("open_l5")
    if base:
        print("\nport variants against open_l5 (S11 complex difference, rms over the fit band):")
        for name in sorted(res):
            if name.startswith("open_l5_"):
                d = [abs(s - t) for (f, s), (_, t) in zip(res[name][3], base[3]) if FIT[0] <= f <= FIT[1]]
                print("  %-14s rms |dS11| %.4f   mean|S11| %.3f (base %.3f)" % (name, math.sqrt(sum(x * x for x in d) / len(d)), res[name][2], base[2]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("gen")
    s = sub.add_parser("submit")
    s.add_argument("--only", help="a regular expression on the case name")
    sub.add_parser("collect")
    sub.add_parser("table")
    a = ap.parse_args()
    if a.cmd == "gen":
        gen()
    elif a.cmd == "submit":
        submit(a.only)
    elif a.cmd == "collect":
        collect()
    else:
        table()


if __name__ == "__main__":
    main()
