+++
path = "/rendering/terrain/biomes"
title = "Biomes: definition, base fallback and weighted placement (M19, M20)"
kind = "subsystem"
status = "stable"
summary = "BiomeService (one per terrain-bearing body) holds BiomeDefinitions with surface rules and scatter rules, an implicit BaseBiome that guarantees full coverage, weighted automatic placement from selector fields and durable authored mask overrides."
owner_module = "OrbitTerrainBiome"
keywords = ["biome", "base biome", "placement", "selector", "authored mask", "weights", "coverage", "biome definition", "composition mode", "forest", "moss"]
sources = [
  "engine/terrain_biome/include/orbit/terrain_biome/BiomeService.hpp",
  "engine/terrain_biome/CMakeLists.txt",
]
symbols = ["BiomeService", "BiomeDefinition", "BiomePlacementRules", "BiomeAutomaticSelector", "BiomeAuthoredMask", "ResolvedBiomeWeight"]
invariants = [
  "Only bodies that own a Terrain Surface capability get a BiomeService (SurfaceComposition creates exactly one per such body).",
  "Every BiomeService contains one implicit, stable BaseBiome with a deterministic ID derived from the stable body ID: it is stored separately from optional biomes, cannot be removed, is always in resolved coverage (possibly with zero residual weight) and receives all coverage not consumed by eligible optional biomes - a new terrain-bearing planet resolves to 100 % BaseBiome without any authored asset.",
  "Placement has two independent authored inputs: procedural selector rules stored on the biome asset (temperature, moisture, rainfall, elevation, slope, aspect, latitude, continentality, distance to coast/water, drainage, soil depth, sand depth, geology/material identity ...) and authored mask objects stored as semantic children of the biome asset; generated terrain pages are authority for neither.",
  "Final biome weights are derived from those rules and the current physical/climate fields.",
  "Biome edits are deliberately split into placement/rules, surface material and scatter so a scatter edit never reruns terrain processes (/rendering/terrain/invalidation).",
]
related = ["/rendering/terrain/scatter", "/rendering/terrain/invalidation", "/world/surface-composition", "/legacy/v0-0-4-m19-biome-base-fallback", "/legacy/v0-0-4-m20-biome-placement"]
depends_on = ["/foundation/core", "/rendering/terrain/geology", "/rendering/terrain/material-column", "/world/universe"]
used_by = ["/editor/model", "/rendering/terrain/debug-fields", "/rendering/terrain/scatter", "/world/surface-composition"]
verify = [
  "ctest -R Orbit.TerrainBiome",
]
verified = "b0a0de7f"
+++


