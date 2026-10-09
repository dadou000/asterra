# Erosion and watersheds read geological age and uplift

- Existing owners: `MacroGeologyField` (M05 uplift/drainage guidance),
  `SolveStreamPowerErosion` (M10) and the M09 `DrainagePage` router, driven by
  the physical-page builder in `StudioTerrainPhysicalPageService`.
- Primary insertion points: `MacroGeologySample` (age/crust fields, tectonic
  drainage steer), `StreamPowerCellForcing::geologicalAge`, and the persisted
  stream-power process settings (`ageErodibilityGain`, `ageUpliftDecay`,
  `tectonicDrainageGuidance`).
- Canonical state: three Terrain Process Settings properties; everything else is
  derived from the tectonic structural layer. Defaults on the config are active
  (0.6 / 0.35 / 0.5); the `MacroGeologyDesc` defaults stay neutral so direct
  users are unchanged.
- Watersheds: the steer is folded into the existing M09 `authoredDrainage`
  channel, so belts act as divides without forcing uphill flow.
- UI/MCP parity: Planet Tectonics menu "Geological Coupling" section,
  `terrain.erosion_coupling_get/set`, `orbit_terrain_erosion_coupling_get/set`.
- Iteration: native changes follow the central Studio generation handoff;
  setting edits queue global process-settings invalidation, no rebuild.
