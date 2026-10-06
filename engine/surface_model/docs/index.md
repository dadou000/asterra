+++
path = "/world/surface-composition"
title = "Surface composition and material resolution"
kind = "subsystem"
status = "stable"
summary = "SurfaceComposition reconstructs concrete body surface capabilities from the authoritative semantic scene (per-body terrain services: geology, processes, biomes, water, constraints, GPU cache) and rebuilds the SurfaceRegistry; SurfaceMaterialResolver blends rendered surface materials from feature masks."
owner_module = "OrbitSurfaceModel"
keywords = ["surface composition", "terrain body services", "surface material resolver", "material blend", "process service", "rocky body"]
sources = [
  "engine/surface_model/include/orbit/surface_model/SurfaceComposition.hpp",
  "engine/surface_model/include/orbit/surface_model/SurfaceMaterialResolver.hpp",
  "engine/surface_model/include/orbit/surface_model/TerrainBodyServices.hpp",
  "engine/surface_model/CMakeLists.txt",
]
symbols = ["SurfaceCompositionStats", "SurfaceMaterialFeatureMasks", "TerrainProcessService"]
invariants = [
  "Surface capabilities are reconstructed from authoritative semantic children; they are derived, never a second authority.",
  "The registry is rebuilt whenever UniverseComposition changes, because SurfaceRegistry intentionally references the active BodyRegistry.",
  "Terrain process state remains in the canonical M08/M14 physical products; the service-level process configuration is solver policy only.",
]
related = ["/rendering/terrain", "/authoring/scene"]
depends_on = ["/authoring/commands", "/authoring/documents", "/authoring/scene", "/authoring/schema", "/foundation/core", "/rendering/terrain", "/rendering/terrain/clipmaps", "/rendering/terrain/gpu-cache", "/world/surface-registry", "/world/terrain-constraints", "/world/universe"]
verify = [
  "ctest -R Orbit.SurfaceComposition",
  "ctest -R Orbit.SurfaceMaterialResolver",
]
verified = "b0a0de7f"
+++


