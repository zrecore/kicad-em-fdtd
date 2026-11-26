---
name: antenna-matching
description: Design and check an impedance match for an antenna (L, pi or T network), read an S11 or impedance and decide what to change, and judge whether a match is real or only loss. Use when S11 is off band, when asked what pi-network values to try, when placing match parts, or before fitting parts on a board.
---

# Antenna matching

Helpers (standard library only, in the repository): `python3 scripts/antenna/match.py {summary,lmatch,eval,search}`, `python3 scripts/antenna/lines.py`, self-test `python3 scripts/antenna/selftest.py`. They work on any `.s1p` or on a typed impedance; nothing is specific to one board. They model ideal parts: results are starting points to be checked with the real parts' data.

## 1. Fix the reference plane first
S11 means nothing without the plane it refers to. Note where it was measured or simulated (the match pad, the connector, the chip pin, a port inside the board), the reference impedance, and any cable or line between that plane and the antenna feed. A match network placed at a different plane than the one you optimised in gives different numbers; a line of length l rotates the locus by 2 beta l. Put the network as close to the antenna feed as the layout allows, with its shunt parts grounded by short vias.

## 2. Read the load
`match.py summary FILE.s1p --band F0 F1` prints the deepest dip, the -10 dB band, the best and worst in your band, and the impedance in the middle of the band with a plain-language reading (inductive or capacitive, R above or below the reference). Decide which problem it is:
- **Resonance in the wrong place** (X crosses zero outside the band): a match network only moves it by a small amount and costs bandwidth; fix the antenna (length, tuning stub, ground, keepout, surroundings) first. See `antenna-debug` and `antenna-placement`.
- **Resonance in band, R off** (a dip that is shallow because R is far from the reference): a match network is the right tool.
- **Dip shallow and wide, impedance near the real axis but R small**: possibly loss (a lossy substrate, a lossy match part) or a good antenna with high loss resistance. A dip made by loss looks like a match and radiates nothing: check the radiated power or efficiency, not only |S11|.
Matching trades bandwidth for match, and there is a hard limit. R. M. Fano, "Theoretical limitations on the broadband matching of arbitrary impedances", MIT Research Laboratory of Electronics Technical Report 41, 2 January 1948 (hdl.handle.net/1721.1/5007; no DOI; read in full) states that for a given load and a given maximum reflection there is an upper limit to the bandwidth a lossless network can reach, and gives Bode's result for a load of a resistance R in parallel with a capacitance C: the integral of ln(1/|reflection|) over frequency cannot exceed pi/(RC), so with a constant |reflection| over a band of width w (rad/s) the product of w and ln(1/|reflection max|) is at most pi/(RC) (eqs. 3 and 4). Two practical consequences stated there: (1) a deep match at one frequency wastes that area and narrows the band, so aim for a moderate match across the band (for example below -10 dB over the whole band) and not a null at the centre; (2) matching exactly at several frequencies does not give a good match across the band between them. The report assumes a lossless lumped network and a given load; losses and distributed parts are corrections applied afterwards. Its appendix works a resonant quarter-wave antenna (30 ohm radiation resistance, resonance 10 MHz, Q of 10) matched to 50 ohm over a 3 MHz band with under 1 dB loss, which needs a four-element network: a narrow antenna needs more parts for the same band and has less to give. The journal version (J. Franklin Institute 249, 57-83, 1950, doi:10.1016/0016-0032(50)90006-8, DOI checked on Crossref) is paywalled and was not read.

## 3. Candidates
- One frequency, two parts: `match.py lmatch --f HZ --z R+jX` lists the L networks (series then shunt, or shunt then series) with L and C values, load side first. Which topology exists follows from R < Z0 or G < 1/Z0.
- A band, pi or T slots: `match.py search --s1p FILE --f0 HZ --band F0 F1 --topology shunt,series,shunt` tries E12 values (0.2 to 10 pF, 0.5 to 20 nH, or no part) in each slot and ranks by the worst |S11| in the band. Lowest part count wins ties. A different slot list gives other networks (`series,shunt`, `shunt,series`).
- Compare by hand: `match.py eval --s1p FILE --net "shunt C0.5p; series L2n" --band F0 F1` (listed from the load outward; suffixes f p n u m).
Prefer the least change: the first values to try are the smallest parts that move the locus the right way; a 0 ohm series part and unfitted shunt pads on the board give room for all pi and L variants without a respin (as the reference boards in `boards/` do).

## 4. Real parts
- Inductors: Q of 0402 parts at 2.4 GHz is tens, not hundreds (read the datasheet); the ESR dissipates power, so a match that needs a high-value series L loses efficiency. Estimate the loss before choosing, and prefer a topology with a shunt L to ground (less current) or a capacitive one when both work.
- Self-resonance and tolerance: a small C (0.5 pF) with 0.1 pF tolerance moves the match; pad capacitance and the land pattern add to it. Use the manufacturer's S-parameter files for the final check; those files have terms: the user supplies them, never commit them.
- Layout: keep the network small, on the feed line, with an unbroken ground reference under it; each extra millimetre of track is inductance.

## 5. Verify, one change at a time
Measure at the network's plane with a calibration there (SOL at that plane); change one part, re-measure. If the VNA connects through a probe tap or connector, know its stub and leave it unfitted when measuring the antenna. In simulation: run the antenna alone to a Touchstone file, evaluate networks offline with `eval` (fast, valid while the network is separated from the antenna by a known plane), then confirm the chosen one with one full run using lumped parts (`--part REF=r..,l..,c..`). Run the `fdtd-control-check` skill before trusting the simulated load.
