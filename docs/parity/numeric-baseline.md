# AtomX numeric baseline (OVITO parity)

Reference values AtomX must reproduce. All computations: float64, brute-force,
periodic minimum-image convention on the full simulation box, by
`docs/parity/scripts/numeric_baseline.py` (Python 3.13.9, numpy 2.5.3). Regenerate with `python numeric_baseline.py`.

Shared dataset: `test.xyz` (30 frames, 108 atoms; FCC 3x3x3 conventional cells,
a = 3.6 A, cubic box L = 10.8 A, PBC xyz; basis atom 0 of each cell = Cu (27 total),
remaining 81 = Ni; thermal displacement amplitude 0.12 A, positions written with
4 decimals). Fixtures generated in the script: perfect FCC (same cell, 108 atoms,
a = 3.6), perfect BCC (a = 3.0, 4x4x4 cells, 128 atoms, L = 12.0), FCC + atom 0
displaced by (0.5, 0, 0) A, FCC with atom 0 removed (vacancy at origin, 107 atoms).

Tolerances: integer counts must match exactly. Real-valued entries are float64
references; AtomX comparing float32-rendered positions should pass within the
listed tolerance (float32 rel. eps ~1.2e-7; the 4-decimal rounding of test.xyz
dominates at ~5e-5 per coordinate component).

## A. Common neighbor analysis (fixed cutoff)

Bond signature = (number of common neighbors, bonds among them, longest bond
chain); rings count as chain length 1 (reproduces published 1441/1661/1551).
Classification (LAMMPS `compute cna/atom` / OVITO rules): FCC = 12x(4,2,1);
HCP = 6x(4,2,1) + 6x(4,2,2); BCC = 8x(6,6,1) + 6x(4,4,1) [LAMMPS:
`nbcc4 == 6 && nbcc6 == 8`]; else Other. Structure type codes: OVITO
0=Other, 1=FCC, 2=HCP, 3=BCC; LAMMPS 1=fcc, 2=hcp, 3=bcc, 5=unknown.

Cutoffs: FCC uses 3.1 A (between shell 1 at a/sqrt(2)=2.5456 and shell 2 at
a=3.6). For BCC, OVITO/LAMMPS prescribe a cutoff between the SECOND and THIRD
shells (a=3.0 and a*sqrt(2)=4.243); 3.6 A is used here. The naive 2.8 A cutoff
(between shells 1 and 2) is also computed to document why it fails.

| dataset | cutoff (A) | FCC | HCP | BCC | Other | per-atom bond signatures |
|---|---|---|---|---|---|---|
| test.xyz frame 0 | 3.1 | 108 | 0 | 0 | 0 | 108 atoms x 12x(4, 2, 1) |
| perfect FCC fixture | 3.1 | 108 | 0 | 0 | 0 | 108 atoms x 12x(4, 2, 1) |
| perfect BCC fixture (4x4x4) | 2.8 | 0 | 0 | 0 | 128 | 128 atoms x 8x(0, 0, 0) |
| perfect BCC fixture (4x4x4) | 3.6 | 0 | 0 | 128 | 0 | 128 atoms x 6x(4, 4, 1), 8x(6, 6, 1) |
| BCC 3x3x3 control (L = 3a, TOO SMALL) | 3.6 | 0 | 0 | 0 | 54 | 54 atoms x 6x(5, 4, 1), 8x(6, 6, 1) |

Key facts:

- test.xyz frame 0, cutoff 3.1: **FCC = 108/108**, Other = 0.
  Every atom's 12 bonds have signature (4,2,1) = 421 despite the 0.12 A thermal
  displacement and 4-decimal file rounding.
- Cutoff safety margin, frame 0: largest first-shell distance 2.8671 A,
  smallest second-shell distance 3.3660 A; cutoff 3.1 clears them by
  0.233 A and 0.266 A -- no float32 cutoff-comparison risk.
- Perfect BCC at cutoff 2.8 A: **Other = 128/54** (all bonds (0,0,0): a corner and a body-center atom share no common neighbors when only the 8 first-shell atoms are in the neighbor lists). Coordination is 8 for every atom. This cutoff does NOT identify BCC.
- Perfect BCC at cutoff 3.6 A (between shells 2 and 3): **BCC = 128/128**;
  per atom: 8 first-shell bonds with signature 1661 and 6 second-shell bonds with
  signature 1441. (Coordination 14 = 8 + 6.)
