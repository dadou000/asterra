# Planet Rivers toolbar bubble

## Behavior
Add a dedicated Rivers popup beside Hydrology. Keep runoff and seasonal precipitation controls in Hydrology; put river-network generation, channel sizing, meander/cutoff tuning, viewport preview, drainage-path authoring, nearby graph/lake inspection and basin-local river constraints under Rivers.

## Existing owner and canonical state
- Module: `editor_model` / `terrain_erosion`
- Owners: `SurfaceAuthoringModel::ProcessSettings`, `SetProcessSettings`, `AddRiverBasinConstraint`, and the existing viewport drainage-path authoring operation.
- Canonical river recipe: `ProcessSettings::riversEnabled` plus `ProcessSettings::rivers` on the selected Terrain Surface.
- Automation: existing `terrain.rivers_get/set`, `terrain.rivers_nearby`, `terrain.lakes_nearby` RPC/MCP routes.

## Primary insertion point
- File: `engine/studio_ui/src/StudioViewportPanels.cpp`
- Symbol: `StudioViewportPanels::DrawPlanetToolbar`
- Reason: this is the existing Planet toolbar owner for Hydrology and river inspection; split the workflow into adjacent task-focused bubbles without adding another source of state.

## Must not be implemented in
- A parallel river configuration store or UI-only mutation path.
- A new RPC/MCP route when the existing process settings, graph inspection and constraint operations already cover the workflow.

## Validation
- [x] Dedicated Rivers bubble exposes every `RiverNetworkConfig` field with progressive disclosure and keeps runoff controls in Hydrology.
- [x] All mutations continue through `SurfaceAuthoringModel` and queue the existing terrain invalidation.
- [x] Documentation and the existing RPC/MCP parity are clear.

## Inspection
- `cmake --build build --target OrbitStudio --config Debug -- /m:1` completed successfully.
- `python tools/orbit_docs_cli.py check` reported 162 structured nodes, 0 errors and 0 warnings.
- `git diff --check` passed for the changed UI and documentation files.
- No new tests were added or run.
