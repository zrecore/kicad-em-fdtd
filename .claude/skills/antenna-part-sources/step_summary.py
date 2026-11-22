#!/usr/bin/env python3
"""Read-only summary of an AP203/AP214 STEP file: per solid, its bounding box, face count and colour, and with
`--faces` the box of every face. Dependency-free (regular expressions over the text), enough to read the shapes of
a small SMD part: ceramic body, metal terminals, their thickness and gaps.

Usage: step_summary.py FILE.step [--faces] [SOLID_ID ...]    (SOLID_ID: also list that solid's distinct vertices)
Units are the file's (nearly always mm). Curved faces report the box of their edge vertices only.
"""
import re
import sys


def load(path):
    text = open(path, errors="replace").read()
    ents = {}
    for m in re.finditer(r"#(\d+)\s*=\s*(.*?);\s*(?=\n#|\nENDSEC)", text, re.S):
        ents[int(m.group(1))] = m.group(2)
    return ents


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    want_faces = "--faces" in sys.argv
    if not args:
        raise SystemExit(__doc__)
    ents = load(args[0])

    def refs(s):
        return [int(x) for x in re.findall(r"#(\d+)", s)]

    def kind(i):
        return ents.get(i, "")

    def point(i):
        m = re.search(r"\(\s*([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*\)\s*\)", ents[i])
        return tuple(float(v) for v in m.groups())

    def face_points(face):
        out = []
        for b in refs(ents[face]):
            if kind(b).startswith(("FACE_OUTER_BOUND", "FACE_BOUND")):
                for oe in refs(ents[refs(ents[b])[0]]):
                    if kind(oe).startswith("ORIENTED_EDGE"):
                        for v in refs(ents[refs(ents[oe])[-1]])[:2]:
                            out.append(point(refs(ents[v])[0]))
        return out

    def box(ps):
        return [(min(p[i] for p in ps), max(p[i] for p in ps)) for i in range(3)]

    def fmt(b):
        return "  ".join(f"{a} {lo:.3f}..{hi:.3f}" for a, (lo, hi) in zip("xyz", b))

    colour = {}
    for i, e in ents.items():
        if not e.startswith("STYLED_ITEM"):
            continue
        r = refs(e)
        seen, stack, col = set(), list(r), None
        while stack and col is None:  # follow the presentation-style chain to a COLOUR_RGB
            k = stack.pop()
            if k in seen or k not in ents:
                continue
            seen.add(k)
            if ents[k].startswith("COLOUR_RGB"):
                col = tuple(float(x) for x in re.findall(r"[\d.]+", ents[k].split("(", 1)[1])[:3])
            stack += refs(ents[k])
        for k in r:
            if kind(k).startswith(("MANIFOLD_SOLID_BREP", "ADVANCED_FACE")):
                colour[k] = col

    solids = {}
    for i, e in ents.items():
        if e.startswith("MANIFOLD_SOLID_BREP"):
            faces = [f for f in refs(ents[refs(e)[0]]) if kind(f).startswith("ADVANCED_FACE")]
            solids[i] = faces
            ps = [p for f in faces for p in face_points(f)]
            print(f"solid #{i}: {len(faces)} faces, colour {colour.get(i)}, {fmt(box(ps))}")
            for f in faces if want_faces else [f for f in faces if colour.get(f)]:
                print(f"   face #{f} colour {colour.get(f)}  {fmt(box(face_points(f)))}")
    for sid in map(int, args[1:]):
        vs = sorted({tuple(round(c, 3) for c in p) for f in solids[sid] for p in face_points(f)})
        print(f"solid #{sid}: {len(vs)} distinct vertices: {vs}")


if __name__ == "__main__":
    main()
