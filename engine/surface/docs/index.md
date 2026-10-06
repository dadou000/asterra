+++
path = "/world/surface-registry"
title = "Surface registry (terrain capability attachment)"
kind = "subsystem"
status = "stable"
summary = "SurfaceRegistry is the CPU-authoritative attachment point between generic celestial bodies and terrain-specific surface implementations: convert body points to and from SurfaceCoordinate, attach or detach a TerrainSource, and derive a spherical PlanetDefinition."
owner_module = "OrbitSurface"
keywords = ["surface", "surface registry", "terrain capability", "attach terrain", "surface coordinate", "radial offset"]
sources = [
  "engine/surface/include/orbit/surface/SurfaceRegistry.hpp",
  "engine/surface/CMakeLists.txt",
]
symbols = ["SurfaceCoordinate"]
invariants = [
  "SurfaceCoordinate.radialOffsetMeters is a geometric offset from the reference shape, deliberately distinct from terrain elevation: it works for bodies with no terrain capability.",
  "Terrain is a capability attached per body; bodies without it still have a valid surface.",
]
related = ["/world/universe", "/rendering/terrain"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain/contracts", "/world/planet-coordinates", "/world/universe"]
used_by = ["/apps/sandbox", "/world/path-routing", "/world/surface-composition"]
verify = [
  "ctest -R Orbit.SurfaceRegistry",
]
verified = "b0a0de7f"
+++


