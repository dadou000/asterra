# Planet structural authoring entry point

- Existing owners: `StudioViewportPanels` owns the Planet workspace toolbar;
  `SurfaceAuthoringUi` and `SurfaceAuthoringModel` own the existing selected
  terrain authoring presentation and canonical edits.
- Primary insertion point: `SurfaceAuthoringUi::Draw` explains the structural
  fields before its existing relief controls. The Planet toolbar stays focused
  on element creation and active terrain tools; a separate settings button was
  removed after visual review because it added clutter.
- Canonical state: ObjectStore-backed surface settings and terrain service
  outputs. No new tectonic or terrain state is introduced by this UI wiring.
- Keep existing invalidation, undo, and command paths authoritative. The engine
  already generates spherical plates and classified boundary fields. A future
  authoring UI must first give those recipe values a canonical persisted owner,
  then add plate diagnostics/map visualization and RPC/MCP parity; do not edit
  temporary terrain samples as if they were the recipe.
- Native UI edits use the existing automatic Studio generation handoff.
