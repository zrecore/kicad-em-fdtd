#!/usr/bin/env python3
"""Known-answer validation of the solver: a probe-fed rectangular patch run over domain size, Dk and mesh.

Why: on the TI CC2430DB board the series resonance moved by about +-1.6 % with the domain size and a Dk change
(4.5 to 4.2) moved it by +2..3 %, +0.35 % and -2.96 % at boxes of 0.25, 0.5 and 0.75 wavelengths: a sign that
physics does not allow. A patch has a cavity-model answer, so (a) the drift with the box, (b) the fractional shift
for a Dk change and (c) the distance from the analytic answer can be measured on a structure that is not a
board-specific effect.

  known_answer.py gen                  write the two snapshots (Dk 4.5 and 4.2, same geometry) in out/validation
  known_answer.py analytic             the cavity-model resonance for both Dk
  known_answer.py submit [--mesh 20 30] [--boxes ...] [--device NAME]   queue the matrix on the farm
  known_answer.py collect              fetch finished jobs and extract the series resonance
  known_answer.py table                print the results as a Markdown table

Everything is written under out/validation (git-ignored). Standard library only.
"""

import argparse
import importlib.util
import json
import math
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "out", "validation")
JOBS = os.path.join(OUT, "jobs.json")
C = 299_792_458.0
ER_A, ER_B = 4.5, 4.2
BOXES = (0.25, 0.5, 0.75, 1.0, 1.5, 2.0)
# One band for every run: the sweep, the cell size and the box (in wavelengths of the band centre) follow it.
BANDS = {"patch": ("2.0", "2.6"), "cavity": ("2.4", "3.3")}
PREFIX = {"patch": "pa", "cavity": "ca"}
POINTS = 601  # about 2 MHz bins: the shifts to resolve are 0.2 % of 2.2 GHz = 4 MHz


