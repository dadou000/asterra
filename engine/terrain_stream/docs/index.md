+++
path = "/rendering/terrain/streaming"
title = "Terrain sample streaming and toroidal residency"
kind = "subsystem"
status = "stable"
summary = "The CPU side of clipmap data supply: ToroidalResidency turns tracker motion into exposed-strip refresh regions, TerrainSampleStreamer evaluates the terrain source for those regions as jobs (including fine-footprint shading slopes), and TerrainMorphRefresh extends strip updates for morph-dependent samples. The static UniformPlanetMesh was removed in 0.0.9 with the sandbox app that drew it."
owner_module = "OrbitTerrainStream"
keywords = ["streaming", "toroidal", "residency", "sample streamer", "strip refresh", "morph refresh", "slope", "normal footprint", "jobs"]
sources = [
  "engine/terrain_stream/include/orbit/terrain_stream/TerrainMorphRefresh.hpp",
  "engine/terrain_stream/include/orbit/terrain_stream/TerrainSampleStreamer.hpp",
  "engine/terrain_stream/include/orbit/terrain_stream/ToroidalResidency.hpp",
  "engine/terrain_stream/CMakeLists.txt",
]
symbols = ["ToroidalResidency", "TerrainSampleStreamer", "TerrainSampleRequest", "TerrainSamplePatch"]
invariants = [
  "Ordinary clipmap motion changes a level's window centre by exact integer cell steps while surfaceFrame stays fixed, so retained toroidal samples keep their world-space address (/rendering/terrain/clipmaps/lattice-and-tracking).",
  "Shading normals/slopes are sampled at a fine, LOD-independent footprint (finite differences at a small epsilon) regardless of the level's own geometric spacing, so every ring shades with the micro-relief the finest level would see; a zero footprint falls back to the level's own spacing (legacy behaviour).",
  "Samples containing frame-relative morph targets and pre-blended heights/biomes need extended strip updates: call TerrainMorphRefresh before submitting sample jobs and commit the resulting patches together with the matching motion and residency state, otherwise rings and residency origins disagree.",
  "Eight normalised biome weights are packed as two RGBA8 values per sample.",
]
related = ["/rendering/terrain/clipmaps/lattice-and-tracking", "/rendering/terrain/contracts", "/rendering/terrain/clipmaps", "/foundation/jobs", "/legacy/problems"]
depends_on = ["/foundation/core", "/foundation/jobs", "/rendering/terrain/clipmaps", "/rendering/terrain/contracts", "/world/planet-coordinates"]
used_by = ["/editor/studio-session", "/rendering/terrain/gpu-passes"]
verify = [
  "ctest -R Orbit.TerrainStream",
  "ctest -R Orbit.TerrainSampleStreamer",
  "ctest -R Orbit.TerrainMorphRefresh",
]
verified = "b0a0de7f"
+++


