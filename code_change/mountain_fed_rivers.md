# Mountain-fed river networks

## Behavior
Rivers should begin in wet mountain headwaters, follow accumulated downhill drainage, merge into larger channels at confluences, and retain coherent catchment identity as flow crosses terrain pages. Planet authoring should expose Rivers and Tectonics near the start of the Planet toolbar whenever the selected body has a terrain surface. The toolbar must use its already-resolved selected body as the source of that terrain child rather than run a second selection lookup or duplicate the contextual Terrain selector.

## Existing owner
- Module: OrbitTerrainHydrology and OrbitTerrainErosion
- Canonical state: M09 DrainagePage and M16 RiverNetwork derived from M08 material columns.
- No second global hydrology or render authority.

## Primary insertion point
- File: engine/terrain_hydrology/src/DrainagePage.cpp and engine/terrain_erosion/src/RiverNetwork.cpp
- Symbols: BuildDrainagePage and BuildRiverNetwork/ResolveBasin.
- File: engine/editor_model/src/SurfaceAuthoringModel.cpp
- Symbol: DrawPlanetToolbar uses CelestialAuthoringModel::SelectedBody and its direct Terrain Surface child as the single UI target; SurfaceAuthoringModel owns the recipe edits.

## Secondary touch points
- Studio terrain physical page service supplies sea-level outlets and mountain/runoff inputs.
- Planet toolbar exposes river authoring and preview through the existing surface authoring model.
- RPC/MCP mirrors the same persisted operation if a new user-facing operation is added.
- Planet toolbar consumes the shared celestial selection model; it does not own a second terrain selection resolver or duplicate the contextual Terrain selector.
- Existing terrain docs and their source fingerprints stay current.

## Must not be implemented in
- Legacy regional HydrologyGrid/RiverGraph as a parallel authority.
- Viewport-only transient state or per-frame generated basin identities.

## Data/control flow
`Planet toolbar -> surface authoring/service -> M09 drainage -> M16 connected river graph -> M08 incision and viewport presentation`

## Validation
- [ ] Existing authority remains canonical
- [ ] No duplicate state/controller was introduced
- [ ] UI/automation reach the same operation
- [ ] Toolbar actions target the Terrain Surface owned by the resolved primary body
- [ ] Cross-page confluences and basin identity are deterministic
- [ ] Save-to-reflect remains automatic
