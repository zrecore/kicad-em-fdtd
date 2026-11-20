"""Read-only probe: what does the KiCAD 10 IPC API expose about the open board?

Run with KiCAD's pcbnew open on a board:  .venv/bin/python probe/dump_board.py [out.json]
Never modifies the board. Each call is wrapped so one missing feature does not hide the others.
"""

import json
import sys

from kipy import KiCad


def attempt(fn):
    try:
        return {"ok": True, "value": fn()}
    except Exception as e:  # noqa: BLE001 - a probe reports every failure kind
        return {"ok": False, "error": f"{type(e).__name__}: {str(e)[:160]}"}


def describe(obj, names):
    return {n: attempt(lambda n=n: str(getattr(obj, n))[:120]) for n in names}


def main(out_path):
    kicad = KiCad()
    board = kicad.get_board()
    report = {"kicad_version": str(kicad.get_version()), "board": board.name}

    stackup = attempt(board.get_stackup)
    if stackup["ok"]:
        st = stackup["value"]
        report["stackup_attrs"] = [a for a in dir(st) if not a.startswith("_")]
        layers = attempt(lambda: list(st.layers))
        report["stackup_layers"] = (
            layers["error"]
            if not layers["ok"]
            else [
                {
                    a: str(getattr(layer, a, None))[:80]
                    for a in ("name", "type", "thickness", "material_name", "epsilon_r", "loss_tangent")
                }
                for layer in layers["value"]
            ]
        )
    else:
        report["stackup"] = stackup

    report["copper_layer_count"] = attempt(board.get_copper_layer_count)
    report["nets"] = attempt(lambda: [n.name for n in board.get_nets()])
    report["counts"] = {
        "footprints": attempt(lambda: len(board.get_footprints())),
        "pads": attempt(lambda: len(board.get_pads())),
        "tracks": attempt(lambda: len(board.get_tracks())),
        "vias": attempt(lambda: len(board.get_vias())),
        "zones": attempt(lambda: len(board.get_zones())),
        "selection": attempt(lambda: len(board.get_selection())),
    }

    fps = attempt(board.get_footprints)
    if fps["ok"] and fps["value"]:
        fp = fps["value"][0]
        report["footprint_attrs"] = [a for a in dir(fp) if not a.startswith("_")]
        report["footprint_sample"] = describe(
            fp, ["reference_field", "value_field", "position", "orientation", "layer", "definition"]
        )
        defn = attempt(lambda: fp.definition)
        if defn["ok"]:
            report["footprint_definition_attrs"] = [a for a in dir(defn["value"]) if not a.startswith("_")]
            report["footprint_models"] = attempt(
                lambda: str(getattr(defn["value"], "models", "no 'models' attribute"))[:200]
            )

    zones = attempt(board.get_zones)
    if zones["ok"] and zones["value"]:
        z = zones["value"][0]
        report["zone_attrs"] = [a for a in dir(z) if not a.startswith("_")]
        report["zone_sample"] = describe(z, ["layers", "net", "filled", "name", "priority", "clearance"])
        report["zone_filled_polygons"] = attempt(lambda: str(z.filled_polygons)[:160])

    vias = attempt(board.get_vias)
    if vias["ok"] and vias["value"]:
        report["via_sample"] = describe(
            vias["value"][0], ["position", "diameter", "drill_diameter", "type", "net", "padstack"]
        )

    pads = attempt(board.get_pads)
    if pads["ok"] and pads["value"]:
        report["pad_sample"] = describe(
            pads["value"][0], ["number", "position", "net", "pad_type", "padstack"]
        )
        report["pad_polygons"] = attempt(
            lambda: str(board.get_pad_shapes_as_polygons(pads["value"][:1]))[:160]
        )

    report["bounding_box"] = attempt(
        lambda: str(board.get_item_bounding_box(board.get_footprints()[0]))[:160]
    )
    report["origin"] = attempt(lambda: str(board.get_origin(0)) if False else "see get_origin(origin_type)")

    with open(out_path, "w") as f:
        json.dump(report, f, indent=2, default=str)
    print(json.dumps({k: report[k] for k in ("kicad_version", "board", "counts")}, default=str)[:600])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "probe_report.json")
