# Planet toolbar tectonics authoring

- Existing owners: `TectonicFieldDesc` defines the procedural plate recipe;
  `SurfaceComposition::TerrainDescription` rebuilds `AnalyticTerrainSource`
  from the Terrain Surface ObjectStore record; `StudioViewportPanels` owns the
  Planet toolbar; `StudioFlatMapRenderer` owns the shared equirectangular map.
- Primary insertion points: terrain schema/property composition for persistent
  recipe fields; the Planet toolbar popup for user controls; Flat Map's layer
  sampler for preview; StudioSession RPC and MCP adapter for the same settings.
- Canonical state: ObjectStore properties on the selected Terrain Surface.
  Terrain source and map pixels are derived from that recipe and its revision.
- Preserve CPU/GPU generator parity by resolving one `TectonicFieldDesc` and
  exporting its generated plate data. Never regenerate a second map-only plate
  layout. Tectonic controls and presets must use one model operation that
  validates and updates the recipe transactionally; the toolbar and RPC call
  the same model/service. Do not add a separate authoring state store.
- The Planet toolbar remains the primary editor surface. Keep the recipe
  progressively disclosed in one Tectonics menu instead of adding a row of
  persistent controls. The selected map preview must use the same analytic
  field instance/recipe as terrain generation.
- C++ implementation saves use the central automatic Studio-generation
  handoff; recipe edits invalidate/recompose terrain through ObjectStore's
  existing semantic revision path and remain undoable.
