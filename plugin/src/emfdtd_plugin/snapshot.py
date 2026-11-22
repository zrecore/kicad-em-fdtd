"""Builds and checks the board snapshot (docs/snapshot.md); the C++ reader is src/board/snapshot.cpp."""

FORMAT = "kicad-em-fdtd.snapshot"
VERSION = 1


class SnapshotError(ValueError):
    pass


def build_snapshot(
    board_name, kicad_version, outline, stackup, copper, vias, components, ports=(), notes=(), cutouts=()
):
    """Lengths in metres, layers top to bottom; `copper` items: dict(layer, owner, net, polygon, holes)."""
    snap = {
        "format": FORMAT,
        "version": VERSION,
        "board": {"name": board_name, "kicad_version": kicad_version},
        "outline": outline,
        "stackup": list(stackup),
        "copper": list(copper),
        "vias": list(vias),
        "components": list(components),
        "ports": list(ports),
        "notes": list(notes),
    }
    if cutouts:
        snap["cutouts"] = [list(c) for c in cutouts]
    validate_snapshot(snap)
    return snap


def validate_snapshot(snap):
    """Raise SnapshotError, naming the field, if the snapshot would not be accepted by the C++ reader."""
    for key in ("format", "version", "board", "outline", "stackup", "copper", "vias", "components"):
        if key not in snap:
            raise SnapshotError(f"missing field '{key}'")
    if snap["format"] != FORMAT or snap["version"] != VERSION:
        raise SnapshotError("unsupported format or version")
    if len(snap["outline"]) < 3:
        raise SnapshotError("outline needs at least 3 points")
    names = set()
    for layer in snap["stackup"]:
        if layer["kind"] == "dielectric":
            if not layer["thickness"] > 0 or not layer["epsilon_r"] >= 1:
                raise SnapshotError(f"stackup layer '{layer['name']}': need thickness > 0 and epsilon_r >= 1")
        else:
            names.add(layer["name"])
    if not any(layer["kind"] == "dielectric" for layer in snap["stackup"]):
        raise SnapshotError("stackup has no dielectric layer")
    for shape in snap["copper"]:
        if shape["layer"] not in names:
            raise SnapshotError(
                f"copper on layer '{shape['layer']}' which is not a copper layer of the stackup"
            )
        if len(shape["polygon"]) < 3:
            raise SnapshotError("copper polygon needs at least 3 points")
