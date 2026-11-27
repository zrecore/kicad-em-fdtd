#!/usr/bin/env python3
"""Write a KiCad 10 board (.kicad_pcb) from a board snapshot: the copper-only twin of a Gerber-built model.

  python3 scripts/gerber/snapshot_to_kicad.py SNAPSHOT.json OUT.kicad_pcb [--project]

What goes in: the outline (as the Edge.Cuts rectangle), the stackup (copper and dielectric thicknesses, permittivity,
loss tangent), every copper polygon as a filled zone on its layer (holes joined to the outline by a zero-width bridge, the
way KiCad stores filled areas), every via, and a note on the port. Snapshot lengths are metres in KiCad's own frame
(x right, y down, no flip), so a board exported from this file by the plugin has the snapshot's coordinates.
What does not: parts, nets (a Gerber-built snapshot has none: every zone is net 0), solder mask, silkscreen, and the
port itself (a port belongs to the plugin's selection, not to the board file); its place is marked on Cmts.User.

--project also writes OUT's .kicad_pro next to it (a minimal one; KiCad fills in the rest when it opens it).
Authored for this repository; nothing here is taken from a vendor.
"""

import argparse
import json
import pathlib
import uuid

NS = uuid.UUID("6f1c7b52-2a0e-4b8d-9d77-3a5f0a1b9c01")

TECH_LAYERS = """\
		(9 "F.Adhes" user "F.Adhesive")
		(11 "B.Adhes" user "B.Adhesive")
		(13 "F.Paste" user)
		(15 "B.Paste" user)
		(5 "F.SilkS" user "F.Silkscreen")
		(7 "B.SilkS" user "B.Silkscreen")
		(1 "F.Mask" user)
		(3 "B.Mask" user)
		(17 "Dwgs.User" user "User.Drawings")
		(19 "Cmts.User" user "User.Comments")
		(21 "Eco1.User" user "User.Eco1")
		(23 "Eco2.User" user "User.Eco2")
		(25 "Edge.Cuts" user)
		(27 "Margin" user)
		(31 "F.CrtYd" user "F.Courtyard")
		(29 "B.CrtYd" user "B.Courtyard")
		(35 "F.Fab" user)
		(33 "B.Fab" user)
"""


def uid(*parts):
    return str(uuid.uuid5(NS, "/".join(str(p) for p in parts)))


def mm(v):
    """Metres to millimeters, rounded to a nanometer (KiCad's own resolution)."""
    return "%.6f" % (v * 1e3)


def pt(p):
    return "(xy %s %s)" % (mm(p[0]), mm(p[1]))


def kicad_layer_ids(copper_names):
    """KiCad's ids: F.Cu 0, B.Cu 2, In1.Cu 4, In2.Cu 6, ... (even numbers in order)."""
    ids = {}
    for name in copper_names:
        if name == "F.Cu":
            ids[name] = 0
        elif name == "B.Cu":
            ids[name] = 2
        elif name.startswith("In") and name.endswith(".Cu"):
            ids[name] = 2 + 2 * int(name[2:-3])
        else:
            raise ValueError("unexpected copper layer name %r" % name)
    return ids


