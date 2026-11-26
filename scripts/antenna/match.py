#!/usr/bin/env python3
"""Impedance matching helpers for antenna work. Standard library only; see .claude/skills/antenna-matching.

  match.py summary FILE.s1p [--band F0 F1]              dip, -10 dB band, where the reactance crosses zero (the resonance and its R), impedance at the dip and in the band
  match.py lmatch  --f HZ --z R+jX [--z0 50]            the L-networks that match a load at one frequency
  match.py eval    (--s1p FILE | --z R+jX --f0 HZ) --net "shunt C0.5p; series L2n" [--band F0 F1]
                                                        S11 seen at the source after a network (listed from the load outward)
  match.py search  (--s1p FILE | --z R+jX) --f0 HZ [--band F0 F1] [--topology shunt,series,shunt] [--top 5]
                                                        brute-force E12 values for a pi or T slot list, ranked by the worst |S11| in band

Ideal lumped parts: no ESR, no self-resonance, no pad parasitics. Treat a result as a starting point for the
real parts' S-parameters (see the skill). Frequencies in Hz, suffixes allowed on part values: f p n u m.
Method: textbook L-match and ABCD-free impedance cascading (Pozar, Microwave Engineering, Wiley; no DOI found; not read, the relations are the standard ones and the self-test checks them by cascading back to the reference).
"""

import argparse
import itertools
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import touchstone as ts  # noqa: E402

SUFFIX = {"f": 1e-15, "p": 1e-12, "n": 1e-9, "u": 1e-6, "m": 1e-3}


def value(text):
    t = text.strip().lower()
    return float(t[:-1]) * SUFFIX[t[-1]] if t and t[-1] in SUFFIX else float(t)


def cplx(text):
    return complex(text.replace(" ", ""))


def series_z(kind, x, f):
    w = 2 * math.pi * f
    return complex(0, w * x) if kind == "L" else complex(0, -1.0 / (w * x))


def shunt_y(kind, x, f):
    w = 2 * math.pi * f
    return complex(0, -1.0 / (w * x)) if kind == "L" else complex(0, w * x)


def apply_network(z, net, f):
    """net: list of (topology, kind, value) from the load outward; returns the impedance at the source."""
    for topo, kind, x in net:
        if topo == "series":
            z = z + series_z(kind, x, f)
        else:
            y = (1 / z if abs(z) > 1e-15 else complex(1e15, 0)) + shunt_y(kind, x, f)
            z = 1 / y if abs(y) > 1e-15 else complex(1e15, 0)
    return z


def parse_net(text):
    net = []
    for item in text.replace(",", ";").split(";"):
        item = item.strip()
        if not item:
            continue
        topo, comp = item.split()
        if topo not in ("series", "shunt") or comp[0].upper() not in "LC":
            raise SystemExit("bad network element: " + item + "   (use: shunt C0.5p; series L2n)")
        net.append((topo, comp[0].upper(), value(comp[1:])))
    return net


def element_text(kind, topo, react, f):
    """Value of the part that gives the reactance (series) or susceptance (shunt) react at f."""
    w = 2 * math.pi * f
    if topo == "series":
        return ("L", react / w) if react > 0 else ("C", -1.0 / (w * react))
    return ("C", react / w) if react > 0 else ("L", -1.0 / (w * react))


def fmt_part(kind, x):
    if kind == "L":
        return f"{x * 1e9:.3g} nH"
    return f"{x * 1e12:.3g} pF"


def lmatch(zl, f, z0=50.0):
    """All two-element L networks (load side first) that match zl to z0 at f. Returns a list of dicts."""
    out = []
    r, x = zl.real, zl.imag
    y = 1 / zl
    g, b = y.real, y.imag
    # shunt element across the load, then a series element towards the source (needs G < 1/Z0)
    if 0 < g < 1 / z0:
        for sign in (1, -1):
            bt = sign * math.sqrt(g / z0 - g * g)
            bsh = bt - b
            xs = bt / (g * g + bt * bt)
            out.append(
                {
                    "net": [
                        ("shunt",) + element_text("", "shunt", bsh, f),
                        ("series",) + element_text("", "series", xs, f),
                    ]
                }
            )
    # series element at the load, then a shunt element towards the source (needs R < Z0)
    if 0 < r < z0:
        for sign in (1, -1):
            xt = sign * math.sqrt(r * z0 - r * r)
            xser = xt - x
            bsh = xt / (r * r + xt * xt)
            out.append(
                {
                    "net": [
                        ("series",) + element_text("", "series", xser, f),
                        ("shunt",) + element_text("", "shunt", bsh, f),
                    ]
                }
            )
    for o in out:
        o["s11_db"] = ts.db(ts.z_to_gamma(apply_network(zl, o["net"], f), z0))
    return out


