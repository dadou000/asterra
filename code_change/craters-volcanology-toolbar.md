# Crater and volcanology Planet toolbar bubbles

## Behavior
Planet authors can edit every authored impact field in a dedicated Craters bubble and place/shape lava resurfacing regions in a Volcanology bubble. Lunar maria are lava resurfacing records, grouped under Volcanology.

## Existing owner
- Module: `terrain_impacts`
- Class/service: `ImpactFieldDefinition`, `SurfaceAuthoringModel`
- Canonical state: the validated geological event history TOML on the selected Terrain Surface.

## Primary insertion point
- File: `engine/studio_ui/src/StudioViewportPanels.cpp`
- Symbol/function: `StudioViewportPanels::DrawPlanetToolbar`
- Reason: Planet toolbar bubbles are the requested entry point; controls edit drafts and save through the existing surface authoring model.

## Secondary touch points
- `engine/studio_ui/include/orbit/studio_ui/StudioViewportPanels.hpp`: retain per-terrain drafts and popup state.
- `engine/studio_ui/docs/surface-authoring.md` and the Studio UI docs block: document controls and mare placement.
- Existing `terrain.impacts_get/set` RPC and MCP wrappers remain the canonical automation path.

## Must not be implemented in
- A second crater or volcanology database: both bubbles edit the existing chronological impact history.
- Viewport-only state: edited geology must persist on the terrain object and reach the same invalidation path as RPC.

## Data/control flow
`Planet toolbar -> ImpactFieldDefinition draft -> SurfaceAuthoringModel::SetImpactHistoryToml -> terrain.impacts_get/set state -> terrain invalidation -> terrain source`

## Validation
- [x] Draft changes persist only on Save and invalid input leaves the prior history intact.
- [x] Crater controls cover all `ImpactRecord` and procedural distribution fields.
- [x] Volcanology controls cover lava resurfacing placement and physical fields; mare tools live here.
- [x] Both bubbles save through existing canonical state and the RPC/MCP API remains equivalent.

## Inspection
- `cmake --build build --target OrbitStudio --config Debug -- /m:1` completed successfully.
- `python tools/orbit_docs_cli.py check` reported 162 structured nodes, 0 errors and 0 warnings.
- `git diff --check` passed for the changed UI and documentation files.
- The build produced `build/apps/editor/Debug/OrbitStudio.exe`; publishing to the root `Orbit.exe` was deferred because the destination is in use.