- Finite-size caveat: a 3x3x3 BCC box (L = 3a = 9.0) is TOO SMALL for CNA at cutoff
  3.6: the second-neighbor bond (0,0,0)-(a,0,0) gains a spurious 5th common neighbor
  (the corner at -a and the corner at +2a coincide under PBC), giving signature (5,4,1)
  instead of (4,4,1) and classifying every atom as Other (0/54 BCC).
  AtomX BCC tests must use at least 4x4x4 conventional cells.
- HCP (for reference; no fixture): 6x421 + 6x422. Icosahedral: 12x(5,5,1).

## B. Coordination analysis (cutoff 3.1 A, test.xyz frame 0)

OVITO semantics apply PBC. With PBC there is no surface in this fully periodic box.

| convention | distribution (coordination: count) | mean |
|---|---|---|
| with PBC (OVITO default) | 12: 108 | 12.0000 |
| no PBC (surface-affected) | 3: 4, 5: 24, 8: 48, 12: 32 | 8.333333 |

- Expected with PBC: **108/108 atoms at coordination 12**, mean = 12.0.
- BCC fixture: coordination 8 (cutoff 2.8) or 14 (cutoff 3.6) for every atom.

## C. Radial distribution function (test.xyz frame 0, PBC)

Ideal FCC shells (a = 3.6, computed from the perfect fixture, exact):

| shell | ideal distance (A) | multiplicity/atom | pairs in 108-atom box |
|---|---|---|---|
| 1st (NN) | 2.545584 | 12 | 648 |
| 2nd | 3.600000 | 6 | 324 |
| 3rd | 4.409082 | 24 | 1296 |
| 4th | 5.091169 | 12 | 648 |
| 5th (at L/2) | 5.692100 | 12 | 648 |
| 6th (beyond L/2) | 6.235383 | 8 | 432 |
|  | 6.734983 | 24 | 1296 |
|  | 7.636753 | 3 | 162 |
|  | 8.442748 | 6 | 324 |

Thermal frame 0 (amplitude 0.12 A, file-rounded positions), 0.01 A bins, r < 5.4 = L/2:

- **First peak: bin center 2.54 A** (ideal a/sqrt(2) = 2.5456 A);
  thermal shell 1 spans 2.2134-2.8671 A.
- **Second peak: bin center 3.59 A** (ideal a = 3.6 A);
  shell 2 spans 3.3660-3.8441 A.

| band (A) | pair count | atoms/atom (=count x 2/108) | min (A) | max (A) | peak bin center (A) |
|---|---|---|---|---|---|
| 2.20-3.00 | 648 | 12.00 | 2.2134 | 2.8671 | 2.53 |
| 3.30-3.95 | 324 | 6.00 | 3.3660 | 3.8441 | 3.59 |
| 4.10-4.75 | 1294 | 23.96 | 4.1089 | 4.7460 | 4.35 |
| 4.78-5.35 | 640 | 11.85 | 4.8028 | 5.3472 | 5.10 |
| 5.35-5.45 | 20 | 0.37 | 5.3552 | 5.4492 | 5.44 |

Finite-size effects (3x3x3 box, L = 10.8):

- RDF is only defined to r = L/2 = 5.4 A; shells 1-4 (2.546, 3.6, 4.409, 5.091 A;
  12, 6, 24, 12 neighbors) are complete and untruncated -- coordination integrals
  over shells 1-4 reproduce exactly 12, 6, 24, 12.
- The 5th shell (24 neighbors at 1.5a = 5.4 A) coincides with the r = L/2 cutoff
  and suffers minimum-image ambiguity (each such pair sits exactly at the box
  half-width); the histogram shows 20 pairs in 5.35-5.45 A. Do not use it as a test.
- Shell 6 (8 at a*sqrt(3) = 6.235 A) and beyond are unreachable at this box size.

## D. Centrosymmetry parameter (CSP)

p = sum over N/2 pairs of |r_i + r_j|^2 with r = vectors to the N nearest
neighbors (N = 12 FCC, 8 BCC). Two modes computed:
greedy = sum of the N/2 smallest of all N(N-1)/2 pair weights (LAMMPS
`compute centro/atom`, OVITO conventional); matching = minimum-weight perfect
pairing (OVITO 'minimum-weight matching', Larsen arXiv:2003.08879).

