#!/usr/bin/env python3
"""Self-test of the Gerber tools on synthetic files (needs numpy; skipped without it). Run: python3 scripts/gerber/selftest.py"""
import json
import math
import os
import shutil
import sys

try:
    import numpy  # noqa: F401
except ImportError:
    print("gerber tools: skipped (numpy is not installed)")
    sys.exit(0)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "plugin", "src"))
import gerber  # noqa: E402
import gerber_to_snapshot  # noqa: E402
import trace  # noqa: E402

OUT = os.path.join(REPO, "out", "gerber-selftest")
shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + (" " + detail if detail else ""))
    if not ok:
        shutil.rmtree(OUT, ignore_errors=True)
        sys.exit(1)


def write(name, text):
    p = os.path.join(OUT, name)
    open(p, "w").write(text)
    return p


def area(ops, box=(0, 0, 40, 30), res=0.02):
    c = gerber.Canvas(*box, res)
    c.render(ops)
    return c.img.sum() * res * res


HEAD = "%FSLAX23Y23*%\n%MOMM*%\n"  # coordinates in micrometres, millimetres
# a stroke 10 mm long with a 0.2 mm round brush: a 10 x 0.2 rectangle and two half discs
p = write("stroke.gbr", HEAD + "%ADD10C,0.2*%\nD10*\nX5000Y5000D02*\nX15000Y5000D01*\nM02*\n")
want = 10 * 0.2 + math.pi * 0.1**2
check("a round stroke has the area of a capsule", abs(area(gerber.parse(p)) - want) < 0.03 * want, "%.3f vs %.3f" % (area(gerber.parse(p)), want))

# a rectangular flash, an obround flash, a region, modal D codes and a bare G01 that must not draw
p = write("shapes.gbr", HEAD + "%ADD11R,2.0X1.0*%\n%ADD12O,3.0X1.0*%\nD11*\nX5000Y5000D03*\nD12*\nX10000Y5000D03*\nG01*\n"
          "G36*\nX20000Y10000D02*\nX25000Y10000D01*\nX25000Y13000*\nX20000Y13000*\nX20000Y10000*\nG37*\nM02*\n")
want = 2 * 1 + (math.pi * 0.5**2 + 2 * 1) + 15
check("a rectangle, an obround and a region", abs(area(gerber.parse(p)) - want) < 0.03 * want, "%.3f vs %.3f" % (area(gerber.parse(p)), want))

# a half circle of radius 2 (G75, counter-clockwise) drawn with a 0.2 mm brush
p = write("arc.gbr", HEAD + "%ADD10C,0.2*%\nD10*\nG75*\nG01*\nX10000Y10000D02*\nG03*\nX6000Y10000I-2000J0D01*\nM02*\n")
want = math.pi * 2 * 0.2 + math.pi * 0.1**2
check("a G75 arc is a half circle", abs(area(gerber.parse(p)) - want) < 0.05 * want, "%.3f vs %.3f" % (area(gerber.parse(p)), want))

# clear polarity: a 6 mm disc with a 2 mm hole cleared out of it
p = write("clear.gbr", HEAD + "%ADD10C,6.0*%\n%ADD11C,2.0*%\n%LPD*%\nD10*\nX10000Y10000D03*\n%LPC*%\nD11*\nX10000Y10000D03*\nM02*\n")
want = math.pi * (3**2 - 1**2)
check("clear polarity cuts a hole", abs(area(gerber.parse(p)) - want) < 0.03 * want, "%.3f vs %.3f" % (area(gerber.parse(p)), want))

