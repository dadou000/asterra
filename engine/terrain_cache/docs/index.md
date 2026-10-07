+++
path = "/rendering/terrain/page-cache"
title = "CPU terrain page cache (CachedTerrainSource)"
kind = "subsystem"
status = "stable"
summary = "The CPU-side TerrainPageCache and CachedTerrainSource: pages of terrain samples (storage-only form: float heights and depth, float climate, RGBA8 biomes) with a byte budget, per-level entries and far-page pruning. Not the M26 persistent GPU cache (/rendering/terrain/gpu-cache)."
owner_module = "OrbitTerrainCache"
keywords = ["terrain page", "page cache", "cached terrain source", "prune", "budget", "cpu cache", "terrain page builder"]
sources = [
  "engine/terrain_cache/include/orbit/terrain_cache/CachedTerrainSource.hpp",
  "engine/terrain_cache/include/orbit/terrain_cache/TerrainPage.hpp",
  "engine/terrain_cache/include/orbit/terrain_cache/TerrainPageBuilder.hpp",
  "engine/terrain_cache/include/orbit/terrain_cache/TerrainPageCache.hpp",
  "engine/terrain_cache/CMakeLists.txt",
]
symbols = ["TerrainPageCache", "CachedTerrainSource", "TerrainPage", "TerrainPageDesc"]
invariants = [
  "PruneFarPages proactively drops cached pages that have fallen far behind the observer instead of waiting for the byte budget to fill; it is cheap enough to call about once a second, not every frame.",
  "EntriesByLevel on the source is diagnostic only.",
]
related = ["/rendering/terrain/gpu-cache", "/rendering/terrain"]
depends_on = ["/foundation/core", "/foundation/jobs", "/rendering/terrain/contracts", "/world/planet-coordinates"]
used_by = []
verify = [
  "ctest -R Orbit.TerrainCache",
]
verified = "b0a0de7f"
+++