def with_bridges(ring, holes):
    """One outline for a polygon with holes: each hole is joined to the outline at its nearest vertex by a pair of
    coincident edges (how KiCad stores a filled area with a hole)."""
    out = list(ring)
    for hole in holes:
        # nearest pair of vertices between the hole and the current outline
        best = None
        for i, p in enumerate(out):
            for j, q in enumerate(hole):
                d = (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2
                if best is None or d < best[0]:
                    best = (d, i, j)
        _, i, j = best
        rot = hole[j:] + hole[:j]
        out = out[: i + 1] + rot + [rot[0]] + [out[i]] + out[i + 1 :]
    return out


def zone_text(layer, polygon, holes, name):
    outline = " ".join(pt(p) for p in polygon)
    filled = " ".join(pt(p) for p in with_bridges(polygon, holes))
    return (
        '\t(zone (net 0) (net_name "") (layer "%s") (uuid "%s") (hatch edge 0.5) (connect_pads (clearance 0))'
        " (min_thickness 0.001) (filled_areas_thickness no)"
        " (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))"
        " (polygon (pts %s)) (filled_polygon (layer \"%s\") (pts %s)))\n" % (layer, uid("zone", name), outline, layer, filled)
    )


def board_text(snap, source_name):
    stack = snap["stackup"]
    copper = [s["name"] for s in stack if s["kind"] == "copper"]
    ids = kicad_layer_ids(copper)
    total = sum(s.get("thickness", 35e-6 if s["kind"] == "copper" else 0.0) for s in stack)
    lines = ["(kicad_pcb\n\t(version 20260206)\n\t(generator \"kicad-em-fdtd\")\n\t(generator_version \"0\")\n"]
    lines.append("\t(general (thickness %.6f) (legacy_teardrops no))\n\t(paper \"A4\")\n\t(layers\n" % (total * 1e3))
    # KiCad reads the copper count from this order: F.Cu, the inner layers top to bottom, B.Cu last (B.Cu earlier
    # makes it count a copper layer short and drops the last inner layer's zones onto B.Cu).
    for name in sorted(copper, key=lambda n: (n == "B.Cu", n != "F.Cu", ids[n])):
        lines.append('\t\t(%d "%s" signal)\n' % (ids[name], name))
    lines.append(TECH_LAYERS + "\t)\n")
    lines.append("\t(setup\n\t\t(stackup\n")
    di = 0
    for s in stack:
        t = s.get("thickness", 35e-6) * 1e3  # a copper layer without a thickness is a sheet: draw it as 1 oz
        if s["kind"] == "copper":
            lines.append('\t\t\t(layer "%s" (type "copper") (thickness %.6f))\n' % (s["name"], t))
        else:
            di += 1
            lines.append(
                '\t\t\t(layer "dielectric %d" (type "core") (thickness %.6f) (material "FR4") (epsilon_r %g)'
                " (loss_tangent %g))\n" % (di, t, s.get("epsilon_r", 4.5), s.get("loss_tangent", 0.02))
            )
    lines.append("\t\t\t(copper_finish \"None\")\n\t\t\t(dielectric_constraints no)\n\t\t)\n\t\t(pad_to_mask_clearance 0)\n\t)\n")
    lines.append('\t(net 0 "")\n')
    # outline: the snapshot's rectangle
    xs = [p[0] for p in snap["outline"]]
    ys = [p[1] for p in snap["outline"]]
    lines.append(
        '\t(gr_rect (start %s %s) (end %s %s) (stroke (width 0.1) (type solid)) (fill no) (layer "Edge.Cuts") (uuid "%s"))\n'
        % (mm(min(xs)), mm(min(ys)), mm(max(xs)), mm(max(ys)), uid("outline"))
    )
    for k, c in enumerate(snap["copper"]):
        lines.append(zone_text(c["layer"], c["polygon"], c.get("holes", []), "%d/%s" % (k, c["layer"])))
    for k, v in enumerate(snap.get("vias", [])):
        lo = v.get("from", copper[0])
        hi = v.get("to", copper[-1])
        lines.append(
            '\t(via (at %s %s) (size %s) (drill %s) (layers "%s" "%s") (net 0) (uuid "%s"))\n'
            % (mm(v["x"]), mm(v["y"]), mm(v["diameter"]), mm(v["drill"]), lo, hi, uid("via", k))
        )
    for k, p in enumerate(snap.get("ports", [])):
        a = p["a"]
        text = "%s: %s to %s, %g ohm, width %s mm (port: add in the plugin, not in the board)" % (
            p["name"],
            p["layer"],
            p.get("layer_b", "in-plane"),
            p["impedance"],
            mm(p["width"]),
        )
        lines.append(
            '\t(gr_text "%s" (at %s %s) (layer "Cmts.User") (uuid "%s") (effects (font (size 1 1) (thickness 0.15))))\n'
            % (text, mm(a[0]), mm(a[1]), uid("port", k))
        )
    lines.append('\t(gr_text "Copper-only board made from %s by scripts/gerber/snapshot_to_kicad.py" (at %s %s) (layer "Cmts.User") (uuid "%s") (effects (font (size 1 1) (thickness 0.15))))\n'
                 % (source_name, mm(min(xs)), mm(min(ys) - 2e-3), uid("note")))
    lines.append(")\n")
    return "".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("snapshot")
    ap.add_argument("out")
    ap.add_argument("--project", action="store_true", help="also write a minimal .kicad_pro next to OUT")
    a = ap.parse_args()
    snap = json.load(open(a.snapshot))
    out = pathlib.Path(a.out)
    out.write_text(board_text(snap, pathlib.Path(a.snapshot).name))
    if a.project:
        pro = out.with_suffix(".kicad_pro")
        pro.write_text(json.dumps({"meta": {"filename": pro.name, "version": 3}}, indent=2) + "\n")
    zones = len(snap["copper"])
    print("wrote %s: %d zones, %d vias" % (out, zones, len(snap.get("vias", []))))


if __name__ == "__main__":
    main()
