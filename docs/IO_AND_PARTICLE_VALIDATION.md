# Native I/O and particle appearance repair

This change addresses import/export correctness and particle rendering. It does
not implement every ASE format or the full OVITO analysis plan. No Python
runtime is required by AtomX.

## User workflow

Open/drag a supported structure file. The source panel reports the actual
reader. XYZ and LAMMPS text dumps are indexed as trajectories and use the same
frame controls. In Render, choose **Export data...** and then select the format.
Only relevant format options appear. File extensions must match the selected
format, and exporting over the active source is rejected.

| Format | Import | Export and specific options |
| --- | --- | --- |
| XYZ / Extended XYZ | Multiple frames, reordered columns, numeric scalar/3-vector properties, cell/PBC | Basic or extended, precision, selected property columns, one file or frame sequence |
| POSCAR / CONTCAR | Extensionless names, VASP4 counts with synthetic type labels, positive scalar scale, Direct/Cartesian, selective dynamics | Grouped type/position rows, Direct or Cartesian, precision, selective dynamics when available |
| CIF | P1 fractional structure, reordered loop columns, quoted symbols, uncertainty suffixes, triclinic cell | P1 cell lengths/angles, fractional positions, precision |
| LAMMPS data | Atomic style, restricted triclinic cell, nonzero origin | Atomic style, precision; no masses/bonds, fresh sequential IDs |
| LAMMPS text dump | Indexed frames, reordered columns, x/y/z, xu/yu/zu, xs/ys/zs, numeric properties, restricted triclinic cell, PBC | id/type/element/x/y/z, precision, single trajectory or frame sequence; fresh sequential IDs |
| PDB | One model, fixed-width atom coordinates, element names, CRYST1, primary alternate location | Read-only; residue/bond topology is not imported |
| GRO | One structure, nm-to-Angstrom conversion, 3/9-value cell, optional velocities | Fixed 3-decimal nm coordinates, fixed 5-decimal cell; synthetic MOL residue, at most 99999 atoms; velocities/topology not exported |

Formats with a single structure use one file per frame when exporting a range.
Ranges evaluate the current modifier list separately at each requested frame.
XYZ property arrays stay aligned after slice/delete/replication. Sampled data
requires an explicit preview-export choice. Single-file exports are staged in
the destination directory and published only after successful serialization;
file-sequence export refuses existing target names. Cancellation is checked
between exported frames. A publication failure can leave already-published
files in a sequence; remaining temporary files are cleaned up.

CIF symmetry expansion/partial occupancies/multiline fields, compressed inputs,
general triclinic LAMMPS dumps, molecular LAMMPS data styles, negative/three-axis
POSCAR scale, and PDB/GRO multi-model trajectories are not advertised as
supported. Unsupported variants fail explicitly. PDB and GRO preserve
coordinates rather than molecular topology. The application uses the new
registry in `structure_io.hpp`; legacy helpers in `core.hpp` remain for existing
callers and do not represent the new format capability table.

## Particle appearance

The Pipeline panel's Particle appearance section has defaults plus per-type
color, visibility, radius and shape overrides. Overrides follow type names
across frames. Selection/color coding can override the base type color.

- Sphere / Ellipsoid: analytical ray/ellipsoid intersection and axis scales.
- Circle and Square: camera-facing flat shapes.
- Cube / Box: world-aligned 3D surface, adjustable axis scales.
- Cylinder: world-Z axis, flat caps, adjustable half-length/radius ratio.
- Spherocylinder: world-Z axis with hemispherical caps.
- Mesh / User-defined: shared, normalized triangulated OBJ, maximum 256 triangles;
  per-type radius and axis scales. Concave polygons must be triangulated before
  import. This renderer is intended for small mesh instances, not a guarantee
  of large-dataset performance for arbitrary meshes.

Default spherical particles retain an analytical path. Other solid primitives
use bounded surface intersection; all shapes write actual surface depth. Type
styles occupy a separate small GPU buffer; the atom buffer remains 16 bytes
per atom. Scene bounds and the cell overlay now include the cell origin.

## Validation performed

- Core regression suite passes.
- POSCAR tests: type/position association, skewed cells, scaling, VASP4, filenames,
  malformed input and preserving an existing file on invalid export.
- Structure I/O tests: properties after deletion, numeric property roundtrip,
  dump frame seeking/budget/scaled coordinates, GRO unit conversion, CIF and
  POSCAR triclinic coordinates, selective-dynamics ordering and PDB columns.
- Application export worker test: frame stride, modifiers per frame, forced
  sequences, extension mismatch, input-file protection, failed-export cleanup.
- GPU shape tests on NVIDIA GeForce RTX 5090 D v2 and Intel(R) Graphics:
  seven distinct nonempty shape images and finite depth buffers. Hidden-type
  output was visually checked to be empty.
- Independent application build and export-dialog screenshot inspected.

`build.ps1` runs the three CPU suites. CMake also provides `render_shapes` and
`export_workflow` executables; these require an available D3D11 GPU and are
run explicitly. The independently built executable is `build/review/AtomX.exe`.
No ASE runtime comparison or large-scale performance certification is claimed.

## Implementation references

Inspected the vendored ASE `ase/io/vasp.py`, `gromacs.py`,
`proteindatabank.py`, and `lammpsrun.py` for coordinate conventions, field layouts,
type ordering and unit conversion. The application implementation is C++.
