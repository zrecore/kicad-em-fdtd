"""Reads the board open in KiCAD (IPC API, via kipy) into snapshot inputs. Read-only.

Needs a running KiCAD, so it is exercised by running tools/export_snapshot.py against a scratch copy of
a board rather than by unit tests. What can be tested without KiCAD lives in geometry.py and snapshot.py.
"""

import json
import math
import re
from pathlib import Path

from kipy.proto.board import board_pb2
from kipy.proto.board.board_types_pb2 import BoardLayer

from emfdtd_plugin import geometry, outline, snapshot
from emfdtd_plugin.units import nm_to_m

# Installed copy: <plugin>/data/ (written by tools/install.sh); repository checkout: <repo>/data/.
_ROOT = Path(__file__).resolve().parents[2]
DEFAULTS_PATH = next(
    (
        p
        for p in (_ROOT / "data" / "fab_profiles.json", _ROOT.parent / "data" / "fab_profiles.json")
        if p.is_file()
    ),
    _ROOT / "data" / "fab_profiles.json",
)


def layer_name(value):
    """BoardLayer enum value -> KiCAD's layer name, e.g. BL_F_Cu -> F.Cu, BL_In1_Cu -> In1.Cu."""
    return re.sub(r"_(Cu|Cuts)$", r".\1", BoardLayer.Name(value)[3:])


def load_defaults(path=DEFAULTS_PATH):
    return json.loads(Path(path).read_text())


def _xy(v):
    return [nm_to_m(v.x), nm_to_m(v.y)]


def _ring(polyline):
    return [_xy(n.point) for n in polyline.nodes if n.has_point]


def read_stackup(board, defaults, notes):
    """Top-to-bottom stackup entries. Missing material values come from data/fab_profiles.json, and say so."""
    fallback = defaults["materials"][defaults["profiles"][defaults["defaults"]["profile"]]["substrate"]]
    out = []
    for layer in board.get_stackup().layers:
        kind = board_pb2.BoardStackupLayerType.Name(layer.type)
        if kind == "BSLT_COPPER":
            out.append({"name": layer_name(layer.layer), "kind": "copper"})
        elif kind == "BSLT_DIELECTRIC":
            for i, sub in enumerate(layer.dielectric.layers if layer.dielectric else []):
                eps, tan_d = sub.epsilon_r, sub.loss_tangent
                if eps < 1:
                    eps, tan_d = fallback["epsilon_r"], fallback["loss_tangent"]
                    notes.append(
                        f"dielectric {i} has no permittivity in the KiCAD stackup: default {eps} used"
                    )
                entry = {
                    "name": f"dielectric{len(out)}.{i}",
                    "kind": "dielectric",
                    "thickness": nm_to_m(sub.thickness),
                    "epsilon_r": eps,
                    "loss_tangent": tan_d,
                    "material": sub.material_name,
                }
                out.append(entry)
        # silkscreen, solder mask and paste are not modelled (noted once by the caller)
    return out


def _edge_items(board):
    """Edge.Cuts shapes of the open board as outline.py items (nm)."""
    items = []
    for shape in board.get_shapes():
        if shape.layer != BoardLayer.BL_Edge_Cuts:
            continue
        kind = type(shape).__name__
        pt = lambda v: (float(v.x), float(v.y))  # noqa: E731
        if kind == "BoardSegment":
            items.append(("line", pt(shape.start), pt(shape.end)))
        elif kind == "BoardArc":
            items.append(("arc", pt(shape.start), pt(shape.mid), pt(shape.end)))
        elif kind == "BoardCircle":
            c, r = pt(shape.center), pt(shape.radius_point)
            items.append(("circle", c, math.hypot(r[0] - c[0], r[1] - c[1])))
        elif kind == "BoardRectangle":
            items.append(("rect", pt(shape.top_left), pt(shape.bottom_right)))
        elif kind == "BoardBezier":
            items.append(("bezier", pt(shape.start), pt(shape.control1), pt(shape.control2), pt(shape.end)))
        elif kind == "BoardPolygon":
            for poly in shape.polygons:
                items.append(("poly", [pt(n.point) for n in poly.outline.nodes if n.has_point]))
    return items


