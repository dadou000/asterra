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
  "engine/terrain_stream/src/TerrainSampleStreamer.cpp",
  "engine/terrain_render/src/TerrainPreviewRenderer.cpp",
  "engine/terrain_gpu/src/FieldGenerationCompute.hpp",
  "tests/TerrainSampleStreamerTests.cpp",
  "engine/terrain_stream/include/orbit/terrain_stream/ToroidalResidency.hpp",
  "engine/terrain_stream/CMakeLists.txt",
]
symbols = ["ToroidalResidency", "TerrainSampleStreamer", "TerrainSampleRequest", "TerrainSamplePatch"]
invariants = [
  "Ordinary clipmap motion changes a level's window centre by exact integer cell steps while surfaceFrame stays fixed, so retained toroidal samples keep their world-space address (/rendering/terrain/clipmaps/lattice-and-tracking).",
  "Shading-normal footprint/epsilon overrides are optional; zero falls back to the level footprint/spacing. Production explicitly supplies each ring's own footprint/spacing to avoid undersampled micro-relief on coarse rings.",
  "Morph slopes blend toward the parent evaluated with coarseFootprintMeters and coarseSpacingMeters, matching the GPU. Fine overrides must not leak into the coarse endpoint. The CPU reuses coarseSample as the center height at that same footprint.",
  "Samples containing frame-relative morph targets and pre-blended heights/biomes need extended strip updates: call TerrainMorphRefresh before submitting sample jobs and commit the resulting patches together with the matching motion and residency state, otherwise rings and residency origins disagree.",
  "Eight normalised biome weights are packed as two RGBA8 values per sample.",
]
related = ["/rendering/terrain/clipmaps/lattice-and-tracking", "/rendering/terrain/contracts", "/rendering/terrain/clipmaps", "/foundation/jobs", "/legacy/problems"]
depends_on = ["/foundation/core", "/foundation/jobs", "/rendering/terrain/clipmaps", "/rendering/terrain/contracts", "/world/planet-coordinates"]
used_by = ["/editor/studio-session", "/rendering/terrain/gpu-passes"]
verify = [
  "ctest -R Orbit.TerrainStream",
  "ctest -R Orbit.TerrainSampleStreamer (also compares child slopes with independently generated parent samples on a footprint-dependent curved surface)",
  "ctest -R Orbit.TerrainMorphRefresh",
]
verified = "8dc17324af29e897a3eb204ad9d93e3319b832c4"
+++


