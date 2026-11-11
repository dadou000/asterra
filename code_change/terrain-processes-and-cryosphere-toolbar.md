# Terrain processes and cryosphere toolbar bubbles

## Behavior
Add focused Planet toolbar bubbles for Terrain Processes and Ice & Cryosphere. Terrain Processes edits solver switches and supported process tuning; Ice & Cryosphere edits ice fracture networks and ice-renewal resurfacing events. Both bubbles are implemented in the Planet toolbar and use canonical model owners.

## Existing owners and canonical state
- Process owner: `editor_model::SurfaceAuthoringModel::ProcessSettings` / `SetProcessSettings`; canonical `TerrainProcessService` on the selected Terrain Surface.
- Ice owner: `SurfaceAuthoringModel::ImpactHistoryToml` / `SetImpactHistoryToml`; canonical geological event history TOML, including `IceFractureDefinition` and `IceRenewal` records.
- Automation: existing `terrain.impacts_get/set`; process controls use the generic `terrain.processes_get/set` RPC/MCP operation. Hydrology and river controls retain their existing focused routes.

## Primary insertion points
- `engine/studio_ui/src/StudioViewportPanels.cpp`: Planet toolbar bubble composition and process/cryosphere controls.
- `engine/studio_session/src/StudioTerrainStatusRpc.cpp`: generic process settings RPC routed through the same model and invalidation path.
- `tools/mcp_server/orbit_editor_mcp_server.py`: dedicated process settings MCP tools.

## Must not be implemented in
- A second process configuration or cryosphere store.
- A UI-only mutation path or a separate RPC authority.

## Validation
- [x] Process controls expose solver switches and useful settings through grouped sections; configuration persists through `SurfaceAuthoringModel` and queues the existing global process-settings invalidation.
- [x] Ice controls cover fracture parameters and ice-renewal placement, shape and chronology, using geographic point controls.
- [x] Both bubbles use existing model transactions and terrain invalidation; MCP calls the same RPC authority.
- [x] Toolbar, canonical state and RPC/MCP parity are documented in Studio UI, Studio session and MCP docs.
- [x] `python tools/orbit_docs_cli.py check` passes: 162 structured nodes, 0 errors, 0 warnings.
- [x] The full `OrbitStudio` Debug build completed, including `OrbitStudioUi` and `OrbitStudioSession`; no tests were run.
