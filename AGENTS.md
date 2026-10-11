# AtomX project rules

## Sources visible in the application

For any new or changed function based on a paper, external parameter table,
open source component, algorithm description, or reference application:

- Add an accurate entry to `src/references.hpp` with authors/project, title,
  DOI or official URL, applicable version, and what AtomX actually uses.
- Put a clickable small `?` source entry in the related dialog or panel using
  `referenceHelp`. Keep it reachable without obscuring the structure viewport.
- Preserve required copyright and license notices and source/license files for
  redistributed components/data. A citation alone does not replace license duties.
- Distinguish UI/workflow references from numerical algorithms. Do not claim
  equivalent calculations or copy unsupported parameter values.
- Scientific calculations must state units, assumptions, and parameter scope;
  run expensive work asynchronously with cancellation and bounded resource use.

The currently authorized calculation feature is LJ single-point energy/forces.
Other MS Calc modules are outside the current scope.