def _patch_module():
    spec = importlib.util.spec_from_file_location(
        "make_patch_snapshot", os.path.join(ROOT, "plugin", "tools", "make_patch_snapshot.py")
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def geometry(f0=2.45e9, er=ER_A, h=1.6e-3):
    """Width, length and the effective permittivity of the design (cavity model, Balanis ch. 14)."""
    return _patch_module().design(f0, er, h)


def fringing(er, w, h):
    eeff = (er + 1) / 2 + (er - 1) / 2 / math.sqrt(1 + 12 * h / w)
    dl = 0.412 * h * (eeff + 0.3) * (w / h + 0.264) / ((eeff - 0.258) * (w / h + 0.8))
    return eeff, dl


def patch_resonance(er, w, length, h=1.6e-3):
    """Cavity-model resonance of a patch of fixed size on a substrate of permittivity er (Hz)."""
    eeff, dl = fringing(er, w, h)
    return C / (2 * (length + 2 * dl) * math.sqrt(eeff))


def expected_shift(w, length, h=1.6e-3):
    """Fractional change of the resonance for er 4.5 to 4.2 with the patch itself unchanged."""
    return patch_resonance(ER_B, w, length, h) / patch_resonance(ER_A, w, length, h) - 1


def snapshot_path(er, struct="patch"):
    return os.path.join(OUT, "%s_er%s.json" % (struct, ("%g" % er).replace(".", "p")))


# --- the closed cavity: a via-walled box between two copper planes (exact analytic resonance) -------------------
CAV_A, CAV_B, CAV_H = 45e-3, 30e-3, 1.6e-3  # via-center rectangle and substrate thickness
CAV_VIA_D, CAV_PITCH = 0.6e-3, 0.4e-3  # diameter bigger than the pitch: the ring is one solid wall


def cavity_resonance(er, a=CAV_A, b=CAV_B, wall=CAV_VIA_D):
    """TM110 of a PEC box a x b (inner faces: the via centers moved in by half a via) filled with er: a thin
    cavity (h << wavelength) has no variation in z, so f = c / (2 sqrt(er)) sqrt((1/a)^2 + (1/b)^2)."""
    ai, bi = a - wall, b - wall
    return C / (2 * math.sqrt(er)) * math.sqrt((1 / ai) ** 2 + (1 / bi) ** 2)


def cavity_snapshot(er):
    board_a, board_b = CAV_A + 16e-3, CAV_B + 16e-3
    x0, y0 = 8e-3, 8e-3
    rect = lambda a0, b0, a1, b1: [[a0, b0], [a1, b0], [a1, b1], [a0, b1]]  # noqa: E731
    vias = []

    def ring(xa, ya, xb, yb):
        n = max(1, round(math.hypot(xb - xa, yb - ya) / CAV_PITCH))
        for i in range(n):
            t = i / n
            vias.append(
                {"x": xa + (xb - xa) * t, "y": ya + (yb - ya) * t, "diameter": CAV_VIA_D, "drill": CAV_VIA_D * 0.5, "net": "GND", "owner": ""}
            )

    ring(x0, y0, x0 + CAV_A, y0)
    ring(x0 + CAV_A, y0, x0 + CAV_A, y0 + CAV_B)
    ring(x0 + CAV_A, y0 + CAV_B, x0, y0 + CAV_B)
    ring(x0, y0 + CAV_B, x0, y0)
    return {
        "format": "kicad-em-fdtd.snapshot",
        "version": 1,
        "board": {"name": "closed cavity", "kicad_version": "none"},
        "outline": rect(0, 0, board_a, board_b),
        "stackup": [
            {"name": "F.Cu", "kind": "copper"},
            {"name": "core", "kind": "dielectric", "thickness": CAV_H, "epsilon_r": er, "loss_tangent": 0.02},
            {"name": "B.Cu", "kind": "copper"},
        ],
        "copper": [
            {"layer": "B.Cu", "owner": "", "net": "GND", "polygon": rect(-1e-3, -1e-3, board_a + 1e-3, board_b + 1e-3)},
            {"layer": "F.Cu", "owner": "", "net": "GND", "polygon": rect(x0 - 1.5e-3, y0 - 1.5e-3, x0 + CAV_A + 1.5e-3, y0 + CAV_B + 1.5e-3)},
        ],
        "vias": vias,
        "components": [],
        "ports": [
            {
                "name": "probe",
                "layer": "F.Cu",
                "layer_b": "B.Cu",
                "a": [x0 + CAV_A / 4 + 1e-6, y0 + CAV_B / 4 + 1e-6],
                "width": 0.0,
                "impedance": 50,
            }
        ],
        "notes": ["closed cavity %.0f x %.0f mm, via wall d %.1f mm pitch %.1f mm" % (CAV_A * 1e3, CAV_B * 1e3, CAV_VIA_D * 1e3, CAV_PITCH * 1e3)],
    }


def gen():
    """The snapshots: the geometry is designed once, for er 4.5, and only the substrate's er differs."""
    os.makedirs(OUT, exist_ok=True)
    base = os.path.join(OUT, "_patch_design.json")
    subprocess.run(
        [sys.executable, os.path.join(ROOT, "plugin", "tools", "make_patch_snapshot.py"), base],
        check=True,
        capture_output=True,
    )
    snap = json.load(open(base))
    for er in (ER_A, ER_B):
        s = json.loads(json.dumps(snap))
        for layer in s["stackup"]:
            if layer["kind"] == "dielectric":
                layer["epsilon_r"] = er
        with open(snapshot_path(er), "w") as f:
            json.dump(s, f)
    os.remove(base)
    for er in (ER_A, ER_B):  # the same patch with 35 um copper, for copper_model=slab
        s = json.load(open(snapshot_path(er)))
        for layer in s["stackup"]:
            if layer["kind"] == "copper":
                layer["thickness"] = 35e-6
        with open(snapshot_path(er)[:-5] + "_slab.json", "w") as f:
            json.dump(s, f)
    out = [snapshot_path(ER_A), snapshot_path(ER_B)]
    for er in (ER_A, ER_B):
        with open(snapshot_path(er, "cavity"), "w") as f:
            json.dump(cavity_snapshot(er), f)
        out.append(snapshot_path(er, "cavity"))
    return out


def translated(snap, dx, dy=0.0):
    """The same board moved by (dx, dy) metres: the grid then meets the copper edges in different places."""
    def pt(q):
        return [q[0] + dx, q[1] + dy]

    out = json.loads(json.dumps(snap))
    out["outline"] = [pt(q) for q in out["outline"]]
    for c in out["copper"]:
        c["polygon"] = [pt(q) for q in c["polygon"]]
    for v in out.get("vias", []):
        v["x"] += dx
        v["y"] += dy
    for prt in out["ports"]:
        prt["a"] = pt(prt["a"])
        if "b" in prt:
            prt["b"] = pt(prt["b"])
    return out


def nudged(snap, dx_m, dy_m):
    """Only the top-layer copper (the patch) moved by a few nanometers: nothing physical changes, but a copper edge
    that sits exactly on a grid line is then decided by rounding (the voxelizer's ray cast on the edge)."""
    out = json.loads(json.dumps(snap))
    for c in out["copper"]:
        if c["layer"] == "F.Cu":
            c["polygon"] = [[q[0] + dx_m, q[1] + dy_m] for q in c["polygon"]]
    return out


def shifted_path(er, struct, dx_mm):
    return os.path.join(OUT, "%s_er%s_dx%s.json" % (struct, ("%g" % er).replace(".", "p"), ("%g" % dx_mm).replace(".", "p")))


def cpw_for(er, cpw_at_a):
    """Cells per wavelength that give the SAME coarse cell as `cpw_at_a` has at er 4.5 (the cell is
    lambda_min / cpw with lambda_min = c / (f_max sqrt(er)))."""
    return cpw_at_a * math.sqrt(er / ER_A)


def flags(bd, cpw, er, held, struct="patch", extra=(), refine=()):
    f = [
        "--profile",
        "wifi",
        "--set",
        "band_min_ghz=" + BANDS[struct][0],
        "--set",
        "band_max_ghz=" + BANDS[struct][1],
        "--set",
        "decay_threshold=0.001",
        "--set",
        "boundary_distance_wavelengths=%g" % bd,
        "--set",
        "max_steps=200000",
        "--set",
        "sweep_points=%d" % POINTS,
    ]
    for kv in extra:
        f += ["--set", kv]
    for box in refine:  # "x0 y0 x1 y1 cell" in mm
        f += ["--refine", *box.split()]
    if cpw == 20 and not held:
        f += ["--set", "mesh_preset=medium"]
    else:
        value = cpw_for(er, cpw) if held else cpw
        f += ["--set", "cells_per_wavelength=%.6f" % value]
    return f


def label(bd, er, cpw, held, struct="patch", tag=""):
    return "%s_b%03d_e%s_m%d%s%s" % (
        PREFIX[struct], round(bd * 100), ("%g" % er).replace(".", ""), cpw, "h" if held else "", ("_" + tag) if tag else ""
    )


def combos(boxes, meshes):
    out = []
    for cpw in meshes:
        for bd in boxes:
            out.append((bd, ER_A, cpw, False))
            out.append((bd, ER_B, cpw, False))
            out.append((bd, ER_B, cpw, True))  # Dk 4.2 with the coarse cell kept as at Dk 4.5
    return out


def load_jobs():
    try:
        return json.load(open(JOBS))
    except (OSError, ValueError):
        return {}


def save_jobs(jobs):
    os.makedirs(OUT, exist_ok=True)
    tmp = JOBS + ".tmp"
    with open(tmp, "w") as f:
        json.dump(jobs, f, indent=1, sort_keys=True)
    os.replace(tmp, JOBS)


def farm(*args, **kw):
    tool = os.path.join(ROOT, "plugin")
    return subprocess.run(
        [os.path.join(tool, ".venv", "bin", "python"), os.path.join(tool, "tools", "emfdtd_farm.py"), *args],
        cwd=tool,
        capture_output=True,
        text=True,
        **kw,
    )


def submit(boxes, meshes, device, struct="patch", extra=(), tag="", held_runs=True, shift_mm=0.0, refine=(), nudge_nm=None, suffix=""):
    gen()
    jobs = load_jobs()
    for bd, er, cpw, held in combos(boxes, meshes):
        if held and not held_runs:
            continue
        name = label(bd, er, cpw, held, struct, tag)
        if name in jobs:
            continue
        snap = snapshot_path(er, struct)
        if suffix:
            snap = snap[:-5] + "_" + suffix + ".json"
        if shift_mm:
            snap = shifted_path(er, struct, shift_mm)
            json.dump(translated(json.load(open(snapshot_path(er, struct))), shift_mm * 1e-3), open(snap, "w"))
        if nudge_nm:
            snap = os.path.join(OUT, "%s_er%s_nudge_%s.json" % (struct, ("%g" % er).replace(".", "p"), tag))
            json.dump(nudged(json.load(open(snapshot_path(er, struct))), nudge_nm[0] * 1e-9, nudge_nm[1] * 1e-9), open(snap, "w"))
        cmd = ["submit", snap, *flags(bd, cpw, er, held, struct, extra, refine), "--label", name]
        if device:
            cmd += ["--device", device]
        r = farm(*cmd)
        job = r.stdout.strip().splitlines()[-1] if r.returncode == 0 and r.stdout.strip() else ""
        if not job:
            print("FAILED to submit", name, r.stderr.strip()[-200:])
            continue
        jobs[name] = {"job": job, "bd": bd, "er": er, "cpw": cpw, "held": held, "struct": struct, "tag": tag, "shift_mm": shift_mm}
        save_jobs(jobs)
        print("submitted", name, job)


def resonance(s1p):
    """Where the patch resonates, three ways (Hz): `dip` = parabola through the |S11| minimum (dB) and its two
    neighbors; `rpeak` = parabola through the maximum of Re(Z) near the dip; `xz` = the rising reactance zero
    crossing nearest the dip (None when the reactance never crosses zero: a probe-fed patch sits on an
    inductive baseline). Also returns the dip depth in dB."""
    d = []
    for line in open(s1p):
        if line[0] in "!#":
            continue
        p = line.split()
        d.append((float(p[0]), complex(float(p[1]), float(p[2]))))
    k = min(range(len(d)), key=lambda i: abs(d[i][1]))

    def parabola(ys, k):
        y0, y1, y2 = ys[k - 1], ys[k], ys[k + 1]
        den = y0 - 2 * y1 + y2
        shift = 0.5 * (y0 - y2) / den if den else 0.0
        return d[k][0] + shift * (d[k + 1][0] - d[k][0])

    db = [20 * math.log10(abs(s)) for _, s in d]
    zs = [50 * (1 + s) / (1 - s) for _, s in d]
    out = {"dip": parabola(db, k), "depth": db[k], "rpeak": None, "xz": None}
    lo, hi = max(1, k - 60), min(len(d) - 2, k + 60)
    kr = max(range(lo, hi), key=lambda i: zs[i].real)
    if lo < kr < hi - 1:
        out["rpeak"] = parabola([z.real for z in zs], kr)
    best = None
    for i in range(max(1, k - 60), min(len(zs) - 1, k + 60)):
        if zs[i].imag < 0 <= zs[i + 1].imag:
            fz = d[i][0] + (d[i + 1][0] - d[i][0]) * (-zs[i].imag) / (zs[i + 1].imag - zs[i].imag)
            if best is None or abs(fz - d[k][0]) < abs(best - d[k][0]):
                best = fz
    out["xz"] = best
    return out


def collect():
    jobs = load_jobs()
    status = json.loads(farm("queue", "--json").stdout)
    state = {j["job"]: j for j in status["jobs"]}
    n = 0
    for name, j in jobs.items():
        dest = os.path.join(OUT, name)
        if os.path.exists(os.path.join(dest, "s11.s1p")):
            continue
        if state.get(j["job"], {}).get("state") != "done":
            continue
        r = farm("results", j["job"], "--dest", dest)
        if r.returncode == 0:
            n += 1
    print("fetched", n, "new result(s)")


def table():
    jobs = load_jobs()
    ghz = lambda v: ("%.4f" % (v / 1e9)) if v else "none"  # noqa: E731
    for struct in sorted({j.get("struct", "patch") for j in jobs.values()}):
        rows = []
        for name, j in sorted(jobs.items()):
            p = os.path.join(OUT, name, "s11.s1p")
            if j.get("struct", "patch") == struct and os.path.exists(p):
                rows.append((("%d%s" % (j["cpw"], ("+" + j["tag"]) if j.get("tag") else "")), j["held"], j["bd"], j["er"], resonance(p)))
        if not rows:
            continue
        if struct == "patch":
            w, length, _ = geometry()
            ref = {er: patch_resonance(er, w, length) for er in (ER_A, ER_B)}
            what = "cavity model (fixed patch)"
        else:
            ref = {er: cavity_resonance(er) for er in (ER_A, ER_B)}
            what = "exact TM110, inner faces at the via centers moved in by half a via"
        print("### %s" % struct)
        print()
        print("| mesh (cells/wl) | cell held | box (wl) | Dk | dip (GHz) | Re Z peak (GHz) | dip (dB) |")
        print("|---|---|---|---|---|---|---|")
        for cpw, held, bd, er, r in sorted(rows, key=lambda x: x[:4]):
            print("| %s | %s | %g | %g | %s | %s | %.1f |" % (cpw, "yes" if held else "no", bd, er, ghz(r["dip"]), ghz(r["rpeak"]), r["depth"]))
        print()
        print(
            "%s: er 4.5 -> %.4f GHz, er 4.2 -> %.4f GHz, shift %+.2f %%"
            % (what, ref[ER_A] / 1e9, ref[ER_B] / 1e9, 100 * (ref[ER_B] / ref[ER_A] - 1))
        )
        by = {(cpw, bd, er, held): r["dip"] for cpw, held, bd, er, r in rows}
        print()
        print("| mesh | box (wl) | f(4.5) GHz | vs analytic | Dk 4.2 vs 4.5 (default cells) | Dk 4.2 vs 4.5 (cell held) |")
        print("|---|---|---|---|---|---|")
        for cpw in sorted({k[0] for k in by}):
            for bd in BOXES:
                a, b, h = by.get((cpw, bd, ER_A, False)), by.get((cpw, bd, ER_B, False)), by.get((cpw, bd, ER_B, True))
                if a and (b or h):
                    print(
                        "| %s | %g | %.4f | %+.2f %% | %s | %s |"
                        % (cpw, bd, a / 1e9, 100 * (a / ref[ER_A] - 1), ("%+.2f %%" % (100 * (b / a - 1))) if b else "", ("%+.2f %%" % (100 * (h / a - 1))) if h else "")
                    )
        print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("gen")
    sub.add_parser("analytic")
    s = sub.add_parser("submit")
    s.add_argument("--mesh", type=int, nargs="+", default=[20])
    s.add_argument("--boxes", type=float, nargs="+", default=list(BOXES))
    s.add_argument("--device")
    s.add_argument("--struct", choices=sorted(PREFIX), default="patch")
    s.add_argument("--set", dest="extra", action="append", default=[], help="an extra KEY=VALUE for every run")
    s.add_argument("--tag", default="", help="label suffix naming the extra settings")
    s.add_argument("--refine", action="append", default=[], help='a refinement box "x0 y0 x1 y1 cell" (mm)')
    s.add_argument("--suffix", default="", help="use patch_erX_SUFFIX.json (for example slab)")
    s.add_argument("--nudge-nm", type=float, nargs=2, help="move only the F.Cu copper by DX DY nanometers")
    s.add_argument("--shift-mm", type=float, default=0.0, help="move the whole board by this much in x")
    s.add_argument("--no-held", action="store_true", help="skip the Dk 4.2 runs with the coarse cell held")
    sub.add_parser("collect")
    sub.add_parser("table")
    args = ap.parse_args()
    if args.cmd == "gen":
        print("\n".join(gen()))
    elif args.cmd == "analytic":
        w, length, eeff = geometry()
        print("design W=%.2f mm L=%.2f mm eeff=%.3f" % (w * 1e3, length * 1e3, eeff))
        for er in (ER_A, ER_B):
            print("er %g: %.4f GHz" % (er, patch_resonance(er, w, length) / 1e9))
        print("shift %+.3f %%" % (100 * expected_shift(w, length)))
    elif args.cmd == "submit":
        submit(args.boxes, args.mesh, args.device, args.struct, args.extra, args.tag, not args.no_held, args.shift_mm, args.refine, args.nudge_nm, args.suffix)
    elif args.cmd == "collect":
        collect()
    else:
        table()


if __name__ == "__main__":
    main()
