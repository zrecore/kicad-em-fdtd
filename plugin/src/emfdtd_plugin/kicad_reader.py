"""Reads the board open in KiCAD (IPC API, via kipy) into snapshot inputs. Read-only.

Needs a running KiCAD, so it is exercised by running tools/export_snapshot.py against a scratch copy of
a board rather than by unit tests. What can be tested without KiCAD lives in geometry.py and snapshot.py.
"""

import json
import re
from pathlib import Path

from kipy.proto.board import board_pb2
from kipy.proto.board.board_types_pb2 import BoardLayer

from emfdtd_plugin import geometry, snapshot
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


def read_outline(board, notes):
    """Bounding rectangle of everything on Edge.Cuts (non-rectangular outlines are simplified, and noted)."""
    boxes = []
    for shape in board.get_shapes():
        if shape.layer == BoardLayer.BL_Edge_Cuts:
            boxes.append(board.get_item_bounding_box(shape))
    if not boxes:
        raise snapshot.SnapshotError("the board has no outline on Edge.Cuts")
    x0 = min(b.pos.x for b in boxes)
    y0 = min(b.pos.y for b in boxes)
    x1 = max(b.pos.x + b.size.x for b in boxes)
    y1 = max(b.pos.y + b.size.y for b in boxes)
    notes.append("board outline simplified to its bounding rectangle")
    pts = [[x0, y0], [x1, y0], [x1, y1], [x0, y1]]
    return [[nm_to_m(x), nm_to_m(y)] for x, y in pts]


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


def read_vias(board):
    return [
        {
            "x": nm_to_m(v.position.x),
            "y": nm_to_m(v.position.y),
            "diameter": nm_to_m(v.diameter),
            "drill": nm_to_m(v.drill_diameter),
            "net": v.net.name,
            "owner": "",
        }
        for v in board.get_vias()
    ]


def read_components(board):
    return [
        {
            "ref": fp.reference_field.text.value,
            "value": fp.value_field.text.value,
            "layer": layer_name(fp.layer),
            "x": nm_to_m(fp.position.x),
            "y": nm_to_m(fp.position.y),
        }
        for fp in board.get_footprints()
    ]


def read_board(kicad, ports=(), defaults=None, refill_zones=False):
    """The snapshot dict for the board currently open in `kicad` (a kipy KiCad)."""
    defaults = defaults or load_defaults()
    board = kicad.get_board()
    if refill_zones:  # changes the open board in memory (never saved by us): only when the user asks
        board.refill_zones(block=True)
    notes = ["silkscreen, solder mask and paste layers are not modelled"]
    stackup = read_stackup(board, defaults, notes)
    copper_layers = [e["name"] for e in stackup if e["kind"] == "copper"]
    return snapshot.build_snapshot(
        board.name,
        str(kicad.get_version()),
        read_outline(board, notes),
        stackup,
        read_copper(board, copper_layers, notes),
        read_vias(board),
        read_components(board),
        ports,
        notes,
    )