# a macro pad with a rotation: a 4 x 1 centre rectangle turned 90 degrees stands 1 wide and 4 tall
p = write("macro.gbr", HEAD + "%AMPAD*\n21,1,4.0,1.0,0,0,90*%\n%ADD10PAD*%\nD10*\nX10000Y10000D03*\nM02*\n")
c = gerber.Canvas(0, 0, 40, 30, 0.02)
c.render(gerber.parse(p))
ys, xs = numpy.nonzero(c.img)
w_, h_ = (xs.max() - xs.min() + 1) * 0.02, (ys.max() - ys.min() + 1) * 0.02
check("a rotated macro rectangle", abs(w_ - 1.0) < 0.1 and abs(h_ - 4.0) < 0.1, "%.2f x %.2f" % (w_, h_))

# the tracer: a 10 x 6 mm block with a 2 x 2 mm hole, and a speck under the minimum
c = gerber.Canvas(0, 0, 40, 30, 0.05)
c.fill_box((5, 5, 15, 11), True)
c.fill_box((8, 7, 10, 9), False)
c.fill_box((30, 20, 30.3, 20.3), True)
shapes = trace.polygons(c.img, 0.05, 0, 0, min_area_mm2=1.0, min_hole_mm2=0.25)
outer, holes = shapes[0]
check("the tracer returns the block and its hole, and drops the speck",
      len(shapes) == 1 and len(holes) == 1 and abs(trace.area(outer) - 60) < 1.5 and abs(-trace.area(holes[0]) - 4) < 0.4,
      "%d shape(s), outer %.1f hole %.1f" % (len(shapes), trace.area(outer), -trace.area(holes[0]) if holes else 0))

# Excellon: inch, 2.3 with trailing zeros omitted, tool diameters, modal X and Y
p = write("drill.drl", "M48\nINCH\nT01C0.010\nT02C0.035\n%\nT01\nX01000Y02000\nY0315\nX0125\nT02\nX01500Y01\n")
holes, dia = gerber.excellon(p, 2, 3, "inch", "trailing")
check("Excellon positions and diameters", len(holes) == 4 and abs(dia[1] - 0.254) < 1e-9
      and abs(holes[0][1] - 1.0 * 25.4) < 1e-6 and abs(holes[1][2] - 3.15 * 25.4) < 1e-6 and abs(holes[2][1] - 1.25 * 25.4) < 1e-6
      and holes[3][0] == 2 and abs(holes[3][2] - 1.0 * 25.4) < 1e-6, str(holes))

# the first closed loop of strokes is the outline
p = write("outline.gbr", HEAD + "%ADD10C,0.1*%\nD10*\nX0Y0D02*\nX20000Y0D01*\nX20000Y10000D01*\nX0Y10000D01*\nX0Y0D01*\nM02*\n")
loop = gerber.first_closed_loop(gerber.parse(p))
check("the outline loop is found", len(loop) == 4 and abs(max(x for x, _ in loop) - 20) < 1e-6)

# end to end: a recipe with two copper layers builds a snapshot the plugin's validator accepts
write("top.gbr", HEAD + "%ADD10C,0.4*%\nD10*\nX5000Y5000D02*\nX35000Y5000D01*\n"
      "G36*\nX2000Y15000D02*\nX38000Y15000D01*\nX38000Y28000*\nX2000Y28000*\nX2000Y15000*\nG37*\nM02*\n")
write("bottom.gbr", HEAD + "G36*\nX1000Y1000D02*\nX39000Y1000D01*\nX39000Y29000*\nX1000Y29000*\nX1000Y1000*\nG37*\nM02*\n")
write("holes.drl", "INCH\nT01C0.010\n%\nT01\nX00300Y00800\nX00500Y00200\n")  # (7.62, 20.32) mm in the pour; (12.7, 5.08) mm outside it
recipe = {"name": "selftest", "outline": {"rect_mm": [0, 0, 40, 30]},
          "stackup": [{"name": "F.Cu", "kind": "copper", "thickness": 35e-6},
                      {"name": "core", "kind": "dielectric", "thickness": 1.0e-3, "epsilon_r": 4.4, "loss_tangent": 0.02},
                      {"name": "B.Cu", "kind": "copper"}],
          "layers": [{"name": "F.Cu", "file": "top.gbr", "res_mm": 0.05, "min_area_mm2": 0.5},
                     {"name": "B.Cu", "file": "bottom.gbr", "res_mm": 0.1, "min_area_mm2": 1.0}],
          "vias": {"file": "holes.drl", "format": {"int_digits": 2, "dec_digits": 3, "units": "inch", "zeros": "trailing"},
                   "tool": "most", "diameter_mm": 0.5, "drill_mm": 0.254, "ground_in_layers": ["F.Cu", "B.Cu"],
                   "ground_min_area_mm2": 50},
          "port": {"name": "P1", "layer": "F.Cu", "layer_b": "B.Cu", "a_mm": [20, 5], "width_mm": 0.3, "impedance": 50},
          "erase": [{"layer": "F.Cu", "box_mm": [30, 4, 36, 6]}]}
