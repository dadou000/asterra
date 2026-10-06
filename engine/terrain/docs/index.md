+++
path = "/rendering/terrain/contracts"
title = "Terrain contracts, sources and global fields (M00, M01)"
kind = "subsystem"
status = "stable"
summary = "The frozen terrain vocabulary: authority domains, residency, revision domains and generation triggers (TerrainContracts), the physical page address and key, planet-space surface positions (TerrainPosition), the TerrainSource interface with TerrainSample (elevation, climate, biome weights, standing water), the analytic whole-planet source (continents, tectonic plates, hotspots, mountains, craters) and its exported GlobalTerrainFields."
owner_module = "OrbitTerrain"
keywords = ["terrain contracts", "authority domain", "revision domain", "terrain source", "terrain sample", "physical page address", "analytic terrain", "tectonic", "plates", "hotspot", "seed domain", "stable hash", "canonical position"]
sources = [
  "engine/terrain/include/orbit/terrain/AnalyticTerrainSource.hpp",
  "engine/terrain/include/orbit/terrain/GlobalTerrainFields.hpp",
  "engine/terrain/include/orbit/terrain/TectonicFieldDesc.hpp",
  "engine/terrain/include/orbit/terrain/TerrainContracts.hpp",
  "engine/terrain/include/orbit/terrain/TerrainFields.hpp",
  "engine/terrain/include/orbit/terrain/TerrainPosition.hpp",
  "engine/terrain/include/orbit/terrain/TerrainSource.hpp",
  "engine/terrain/CMakeLists.txt",
]
symbols = ["TerrainAuthorityDomain", "TerrainGenerationRevisions", "TerrainGenerationTrigger", "PhysicalTerrainPageAddress", "PlanetSurfacePosition", "TerrainSample", "AnalyticTerrainSource", "GlobalTerrainFields"]
invariants = [
  "Authored intent, canonical physical state and derived/view state stay separate so editor, renderer and cache code cannot silently become terrain simulation authorities: only TerrainAuthorityDomain::TerrainPhysical owns canonical terrain, only WaterPhysical owns canonical water, and DerivedCache and ViewInterest are derived-only.",
  "Residency (Absent, Cpu, Gpu, CpuAndGpu) says where a representation is materialised and is deliberately neither authority nor identity.",
  "Physical products may be generated for exactly three reasons: MissingPhysicalPage, AuthorityRevisionChanged or ExplicitBake. A view may reveal that a page is missing, but camera motion itself is never a generation or invalidation reason.",
  "Every domain that can change canonical generated terrain has an explicit, monotonic revision (geology, climate, authoring, biome, water, processes); view/camera/render revisions are intentionally absent, so they cannot invalidate physical products.",
  "PhysicalTerrainPageAddress is a stable planet-space address with no render clipmap ring, GPU slot or frame state, so moving a camera cannot change physical page identity by construction; resolution and generation revisions invalidate cached output without changing the address.",
  "Persisted/cache identity and procedural seeds use fixed, platform-independent mixing (StableCombine64); std::hash is deliberately excluded because its representation is not an Orbit persistence contract; TerrainSeedDomain numeric values are persisted domain separators: append new domains, never renumber.",
  "Terrain locations are canonical physical surface positions in planet/body space, never a cube face, clipmap or render patch (PlanetSurfacePosition, TerrainSampleFootprint).",
  "The GPU and CPU generators must render the SAME plates and hotspots: GlobalTerrainFields exports the tectonic state for the GPU to upload verbatim instead of regenerating it from a different hash, so a range on the 2D map is the range the 3D clipmap shows (/rendering/terrain/clipmaps/generator-parity).",
  "Plate convergence/divergence/transform masks are direction-only (never footprint-dependent), and nearby plate types let a caller tell a continental collision from a subduction zone or a spreading ridge from a rift.",
]
related = ["/rendering/terrain", "/rendering/terrain/macro-geology", "/rendering/terrain/invalidation", "/world/planet-coordinates", "/legacy/v0-0-4-m00-contracts", "/legacy/v0-0-4-m01-surface-coordinates"]
depends_on = ["/foundation/core", "/foundation/math", "/world/planet-coordinates"]
used_by = ["/apps/sandbox", "/celestial/appearance", "/celestial/compact-objects", "/celestial/far-render", "/celestial/giants", "/celestial/globe", "/celestial/magnetosphere", "/celestial/ocean", "/celestial/rings", "/celestial/small-bodies", "/celestial/stellar", "/editor/studio-session", "/rendering/planet-map", "/rendering/terrain/debug-fields", "/rendering/terrain/erosion", "/rendering/terrain/geology", "/rendering/terrain/hydrology", "/rendering/terrain/impacts", "/rendering/terrain/macro-geology", "/rendering/terrain/material-column", "/rendering/terrain/page-cache", "/rendering/terrain/regions", "/rendering/terrain/relief", "/rendering/terrain/streaming", "/world/path-routing", "/world/surface-composition", "/world/surface-registry", "/world/terrain-constraints"]
verify = [
  "ctest -R Orbit.PlanetTerrain",
  "ctest -R Orbit.TerrainMountains",
  "ctest -R Orbit.TerrainFields",
]
verified = "b0a0de7f"
+++

Everything under `/rendering/terrain` builds on this module; the physical-process authority chain is documented on `/rendering/terrain`.