- Perfect FCC fixture: **CSP = 0.0 exactly for all 108 atoms** (both modes;
  max computed value 1.302e-29 greedy, 1.302e-29 matching).
- Perfect BCC fixture, N = 8: **CSP = 0.0 exactly for all 128 atoms**
  (max 0.000e+00).

FCC + atom 0 displaced by (0.5, 0, 0) A -- atom-index -> CSP (A^2):

| atom | role | CSP greedy | CSP matching |
|---|---|---|---|
| 0 | displaced atom | 6.000000 | 6.000000 |
| 1 | NN neighbor of displaced | 0.250000 | 0.250000 |
| 2 | NN neighbor of displaced | 0.250000 | 0.250000 |
| 3 | NN neighbor of displaced | 0.250000 | 0.250000 |
| 10 | NN neighbor of displaced | 0.250000 | 0.250000 |
| 11 | NN neighbor of displaced | 0.250000 | 0.250000 |
| 25 | NN neighbor of displaced | 0.250000 | 0.250000 |
| 27 | NN neighbor of displaced | 0.250000 | 0.250000 |
| 35 | NN neighbor of displaced | 0.250000 | 0.250000 |
| 73 | NN neighbor of displaced | 0.250000 | 0.250000 |
| 74 | NN neighbor of displaced | 0.250000 | 0.250000 |
| 82 | NN neighbor of displaced | 0.250000 | 0.250000 |
| 97 | NN neighbor of displaced | 0.250000 | 0.250000 |
| all other 95 atoms | undisturbed 2nd shell+ | 1.0e-29 (max) | 1.0e-29 (max) |

- BCC using 8 neighbors: exact 0 (see above). The 8 vectors (±1.5, ±1.5, ±1.5) A
  contain exact opposite pairs.

FCC with one vacancy (atom 0 at origin removed) -- the 12 first-shell neighbors:

| atom | CSP greedy | CSP matching (tie-range) |
|---|---|---|
| 0 | 6.480000 | 6.480000 - 19.440000 |
| 1 | 6.480000 | 6.480000 - 19.440000 |
| 2 | 6.480000 | 6.480000 - 19.440000 |
| 9 | 6.480000 | 6.480000 - 19.440000 |
| 10 | 6.480000 | 6.480000 - 19.440000 |
| 24 | 6.480000 | 6.480000 - 19.440000 |
| 26 | 6.480000 | 6.480000 - 19.440000 |
| 34 | 6.480000 | 6.480000 - 19.440000 |
| 72 | 6.480000 | 6.480000 - 19.440000 |
| 73 | 6.480000 | 6.480000 - 19.440000 |
| 81 | 6.480000 | 6.480000 - 19.440000 |
| 96 | 6.480000 | 6.480000 - 19.440000 |
| other 95 atoms | 1.0e-29 (max) | |

- **Vacancy neighbor CSP (greedy) = a^2/2 = 6.4800 A^2 exactly for all 12 neighbors**
  (analytic: the 5 surviving opposite pairs contribute 0, and the lowest remaining
  weight is a^2/2 whether it comes from the un-paired first-shell vector u paired
  with a second-shell vector at 135 degrees, or from a 120-degree first-shell pair).
- Matching mode is tie-break dependent here: each neighbor has only 11 first-shell
  neighbors, and the 12th slot is filled by one of 6 exactly-equidistant second-shell
  atoms (3.6 A); the minimum-weight matching then lands in the range listed in the
  table (computed 6.48-19.44 A^2; the worst naive forced pairing u-f alone would be
  2.5a^2 = 32.4 A^2, but the matcher always avoids it). AtomX should reproduce
  greedy = 6.48 for all 12 and accept matching-mode values within the tabled range.

## E. Displacement (thermal vibration, generator formula)

Generator (gen_xyz.py): for atom idx at ideal lattice position (x, y, z),
dx = 0.12 sin(2*pi*fr/30 + 1.1x + 0.7*idx), dy = 0.12 sin(2*pi*fr/30 + 1.3y + 1.19*idx),
dz = 0.12 sin(2*pi*fr/30 + 0.9z + 1.61*idx). Magnitudes |d| (A), float64:

