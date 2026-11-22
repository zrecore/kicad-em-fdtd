#!/usr/bin/env python3
"""Read-only summary of a KiCAD footprint (.kicad_mod, s-expression): pads (number, type, position, size, layers),
keepout/rule areas, courtyard extent, attributes and 3D model references. Verifies against a datasheet by eye:
pad pitch, pad size, and the extent of any copper-free area.

Usage: footprint_summary.py FILE.kicad_mod
"""
import re
import sys


def parse(text):
    tok = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    pos = 0

    def node():
        nonlocal pos
        pos += 1
        out = []
        while tok[pos] != ")":
            if tok[pos] == "(":
                out.append(node())
            else:
                out.append(tok[pos].strip('"'))
                pos += 1
        pos += 1
        return out

    return node()


def find(n, name):
    return [c for c in n if isinstance(c, list) and c and c[0] == name]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    fp = parse(open(sys.argv[1]).read())
    print("footprint", fp[1])
    for a in find(fp, "attr"):
        print("  attributes:", " ".join(map(str, a[1:])))
    for p in find(fp, "pad"):
        at, size, layers = find(p, "at"), find(p, "size"), find(p, "layers")
        print(f"  pad {p[1]!s:>4} {p[2]:8s} {p[3]:9s} at ({at[0][1]}, {at[0][2]}) size {size[0][1]} x {size[0][2]} "
              f"layers {' '.join(layers[0][1:]) if layers else ''}")
    xs, ys = [], []
    for g in find(fp, "fp_line") + find(fp, "fp_rect"):
        if find(g, "layer") and find(g, "layer")[0][1].endswith("CrtYd"):
            for key in ("start", "end"):
                q = find(g, key)
                if q:
                    xs.append(float(q[0][1]))
                    ys.append(float(q[0][2]))
    if xs:
        print(f"  courtyard x {min(xs):.3f}..{max(xs):.3f}  y {min(ys):.3f}..{max(ys):.3f}")
    for z in find(fp, "zone"):
        print("  zone (keepout/rule area):", [c for c in z if isinstance(c, list) and c[0] in ("name", "layers", "keepout")])
    for m in find(fp, "model"):
        print("  3D model:", m[1], [c for c in m[2:] if isinstance(c, list)])


if __name__ == "__main__":
    main()
