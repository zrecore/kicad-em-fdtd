# What we trust, and how far (the trust ledger)

One page, kept current: every claim about the solver and the antenna model with its status, its number, and the evidence.
`docs/PLAN.md` holds the narrative and the runs; this page holds the verdicts. Update it whenever a result lands.

Statuses: **established** (reproduced, understood, tested), **provisional** (measured, but one more run or check could move
it), **open** (not yet measured or not explained), **refuted** (we believed it and were wrong).
The model under test: the TI CC2430DB inverted-F antenna built from TI's Gerbers (`ti_nol3_nodrop.json`), Bluetooth profile,
slab copper, medium mesh, series resonance (reactance zero crossing, 8.2 MHz bins). Last updated 2026-11-27.

## Established

| Claim | Number | Evidence |
|---|---|---|
| The RTX reproduces the Mac | 4e-7 relative on S11 | `nd_med_01` on both (PLAN, farm cross-device control) |
| The two Apple Vulkan drivers agree | 3e-8 | `nd_kk_01` against the MoltenVK run |
| A resumed run is bit-identical on the same machine | identical in tests, and a real farm job paused by `agent stop` at step 71 424 and resumed (RTX, GPU checkpoint format 2) gave an S11 file byte-identical to the uninterrupted run (2026-11-27) | `checkpoint_*` tests; `cpp_load_b8` against `cpp_load_resume_b8`; earlier resumed jobs came out 0.04 % from their neighbors (`fb05_dk42_bd75w`, `fb05_dk42_bd100`) |
| Three GPUs from three vendors agree on the current build | full S11 of the coarse 2.4 GHz patch (12 544 steps, 101 points): Apple M1 Pro, RTX 4060 Ti and Radeon Pro 555X within 3e-8 (complex); all 19 non-hidden GPU test cases pass on the Mac (both Apple drivers) and on Windows (RTX and Radeon) | `out/validation/patchcheck/`; PLAN, 2026-11-27 |
| The time-step growth ratio does not matter | +-0.25 % | growth-ratio pair |
| Sheet-copper edges were decided by rounding (a defect, fixed) | patch resonance jumped up to 6 % with the box; a 2 nm shift of the copper moved it 2.8 % | known-answer study; fix `872d69b` with a test. The board runs use slab copper, which already used the robust test |
| The decay threshold does not matter at the converged mesh | 1e-3 and 1e-4 give 2.2856 and 2.2857 GHz at 0.5 mm, box 0.5 (0.004 %; 158 992 and 273 312 steps); the early -0.2 % was a coarse-mesh effect | `fb05_off000_bd50_dc4` |
| At 0.5 mm cells the box size no longer matters | Dk 4.5: 2.2829, 2.2856, 2.2838, 2.2855 GHz at boxes 0.25, 0.5, 0.75, 1.0 (spread 0.12 %) | `fb_ref05_*` |
| A Dk change shifts the resonance the right way and by a plausible amount | +2.2 to +2.3 % for Dk 4.5 to 4.2 at boxes 0.5 to 1.0 (a patch fully in the material would show 3.4 %) | `fb05_dk42_*` |
| Lumped loads prepare in time proportional to their edges | 238 load edges: 383 s before, 7.7 s after on a 3.5 M cell case, S11 byte-identical; a new test bounds it | fix `d97b6f3` (CI green with sanitizers; the full local check, GPU tests on both Apple drivers and on the RTX also pass) |
| A closed cavity is reproduced | 0.12 to 0.17 % below the exact 2.8826 GHz at 0.25 mm cells; Dk shift +3.48 to +3.56 % against the exact +3.51 % | known-answer cavity |
| openEMS does not depend on the box | 2.2231 GHz, R 55 ohm at margins 32 and 94 mm | `out/openems-case/recheck/` |
| The old "slope 13 against 31" was a definition mismatch | at its own resonance ours 20 to 21, openEMS 20 to 26 | `scripts/antenna/slope.py` |
| openEMS is a stable reference on the TI board | series resonance 2.2231, 2.3452, 2.2389, 2.2371 GHz at coarse cells 3.0, 1.5, 1.0, 0.75 mm (antenna box 0.4 mm, margin 32 mm): converged to 0.1 % from 1.0 mm on, R 55 to 58 ohm; the 1.5 mm rung is an outlier (grid-phase scatter); the 0.5 mm rung is still running | `out/openems-case/ladder/` (`match.py summary`) |
| The plugin's export reproduces the geometry of a complicated antenna | copper area per layer equal to six digits, 58 vias, outline | `cc2430db_copper*.kicad_pcb` read back through the plugin |

## Provisional

