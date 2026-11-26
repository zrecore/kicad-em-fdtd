# The Bode-Fano limit for a parallel RC load: statement, proof and numerical check

*In-house methods note, 2026-11-23. **Provenance:** the statement is Bode's, as Fano states it in R. M. Fano, "Theoretical Limitations on the Broadband Matching of Arbitrary Impedances", MIT Research Laboratory of Electronics Technical Report No. 41 (1948), which was read in full; the journal version (J. Franklin Institute 249, 1950, DOI 10.1016/0016-0032(50)90006-8; no abstract reachable, full text paywalled) is unread. The proof below was written here from standard network theory (positive-real functions, the scattering matrix of a lossless two-port, Cauchy's theorem) and checked numerically; it follows the idea TR-41 uses (a Cauchy integral of ln(1/|reflection|) over the right half plane), not its text. The numbers are this project's.*

## 1. Claim
A lossless matching network N sits between a generator of resistance R0 and the load R in parallel with C. Let Gamma(jw) be the reflection coefficient at the generator port, normalised to R0. Then

    integral from 0 to infinity of ln(1/|Gamma(jw)|) dw  <=  pi / (R C)                (1)

with equality when Gamma has no zeros in the open right half plane and N adds no shunt capacitance at the load. Consequence (Fano's Eq. 4): if |Gamma| <= Gamma_max over a band 0..wc, then

    wc * ln(1/Gamma_max)  <=  pi / (R C).                                              (2)

(2) follows from (1) because ln(1/|Gamma|) >= 0 everywhere (passivity) and >= ln(1/Gamma_max) on the band. A deep match at one frequency spends area that a wider band could have used; a flat match over the band wastes none.

## 2. Proof
**Step 1: move to the resistor's side.** Put the load capacitor inside the network: N' = N followed by a shunt C across the output terminals, terminated by the resistor R. N' is a lossless reciprocal two-port with a resistive termination at each port (R0 and R). With the scattering matrix S' of N' normalised to R0 and R, S' is unitary on the jw axis, so |S'11| = |S'22| there (1 - |S'11|^2 = |S'21|^2 = 1 - |S'22|^2). The quantity in (1) is Gamma = S'11 (the generator port sees N' ended in its reference resistance R). Let G(s) = S'22 be the reflection looking into N' from the resistor with the generator side ended in R0. Then |G(jw)| = |Gamma(jw)| and (1) can be proved for G.

**Step 2: G is analytic and bounded in the right half plane.** The impedance Z(s) looking into N' from the resistor's port is positive real (a passive network), so G(s) = (Z(s) - R) / (Z(s) + R) is analytic in Re s > 0 with |G(s)| <= 1 there.

**Step 3: the behaviour at infinity.** Write Y(s) = 1/Z(s) = s C_tot + Y_rest(s), where s C_tot is the part of the admittance that grows like s: the load's own C plus any shunt capacitance N presents at its output port, so C_tot >= C, and Y_rest(s) = O(1) as s -> infinity (a positive-real admittance of a finite network grows at most like s). Then Z(s) = 1/(s C_tot) (1 + O(1/s)) and

    -G(s) = (1 - Z/R) / (1 + Z/R) = 1 - A/s + O(1/s^2),    A = 2 / (R C_tot) <= 2/(R C).        (3)

**Step 4: remove the zeros.** Let s_k = sigma_k + j omega_k, sigma_k > 0, be the zeros of G in the right half plane and B(s) = product of (s - s_k)/(s + conj(s_k)) the Blaschke product: |B(jw)| = 1, B(s) = 1 - 2 (sum of sigma_k)/s + O(1/s^2). G~ = G/B has no zeros and, by the maximum modulus theorem, |G~| <= 1 in the half plane, with |G~(jw)| = |G(jw)|. From (3), -G~(s) = 1 - A~/s + O(1/s^2) with A~ = A - 2 sum(sigma_k).

**Step 5: the contour.** f(s) = ln(1 / (-G~(s))) is analytic in Re s > 0, f(s) = A~/s + O(1/s^2). Integrate f around the contour made of the jw axis from -jR to +jR (with small indentations around any zero on the axis; the logarithmic singularity is integrable, the indentations vanish with their radius) and the right-half-plane semicircle of radius R back from +jR to -jR. Cauchy: the total is 0. On the axis, f = ln(1/|G|) - j arg(-G~): the first part is even in w and the second odd, so the axis integral is j * 2 * integral from 0 to R of ln(1/|G(jw)|) dw. On the semicircle s = R e^{j theta}, theta from pi/2 to -pi/2, the integral of A~/s ds = A~ * j * (-pi); the O(1/s^2) terms integrate to O(1/R) and vanish. Therefore

    integral 0..infinity of ln(1/|G|) dw = (pi/2) A~ = pi/(R C_tot) - pi * sum(sigma_k) <= pi/(R C).      (4)

Since |G| = |Gamma| on the axis, this is (1). Equality needs no zeros (sum = 0) and C_tot = C. **QED.** (Nothing in the proof used the order or structure of N: it is any finite lossless reciprocal network.)

## 3. What the numbers show (`scripts/antenna/bodefano.py`, R = C = 1, bound pi = 3.1416)
* **Equality case:** a bare resistor R0 = R (no network): Gamma = -jw/(2 + jw), ln(1/|Gamma|) = (1/2) ln(1 + 4/w^2), whose integral is exactly pi (the script's grid gives 3.1357 = 99.8 percent; the rest is the tail above its grid). 3000 random ladders of up to 6 elements: the largest total area 3.1415 (100.00 percent), never above.
* **Networks move the area, they cannot add to it.** Every optimised ladder has total area 3.1413 to 3.1415. What an order-n ladder does is flatten it into a band. Smallest worst |Gamma| over 0..1 rad/s, and the in-band product of (2) as a share of pi: no network 0.414 (-7.7 dB, 28.0 percent); 1 element 0.178 (-15.0 dB, 54.9 percent); 2 elements 0.116 (-18.7 dB, 68.5 percent); 3: 0.090 (-20.9 dB, 76.6 percent); 4: 0.077 (-22.3 dB, 81.7 percent); 5: 0.069 (-23.2 dB, 85.1 percent). The product approaches pi and never exceeds it, as (2) says; the limit (a brick-wall response) needs infinitely many elements.
* A first version of the script let an optimiser reach 150 percent of the bound; the area it counted was an under-resolved high-Q notch on its integration grid, not a network (the element values were 0.003 to 0.1, with a resonance between the grid points). The bound was fine; the integration was not. The script now bounds the element range and reports the total area to be checked.

## 4. Use in this project
For an antenna that looks like a series R, L, C or a parallel RC near resonance, the bound says how much bandwidth a given depth of match costs (docs/PLAN.md, antenna-matching skill). Worked, with R C = 1: a match with worst |Gamma| of 0.178 (-15 dB) cannot extend beyond wc = pi / ln(1/0.178) = 1.82 rad/s whatever the network; the one-element ladder above reaches 1.0 rad/s, 55 percent of that. In general wc <= pi / (R C ln(1/Gamma_max)). A real antenna's Q fixes R C near its resonance, so the limit turns an antenna's measured Q into a ceiling on what any matching network can recover. The limit does not apply to a lossy network (it can be beaten by dissipating power, at the cost of efficiency) or to a network containing active parts.

## 5. Not covered
The journal paper's other results (matching to loads with several reactive elements, the tables of optimum Chebyshev matching networks, the explicit numbers for series RL and shunt RC) are in TR-41, which this note does not reproduce. Only the single-pole RC case is proved here.