def read_outline(board, notes, seed=None):
    """(outline ring, cutout rings) in metres from Edge.Cuts: arcs, circles, slots and openings are kept.
    With `seed` (nm, a point on the board: the middle of the footprints) a panel frame is cut away at its
    breakaway tabs and the notes say so."""
    items = _edge_items(board)
    if not items:
        raise snapshot.SnapshotError("the board has no outline on Edge.Cuts")
    try:
        outer, cuts, more = outline.board_and_cutouts(items)
    except ValueError:  # no closed loop: fall back to the bounding box of the pieces, and say so
        boxes = [
            board.get_item_bounding_box(s) for s in board.get_shapes() if s.layer == BoardLayer.BL_Edge_Cuts
        ]
        x0, y0 = min(b.pos.x for b in boxes), min(b.pos.y for b in boxes)
        x1, y1 = max(b.pos.x + b.size.x for b in boxes), max(b.pos.y + b.size.y for b in boxes)
        notes.append("Edge.Cuts does not close into a loop: board outline replaced by its bounding rectangle")
        outer, cuts = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)], []
        more = []
    notes.extend(more)
    if seed is not None and cuts:
        proper = outline.board_proper(outer, cuts, seed)
        if proper:
            outer, cuts, more = proper
            notes.extend(more)
    ring = lambda r: [[nm_to_m(x), nm_to_m(y)] for x, y in r]  # noqa: E731
    return ring(outer), [ring(c) for c in cuts]


def read_copper(board, copper_layers, notes):
    """Tracks, arcs, pads (per owner footprint) and filled zones as polygons per copper layer."""
    names = {layer_name(v): v for v in BoardLayer.values() if layer_name(v).endswith(".Cu")}
    copper = []

    def add(layer, owner, net, polygon, holes=()):
        copper.append(
            {
                "layer": layer,
                "owner": owner,
                "net": net,
                "polygon": polygon,
                "holes": [list(h) for h in holes],
            }
        )

    for t in board.get_tracks():
        lname = layer_name(t.layer)
        if lname not in copper_layers:
            continue
        width = nm_to_m(t.width)
        if hasattr(t, "mid"):  # arc: approximate by short straight segments
            pts = geometry.arc_points(_xy(t.start), _xy(t.mid), _xy(t.end))
            for a, b in zip(pts, pts[1:]):
                add(lname, "", t.net.name, geometry.segment_ring(a[0], a[1], b[0], b[1], width))
        else:
            s, e = _xy(t.start), _xy(t.end)
            add(lname, "", t.net.name, geometry.segment_ring(s[0], s[1], e[0], e[1], width))

    refs = {fp.id.value: fp.reference_field.text.value for fp in board.get_footprints()}
    pads = list(board.get_pads())
    present = board.check_padstack_presence_on_layers(pads, [names[n] for n in copper_layers])
    for pad in pads:
        owner = refs.get(pad.parent.value, "") if pad.parent else ""
        for lname in copper_layers:
            if not present.get(pad, {}).get(names[lname], False):
                continue
            poly = board.get_pad_shapes_as_polygons(pad, names[lname])
            if poly is not None:
                add(lname, owner, pad.net.name, _ring(poly.outline), [_ring(h) for h in poly.holes])

    for zone in board.get_zones():
        filled = zone.filled_polygons
        if not filled:
            notes.append(f"zone '{zone.name}' has no fill in the open board (refill zones in KiCAD first)")
        for layer, polys in filled.items():
            lname = layer_name(layer)
            if lname in copper_layers:
                for poly in polys:
                    add(lname, "", zone.net.name, _ring(poly.outline), [_ring(h) for h in poly.holes])
    return copper


def _via_span(v):
    """The layers a blind or micro via joins as {"from", "to"}; {} for a through via or no span."""
    try:
        drill = v.padstack.drill
        first, last = layer_name(drill.start_layer), layer_name(drill.end_layer)
    except Exception:  # noqa: BLE001 - an API without drill layers must not stop the export
        return {}
    if {first, last} == {"F.Cu", "B.Cu"}:
        return {}
    return {"from": first, "to": last}


def read_vias(board):
    return [
        {
            "x": nm_to_m(v.position.x),
            "y": nm_to_m(v.position.y),
            "diameter": nm_to_m(v.diameter),
            "drill": nm_to_m(v.drill_diameter),
            "net": v.net.name,
            "owner": "",
            **_via_span(v),
        }
        for v in board.get_vias()
    ]


