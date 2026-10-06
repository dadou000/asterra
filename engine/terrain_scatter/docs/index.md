+++
path = "/rendering/terrain/scatter"
title = "Deterministic biome scatter (M22) and physical surface input (M18)"
kind = "subsystem"
status = "stable"
summary = "Reproducible, non-authoritative bulk instance placement (trees, shrubs, grass, stones, debris, clutter) from a deterministic planting grid on CPU and a fixed-slot GPU page, driven by biome weights and the canonical exposed physical surface."
owner_module = "OrbitTerrainScatter"
keywords = ["scatter", "vegetation", "instances", "planting grid", "trees", "grass", "stones", "deterministic", "density", "clutter"]
sources = [
  "engine/terrain_scatter/include/orbit/terrain_scatter/DeterministicScatter.hpp",
  "engine/terrain_scatter/include/orbit/terrain_scatter/PhysicalSurface.hpp",
  "engine/terrain_scatter/CMakeLists.txt"]
symbols = ["DeterministicScatter", "ScatterPageRequest", "DerivedScatterInstance", "PhysicalSurfaceScatterInput"]
invariants = [
  "Bulk scatter is derived data regenerated from stable physical/page inputs and biome scatter rules; named or explicitly authored placements stay authoring data, generated instances are never serialised.",
  "Scatter identity includes the planet and physical tile, source revision, scatter revision, generation seed and the persistent scatter-rule hash.",
  "Per-cell acceptance consumes the M20 biome weight, the M18 exposed material, slope, soil depth, moisture, exclusion mask and authored density; soil-dependent vegetation rejects bare bedrock through the same physical-surface input the renderer-facing stack uses.",
  "The GPU page has fixed capacity and writes one deterministic slot per planting-grid cell instead of append/atomic ordering, so CPU and GPU agree.",
  "PhysicalSurfaceScatterInput carries physical substrate context only: M18 contains no biome rules and biome filters can never replace geological identity."]
related = ["/rendering/terrain/biomes", "/rendering/terrain/material-column", "/rendering/terrain/invalidation"]
depends_on = ["/foundation/core", "/rendering/terrain/biomes", "/rendering/terrain/geology", "/rendering/terrain/material-column", "/world/planet-coordinates"]
used_by = ["/editor/studio-session", "/rendering/terrain/debug-fields", "/rendering/terrain/gpu-passes", "/rendering/terrain/material-column"]
verify = [
  "ctest -R Orbit.TerrainScatter"]
verified = "b0a0de7f"
+++


