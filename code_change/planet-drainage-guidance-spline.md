# Planet drainage guidance spline

- Existing owner: `SurfaceAuthoringModel` creates authored terrain constraints;
  `MacroGeologyField` samples drainage guidance; M09 uses guidance only to
  prefer among eligible downhill neighbors; M16 builds the river graph.
- Primary insertion points: add `AddDrainageSpline` to
  `SurfaceAuthoringModel`, use the existing viewport spline-picking path in
  `StudioViewportPanels`, and expose the same model operation through
  `terrain.drainage_spline_add` RPC/MCP.
- Canonical state: ObjectStore Terrain Constraint with Drainage channel and
  Spline shape. No editor-only route state is persisted.
- The spline biases downhill routing inside a corridor; it does not force
  uphill flow or directly author river geometry. Resulting graph and channel
  geometry remain M09/M16-derived.
- This is an additive authored constraint, undoable through CommandService,
  invalidated through the existing terrain dependency path, and inspectable in
  the Explorer/Properties UI.
- Native edits use the central Studio generation handoff.
