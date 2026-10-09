+++
path = "/world/surface-composition"
title = "Surface composition and material resolution"
kind = "subsystem"
status = "stable"
summary = "SurfaceComposition reconstructs concrete body surface capabilities from the authoritative semantic scene (per-body terrain services: geology, stratigraphy, chronological impact/resurfacing history, processes, biomes, water, constraints, GPU cache) and rebuilds the SurfaceRegistry; SurfaceMaterialResolver blends rendered surface materials from feature masks."
owner_module = "OrbitSurfaceModel"
keywords = ["surface composition", "terrain body services", "surface material resolver", "material blend", "process service", "rocky body"]
sources = [
  "engine/surface_model/include/orbit/surface_model/SurfaceComposition.hpp",
  "engine/surface_model/include/orbit/surface_model/SurfaceMaterialResolver.hpp",
  "engine/surface_model/include/orbit/surface_model/TerrainBodyServices.hpp",
  "engine/surface_model/src/SurfaceComposition.cpp",
  "engine/surface_model/CMakeLists.txt",
]
symbols = ["SurfaceCompositionStats", "SurfaceMaterialFeatureMasks", "TerrainProcessService"]
invariants = [
  "Surface capabilities are reconstructed from authoritative semantic children; they are derived, never a second authority.",
  "TerrainDescription reconstructs its tectonic recipe from the Terrain Surface semantic properties; the Planet toolbar's SurfaceAuthoringModel edits those values transactionally, and composition feeds them into the same analytic GlobalTerrainFields used by terrain generation.",
  "TerrainDescription parses the saved .orbitimpacts recipe from the terrain semantic property, rejects a recipe bound to another planet, and composes the immutable impact/resurfacing/ice authority into AnalyticTerrainSource; the scene property remains canonical and the compiled spatial fields remain derived.",
  "The saved .orbitstratigraphy TOML is parsed and compiled against the body's M02 material library during composition. Studio physical-page builds consume the immutable profile to resolve the bedrock identity exposed after impact excavation; the scene profile is canonical and the compiled cumulative layer boundaries are derived.",
  "The registry is rebuilt whenever UniverseComposition changes, because SurfaceRegistry intentionally references the active BodyRegistry.",
  "Terrain process state remains in the canonical M08/M14 physical products; the service-level process configuration is solver policy only.",
]
related = ["/rendering/terrain", "/authoring/scene"]
depends_on = ["/authoring/commands", "/authoring/documents", "/authoring/scene", "/authoring/schema", "/foundation/core", "/rendering/terrain/biomes", "/rendering/terrain/clipmaps", "/rendering/terrain/contracts", "/rendering/terrain/erosion", "/rendering/terrain/geology", "/rendering/terrain/gpu-passes", "/rendering/terrain/material-column", "/rendering/terrain/water", "/world/surface-registry", "/world/terrain-constraints", "/world/universe", "/world/world-model"]
used_by = ["/editor/model", "/editor/session", "/editor/studio-session"]
verify = [
  "ctest -R Orbit.SurfaceComposition",
  "ctest -R Orbit.SurfaceMaterialResolver",
]
verified = "b0a0de7f"
+++