def load_from(args):
    if args.s1p:
        freqs, s11, z0 = ts.read(args.s1p)
        return freqs, [ts.gamma_to_z(g, z0) for g in s11], z0
    f0 = args.f0
    return [f0], [cplx(args.z)], args.z0


def resonances(freqs, s11, z0):
    """Frequencies where the reactance crosses zero (linear interpolation between points): a list of
    (f_hz, r_ohm, rising) with rising True for a series resonance (X from - to +, the shape of a resonant
    antenna's feed impedance) and False for a parallel one (X from + to -)."""
    zs = [ts.gamma_to_z(g, z0) for g in s11]
    out = []
    for k in range(len(freqs) - 1):
        x0, x1 = zs[k].imag, zs[k + 1].imag
        if x0 == 0 or x0 * x1 < 0:
            t = 0.0 if x1 == x0 else -x0 / (x1 - x0)
            out.append(
                (freqs[k] + t * (freqs[k + 1] - freqs[k]), zs[k].real + t * (zs[k + 1].real - zs[k].real), x1 > x0)
            )
    return out


def cmd_summary(args):
    freqs, s11, z0 = ts.read(args.file)
    mags = [abs(g) for g in s11]
    i = min(range(len(mags)), key=lambda k: mags[k])
    print(f"{len(freqs)} points, {freqs[0] / 1e9:.4f} to {freqs[-1] / 1e9:.4f} GHz, reference {z0:g} ohm")
    print(
        f"deepest dip: {ts.db(s11[i]):.2f} dB at {freqs[i] / 1e9:.4f} GHz, Z = {ts.gamma_to_z(s11[i], z0):.1f} ohm"
    )
    thr = 10 ** (-10 / 20)
    lo = hi = i
    while lo > 0 and mags[lo - 1] <= thr:
        lo -= 1
    while hi < len(freqs) - 1 and mags[hi + 1] <= thr:
        hi += 1
    if mags[i] <= thr:
        bw = freqs[hi] - freqs[lo]
        print(
            f"-10 dB band around the dip: {freqs[lo] / 1e9:.4f} to {freqs[hi] / 1e9:.4f} GHz "
            f"({bw / 1e6:.0f} MHz, {100 * bw / freqs[i]:.1f} % of the centre; resolution {(freqs[1] - freqs[0]) / 1e6:.1f} MHz)"
        )
    else:
        print("never below -10 dB")
    for f, r, rising in resonances(freqs, s11, z0):
        print(f"{'series' if rising else 'parallel'} resonance (reactance crosses zero) at {f / 1e9:.4f} GHz, R = {r:.1f} ohm")
    if args.band:
        sel = [k for k, f in enumerate(freqs) if args.band[0] <= f <= args.band[1]]
        if not sel:
            print("no points in the band")
            return
        worst = max(sel, key=lambda k: mags[k])
        best = min(sel, key=lambda k: mags[k])
        print(
            f"in band {args.band[0] / 1e9:.4f} to {args.band[1] / 1e9:.4f} GHz: best {ts.db(s11[best]):.2f} dB at "
            f"{freqs[best] / 1e9:.4f}, worst {ts.db(s11[worst]):.2f} dB at {freqs[worst] / 1e9:.4f}"
        )
        mid = sel[len(sel) // 2]
        print(
            f"impedance at the band middle ({freqs[mid] / 1e9:.4f} GHz): {ts.gamma_to_z(s11[mid], z0):.1f} ohm"
        )
        zc = ts.gamma_to_z(s11[mid], z0)
        print(
            "reading: "
            + (
                "inductive (X > 0): cancel with series C or shunt L"
                if zc.imag > 0
                else "capacitive (X < 0): cancel with series L or shunt C"
            )
            + "; R "
            + ("above" if zc.real > z0 else "below")
            + f" {z0:g}"
        )


def cmd_lmatch(args):
    zl = cplx(args.z)
    sols = lmatch(zl, args.f, args.z0)
    print(f"load {zl:.2f} ohm at {args.f / 1e9:.4f} GHz, reference {args.z0:g} ohm")
    if not sols:
        print(
            "no L network (load on the wrong side of both conditions: G >= 1/Z0 and R >= Z0 cannot both hold for a passive load)"
        )
    for s in sols:
        print(
            "  "
            + " -> ".join(f"{t} {fmt_part(k, v)}" for t, k, v in s["net"])
            + f"    (load side first; |S11| {s['s11_db']:.1f} dB at f)"
        )
    if sols:
        print(
            "The match holds at one frequency only; run 'eval' on a swept load to see the band. Use the nearest standard values and "
            "check them with 'eval'; real parts add ESR and parasitics."
        )


def sweep(freqs, zs, net, z0):
    return [ts.db(ts.z_to_gamma(apply_network(z, net, f), z0)) for f, z in zip(freqs, zs)]


def cmd_eval(args):
    freqs, zs, z0 = load_from(args)
    net = parse_net(args.net)
    vals = sweep(freqs, zs, net, z0)
    for f, v in zip(freqs, vals):
        if not args.band or args.band[0] <= f <= args.band[1]:
            print(f"{f / 1e9:.4f} GHz  {v:7.2f} dB")
    if len(freqs) > 1:
        i = min(range(len(vals)), key=lambda k: vals[k])
        print(f"deepest: {vals[i]:.2f} dB at {freqs[i] / 1e9:.4f} GHz")


E12 = [1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2]


def candidates(lmax_nh=20.0, cmax_pf=10.0):
    c = [("C", m * d * 1e-12) for d in (0.1, 1, 10) for m in E12 if 0.2 <= m * d <= cmax_pf]
    el = [("L", m * d * 1e-9) for d in (0.1, 1, 10) for m in E12 if 0.5 <= m * d <= lmax_nh]
    return c + el


def cmd_search(args):
    freqs, zs, z0 = load_from(args)
    band = args.band or (args.f0, args.f0)
    pts = [(f, z) for f, z in zip(freqs, zs) if band[0] <= f <= band[1]] or [
        min(zip(freqs, zs), key=lambda p: abs(p[0] - args.f0))
    ]
    topo = [t.strip() for t in args.topology.split(",")]
    slots = [[None] + candidates() for _ in topo]
    scored = []
    for combo in itertools.product(*slots):
        net = [(t, c[0], c[1]) for t, c in zip(topo, combo) if c is not None]
        worst = max(abs(ts.z_to_gamma(apply_network(z, net, f), z0)) for f, z in pts)
        scored.append((worst, len(net), net))
    scored.sort(key=lambda s: (round(s[0], 4), s[1]))
    print(
        f"{len(scored)} combinations, load side first, ideal E12 parts; ranked by the worst |S11| over {len(pts)} point(s)"
    )
    for w, _, net in scored[: args.top]:
        text = "; ".join(f"{t} {fmt_part(k, v)}" for t, k, v in net) or "(no parts)"
        print(f"  {ts.db(w):7.2f} dB   {text}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("summary")
    p.add_argument("file")
    p.add_argument("--band", type=float, nargs=2)
    p.set_defaults(fn=cmd_summary)
    p = sub.add_parser("lmatch")
    p.add_argument("--f", type=float, required=True)
    p.add_argument("--z", required=True)
    p.add_argument("--z0", type=float, default=50.0)
    p.set_defaults(fn=cmd_lmatch)
    for name, fn in (("eval", cmd_eval), ("search", cmd_search)):
        p = sub.add_parser(name)
        p.add_argument("--s1p")
        p.add_argument("--z")
        p.add_argument("--z0", type=float, default=50.0)
        p.add_argument("--f0", type=float, default=2.44e9)
        p.add_argument("--band", type=float, nargs=2)
        if name == "eval":
            p.add_argument("--net", required=True)
        else:
            p.add_argument("--topology", default="shunt,series,shunt")
            p.add_argument("--top", type=int, default=5)
        p.set_defaults(fn=fn)
    args = ap.parse_args()
    if args.cmd in ("eval", "search") and not (args.s1p or args.z):
        raise SystemExit("give --s1p FILE or --z R+jX")
    args.fn(args)


if __name__ == "__main__":
    main()
