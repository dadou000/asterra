# Planet toolbar hydrology authoring

- Existing owners: `RiverNetworkConfig` owns the M16 river recipe;
  `SurfaceAuthoringModel` persists settings on the Terrain Surface; the Planet
  toolbar presents authoring and preview; Studio RPC/MCP share the same model.
- Primary insertion points: River process properties in `WorldSchemas`,
  `SurfaceComposition`, `SurfaceAuthoringModel`, and the Rivers popup in
  `StudioViewportPanels.cpp`.
- Canonical state: Terrain Surface process settings in the ObjectStore.
- Keep the toolbar, `terrain.rivers_get/set`, and the dedicated MCP tools on
  one persisted recipe. Do not introduce a second hydrology state store or
  present unimplemented global watershed, lake, groundwater, or local solver
  controls as functional.
- The UI exposes the complete existing `RiverNetworkConfig` progressively:
  drainage/discharge thresholds, channel width/depth relationships, and
  meander/cutoff behavior. It also uses the existing viewport diagnostic and
  flat-map layers for river/drainage, precipitation and standing-water review.
  The current implementation remains regional M09/M16 drainage and river
  graphs; future planet-scale hydrology architecture is outside this toolbar
  wiring change.
- Native UI/editor changes use the central automatic Studio-generation
  handoff. Verify the existing surface-authoring and RPC/MCP pathways.
