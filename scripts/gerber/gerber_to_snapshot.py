#!/usr/bin/env python3
"""Build a kicad-em-fdtd board snapshot from a vendor's Gerber (and Excellon) files.

  python3 scripts/gerber/gerber_to_snapshot.py RECIPE.json OUT.json [--erase LAYER:x0,y0,x1,y1]... [--fill LAYER:x0,y0,x1,y1]...

The recipe (paths relative to the recipe file, lengths in mm) says which file is which copper layer, where the
outline comes from, the stackup, how to choose vias, and the port:
  {"name": "board",
   "outline": {"rect_mm": [x0, y0, x1, y1]}  or  {"gerber": "drill.spl"}   (the first closed loop of strokes),
   "stackup": [snapshot stackup entries, top to bottom; copper entries may carry "thickness" in metres],
   "layers": [{"name": "F.Cu", "file": "l1.spl", "res_mm": 0.05, "min_area_mm2": 0.3, "min_hole_mm2": 0.25}, ...],
   "vias": {"file": "NCdrill.spl", "format": {"int_digits": 2, "dec_digits": 3, "units": "inch", "zeros": "trailing"},
            "tool": "most" or N, "diameter_mm": 0.5, "drill_mm": 0.254,
            "ground_in_layers": ["F.Cu", "B.Cu"], "ground_min_area_mm2": 400},
   "port": {"name": "FEED", "layer": "F.Cu", "layer_b": "B.Cu", "a_mm": [x, y], "width_mm": 0.3, "impedance": 50},
   "erase": [{"layer": "F.Cu", "box_mm": [x0, y0, x1, y1]}]}      (cuts the feed, removes the antenna for a control)
A copper layer listed in the stackup but without a layer entry is left empty (say why in the recipe's notes).

What the result is and is not: Gerbers carry no nets, so every shape has net ''. They cannot tell a signal via from a
ground one, so a via is kept only where it lands inside a large copper region (a ground pour) of every layer named in
"ground_in_layers". Copper is traced from a raster at res_mm and simplified to about 0.6 pixels; shapes under
min_area_mm2 and holes under min_hole_mm2 are dropped (the voxelizer's cost grows with the number of shapes and holes).
Check the result before simulating: look at the layers (gerber.write_png), run the known-load controls on the port.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gerber  # noqa: E402
import trace  # noqa: E402


def _box(text):
    layer, nums = text.split(":")
    return layer, [float(v) for v in nums.split(",")]


def build(recipe_path, extra_erase=(), extra_fill=()):
    here = os.path.dirname(os.path.abspath(recipe_path))
    rec = json.load(open(recipe_path))
    path = lambda name: os.path.join(here, name)  # noqa: E731
    if "rect_mm" in rec["outline"]:
        x0, y0, x1, y1 = rec["outline"]["rect_mm"]
        ring = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    else:
        ring = gerber.first_closed_loop(gerber.parse(path(rec["outline"]["gerber"])))
    xs, ys = [p[0] for p in ring], [p[1] for p in ring]
    box = (min(xs), min(ys), max(xs), max(ys))
    erase = [(e["layer"], e["box_mm"]) for e in rec.get("erase", [])] + list(extra_erase)
    fill = list(extra_fill)

    copper, polys = [], {}
    for lay in rec["layers"]:
        canvas = gerber.Canvas(*box, lay["res_mm"])
        canvas.render(gerber.parse(path(lay["file"])))
        for name, b in erase:
            if name == lay["name"]:
                canvas.fill_box(b, False)
        for name, b in fill:
            if name == lay["name"]:
                canvas.fill_box(b, True)
        found = trace.polygons(canvas.img, lay["res_mm"], box[0], box[1], min_area_mm2=lay.get("min_area_mm2", 1.0),
                               min_hole_mm2=lay.get("min_hole_mm2", 0.25))
        polys[lay["name"]] = found
        for outer, holes in found:
            copper.append({"layer": lay["name"], "owner": "", "net": "",
                           "polygon": [[x * 1e-3, y * 1e-3] for x, y in outer],
                           "holes": [[[x * 1e-3, y * 1e-3] for x, y in h] for h in holes]})
    vias = []
    v = rec.get("vias")
    kept = total = 0
    if v:
        holes, _ = gerber.excellon(path(v["file"]), **v.get("format", {}))
        counts = {}
        for t, _, _ in holes:
            counts[t] = counts.get(t, 0) + 1
        tool = max(counts, key=counts.get) if v.get("tool", "most") == "most" else v["tool"]
        grounds = {name: [o for o, _ in polys.get(name, []) if abs(trace.area(o)) >= v.get("ground_min_area_mm2", 400.0)]
                   for name in v["ground_in_layers"]}
        for t, x, y in holes:
            if t != tool:
                continue
            total += 1
            if all(any(trace.inside(o, x, y) for o in grounds[name]) for name in v["ground_in_layers"]):
                kept += 1
                vias.append({"x": x * 1e-3, "y": y * 1e-3, "diameter": v["diameter_mm"] * 1e-3,
                             "drill": v["drill_mm"] * 1e-3, "net": "", "owner": ""})
    snap = {"format": "kicad-em-fdtd.snapshot", "version": 1,
            "board": {"name": rec.get("name", "gerber-board"), "kicad_version": "10"},
            "outline": [[x * 1e-3, y * 1e-3] for x, y in ring],
            "stackup": rec["stackup"], "copper": copper, "vias": vias, "components": [],
            "notes": rec.get("notes", [])}
    if "port" in rec:
        p = rec["port"]
        snap["ports"] = [{"name": p["name"], "layer": p["layer"], "layer_b": p["layer_b"],
                          "a": [p["a_mm"][0] * 1e-3, p["a_mm"][1] * 1e-3], "width": p["width_mm"] * 1e-3,
                          "impedance": p["impedance"]}]
    return snap, {"shapes": {k: len(v) for k, v in polys.items()}, "vias": (kept, total),
                  "vertices": sum(len(c["polygon"]) + sum(len(h) for h in c["holes"]) for c in copper)}


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        sys.exit(2)
    erase, fill = [], []
    i = 2
    while i < len(args):
        if args[i] == "--erase":
            erase.append(_box(args[i + 1]))
        elif args[i] == "--fill":
            fill.append(_box(args[i + 1]))
        else:
            sys.exit("unknown argument " + args[i])
        i += 2
    snap, info = build(args[0], erase, fill)
    json.dump(snap, open(args[1], "w"))
    print("wrote %s: shapes %s, vias kept %d of %d, %d vertices" % (args[1], info["shapes"], info["vias"][0],
                                                                  info["vias"][1], info["vertices"]))


if __name__ == "__main__":
    main()
