---
name: gerber-to-model
description: Turn a vendor's manufacturing package (Gerber, Excellon, readme, DXF) into a snapshot this project's solver can run, so an antenna or RF structure is simulated as the vendor built it instead of as a guess of it. Use when a vendor's reference design is only available as Gerbers, when a model should reproduce the vendor's own board (outline, stackup, ground), or to check a simulation against a vendor's published figures.
---

# Vendor Gerbers to a solver model

Worked example (private, if present): TI's CC2430DB for the SWRU120 inverted-F antenna, `out/antennas/swrr034c/` (recipe, snapshot and runs; `docs/PLAN.md`, "TI's CC2430DB antenna, modeled unchanged"). Tools, tracked: `scripts/gerber/` (`gerber.py` reader and rasterizer, `trace.py` raster to polygons, `gerber_to_snapshot.py` recipe builder, `selftest.py`; needs numpy).

## Before anything
1. **Ask before downloading** (filename, source, size). Do not log in, accept terms or click through anything for the user. A link may sit inside the vendor's PDF as an annotation that the text layer hides: read it from the local PDF (`/URI` entries), do not guess a URL.
2. **License.** Vendor terms such as "only for developing an application with our products" mean: keep the files in git-ignored `out/`, never commit or paste them, say so in `docs/PLAN.md`. A recipe holds vendor coordinates: keep it private too.
3. **Untrusted data.** List the archive first (`unzip -l`, look for `/` or `..` in names), extract into its own new empty folder (nested archives the same), read files as data, run Python on them with `-I`, execute nothing from the package.

## Read the package before drawing anything
- The readme often gives the stackup (the board the vendor measured on, not what the app note says): layer thicknesses, Dk, copper weight, minimum trace. Typos happen ("0.35 um Cu" meant 35 um): say what you assumed. A loss tangent is rarely given: mark it assumed.
- Find the outline. It may be a dedicated layer or a loop of strokes inside the drill drawing; `first_closed_loop` takes the first loop.
- Units and format are in the file (`%FSLAX24Y24*%`, G70, `%MOIN*%`); Excellon zero suppression and digits are in the drill report, not always in the file. Check one hole against the outline.
- **Look at every layer** (`gerber.Canvas(...).render(ops)` then `write_png`). Plane layers are the trap: the readme's "positive" can be copper drawn as thick strokes, as regions, or only the clearances. On the worked board the inner ground plane started 32 mm below the antenna edge: no plane under the antenna, which no text said.

## Recipe and build
`python3 scripts/gerber/gerber_to_snapshot.py recipe.json out.json [--erase LAYER:x0,y0,x1,y1] [--fill LAYER:x0,y0,x1,y1]` (the docstring is the schema). Decisions that matter:
- **What to keep.** The voxelizer's cost grows with shapes and holes. Raster at 0.05 mm on the antenna layer, 0.1 mm on planes; drop shapes under about 0.3 to 1 mm2 and holes under 0.25 mm2. State what was left out (a partial VCC plane, the chip, parts, the balun).
- **No nets in Gerbers.** Every shape has net ''. Vias are kept only where they land in a large copper region (a ground pour) of each layer in `ground_in_layers`: 58 of 337 on the worked board. Signal vias are dropped, not guessed.
- **The port.** Cut the feed line where the model should end it (`erase`) and put a vertical port from the feed's layer to the layer with ground under it. Check by raster that the feed has copper at the port and the reference layer has ground there, and that the inner layer between has none you did not intend.
- Variants for controls come from the same recipe with `--erase` (remove the antenna: open) and `--fill` (a copper tie to ground: short).

## Then, in this order
1. `fdtd-control-check`: the open and the short on this port before the device run (the worked board: 0.8 and 162 degrees). A mid-line port reads as an antenna's S11 and is wrong; the first "valid" model of the project was exactly that.
2. Dry run: grid size, run notes (a port whose terminals are joined by copper is expected for a shorted inverted-F).
3. The device run, with a refinement box over the antenna of 0.2 mm cells or finer (six cells across a 1.2 mm arm) and the feed at 0.06 mm: the default coarse mesh put one antenna's resonance 4.5 percent low. Re-run at half the cell before quoting a frequency.
4. Report against the vendor's published figures (read the plot itself: render the PDF page with `pdftoppm -r 110 -png -f N -l N`, the text only gives round numbers) with the unfitted result first, then sensitivities (Dk, copper model) labeled as sensitivities. Never tune a parameter until the numbers agree and call that validation.

## Limits to state every time
Copper is a zero-thickness sheet unless `copper_model = slab` (a half to one percent in frequency for 35 um copper); solder mask is not modeled; the Gerber is simplified to about the raster pixel; vias, parts and a layer you left out are not in the model. Rotated rectangular pads (macro primitive 21) and obround brushes are handled; step-and-repeat, aperture blocks and mirrored images are not (the reader says so).