def _footprint_body(board, fp):
    """The footprint's bounding rectangle as a polygon in metres, or None if the API cannot give one."""
    try:
        box = board.get_item_bounding_box(fp)
    except Exception:  # noqa: BLE001 - an API without footprint boxes must not stop the export
        return None
    if box is None:
        return None
    x0, y0 = box.pos.x, box.pos.y
    x1, y1 = x0 + box.size.x, y0 + box.size.y
    return [[nm_to_m(x), nm_to_m(y)] for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1))]


_MOUNTING = {1: "through_hole", 2: "smd"}  # kipy FootprintMountingStyle (0 and 3: unknown or unspecified)


def _attributes(fp):
    """do-not-populate and mounting style (DNP is a KiCAD 8 or later attribute); {} if the API lacks them."""
    try:
        a = fp.attributes
        return {
            "dnp": bool(a.do_not_populate),
            "mounting": _MOUNTING.get(int(a.mounting_style), "unknown"),
        }
    except Exception:  # noqa: BLE001 - never stop the export for an attribute
        return {}


def _pads_by_footprint(board):
    """Pads per footprint id: number, net and board position in metres. Layers are not exported here: the
    copper polygons already carry them, and a two-terminal part's gap is found from the pad positions."""
    out = {}
    try:
        pads = list(board.get_pads())
    except Exception:  # noqa: BLE001
        return out
    for pad in pads:
        parent = pad.parent.value if getattr(pad, "parent", None) else None
        if parent is None:
            continue
        out.setdefault(parent, []).append(
            {
                "number": str(pad.number),
                "net": pad.net.name,
                "x": nm_to_m(pad.position.x),
                "y": nm_to_m(pad.position.y),
            }
        )
    return out


def read_components(board, notes=None):
    """Footprints with their bounding rectangle as an optional body outline (no height: KiCAD's API does not
    report one, so the solver side supplies a default), their pads and their DNP and mounting attributes.
    The rectangle may include the footprint's text."""
    pads = _pads_by_footprint(board)
    out = []
    for fp in board.get_footprints():
        comp = {
            "ref": fp.reference_field.text.value,
            "value": fp.value_field.text.value,
            "layer": layer_name(fp.layer),
            "x": nm_to_m(fp.position.x),
            "y": nm_to_m(fp.position.y),
        }
        outline = _footprint_body(board, fp)
        if outline:
            comp["body"] = {"polygon": outline}
        attrs = _attributes(fp)
        if attrs:
            comp["attributes"] = attrs
        fid = fp.id.value if getattr(fp, "id", None) else None
        if fid in pads:
            comp["pads"] = pads[fid]
        out.append(comp)
    if notes is not None and out:
        notes.append(
            "component bodies are the footprints' bounding rectangles (may include their text); "
            "heights are not available from KiCAD and come from a setting"
        )
    return out


def read_board(kicad, ports=(), defaults=None, refill_zones=False):
    """The snapshot dict for the board currently open in `kicad` (a kipy KiCad)."""
    defaults = defaults or load_defaults()
    board = kicad.get_board()
    if refill_zones:  # changes the open board in memory (never saved by us): only when the user asks
        board.refill_zones(block=True)
    notes = ["silkscreen, solder mask and paste layers are not modelled"]
    stackup = read_stackup(board, defaults, notes)
    copper_layers = [e["name"] for e in stackup if e["kind"] == "copper"]
    fps = list(board.get_footprints())
    seed = None
    if fps:  # the middle of the footprints is on the board even when a panel frame carries a few fiducials
        seed = (
            sorted(fp.position.x for fp in fps)[len(fps) // 2],
            sorted(fp.position.y for fp in fps)[len(fps) // 2],
        )
    board_outline, cutouts = read_outline(board, notes, seed)
    copper, vias, components = (
        read_copper(board, copper_layers, notes),
        read_vias(board),
        read_components(board, notes),
    )
    if any("panel frame excluded" in n for n in notes):  # what lies on the frame is not part of the model
        copper = [c for c in copper if outline.keeps(board_outline, c["polygon"])]
        vias = [v for v in vias if outline.inside(board_outline, (v["x"], v["y"]))]
        components = [c for c in components if outline.inside(board_outline, (c["x"], c["y"]))]
    return snapshot.build_snapshot(
        board.name,
        str(kicad.get_version()),
        board_outline,
        stackup,
        copper,
        vias,
        components,
        ports,
        notes,
        cutouts,
    )
