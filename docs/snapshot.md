# Board snapshot format (version 1)

The one file the KiCAD plugin writes and the native app (`emfdtd`) reads. JSON, **all lengths in metres**, written by `plugin/src/emfdtd_plugin/kicad_reader.py` (reader of the open board through the IPC API) or by a script (`plugin/tools/make_patch_snapshot.py`). C++ reader: `src/board/snapshot.cpp`; Python check: `snapshot.validate_snapshot`. A change to the format bumps `version`; the reader refuses versions it does not know, naming the field.

| Field | Meaning |
|---|---|
| `format` | `"kicad-em-fdtd.snapshot"` |
| `version` | `1` |
| `board` | `{name, kicad_version}` |
| `outline` | polygon `[[x, y], ...]` (v1 writes the bounding rectangle of Edge.Cuts and says so in `notes`) |
| `cutouts` | optional: polygons `[[x, y], ...]`, holes in the board (routed slots, openings); no dielectric inside them. Copper never covers a hole (zone fills already exclude it) |
| `board_proper` | optional: `{"outline": ring, "cutouts": [rings]}`, the board without its panel frame when the exporter found one (`outline` and `cutouts` stay as drawn). The setting `board_outline` (proper or whole) picks which one the model uses; with `proper`, copper, vias and parts off that ring are dropped |
| `stackup` | top to bottom: `{name, kind: "copper"}` or `{name, kind: "dielectric", thickness, epsilon_r, loss_tangent, material}`. Copper is a zero-thickness PEC sheet on the dielectric interface in the model by default (the setting `copper_model = slab` makes a layer that has a `thickness` fill z cells of that thickness instead: the outer top layer above its interface, the outer bottom one below it, an inner one just above its interface); a copper entry may carry `thickness` (m, the finished copper; absent or 0 = unknown), which only the feed-line impedance estimate uses; solder mask, silkscreen and paste are not modelled |
| `copper` | filled polygons: `{layer, owner, net, polygon, holes?}`. `owner` is the footprint reference for pads, `""` for tracks, zones and other board-level copper |
| `vias` | `{x, y, diameter, drill, net, owner, from?, to?}`; modelled as PEC columns between the outermost copper layers, or between the copper layers named in `from` and `to` (a blind or micro via; either order). The plugin exporter writes through vias only so far |
| `components` | `{ref, value, layer, x, y, body?}`: what the user can select. `body` is `{polygon, height?, offset?}`: the footprint outline on the board plane (v1 writes its bounding rectangle) and optionally its height in metres. Without a height the run's "Component body height" setting applies. Bodies are used only if the "Component bodies" setting is not `ignore`. Also optional: `pads` = `[{number, net, x, y}]` (board position; a part is two-terminal when it has exactly 2 pads on 2 different nets) and `attributes` = `{dnp, mounting}` (`dnp` is KiCAD's do-not-populate flag, KiCAD 8 or later: not fitted, so open between the pads; `mounting` is `smd` or `through_hole`) |
| `ports` | optional. In-plane: `{name, layer, a, b, width, impedance}` across a gap from `a` (positive) to `b`. Vertical: `{name, layer, layer_b, a, width, impedance}` from `layer` (positive) to `layer_b` |
| `notes` | what the exporter simplified, copied into the run summary |

The component selection is not part of the snapshot: it is applied by the voxelizer (`board::Selection`), so the same snapshot serves any choice. Board-level items (`owner == ""`) are always in the model.

Material defaults used when KiCAD's stackup has no permittivity come from `data/fab_profiles.json` and are named in `notes`.
