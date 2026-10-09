# Tectonic structural layer

- Existing owner: `TectonicField` (plates, boundary masks, hotspots) inside
  `GlobalTerrainFields`; recipe persisted on the Terrain Surface and edited by
  the Planet Tectonics menu / `terrain.tectonics_get/set`.
- Primary insertion points: `TectonicField::SampleStructure`,
  `GlobalTerrainFields::SampleTectonicStructure`, `ProbeTectonicStructure`
  (studio_session), the Tectonics menu readout and `terrain.tectonics_sample`.
- Canonical state: none added. Crust thickness/age, uplift, subsidence, stress
  and volcanism are derived from the existing recipe and per-plate hashes;
  existing terrain elevation is unchanged.
- Consumers (hydrology watersheds, biomes, hazards) should query this layer
  rather than re-deriving plate logic.
- Iteration: native changes follow the central Studio generation handoff; the
  MCP wrapper and docs reflect on server reload.
