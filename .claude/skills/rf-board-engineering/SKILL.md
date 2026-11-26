---
name: rf-board-engineering
description: The judgement layer for RF work on a board with this project's tools: what to trust, in what order to check it, which effects are big and which are small, what a fab's stackup does to an antenna, and where each detailed skill or script lives. Use at the start of any antenna, feed-line, match or stackup question, before believing a simulated S11, or when choosing between simulating, calculating and measuring.
---

# RF board engineering with this project

Detail lives in the skills named below and in `docs/PLAN.md`; this is the order of work and the numbers worth remembering. Numbers marked (example) were measured on one board and say how big an effect can be, not what it is on yours.

## 1. Trust order (do not skip steps to reach the answer sooner)
1. **Read the run notes** before any plot (`fdtd-control-check` step 0): nets joined on the grid, a port whose terminals are shorted or floating, narrow gaps near a port, bodies, vias. A via note now says whether the board really shorts or only a coarse grid joins a legal 0.15 mm gap.
2. **Known loads on the port** (`fdtd-control-check`): the same feed and port, open and short; phase of S11 at f = 0 near 0 and near 180 degrees. A port in the middle of a feed with a long stub behind it gave a plausible, wrong S11 for hours. A model is "valid" only after this, never by label.
3. **Mesh convergence**: refine over the antenna (0.2 mm cells, six across a 1.2 mm arm), rerun at half the cell. (example) the default coarse mesh put an inverted-F resonance 4.5 percent low; 0.2 to 0.1 mm moved it 0.4 percent.
   Snap check: a traced (Gerber) radiator is merged into a large pour and used to miss the edge snap; since 2026-11-24 refine boxes snap long edges of any shape. Use `lateral_growth_ratio=1.1` in antenna boxes. A combined model (slab copper, bodies, inner layers) must be converged separately from the base model: the TI model still drifted 1.8 percent from 0.075 to 0.05 mm. Study feed lines on a narrow ground plane (a wide board biases the two-length fit about 4 percent).
4. **Say what is not in the model**: copper is a sheet unless `copper_model = slab`, no solder mask, parts and the chip often absent, loss tangent assumed.
5. **Report unfitted results first**; sensitivities (Dk, copper, ground length) labelled as such. A parameter tuned until it agrees is a fit, not a validation.

## 2. Size an effect before modelling it
- Feed line impedance: `lines.py` closed forms (microstrip about 2 percent against the solver; zero-thickness coplanar reads 6 to 9 percent high against real copper). The Antenna tab's Feed line row shows both the closed form and a field solve with the copper's thickness; a published fab number is a free check (OSH Park's 15 mil on 7.87 mil FR408HR: they say 49.8 ohm; closed form 54.7, solve with 45.7 um copper 51.0).
- Steps, necks and stubs: `scripts/antenna/cascade.py`. A step or corner under a twentieth of a wavelength is second-order (0.4 mm, 91 ohm neck: about 2 ohm of reactance at 2.44 GHz); an open stub of that length or more is not (6 mm: -16 dB; a quarter wave, about 18 mm on FR-4, shorts the line). The feed report names widths, other parts' pads and turns on the line.
- The solver's own line accuracy: by two-length extraction (`line_extract.py`) the FDTD effective permittivity of a microstrip is 5 to 10 percent high at 0.1 to 0.2 mm lateral cells (2.0 mm line: 3.40 at 0.1 mm, 3.56 at 0.2 mm, quasi-static 3.22): use lateral cells of at most a twentieth of the strip width along lines whose electrical length matters (it still reads 5.7 percent high at that size; the extrapolated limit is about 4 percent above quasi-static and unexplained, an open solver question), and rerun at half the cell. A right-angle corner on a 63 ohm line is a 0.17 pF shunt capacitance and takes 2 mm (one strip width) off the electrical length along the centre lines; a mitre of about 70 percent (cut leg 1.4 strip widths) cancels the capacitance (`bend_extract.py`).
- Closed forms are good: microstrip and grounded-coplanar closed forms agree with an independent 2D solve to 0.3 percent (eeff) over wide ranges (`docs/methods/microstrip-closed-forms.md`); the Bode-Fano limit is proved in `docs/methods/bode-fano.md` (a deep match spends bandwidth).
- Matching and bandwidth: `antenna-matching` and `match.py` (resonances, L and pi networks); the Bode-Fano limit says a deep single-frequency match spends bandwidth.

## 3. Which lever moves which number (example, inverted-F, TI SWRU120)
- Dielectric constant and thickness under the arm set the **resonance**: OSH Park's FR408HR 4-layer (Dk about 3.6 to 3.9) 2.334 GHz against 2.226 GHz on a 1.44 mm FR4-like stack, PCBWay FR4 (Dk 4.6 to 4.7) 2.190 GHz.
- Ground length and shape behind the antenna set the **resistance** more than the frequency: 40, 20 and 10 mm of ground behind the arm gave 19.7, 24.2 and 35.5 ohm at nearly the same resonance.
- Copper thickness (slab model, 35 um): +0.65 percent in frequency and no change in resistance on the IFA test board, +2.0 percent and +10 ohm on TI's thin-dielectric four-layer board: size it for your stackup before calling it small.
- On TI's own board rebuilt from its Gerbers with nothing fitted: dip -26.9 dB at 2.236 GHz, R 55.5 ohm at resonance, 261 MHz below -10 dB, against TI's measured plot (Figure 2-8): -24 dB at about 2.42 GHz, about 295 MHz. Depth and width agree, the frequency is 7.6 percent low; Dk 4.5 to 4.2 moves it only 0.8 percent (`docs/PLAN.md` has the sensitivities). Read vendor plots from the figure, not from the text's round numbers.

## 4. Fabs and stackups
Take the fab's own published stackup into a profile (`data/fab_profiles.json`, every value with a dated source and an `assumed` list) and read the fab's stackup picture, not only the page text: OSH Park's diagram gives the core Dk (3.87) and finished copper (1.80 mil outer) that its page text does not. PCBWay states base and finished outer copper separately and offers custom stackup and impedance control (+-10 percent) as a service; OSH Park publishes controlled geometries as design data. A fab's thickness tolerance (about 10 percent) is a few tenths of a percent to a percent and a half in an antenna's frequency: design with margin or tune on the real board.

## 5. Where things are
- Vendor reference layout in KiCad: `vendor-reference-board`. Vendor Gerbers into a model: `gerber-to-model`. Part files (datasheet, footprint, STEP): `antenna-part-sources`. Compare two boards: `antenna-reference-compare`. Placement and keepout: `antenna-placement`. Debug a detuned antenna: `antenna-debug`. Match networks: `antenna-matching`.
- GPU runs only under `with_gpu_lock.sh`; heavy CPU jobs through `scripts/with_cpu_lock.sh` (CLAUDE.md). GPU driver checks for shader changes: `gpu-driver-check`.
- Methods without a paper in reach are derived in-house and written up under `docs/methods/` (first line says so); cite only papers read in full with a Crossref-checked DOI (`always verify DOIs`).
- Planned, not built: multi-port S-parameters (`docs/methods/multiport-sparameters.md`).