rp = write("recipe.json", json.dumps(recipe))
snap, info = gerber_to_snapshot.build(rp)
from emfdtd_plugin import snapshot  # noqa: E402

snapshot.validate_snapshot(snap)
top = [s for s in snap["copper"] if s["layer"] == "F.Cu"]
check("the recipe builds a valid snapshot with the pour, the trace and the stripe erased from the trace",
      len(top) == 2 and info["vias"] == (1, 2) and abs(snap["vias"][0]["x"] - 0.00762) < 1e-6 and len(snap["outline"]) == 4
      and snap["ports"][0]["layer_b"] == "B.Cu",
      "F.Cu shapes %d, vias kept %s" % (len(top), info["vias"]))
import snapshot_to_kicad  # noqa: E402

outer = [[0.0, 0.0], [10.0, 0.0], [10.0, 10.0], [0.0, 10.0]]
hole = [[4.0, 4.0], [4.0, 6.0], [6.0, 6.0], [6.0, 4.0]]
ring = snapshot_to_kicad.with_bridges(outer, [hole])
shoelace = abs(sum(ring[i][0] * ring[(i + 1) % len(ring)][1] - ring[(i + 1) % len(ring)][0] * ring[i][1]
                   for i in range(len(ring)))) / 2
check("a polygon with a hole becomes one outline whose area is the polygon minus the hole",
      len(ring) == 10 and abs(shoelace - 96.0) < 1e-9, "points %d, area %g" % (len(ring), shoelace))
text = snapshot_to_kicad.board_text(snap, "selftest")
check("the KiCad board has one zone per copper shape, one via per via, the outline and the stackup",
      text.count("(zone ") == len(snap["copper"]) and text.count("(via ") == len(snap["vias"])
      and text.count("Edge.Cuts") >= 2 and text.count('(type "copper")') == 2 and text.count('"dielectric ') == 1,
      "zones %d vias %d" % (text.count("(zone "), text.count("(via ")))
four = json.loads(json.dumps(snap))
four["stackup"] = [
    {"name": "F.Cu", "kind": "copper", "thickness": 35e-6},
    {"name": "d1", "kind": "dielectric", "thickness": 3e-4, "epsilon_r": 4.5},
    {"name": "In1.Cu", "kind": "copper", "thickness": 35e-6},
    {"name": "d2", "kind": "dielectric", "thickness": 3e-4, "epsilon_r": 4.5},
    {"name": "In2.Cu", "kind": "copper", "thickness": 35e-6},
    {"name": "d3", "kind": "dielectric", "thickness": 3e-4, "epsilon_r": 4.5},
    {"name": "B.Cu", "kind": "copper", "thickness": 35e-6},
]
t4 = snapshot_to_kicad.board_text(four, "selftest")
order = [t4.index('"%s" signal' % n) for n in ("F.Cu", "In1.Cu", "In2.Cu", "B.Cu")]
check("a four-layer board lists its copper layers F.Cu, In1, In2, B.Cu (KiCad counts the layers from that order)",
      order == sorted(order), "positions %s" % order)
shutil.rmtree(OUT, ignore_errors=True)
print("gerber tools: ok")
