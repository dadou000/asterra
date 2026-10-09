# Persisted basin-local river constraints

- Existing owner: `SurfaceAuthoringModel` owns authoring transactions;
  `StudioTerrainPhysicalPageService` owns M16 physical-page build inputs.
- Primary insertion points: a semantic child schema on Terrain Surface,
  production page input capture, M16 `BuildRiverNetwork`, Planet Hydrology
  popup and the session RPC/MCP route.
- Canonical state: `River Basin Constraint` semantic object with basin ID,
  page address, local center/direction, radius, strength, kind and enabled flag.
- UI and MCP create through `SurfaceAuthoringModel`; M16 consumes immutable
  captured values and filters constraints by their source page.
- River graph remains derived. Do not persist generated nodes or duplicate
  their geometry as authored terrain constraints.
- Native changes use the central Studio generation handoff.
