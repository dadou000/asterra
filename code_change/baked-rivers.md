# Baked river graph: rivers consumed as a lookup

- Existing owners: `AnalyticTerrainSource` + `GpuFieldGenerator` (consumers),
  `TerrainBakeService` / `StudioTerrainBakeController` (bake lifecycle),
  `SurfaceComposition` (injects bakes). Per-page M09 `DrainagePage` and M16
  `RiverNetwork` still feed water rendering and are untouched.
- Change: `BakedRiverNetwork` (terrain module) is a global graph of centerline
  nodes/reaches with a per-face bucket grid; `AnalyticTerrainSource::Sample`
  and the GPU `RiverCarveDepth` (binding 6) cut channels from it. `BakeRivers`
  (terrain_bake) samples the baked terrain onto a cube grid, priority-floods
  from the ocean, accumulates discharge and extracts rivers above 1000 m3/s.
  `RIVR` section in `.orbitbake`; bake = tectonics then rivers, tectonics
  reused when only the river recipe changed. `TerrainRecipeHash` (revision
  without rivers) is the river recipe identity.
- Fix found on the way: the chord distance to a reach must be taken on the
  chord projected back onto the sphere; the raw chord sags hundreds of metres
  below the surface and rivers vanished between nodes.
- Canonical state: terrain recipe; the river bake is derived.
- MCP parity: `terrain.bake_status` reports the river section; the Planet Bake
  menu shows a Rivers line. No new editable settings (river grid follows the
  bake resolution).
- Tests: `Orbit.TerrainBake` (graph integrity, determinism, dry planet),
  `Orbit.StudioTerrainBake` (hotspot edit rebakes only rivers, tectonics reused),
  `Orbit.TerrainGpuField` (GPU vs CPU carve within 0.1 m).
- Runtime replacement: `StudioTerrainPhysicalPageService` pages are
  independent (halo = terrain source, no published-drainage exchange, no
  neighbour rebuild queueing, no page gate). `ApplyBakedRiverDischarge` sets
  bankfull-channel discharge/area from the bake; `BuildBakedPageRiverNetwork`
  clips the graph to the page as its river network (no routing, meander or
  incision). Pages with authored river constraints keep the local M16 solve +
  incision, as does a source without a bake. `Orbit.BakedRiverPage` covers it.
- Behaviour change: rivers below the bake threshold no longer appear in the
  river network (they remain in page drainage/lakes); meander/cutoff/threshold
  river settings only affect constraint pages.
- Stream-power incision folded into the bake (see below).
- Not done: live Studio check of the page path
  (Studio exited with "Cloud layer parameters are invalid" on focusing the
  scratch Earth project before pages could be inspected).
