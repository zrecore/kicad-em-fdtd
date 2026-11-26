---
name: vendor-reference-board
description: Build a KiCad project (board, schematic, libraries) from a chip vendor's reference layout (Altium or other files), swap in an antenna, and check it before the plugin or solver sees it. Use when asked for a known-working reference design, to import a vendor layout into KiCad, or to generate a board with a script instead of by hand.
---

# Vendor reference board in KiCad

Worked example, if present in this checkout: `boards/nrf52840-w3008/` and `boards/nrf52840-ifa/` (git-ignored, with READMEs).

## Before anything
1. **Ask the user which repositories or folders to look in first** (their own hardware, a parts library, earlier boards), and whether anything there is read-only or must never be copied into this project; follow what they say. Do not assume any particular repository. Then the vendor. Pages are often behind a bot check: never work around it; ask the user to download and say where to put the file (`out/<name>/download/`).
2. **Licence.** No licence file means private: the folder is git-ignored and `docs/PLAN.md` says so. TI-style "use only for developing an application with our products" terms: same. Never publish without asking the vendor.
3. **Check the package.** The vendor may publish a different package from the user's chip (QIAA vs CKAA): the RF match differs. Say so in the README.

## Import
- PCB: KiCad's own Altium importer through KiCad's bundled Python (`<KiCad.app>/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 -I`, `pcbnew.PCB_IO_MGR.Load(ALTIUM_DESIGNER, ...)`). Keeps the stack, nets and vendor rules.
- Schematic: `kicad-cli` has no Altium schematic import. The user does File > Import > Non-KiCad Schematic in the GUI into `out/<name>/sch-import/`.
- Ignore Altium rules that are finer than your fab: write the vendor's real minima into the `.kicad_pro` (clearance, track, via, hole, edge).

## Generating the board with a script (KiCad Python traps)
- `ZONE.SetOutline()` takes ownership and double-frees: edit `z.Outline()` in place (`RemoveAllContours`, `NewOutline`, `Append`). A new keepout's polygon once reused the freed memory and left two pours silently empty.
- Keep removed objects referenced (`RemoveNative`, a `KEEP` list) or iterating `Zones()` segfaults.
- `ZONE_FILLER` needs a `wx.App()`. The interpreter aborts at exit after saving: use `pcbnew.SaveBoard`.
- `LSET` has no `.count`: count `Seq()` copper layers.
- Footprints from KiCad's stock libraries via `FootprintLoad(dir, name)`; set pad nets by pin number with an assert on the expected side (rotation flips pads).
- A part whose copper is one piece with its ground (an inverted-F's legs): custom pad with `net_tie_pad_groups`, a leg pad per ground contact, `zone_connect 2`.
- Link each footprint to its schematic symbol (`SetPath`, `SetFPID`) and rename PCB nets from the schematic's own netlist (local labels carry a leading `/`), so parity and "update from schematic" agree. Write `fp-lib-table`.
- Schematic edits in plain Python on the `.kicad_sch` text: add stock symbols to `lib_symbols`, wire stubs and local labels, a symbols.json mapping reference to instance path and uuid for the board script.
- Regenerate, never hand-edit: scripts are the source, the `.kicad_pcb` is output.

## Checks (on a copy in `out/<name>/final/`, never `--save-board` the original)
1. `kicad-cli pcb drc --refill-zones --schematic-parity --severity-all`: 0 unconnected, no clearance or short errors. Read the inherited items once and list them in the README (dangling stubs, starved thermals, mask bridges, import warnings, NC pads).
2. Check zone fills after every generator change: filled-outline counts per pour, and the unconnected count.
3. Netlist against board pad nets (parity script): identical except single-pin NC nets and vendor inconsistencies (write those down).
4. Render F.Cu and look at it. A DRC pass does not show a wrong shape.

## Into the plugin
Open the copy in KiCad with the IPC API on, then `plugin/tools/export_snapshot.py` (see CLAUDE.md). Net names have the leading slash: `--nets /ANT_FEED --ground-nets GND`. `--part REF=open` stops the net walk crossing a 0R into the rest of the board. Then run the `fdtd-control-check` skill before trusting any S11, and put a `--refine` box around a coplanar feed (gap under a cell merges nets).

## Record
`docs/PLAN.md` (what, why, findings, open items), the board README (provenance table: vendor-qualified, datasheet-derived, invented), and the memory note.
