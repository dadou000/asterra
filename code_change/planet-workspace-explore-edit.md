# Planet workspace Explore & Edit menu

## Behavior
Expand the Planet toolbar with a compact Explore & Edit menu that opens the full Surface Authoring panel and switches directly to any supported planetary flat-map layer.

## Existing owners and canonical operations
- UI owner: `StudioViewportPanels::DrawPlanetToolbar` for contextual toolbar actions.
- Surface authoring remains in `SurfaceAuthoringUi` and `SurfaceAuthoringModel`; the toolbar focuses the existing panel instead of duplicating its relief, stratigraphy, constraint, biome, scatter and diagnostics controls.
- Map view and layer remain owned by `StudioRenderViewSet` and the viewport registry; layer selection reuses `SetMode(FlatMap)`, `SetFlatMapLayer`, and existing `map.layer_set` / `viewport.mode_set` RPC/MCP operations.
- Opening the authoring panel reuses `EditorUi::FocusPanelByTitle`, already exposed as `studio.panel_focus` / `orbit_panel_focus`.

## Must not be implemented in
- A second surface authoring model or a UI-only set of relief/biome controls.
- A duplicate map-layer state or a new map RPC.

## Validation
- [x] Surface Authoring is directly reachable from the Planet toolbar.
- [x] Every supported flat-map layer is selectable from the same Planet toolbar menu.
- [x] Existing model, viewport and MCP operations remain authoritative.
- [x] Studio UI docs and map workflow describe the new entry point.
- [x] `python tools/orbit_docs_cli.py check` passes with 0 errors and 0 warnings.
- [x] `cmake --build build --target OrbitStudio --config Debug -- /m:1` completed successfully. No tests were run.
