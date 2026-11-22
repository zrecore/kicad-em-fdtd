---
name: antenna-reference-compare
description: Compare a user's antenna board with a reference (vendor test board or known-good design) to find and correct tuning problems, and model a part whose internals are unknown. Use when asked why an antenna performs badly, to compare two designs, to check keepout or ground size against a datasheet, or to fit a chip-antenna model to datasheet S11.
---

# Reference-guided antenna comparison

Method and rationale are in `docs/PLAN.md` ("Reference-guided antenna tuning"); this is the working order. Principle: a chip antenna is tuned by its ground plane and keepout as much as by the part, and a datasheet match holds only on the vendor's test board. State accuracy honestly: the aim is a best-effort model with its assumptions listed, never a guarantee.

0. **Get the part's files** (datasheet, footprint, 3D model) with the `antenna-part-sources` skill and cross-check them.
1. **Read the datasheet's conditions first:** test-board size, keepout (all layers), feed line, matching parts, free-space or not, and the revision. Distributor pages quote different numbers for the same part; take the numbers from the datasheet PDF and check the impedance markers reproduce the stated dB values (`Gamma = (Z-50)/(Z+50)`).
2. **Tier 1, rules and geometry diff (no simulation).** For the user's board in the antenna frame (u along the pad row, v from the board edge into the board): copper of every layer inside the keepout (tracks, pours, vias, pads, other parts' pads), distance of the pad row to the board edge, ground-plane size and shape against the vendor board, feed line characteristic impedance from the stackup (closed form for coplanar line with ground), via spans (through, blind, micro) under the feed. Read the board file read-only; never write to the user's design files. Report each difference with its likely effect (detuning, efficiency, match).
3. **Tier 2, same-setup two-board simulation.** Same solver settings, same port, same chip model on the reference and the user board; compare S11 overlays and the field/current views. Differences between the two runs are more trustworthy than either absolute number, because the chip model's errors largely cancel.
4. **The chip model when internals are unknown.** Use the vendor's 3D model for the metal and body shapes, treat the ceramic's permittivity as the one fitted unknown (a fitted value is ours, not the vendor's), assume lossless unless measured, and label every assumption in the run notes. Fit on the reference board so the datasheet's S11 points and the impedance markers are reproduced; if no permittivity does, say so and fall back to a calibrated black-box (Tier 3) rather than forcing a fit.
5. **Validate first, then fit.** Do not start a fit before `fdtd-control-check` passes. Screen on a cropped board, then run the best case on the full reference board at default settings, then halve the cell. Record settings with each result.
6. **Report:** what matched, what did not, which assumptions carry the result, what a VNA measurement of the user's board (at the match plane, with a stated calibration) would settle, and the single most likely correction (keepout copper to remove, ground to extend, a pi match to add and where).

Never copy a user's board data into this repository (the example boards are read-only inputs and some are private); keep notes about them in git-ignored `out/` and write generic wording in `docs/PLAN.md`.
