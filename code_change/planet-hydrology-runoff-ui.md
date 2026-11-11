# Planet hydrology runoff controls

- Existing owner: `TerrainProcessService::hydraulic` stores rainfall source,
  infiltration, moisture capacity and evaporation; `StudioTerrainPhysicalPageService`
  derives M09 runoff from rainfall and the current precipitation sample;
  `SurfaceAuthoringModel` persists edits transactionally.
- Primary insertion points: the Hydrology popup in `StudioViewportPanels.cpp`
  and `terrain.hydrology_get/set` in `StudioTerrainStatusRpc.cpp`.
- Canonical state: hydraulic process settings on the selected Terrain Surface.
- Toolbar and MCP use the same `SurfaceAuthoringModel::ProcessSettings` /
  `SetProcessSettings` operation. Do not add editor-owned climate state.
- The current precipitation field is static and normalized; this change exposes
  its existing runoff budget controls without describing them as seasonal,
  globally accumulated, or groundwater-coupled.
- Native changes use the existing Studio generation handoff. Update the session
  and MCP docs with the new get/set pair.
