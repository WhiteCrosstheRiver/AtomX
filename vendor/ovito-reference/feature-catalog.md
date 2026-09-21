# OVITO modifier and UI catalog

This catalog is derived from the OVITO 3.16.1 manual and is deliberately
written as implementation guidance rather than a promise that every item is
already implemented.

## Analysis

Atomic strain; bond angle/length distribution; bond order; cluster analysis;
coordination analysis; difference between frames; displacement vectors; DXA;
elastic strain; find rings; grain segmentation; histogram; reduce property;
radial distribution function; scatter plot; spatial binning; spatial
correlation; structure factor; time averaging; time series; Voronoi analysis;
Wigner-Seitz defect analysis.

## Coloring

Ambient occlusion, assign color, color by type, and color coding. Color coding
needs a property selector, gradient preset, automatic/symmetric range,
discretization, reverse range, selected-only mode, and an on-canvas legend.

## Modification

Affine transformation, combine datasets, compute property, delete selected,
edit simulation cell, edit types, freeze/remove property, load trajectory,
replicate, slice, smooth trajectory, unwrap trajectories, and wrap at periodic
boundaries.

## Selection

Clear, expand, expression, manual, invert, overlapping-particle, and select by
type. Manual selection should operate in the viewport and publish a Selection
particle property for downstream modifiers.

## Structure identification

Ackland-Jones, centrosymmetry, Chill+, common-neighbor analysis, diamond
structure, polyhedral template matching, and VoroTop. Each should publish a
typed structure identifier plus counts/fractions in the data inspector.

## Visualization

Add text labels, construct surface mesh, create bonds, create isosurface,
coordination polyhedra, and generate trajectory lines. These are visual
elements in the pipeline and should have visibility checkboxes independent of
the source data.

## UI contracts

- Keep **Add modification...** as a dropdown with category headers and search.
- Keep pipeline stages in insertion order; selected stage owns the property
  editor below the stage list.
- Keep the black viewport canvas and a thin accent border around the active
  viewport.
- Keep the lower tab bar for data objects and the timeline/tool strip below it.
- Use disabled-looking controls only when a modifier truly cannot operate on
  the current data; do not gate features by a Pro license.
- For large datasets, show whether values describe the complete dataset or a
  sampled preview.

