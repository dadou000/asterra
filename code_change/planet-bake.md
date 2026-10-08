# Planet bake: baked tectonics consumed by terrain generation

- Existing owners: `TectonicField` / `GlobalTerrainFields` (plate model),
  `AnalyticTerrainSource` + `GpuFieldGenerator` (CPU and GPU consumers),
  `SurfaceComposition` (composes the terrain source), `StudioSession::Tick`.
- Requirement: no true generation of tectonics (and, next, rivers) at runtime;
  one bakable artifact consumed by the procedural terrain generator.
- Change: new `terrain_bake` module (baker, `.orbitbake` section container,
  async `TerrainBakeService`) and `StudioTerrainBakeController` in the session.
  `GlobalTerrainFieldDesc::bakedTectonics` makes the plate fields come from
  baked cube-face rasters on both CPU and GPU. The first bake is blocking and
  precedes any terrain page; recipe edits go stale and rebake in the background
  while the old bake keeps rendering; failed/cancelled bakes leave it alive.
- Canonical state: the tectonics recipe on the Terrain Surface. The bake in
  `<project>/Bakes` is derived. New persisted settings: bake resolution and
  auto-rebake (Terrain Process Settings).
- UI/MCP parity: Planet Tectonics menu "Planet Bake" section,
  `terrain.bake_status/start/cancel/set`, `orbit_terrain_bake_*`.
- Tests: `Orbit.TerrainBake` (fidelity, cube-edge accuracy, file round trip and
  corruption, recipe hash, service stale/rebake/failure), `Orbit.StudioTerrainBake`
  (session path, stale -> rebake -> swap, RPC), `Orbit.TerrainGpuField` (baked
  case: GPU vs CPU within 0.025 m).
- Next phase (not done): the global river graph + carve field as a second
  section of the same file, replacing the per-page M09/M16 runtime builds, and
  folding stream-power incision into the bake.
