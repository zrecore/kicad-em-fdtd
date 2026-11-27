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
| The plugin's export reproduces the geometry of a complicated antenna | copper area per layer equal to six digits, 58 vias, outline | `cc2430db_copper*.kicad_pcb` read back through the plugin |

## Provisional

| Claim | Number | What would move it |
|---|---|---|
| The series resonance is about 2.28 GHz on the unshifted grid, and the in-plane mesh is converged | box 0.5: 2.3241 (default mesh), 2.2811 (1.0 mm), 2.2856 (0.5 mm), 2.2816 GHz (0.25 mm, `fb025_bd50w`): converged to 0.2 %, not monotonic | the offset runs below (about 2.30 GHz when the board is shifted) |
| Grid placement is worth +-0.5 % at 0.5 mm cells | the board shifted by 0, 0.10, 0.17, 0.33 mm: 2.2856, 2.3073, 2.3027, 2.3007 GHz (range 0.95 %). It is not mesh resolution (the unshifted grid sits at 2.282 for 1.0, 0.5 and 0.25 mm cells); a shift only changes how the coarse air grid outside the refinement box (1 mm margin) lines up | `fb05_off*_pad10`: the refinement box grown to a 10 mm margin, shifts 0 and 0.17 mm; if the scatter vanishes the cause is the air mesh next to the antenna |
| A box of 0.25 wavelengths is too small | Dk 4.2 at box 0.25 reads 0.7 % above the other boxes | rerun at 0.25 mm cells |

## Open

| Question | State |
|---|---|
| The solver's device-memory estimate (28 bytes per node plus CPML slabs) against real GPU use | The farm keeps jobs off a GPU that is too small using 1.5 times this estimate against 80 percent of the GPU's memory; the 1.5 is a guess. The estimate leaves out the animation accumulators and the logs, and has never been compared with the memory a run really takes (no allocation counter in the solver). To do: read the real allocation (Vulkan memory budget or a counter in `vkcore`) on one big and one small case on each GPU, then set the margin from it. |
| Why we sit about 3 % above openEMS (about 2.29 against 2.223 GHz) | suspects: the lumped port and feed model, openEMS's own mesh error at 0.4 mm, and how both codes put a mesh line exactly on copper edges (the thirds rule is untried). Fork K is running the port controls and the matched-cell comparison on exact-answer structures. Found 2026-11-26: our openEMS case refines only the antenna box (0.4 mm) and uses 3 mm cells over the rest of the board, while our solver uses 0.5 mm over the whole board, so the comparison was not like for like; a mesh ladder (`out/openems-case/ladder/`, coarse cell 1.0, then 0.75 and 0.5 mm) is queued on the CPU slot. Online reports of openEMS behavior: feed-line mesh sensitivity (one user saw +-30 ohm), lumped ports are not ideal launchers, nothing that explains a 3 % shift |
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

Converged and reproducible to about +-0.5 to 1 % in the series resonance (mesh, grid placement, box), pending the 0.25 mm
run; trustworthy for comparing variants and for the sign and size of a Dk change. Not yet established: the absolute
frequency (a 3 % gap to openEMS is unexplained), the impedance level, and agreement with a real board.
