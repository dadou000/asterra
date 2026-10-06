+++
path = "/rendering/terrain/debug-fields"
title = "Terrain debug field visualiser (M29)"
kind = "subsystem"
status = "stable"
summary = "A stable catalogue of the 21 required debug fields with value classes and explicit upstream stage provenance, immutable live-page captures, per-page derived debug data, seam inspection and the derived GPU raster textures the viewport overlays use."
owner_module = "OrbitTerrainDebug"
keywords = ["debug field", "visualizer", "provenance", "seam", "overlay", "uplift view", "biome weights view", "cache residency", "physical lod", "raster", "debug texture"]
sources = [
  "engine/terrain_debug/include/orbit/terrain_debug/TerrainDebugField.hpp",
  "engine/terrain_debug/include/orbit/terrain_debug/TerrainDebugLivePages.hpp",
  "engine/terrain_debug/include/orbit/terrain_debug/TerrainDebugPageData.hpp",
  "engine/terrain_debug/include/orbit/terrain_debug/TerrainDebugRaster.hpp",
  "engine/terrain_debug/include/orbit/terrain_debug/TerrainDebugSeam.hpp",
  "engine/terrain_debug/include/orbit/terrain_debug/TerrainDebugTexture.hpp",
  "engine/terrain_debug/CMakeLists.txt",
]
symbols = ["TerrainDebugField", "TerrainDebugFieldDescriptor", "TerrainDebugPageData", "TerrainDebugLivePages", "TerrainDebugTexture", "TerrainDebugSeamInspection"]
invariants = [
  "TerrainDebugField contains exactly the 21 V0.0.4 views: Uplift, Bedrock Type, Strata, Regolith, Soil, Sand, Debris, Moisture, Exposed Material, Drainage, Water Flux, Sediment Flux, Wind, Aeolian Flux, Erosion/Deposition, Biome Weights, Final Biome, Scatter Density, Cache Residency, Cache Invalidation, Physical LOD.",
  "Every field has a stable value class and a typed upstream stage trace, so the meaning of a debug mode is defined here and renderer code cannot redefine it.",
  "The debug layer never mutates or replaces source terrain products: TerrainDebugPageData copies from canonical/derived products so returned spans stay stable, and TerrainDebugTexture never owns terrain authority.",
  "A live-page capture is one immutable snapshot of the products a physical page actually owns; every pointer/span is optional and absent products stay absent instead of being synthesised.",
]
related = ["/rendering/terrain/clipmaps/debugging", "/rendering/terrain/gpu-cache", "/legacy/v0-0-4-m29-debug-field-visualizer"]
depends_on = ["/foundation/core", "/rendering/rhi", "/rendering/terrain/biomes", "/rendering/terrain/contracts", "/rendering/terrain/erosion", "/rendering/terrain/geology", "/rendering/terrain/hydrology", "/rendering/terrain/macro-geology", "/rendering/terrain/material-column", "/rendering/terrain/regions", "/rendering/terrain/scatter", "/world/planet-coordinates"]
used_by = ["/editor/studio-session"]
verify = [
  "ctest -R Orbit.TerrainDebug",
  "ctest -R Orbit.TerrainDebugRaster",
  "ctest -R Orbit.TerrainDebugPageData",
  "ctest -R Orbit.TerrainDebugLivePages",
  "ctest -R Orbit.TerrainDebugSeam",
]
verified = "b0a0de7f"
+++


