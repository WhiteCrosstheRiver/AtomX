# LJ calculation and source entries

## Usage

- In Creation Mode click **LJ**, or **计算 → LJ 单点能量与力**.
- OpenKIM Universal parameters for H–Og are included. They reproduce dimer
  separations/dissociation energies; they are not a general adsorption or
  nonbonded force field. Changing parameters produces a user-modified model.
- UFF imports the user's `lj_uff.csv` from the Desktop when available; otherwise
  use the CSV import button. Choose the intended UFF atom type for each species
  explicitly. Different atom environments require separate species/type labels.
  Only the LJ pair term is evaluated, including bonded atom pairs. This is not
  a complete UFF calculation (no bonds, angles, electrostatics or exclusions).
- Edit sigma, epsilon and cutoff in Å/eV. OpenKIM defaults to its supplied
  cutoffs and Lorentz–Berthelot mixing. UFF defaults to geometric mixing and
  an editable **AtomX choice** of 4 sigma cutoff, not a claimed UFF cutoff.
- Enable document PBC for periodic sums or disable for an isolated structure.
  Cutoff energy shifting is available; forces are not shifted. No tail correction.
- Click Calculate. The independent CPU worker uses spatial bins, with full
  finite-cutoff periodic image sums when a nearest image would omit neighbors.
  Viewport rendering remains on Direct3D 11. Cancel/close stops the worker.
- Results show total energy, energy/atom, maximum force, pair count and elapsed
  time. **Write to structure properties** is available in a matching Creation
  document without pipeline modifiers. It writes `LJ energy (eV)` and
  `LJ force (eV/A)`, total energy and a parameter/source table in one undo step.
  Atom coordinates are unchanged. These properties can be inspected/exported.
  Changed documents cannot accept stale results.

Spatial analysis is capped at 2 million atoms, 300 million candidate checks and
4 million ghost positions. Oversized cutoffs or extremely dense structures may
reach the budget; reduce cutoff only if appropriate for the chosen model or use
an external simulation backend. No silent subsampling/truncated answer is returned.

## Verified parameter provenance (2026-10-12)

Official OpenKIM package:
<https://openkim.org/download/LJ_ElliottAkerson_2015_Universal__MO_959249795837_003.txz>

Model DOI: <https://doi.org/10.25950/962b4967>. Ryan S. Elliott and Andrew Akerson;
2015 model family, 2018 `_003` revision. All 139 supplied CSV rows match the
official `LennardJones612_UniversalShifted.params` numerically. The application
includes the 118 real elements, excluding KIM electron/user placeholder types.
Original parameter file SHA256:
`a19a3147dd23aa7da9d81c03818f7c8e8427ec85b28f6a1cc05489cca6496877`.
Original package README, metadata, citations and CDDL-1.0 license are retained in
`third_party/openkim-lj`. `src/lj_openkim.hpp` is the CDDL-covered generated array
with unchanged numerical data. CMake distributes the source and notices under
`licenses/openkim-lj` beside the executable.

UFF paper: Rappé et al., JACS 114 (1992), 10024–10035,
<https://doi.org/10.1021/ja00051a040>.
The user's CSV cites Open Babel `data/UFF.prm`:
<https://github.com/openbabel/openbabel/blob/master/data/UFF.prm>.
The 128-row UFF table is **not embedded or redistributed**; it is read at runtime.
Sigma and epsilon are derived from the original x and D columns rather than
rounded converted columns. `1 kcal/mol = 0.043364104241800934 eV`.

Equation/mixing convention references:
<https://docs.lammps.org/pair_lj.html>,
<https://docs.lammps.org/pair_modify.html>.
The evaluator is implemented independently; LAMMPS/OpenKIM engines are not linked.

## Source UI and future rule

`src/references.hpp` is the common citation catalog. Small **?** buttons expose
the relevant authors, DOI/official links, what is used, and applicable software
license/version. Current entries cover LJ, crystallography/Spglib, element radii
and colors, construction workflow references, analysis workflow references,
export formats, and About/open source components. `AGENTS.md` requires future
externally based functionality to add this entry and preserve license notices.

## Creation copies

Right-click a Creation viewport or its document tab and choose **复制为新创作标签**.
Each invocation makes an independent document with its own structure, scientific
properties, topology, display settings, camera, selections, undo/redo and snapshots.
The copy has no parent save destination. Immutable snapshots can share read-only
storage; restoring/editing copies the snapshot data before mutation.

## Verification

- LJ tests: analytic two-atom energies/force signs, equilibrium, shifted cutoff,
  Lorentz–Berthelot/geometric mixing, finite-difference forces, independent periodic
  lattice oracle for orthogonal/skew/partial PBC cells, self images, overlap/
  sampled/invalid input, cancellation, 118 OpenKIM values and 128 UFF imports.
- Native application workflow: toolbar/dialog/source entry, asynchronous result,
  one-step publication/undo, stale-result rejection and independent repeated copies.

Local benchmark: the existing `build/cuni-55296.xyz` acceptance fixture with
default OpenKIM parameters evaluated 27,122,688 pairs in 1.83 s on this host
(optimized MSVC build, CPU worker). This is a fixture-specific measurement,
not a universal performance guarantee.
