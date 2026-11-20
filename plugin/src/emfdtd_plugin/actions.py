"""The two KiCAD actions' logic (the entry scripts are thin): both export the open board, read-only."""

from kipy import KiCad
from kipy.board_types import Pad

from emfdtd_plugin import geometry, kicad_reader, launcher
from emfdtd_plugin.units import nm_to_m


def pad_box(snapshot, board_pad, refs):
    """Bounding box and layer of a selected pad's copper in the snapshot."""
    owner = refs.get(board_pad.parent.value, "") if board_pad.parent else ""
    cx, cy = nm_to_m(board_pad.position.x), nm_to_m(board_pad.position.y)
    for shape in snapshot["copper"]:
        if shape["owner"] == owner and shape["net"] == board_pad.net.name:
            x0, y0, x1, y1 = geometry.bbox(shape["polygon"])
            if x0 <= cx <= x1 and y0 <= cy <= y1:
                return (x0, y0, x1, y1), shape["layer"]
    raise RuntimeError(f"pad {owner}:{board_pad.number} has no copper in the snapshot")


def port_from_selection(board, snapshot):
    """A port across the gap between exactly two selected pads, or None if two pads are not selected."""
    pads = [item for item in board.get_selection() if isinstance(item, Pad)]
    if len(pads) != 2:
        return None
    refs = {fp.id.value: fp.reference_field.text.value for fp in board.get_footprints()}
    (box_a, layer), (box_b, _) = pad_box(snapshot, pads[0], refs), pad_box(snapshot, pads[1], refs)
    return geometry.port_between(box_a, box_b, layer)


def run(plugin_dir, require_port):
    """Simulate: needs two pads and starts the run. Open window: shows the preview; uses two pads if selected,
    else keeps the previous port."""
    kicad = KiCad()
    board = kicad.get_board()
    snapshot = kicad_reader.read_board(kicad)
    port = port_from_selection(board, snapshot)
    if port is None and require_port:
        raise SystemExit(
            "Select exactly two pads that define the port (RF pad first), then run the action again."
        )
    project_dir = board.get_project().path  # the folder holding the .kicad_pro
    snapshot["ports"] = [port] if port else launcher.previous_ports(project_dir)
    launcher.publish(snapshot, project_dir, plugin_dir, request="run" if require_port else "preview")
