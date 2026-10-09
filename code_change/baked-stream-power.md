# Stream-power incision folded into the planet bake

- Existing owners: `terrain_erosion::SolveStreamPowerErosion` (per-page M10 solve
  in `StudioTerrainPhysicalPageService::BuildProcesses`), `BakeRivers`,
  `AnalyticTerrainSource`, `GpuFieldGenerator`.
- Change: `BakeRivers` runs the persisted stream-power law once on the baked
  drainage tree (area from accumulation, uplift from `MacroGeologyField`,
  erodibility from baked geological age) and stores the relief change as a
  gutter raster inside `BakedRiverNetwork` (river section format v2; v1 files
  still load without it). The terrain source and the GPU generator add it to
  land only. The law is part of the river recipe hash; the controller passes
  `streamPower`/`streamPowerEnabled` from the persisted process settings, so
  editing them restales and rebakes the river section.
- Pages: `SolveStreamPowerErosion` is skipped when the bake has the raster and
  the page has no authored height/uplift/protection/drainage constraints.
- Behaviour change: incision is macro scale (one texel is a hydrology cell, tens
  of km) and follows the baked tree; fine-scale per-page incision no longer
  happens for baked planets, so small-scale valley relief comes from the terrain
  noise only. Baked deltas on the test planet range about -93..+117 m.
- MCP/UI: `terrain.bake_status` reports `incision_active`/`incision_resolution`;
  the Planet Bake menu shows the incision state. No new settings.
- Tests: `Orbit.TerrainBake` (raster baked, recipe/hash, source lowers land,
  ocean untouched, file round trip), `Orbit.TerrainGpuField` (GPU vs CPU
  incision 0.025 m).