| Claim | Number | What would move it |
|---|---|---|
| The series resonance is about 2.28 GHz on the unshifted grid, and the in-plane mesh is converged | box 0.5: 2.3241 (default mesh), 2.2811 (1.0 mm), 2.2856 (0.5 mm), 2.2816 GHz (0.25 mm, `fb025_bd50w`): converged to 0.2 %, not monotonic | the offset runs below (about 2.30 GHz when the board is shifted) |
| Grid placement is worth +-0.5 % at 0.5 mm cells | the board shifted by 0, 0.10, 0.17, 0.33 mm: 2.2856, 2.3073, 2.3027, 2.3007 GHz (range 0.95 %). It is not mesh resolution (the unshifted grid sits at 2.282 for 1.0, 0.5 and 0.25 mm cells); a shift only changes how the coarse air grid outside the refinement box (1 mm margin) lines up | `fb05_off*_pad10`: the refinement box grown to a 10 mm margin, shifts 0 and 0.17 mm; if the scatter vanishes the cause is the air mesh next to the antenna |
| A box of 0.25 wavelengths is too small | Dk 4.2 at box 0.25 reads 0.7 % above the other boxes | rerun at 0.25 mm cells |

| Our solver against openEMS on a plain patch (the project's 60 x 60 mm sample, probe fed) | openEMS (v0.37.0 on the Windows PC, 1.0 mm cell, 0.76 M cells, 28 702 steps, 7 min): minimum -5.65 dB at 2.3510 GHz, Z 18.7+20.5j ohm. Ours (current build, farm GPUs, fine cell 1.0, 0.5, 0.35 mm): -6.01 dB at 2.2804, -5.80 dB at 2.2968, -5.96 dB at 2.3049 GHz, Z 18.6+16.2j to 19.7+20.6j ohm. openEMS refined to a 0.5 mm cell (2.07 M cells, 46 848 steps, 25 min): -5.53 dB at 2.3735 GHz, Z 19.1+23.1j ohm, +0.96 % from its 1.0 mm run. Both codes move UP with refinement, so the gap against openEMS stays about 3 % (3.0 % at 1.0 mm, 3.2 % at 0.5 mm for ours against its 0.5 mm; 2.9 % for our 0.35 mm). Neither is converged from two or three points. The 20 November value (2.18 GHz) is superseded by `872d69b`. openEMS's vertical resolution does not matter here: halving its largest vertical cell in the stack (0.16 to 0.08 mm, 83 to 107 z lines) leaves the minimum at 2.3510 GHz (-5.65 to -5.55 dB), so the unrefined z axis is not the cause of the gap (`win-result-fixed/z08`). Provisional: the meshes are not identical, and this is the S11 minimum, not a reactance zero | `out/validation/patchvsopenems/`, `/Volumes/FileTransfers/emfdtd/openems-ref-run/win-result/` |

## Open

| Question | State |
|---|---|
| The solver's device-memory estimate (28 bytes per node plus CPML slabs) against real GPU use | The farm keeps jobs off a GPU that is too small using 1.5 times this estimate against 80 percent of the GPU's memory; the 1.5 is a guess. The estimate leaves out the animation accumulators and the logs, and has never been compared with the memory a run really takes (no allocation counter in the solver). To do: read the real allocation (Vulkan memory budget or a counter in `vkcore`) on one big and one small case on each GPU, then set the margin from it. |
| Our resonance scatters with the coarse cell (2026-11-27, matched-cell farm runs) | the same TI model, antenna box 0.4 mm, the coarse cell swept 3.0, 1.5, 1.0, 0.75, 0.5 mm: series resonance 2.3877, 2.3119, 2.4403, 2.2908, 2.3188 GHz (R 67, 61, 43, 53, 52 ohm), about +-3 % with no trend; antenna box 0.2 mm: 2.4220, 2.3202, 2.3470 at 3.0, 1.5, 1.0. openEMS on the same ladder is flat to 0.1 % from 1.0 mm (2.2389, 2.2371) with one outlier (1.5 mm: 2.3452). So the earlier "converged to 0.2 %" holds for the antenna-box cell and the box size but not for the coarse cell, and this scatter (1 to 3 %) is of the size of the gap to openEMS (about 2.7 %). The dense sweep (12 values of `cells_per_wavelength`, 24 to 99, coarse cell 2.06 to 0.50 mm; `out/validation/densecpw/`): mean 2.2977 GHz, std 0.8 %, range 3.0 % peak to peak, no trend with coarse cell, cell count or its parity; one low outlier (cpw 60: 2.2587); for cpw 66 and finer 2.2906 to 2.3188 GHz (std 0.5 %), about 2.7 % above the converged openEMS value. The single run at cpw 49 (2.4403) and the neighboring cpw 48 (2.3046) differ by 5.9 %, so one run is not a reliable point. Moving the whole board by 0.25 and 0.5 mm changed cpw 24 by 0.0 % and cpw 60 by 0.3 %: it is not whole-board grid phase. Mesh-line dumps (`out/validation/gridlines/`): near the feed the lines of cpw 49, 54, 60 and 66 agree to about 1 um, so the feed box is not the cause. Refuted: the width of the 250 um strip at y 121.351 to 121.601 on the grid (212 to 325 um depending on cpw and a shift of the strip by -30 or +30 um) does not set the resonance: cpw 60 gives 2.2584, 2.2587 and 2.2588 GHz at strip widths 310, 212 and 222 um, cpw 66 gives 2.2908 at both 325 and 310 um, and at equal widths (310 um) the two cpw still differ by 1.4 % (`out/validation/stripwidth/`; the other strip's width is constant). Open suspects: the other copper edges, the air margin and where the outer mesh lines fall (the boundary distance is 0.25 wavelengths, a fixed length whose lines move with the coarse cell), the y growth ratio fixed at 1.3, and a finer antenna box (0.125 mm run pending). Also noted: `lateral_growth_ratio` applies to x only, y is fixed at 1.3 (`voxelize.cpp`). Flags: `cells_per_wavelength` 16, 33, 49, 66, 99 (3.09 to 0.50 mm), fine cell 0.12 mm at the feed, margin 0.25 wavelengths | `out/validation/matchedcell/` |
| Why we differ from openEMS by 2 to 3 % in the resonance | on the TI board ours (2.28 GHz) is 1.9 % above the converged openEMS value (2.238); on the patch ours is about 3 % below openEMS (2.30 against 2.37 GHz at the finest cells, both still rising). The sign differs between the two structures, so it is not one global offset such as a fixed port inductance; and the quantities differ (a reactance zero crossing, an S11 minimum). Earlier note: | suspects: the lumped port and feed model, openEMS's own mesh error at 0.4 mm, and how both codes put a mesh line exactly on copper edges (the thirds rule is untried). Fork K is running the port controls and the matched-cell comparison on exact-answer structures. Found 2026-11-26: our openEMS case refines only the antenna box (0.4 mm) and uses 3 mm cells over the rest of the board, while our solver uses 0.5 mm over the whole board, so the comparison was not like for like; a mesh ladder (`out/openems-case/ladder/`, coarse cell 1.0, then 0.75 and 0.5 mm) is queued on the CPU slot. Online reports of openEMS behavior: feed-line mesh sensitivity (one user saw +-30 ohm), lumped ports are not ideal launchers, nothing that explains a 3 % shift |
| Impedance level: R, dip depth, Q | moved 2 to 4 times with the box on the default mesh; not rechecked at the converged mesh |
| Sensitivities at the converged mesh (thickness, copper model, mask, loss tangent) | measured only at coarse mesh and box 0.25 |
| The open-versus-short known-load control on real hardware | failed on the nRF52840 IFA board, cause not found; the coupon's open, short and load siblings test it. In the solver: the first coupon controls (`cpn_*`) were void because the hand-made snapshot had no `pads` for R1, so `--part` silently did nothing (open, short and load agree to 1.3e-6); the solver now says so ("part R1 ignored: ... the snapshot lists 0 pads", tested). With pads (`cpp_*`, `cal_pads_launch.json`) the short reads 0.95 and the 49.9 ohm load 0.38 in the 2.4 to 2.5 GHz mean |S11| (best match -12.3 dB, Z 45.4 - 23.4j at 2.203 GHz): the load absorbs. The open case (`cpp_open_b8`, Mac GPU run, b202252) reflects at 0.94 to 0.96 with its phase about 177 deg from the short's at 2.2 GHz; no run exceeds |S11| 0.96 (passive). Not yet established: the expected value for a load across that gap (no model written, so this is a plausibility check, not an agreement); the via short reads as a short (183.6 deg at f=0) |
| Comparison with a measurement | none yet; the OSH Park coupon set is being designed (prediction is in PLAN before any measurement) |
| The real material | FR-4 Dk is known to a few tenths; a 0.3 change moves the resonance 2 to 3 %. This is an input uncertainty, not a solver error, and it limits any absolute prediction |
| Effects not modeled | ENIG finish loss, solder mask in 3-D (about 0.5 to 1 % on the radiator by a 2-D estimate), the cable, parts |
| Earlier sheet-copper results (before `872d69b`) | not rechecked; treat as unvalidated |

## Refuted or corrected

| We believed | Truth |
|---|---|
| The box-size dependence is near-field physics | it is mesh and grid error: it appears on a closed cavity, and openEMS is flat |
| The Dk 4.5 to 4.2 shift changes sign with the box | the sign flip was mesh scatter; at 0.5 mm the shift is always positive |
| A 0.25 mm refinement costs 8 times the cells | 1.5 times (32.9 M against 21.7 M cells): the board is 105 cells thick and the 35 um copper sets the time step |
| The trusted band was about 2.37 GHz +-2 % | superseded by the 0.5 mm series above |

## The current statement, in one paragraph

Reproducible to about +-0.5 to 1 % in the series resonance for the antenna-box cell, grid placement and box size, but it
scatters about +-3 % with the coarse cell (2026-11-27), which is the larger problem; trustworthy for comparing variants and for the sign and size of a Dk change. Not yet established: the absolute
frequency (a 2 to 3 % gap to openEMS on two structures, of different sign, is unexplained), the impedance level, and agreement with a real board.
