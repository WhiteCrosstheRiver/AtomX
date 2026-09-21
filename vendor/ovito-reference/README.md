# OVITO reference pack for AtomX

This directory is an internal engineering reference for AtomX. It records the
public OVITO User Manual pages consulted while matching AtomX's terminology,
pipeline organization, viewport controls, import/export behavior, and modifier
catalog. The HTML snapshots in `sources/` are kept for offline review; the
canonical documentation remains at <https://docs.ovito.org/>.

The attached OVITO screenshots were used as visual references for the layout:
the four viewport grid, black scene background, pipeline command panel,
category headers in **Add modification**, particle appearance controls, data
inspector tabs, and the bottom trajectory ruler/tool strip.

## Reference areas

| Area | OVITO behavior to match | AtomX direction |
| --- | --- | --- |
| Import | Format auto-detection, compressed text input, numbered file sequences, multi-frame files, topology + trajectory pairs | Keep one source card, expose sequence discovery and frame count, stream only the active frame for large trajectories |
| Pipeline | A source followed by ordered modifiers; every stage can be selected, enabled, reordered, and removed | Keep the right command panel as the single place to inspect and edit pipeline stages |
| Viewports | 2x2 views, caption and axis tripod, orbit/pan/zoom, fit, maximize active view | Bottom tool strip now exposes Zoom, Pan, Orbit, FOV, Max, animation settings, and auto-key |
| Rendering | Interactive standard renderer plus optional high-quality ray tracers; output resolution, background, frame range, image/video output | Keep renderer choices explicit and report the active GPU/backend before rendering |
| Data inspector | Particles, simulation cell, global attributes, data tables and surfaces | Tabs are already present; each analysis should publish inspectable properties and tables |
| Export | Current frame or a range, one file or sequence, format-specific properties | Export dialog should show only settings supported by the selected format |
| Modifiers | Analysis, coloring, modification, selection, structure identification, visualization, Python modifiers | Use the same grouped dropdown and make every entry available without Pro gating |

## Feature implementation order

1. Import/export breadth: extended XYZ, LAMMPS dump/data, POSCAR/XDATCAR,
   CIF, PDB, GRO/XTC/TRR, DCD, and ASE-compatible formats. Preserve cell,
   PBC, types, bonds, per-particle properties, and frame metadata.
2. Core pipeline primitives: delete/selection, affine transform, replicate,
   slice, wrap/unwrap, compute property, color coding, and type appearance.
3. Structure analysis: CNA, PTM, centrosymmetry, Ackland-Jones, diamond,
   Voronoi, RDF, cluster analysis, and Wigner-Seitz defects.
4. Visualization objects: bonds, coordination polyhedra, trajectory lines,
   surfaces, isosurfaces, labels, and DXA dislocation networks.
5. Rendering backends: Direct3D interactive GPU first, then a pluggable
   ray-tracing interface (OSPRay/OptiX/Intel-compatible path where available).

The first two groups should remain usable on sampled previews, while analyses
that require complete neighborhoods must clearly report when a preview is not
valid.

