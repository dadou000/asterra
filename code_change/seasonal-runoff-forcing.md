# Simulation-time seasonal runoff forcing

- Existing owner: hydraulic process settings are persisted by
  `SurfaceAuthoringModel`; `StudioTerrainPhysicalPageService` captures those
  settings with simulation time and builds drainage/M16 products.
- Primary insertion points: `HydraulicErosionConfig`, Process Settings schema,
  the Planet Hydrology toolbar, and `terrain.hydrology_get/set` RPC/MCP.
- Canonical state: seasonal amplitude, period and phase stored on the existing
  Process Settings object. Do not create a parallel climate clock or UI cache.
- Runtime: scale the static spatial precipitation field by a sinusoid, sample
  at twelve bins per cycle, and queue global process invalidation when authored
  settings change. The central simulation clock supplies the current time.
- Scope: this does not claim latitude-dependent seasons, snow accumulation or
  melt, groundwater, or dynamic climate circulation.
- Iteration: native changes use the central Studio generation handoff; schema
  edits are reflected through the same running Studio generation.