| atom | frame | d vs ideal lattice | d vs frame 0 (formula) | d vs ideal (from file) | d vs frame 0 (from file) |
|---|---|---|---|---|---|
| 0 | 1 | 0.043214 | 0.043214 | 0.043128 | 0.043128 |
| 0 | 5 | 0.180000 | 0.180000 | 0.179960 | 0.179960 |
| 0 | 15 | 0.000000 | 0.000000 | 0.000000 | 0.000000 |
| 1 | 1 | 0.137791 | 0.032462 | 0.137753 | 0.032460 |
| 1 | 5 | 0.147173 | 0.154403 | 0.147166 | 0.154358 |
| 1 | 15 | 0.138923 | 0.277846 | 0.138884 | 0.277768 |
| 50 | 1 | 0.144823 | 0.032762 | 0.144824 | 0.032743 |
| 50 | 5 | 0.181363 | 0.124384 | 0.181424 | 0.124388 |
| 50 | 15 | 0.128204 | 0.256408 | 0.128186 | 0.256372 |

- Mean |d| over all 108 atoms at frame 5: **0.142916 A** (formula); from the
  rounded file positions: 0.142918 A; max formula-vs-file magnitude
  deviation 6.93e-05 A (4-decimal rounding).
- Max |d| at frame 5: 0.199324 A (bound: 0.12*sqrt(3) = 0.207846 A).
- Interpretation note: an AtomX displacement modifier referencing frame 0 sees
  the 'd vs frame 0' columns; the generator amplitude 0.12 A corresponds to the
  'vs ideal lattice' columns.

## F. Slice (test.xyz frame 0)

OVITO convention (verified against OVITO manual + surface-mesh docs): the plane
is { r : n.r = d } with n normalized; **the positive side (n.r > d) is deleted; the
negative side (n.r <= d) is kept**; 'reverse orientation' swaps the sides; a slab
width w > 0 keeps the closed slab |n.r - d| <= w/2 (and reverse deletes the slab).
Computed with n = (0,0,1), d = 5.4 (mid-plane), w = 3.0:

| operation | kept count (of 108) |
|---|---|
| keep n.r <= d (default) | 66 |
| keep n.r > d (reverse orientation) | 42 |
| slab w = 3.0: \|z - 5.4\| <= 1.5 (3.9 <= z <= 6.9) | 18 |
| slab inverse (delete the slab) | 90 |

- Atoms exactly at the boundary z = 5.4 (to 1e-12): 0. The 18 atoms of
  the ideal z = 5.4 layer are thermally displaced +/-0.12 A, so the <= vs < choice
  does not change counts; only the kept side matters.

## G. Replicate

| input | replicate | atoms | new box (A) | species counts |
|---|---|---|---|---|
| test cell / FCC fixture (108; 27 Cu + 81 Ni) | 2x2x2 | 864 | 21.6 x 21.6 x 21.6 | Cu 216, Ni 648 |
| test cell / FCC fixture (108) | 3x1x2 | 648 | 32.4 x 10.8 x 21.6 | Cu 162, Ni 486 |
| BCC fixture (128 Fe) | 2x2x2 | 1024 | 24.0 x 24.0 x 24.0 | Fe 1024 |

Invariance check on 2x2x2 replicated FCC: CNA(3.1) gives FCC = 864/864,
coordination distribution 12: 864 -- identical to the unreplicated cell (periodic-image invariance).

## Tolerance summary

| quantity | expected | tolerance |
|---|---|---|
| A/B/G integer counts (CNA classes, coordination histogram, slice kept, replicate N) | as listed | 0 (exact) |
| A per-atom bond signatures | as listed | exact match of signature multiset |
| C peak bin centers | 2.55, 3.60 | +/- 0.02 A (bin 0.01 + float32) |
| C shell pair counts / coordination integrals | 648, 324, 1296, 648 pairs (12, 6, 24, 12 per atom) | 0 with band edges as listed |
| D CSP zeros (perfect crystals) | 0.0 | <= 1e-5 (float32 positions) / 1e-10 (float64) |
| D CSP displaced-atom and neighbor values | as tabled | +/- 1e-4 |
| D CSP vacancy neighbors (greedy) | 6.48 | +/- 1e-4 |
| E displacement magnitudes | as tabled | +/- 3e-4 A (file rounding 5e-5/component + float32) |
| E mean \|d\| at frame 5 | as listed | +/- 3e-4 A |

