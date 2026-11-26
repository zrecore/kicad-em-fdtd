---
name: antenna-debug
description: Troubleshoot an antenna that is detuned, poorly matched, inefficient, or whose simulation and measurement disagree, starting from the symptom. Use when S11 is off band, a dip is shallow or double, a change does nothing, efficiency is poor in the product, or measurement and simulation differ.
---

# Antenna troubleshooting

Start from what you see; each row lists causes in the order to check them. Tools: `python3 scripts/antenna/match.py summary FILE.s1p --band F0 F1` to read a result, the Antenna tab for layout rules, the skills `antenna-matching`, `antenna-placement`, `antenna-reference-compare`, `fdtd-control-check`.

| Symptom | Check in this order |
|---|---|
| Dip at the wrong frequency, shape fine | 1. Surroundings not as the reference (enclosure, hand, battery, cable). 2. Ground plane size or shape differs from the vendor board. 3. Metal or copper in the keepout (any layer). 4. Board thickness or permittivity differs from the one the vendor tuned on. 5. A part's tolerance. Matching moves it only a little. |
| Right frequency, shallow dip | `summary` reading: R far from the reference (match it, see `antenna-matching`); or loss (a dip that radiates little: check efficiency); or the reference plane is not where you think (a cable or line rotates the locus). |
| Dip is deep but narrow | Antenna Q is high; matching cannot widen it. Larger antenna volume, a better ground, or accept it. |
| Two dips | Ground-plane or cable resonance next to the antenna's own; or a stub on the feed; or a second radiating path. Move or shorten the suspect and watch the second dip move. |
| Good S11, short range | Match is loss, not radiation: lossy substrate, lossy match part, antenna detuned by the case or a hand (S11 on the bench is free space), or poor ground. Measure radiated power or compare with a reference antenna in the same place. |
| Works on the bench, not in the product | Enclosure, battery, cables, hands, a nearby metal part. Re-measure inside the enclosure. |
| Simulation and measurement disagree | See below. |
| A change (part value, permittivity, geometry) does not change the simulated S11 | The part is not in the circuit or a feature is unresolved. Run `fdtd-control-check`: known loads (open and short), then a halved cell. Never report this as "no dependence". |

## Measurement pitfalls
- Calibrate at the plane you will interpret (SOL at the match pad or connector), not at the VNA port; cable flexing changes the result.
- A cable that radiates (no ferrite, no battery power) changes the antenna's ground; if moving the cable moves the dip, the measurement is of the cable too.
- A probe tap or U.FL connector adds its own stub and capacitance: leave the tap unfitted when measuring the antenna through the other port, or de-embed it.
- Measure with the same surroundings you will simulate.

## Simulation pitfalls (this project)
- Known-load control first (the `fdtd-control-check` skill): open must read about 0 degrees, short about 180 at zero frequency. A coplanar gap narrower than a cell joins nets on the grid: refine around the feed; read the run notes (nets touching, port terminals joined, floating port).
- A chip antenna's internals are usually unknown: simulated S11 is not predictive, only the rule checks and the feed are (see `antenna-reference-compare`).
- Convergence: halve the cell, widen the boundary, lengthen the decay before quoting a number.
- Ports: place the control port where the line ends; a port in a line sees both directions.

## Report
State the symptom, the evidence (S11 summary, impedance reading), the single most likely cause, the test that would confirm it (one change, one measurement), and what you assumed. Do not hand back a list of ten possibilities without ranking them.
