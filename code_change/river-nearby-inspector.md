# Nearby generated river inspection

- Existing owner: `StudioTerrainPhysicalPageService` publishes immutable M16
  river networks inside each built physical-page snapshot.
- Primary insertion points: Planet Hydrology popup in
  `StudioViewportPanels.cpp` and read-only `terrain.rivers_nearby` in
  `StudioTerrainStatusRpc.cpp`.
- Canonical state: none added; M16 networks remain generated, derived data.
- UI and MCP inspect only already-built pages in the observer's physical page
  neighborhood. Missing pages are reported as pending and are not synthesized.
- Node IDs remain stable M16 IDs. Inspection does not change channel geometry
  or add an editor-only river cache.
- Native changes use the central Studio generation handoff.
