# Multi-port S-parameters from lumped ports in the FDTD solver

*In-house methods note, 2026-11-23. **Provenance: derived here from circuit identities; no paper was consulted and none is cited.** The user cannot obtain the usual papers, so the method is derived, stated so it can be checked, and tied to tests the solver can run. Status: a plan with a derivation; nothing in it is built (docs/PLAN.md, "Multi-port S-parameters").*

## 1. What the solver already does

A lumped port is a voltage source `Vs` in series with a resistor `R` (the port's reference impedance) across a gap in the copper. With `I` the current the port sends into the structure,

    V = Vs - R I                                                        (1)

where `V` is the voltage across the gap. The one-port result in `src/fdtd/sparam.hpp` is `S11 = 2 V / Vs - 1`. This note shows that it is the diagonal case of a general rule, and gives the off-diagonal terms.

## 2. Definitions

Phasors use the engineering convention x(t) = Re{X e^{j w t}}, which is what the solver's Fourier transform (kernel e^{-j w t}) produces; a wave moving in +z varies as e^{-j beta z}. For port *i* with real reference `R_i`, define the waves

    a_i = (V_i + R_i I_i) / (2 sqrt(R_i)),    b_i = (V_i - R_i I_i) / (2 sqrt(R_i))        (2)

These are definitions, chosen so that the power flowing into the structure through port *i* is (peak phasors)

    P_i = 1/2 Re(V_i I_i*) = 1/2 (|a_i|^2 - |b_i|^2)                    (3)

Check of (3): |V + R I|^2 - |V - R I|^2 = 4 R Re(V I*), so |a|^2 - |b|^2 = Re(V I*). The scattering matrix is `b = S a`, so `S_ij = b_i / a_j` with every other incident wave zero.

## 3. Extraction at every port from one drive

Substitute (1) into (2). The incident wave at a port is set by its own source alone:

    a_i = Vs_i / (2 sqrt(R_i))                                          (4)
    b_i = (2 V_i - Vs_i) / (2 sqrt(R_i))                                (5)

so `a_i = 0` exactly when port *i* has its source off, whatever the structure does: an undriven port is just its resistor `R_i`, which is the matched termination S-parameters require. Drive port *j* only (`Vs_j != 0`, all other `Vs_i = 0`):

    S_jj = b_j / a_j = 2 V_j / Vs_j - 1                                  (6)
    S_ij = b_i / a_j = 2 V_i sqrt(R_j) / ( sqrt(R_i) Vs_j )   (i != j)      (7)

(6) is the existing one-port formula, as it must be. With equal references, (7) reads `S_ij = 2 V_i / Vs_j`. **Nothing beyond the voltage across each port's gap is needed**; the current `I_i` drops out. One run (one driven port) gives one column of S.

### Time stamps and the frequency transform
The solver stores the port voltage after step n at time (n+1) dt and the source at (n+1/2) dt. `s11FromPort` transforms each series with its own time stamps; (6) and (7) use the same transforms, `V_i(f) = dt sum_n v_i[n] e^{-j 2 pi f (n+1) dt}` and `Vs_j(f)` from `sourceSpectrum`, so the half-step offset is handled once, the same way for every entry.

## 4. Consequences, each a check the solver can run

* **Reciprocity.** A passive structure without ferrites has S = S^T. With the `sqrt(R)` scaling of (2) this holds for unequal references too. A measured |S_ij - S_ji| well above the noise is a finding (an unequal port, a mesh asymmetry, a port that is not where it was meant to be).
* **Passivity and power.** From (3), the power into the structure is (1/2) sum_i (|a_i|^2 - |b_i|^2) >= 0, i.e. the largest singular value of S is at most 1. For one driven port, sum_i |S_ij|^2 = 1 - (fraction lost to dissipation and radiation). A lossless closed structure gives exactly 1; an open board gives less, and the shortfall is the radiated plus dissipated power (which the solver's accepted-power and far-field output measure independently: a cross-check).
* **A matched through line.** A lossless uniform line of length l and impedance R_1 = R_2 gives S21 = e^{-j beta l}, S11 = 0, so |S21| = 1 and the phase falls by 360 degrees per guided wavelength. The existing phase-slope fit (`phase_fit.py`) already measures that slope for one port.
* **Terminations.** Port 2 left open or shorted, or ended in a resistor, fixes S11 of port 1 through the cascade of the line between them (`scripts/antenna/cascade.py`): a second, independent check of the port 2 geometry.
* **Isolation floor.** Two ports on unconnected structures far apart should read S21 falling with distance; the floor, where it stops falling, is the numerical dynamic range of the run (float32 fields, truncated ring-down). That floor is measured, not assumed, and the preview says it.

## 5. Moving the reference plane

A lumped port sits where the model puts its gap, usually on a short line before the pad. If a lossless line of length l_i and impedance equal to R_i joins port *i* to the plane of interest, the waves at that plane are `a'_i = a_i e^{-j beta_i l_i}` and `b'_i = b_i e^{+j beta_i l_i}`, so

    S'_ij = S_ij e^{+j (beta_i l_i + beta_j l_j)}                        (8)

(check: an open at the pad plane reads S11 = e^{-j 2 beta l} at the port plane, and (8) gives it back as 1). beta comes from the line's effective permittivity; the field solve in `src/fdtd/line2d.*` provides it (and the line's impedance, which must equal R_i for (8) to hold). This ignores dispersion, bends and the junction at the plane; the discontinuity sizing in `scripts/antenna/cascade.py` says how much that can matter. Without an explicit plane, only the port plane is meaningful and the output must say so.

## 6. What this does not cover
* Modal (waveguide) ports for non-TEM structures: not needed for lumped feeds on a PCB.
* Parasitics of the port gap itself (its stray capacitance and inductance): they are part of "the port" as modelled, the same limit the one-port result has, measured by the open and short controls.
* Frequency dependence of `R`: the references are real constants, as in the existing ports.
* Time truncation: the run must continue until the voltage at **every** port has decayed, not only the driven one; the stopping rule has to look at all ports.

## 7. Cost
N driven ports need N runs of the same grid and mesh (one source on, the rest off). Reciprocity can halve this for a two-port when the user asks for it; it is never assumed silently. All GPU runs go through the shared lock.

## 8. Validation plan (scenarios first, tolerance in the THEN)
1. CPU: a lossless uniform line of known length between two ports, matched ends: |S21| within 0.05 of 1, |S11| below -20 dB, phase slope within 5 percent of 720 l sqrt(eps_eff) / c degrees per GHz.
2. Reciprocity: |S12 - S21| below 0.02 on an asymmetric two-port (a line with a step).
3. Passivity: the largest singular value of S at most 1 + 0.02 over the band on every scenario.
4. Port 2 open, short and matched against the one-port cascade prediction of port 1's S11.
5. One driven port of an N-port model reproduces the one-port result of the same board to numerical identity.
6. GPU against CPU, within 1 percent in |S_ij| above the noise floor.

## References
No paper. The power-wave definition (2) and the scattering matrix are standard textbook material (for example D. M. Pozar, *Microwave Engineering*, the chapter on network analysis; a book, no DOI) and are used here only as definitions; every other statement above is derived from (1) to (3).
