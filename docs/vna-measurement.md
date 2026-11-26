# Measuring a board's antenna with a NanoVNA (a checklist, not a vendor manual)

Written 2026-11-21 from general practice and the notes in `docs/PLAN.md` ("Reference-guided antenna tuning": TI SWRA726 and the Nordic and Ezurio advice). Menu names differ between NanoVNA models and firmware: follow your unit's own manual where they differ. Nothing here was tested on the user's unit.

## 0. Can it reach 2.4 GHz?
Many units sold as "NanoVNA" stop at 900 MHz, or reach 1.5 GHz and rely on harmonics above that, which is much less accurate. The antenna band here is 2.4 to 2.5 GHz. Check the maximum frequency printed on the case or in the firmware's stimulus menu. Models that reach about 3 GHz or 4.4 GHz natively are fine. If yours does not, a reading at 2.4 GHz is not trustworthy.

## 1. What you need
- The NanoVNA, a USB cable, the supplied calibration standards (open, short, load) and the short SMA cable.
- A U.FL (also called IPEX) to SMA pigtail, and a U.FL connector soldered on the board's feed pads.
- A way to save the trace: the unit's own memory, or the NanoVNA-Saver program, which writes Touchstone `.s1p` files.

## 2. Make the board measurable (antenna alone)
- The point is to see the antenna and its match without the radio. The radio chip's feed must not load the trace: remove the DC-block and the matching parts between the radio and the antenna (or cut the line), and put the U.FL at the plane where the match will sit, so the VNA looks into the match and the antenna. TI's note says to measure where the match goes and to take the match parts off while measuring.
- Keep the board in the real setting: seated in the adapter and plugged into the host, because the adapter and the host are part of its ground.

## 3. Set the sweep
STIMULUS: start 2.0 GHz, stop 3.0 GHz, at least 101 points. (Wider shows the resonance, narrower is easier to read; 2.4 to 2.5 GHz is the band of interest.)

## 4. Calibrate (one port, on port 1 / CH0)
1. Fit the cable on CH0 and the standards on its end.
2. CAL, then RESET, then CALIBRATE: connect OPEN and press OPEN; SHORT and press SHORT; LOAD and press LOAD. For a one-port reading skip ISOLN and THRU or press DONE after LOAD. Then DONE and SAVE to a slot (for example 0).
3. Check: with the load fitted, the dB trace should sit very low (deep below -30 dB) and the Smith trace should be a dot at the centre.
4. Calibrate at the end of the cable that the pigtail attaches to (SMA). The pigtail, from SMA to the U.FL, is then an extra length between the reference plane and the board. Remove its effect with the electrical delay: DISPLAY, SCALE, ELECTRICAL DELAY, and adjust until an open pigtail end (nothing connected) shows a trace that stays at the right-hand point of the Smith chart instead of spiralling. A U.FL calibration kit would avoid this step.

## 5. Measure
1. Attach the pigtail to the board's U.FL. Keep hands and the cable still: a hand near the board detunes it.
2. Read S11 in dB (the return loss) over 2.4 to 2.5 GHz: -10 dB or lower is a good match; 0 dB means all the power is reflected.
3. Also read the Smith chart and the impedance marker at 2.44 GHz: this gives the resistance and reactance that a match network has to correct.
4. If the reading changes when the cable moves, the cable shield is carrying current and radiating: a ferrite on the cable near the board helps.
5. SAVE the trace, and export a `.s1p` (NanoVNA-Saver: File, Export, Touchstone).

## 6. What to bring back
- The `.s1p` file (put it in `out/` in this project), the sweep range, and a line saying how the board sat (in the adapter, plugged into what).
- Then the match can be fitted from the measured impedance (pi or L network from E-series values) and compared with the simulation. A measured reading of the board's real surroundings is worth more than any simulated S11 of the chip.
