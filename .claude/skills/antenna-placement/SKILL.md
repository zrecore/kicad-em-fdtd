---
name: antenna-placement
description: Decide and check where an antenna goes on a board and what surrounds it: edge or corner, keepout, ground plane size, feed line, nearby metal, enclosure, hand, other radios. Use when laying out a board around an antenna, when asked why placement detunes it, or when reviewing a layout against a vendor's rules.
---

# Antenna placement

Helpers: `python3 scripts/antenna/lines.py lambda --f HZ [--er ER]` (wavelengths), `lines.py solve cpwg|microstrip --z0 50 --h MM --er ER [--gap MM]` (feed width for the stackup), `lines.py microstrip|cpwg ...` (check a drawn feed). Zero-thickness quasi-static, a few percent: a first look, not a field solver. `python3 scripts/antenna/cascade.py {steps,stub}` sizes a short neck, step or open stub on the feed by cascading ideal line sections (textbook ABCD matrices): a step or corner under about a twentieth of a wavelength is second-order (0.4 mm, 91 ohm neck: about 2 ohm of reactance at 2.44 GHz), an open stub of that length or more is not (6 mm: -16 dB; a quarter wave, about 18 mm on FR-4, shorts the line). No corner or junction capacitance is in it: check a real discontinuity with the field solver. Microstrip Z0 was checked against this project's solver (within about 2 percent on 50 and 87 ohm lines once the solver's air next to the board was resolved); the coplanar formula was only shown to be consistent with it. In the app, the Antenna tab does the board checks listed in `docs/PLAN.md` (keepout copper, ground, stackup) and a Feed line row: the width, side gap and ground plane of the copper leaving the part's signal pad, and its closed-form impedance against the port (the same formulas as `lines.py`).

## Rules that apply to nearly every small 2.4 GHz antenna
1. **Read the vendor's conditions before drawing:** keepout size and which layers, test-board ground size, free-space or in-case, matching parts, feed line type. A datasheet match holds only on the vendor's board. (`antenna-reference-compare` does the diff against a reference board.)
2. **Place at an edge or corner, with the radiating end outward,** as the datasheet draws it; metal on the outward side or under the antenna on any layer (including inner layers, shield cans, battery, USB shell) detunes it. The keepout is all layers unless the vendor says otherwise.
3. **Ground plane is part of the antenna.** For chip and inverted-F antennas the board's ground is the other half of a dipole: its length (a quarter wave is about 31 mm in air at 2.44 GHz, `lines.py lambda`) moves the resonance and the gain. A ground plane much smaller than the vendor's test board shifts the match; a much larger one rarely hurts. Keep ground unbroken between the feed and the antenna.
4. **Feed:** short, a controlled 50 ohm line (coplanar with ground or microstrip; `lines.py solve`), no stubs, via fence only outside the keepout, match parts at the feed. A feed on an inner layer or through vias adds inductance you must model.
5. **Surroundings:** plastic enclosures and hands lower the resonance (loading) and lower efficiency; metal enclosures change everything. Cables on a VNA test act as a radiating ground: use a ferrite or measure with the board battery-powered if the result moves when the cable moves.
6. **Other radios and noisy parts:** keep switching regulators, crystals and other antennas away; for two antennas, isolation depends on spacing and ground currents, simulate or measure it.

## Procedure
1. List the rules from the datasheet and from `data/antenna_rules.json` (or the project's own `em-fdtd/antenna_rules.json`).
2. Place; run the Antenna tab checks (keepout copper per layer, ground area and reach, via spans under the feed, stackup). Fix every finding or write down why it stays.
3. Check the feed width for the stackup with `lines.py`, and confirm the reference plane under it (it may be several layers down: keep the voids and the ground vias consistent).
4. Simulate with the antenna and its real surroundings (enclosure as metal or dielectric body, host ground), then without, to see how much the surroundings move it; run `fdtd-control-check` first.
5. Measure the first board; compare dip frequency and match with the simulation; then use `antenna-matching` and `antenna-debug`.

## Do not
Put copper, silkscreen-covered pads, vias or components in the keepout; route traces past the antenna; use the datasheet's S11 for a different ground size; trust a simulated S11 of a part whose internals are unknown (see `antenna-reference-compare`).
