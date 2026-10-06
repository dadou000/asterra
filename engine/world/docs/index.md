+++
path = "/world/planet-coordinates"
title = "Planet coordinates (cube-sphere tiles)"
kind = "subsystem"
status = "stable"
summary = "Planet-scale addressing: PlanetDefinition and SurfaceFrame, cube-face tiles (PlanetTileId, CubeCoordinate, CubeBounds), per-face projection and tile edge-neighbour mapping, and WorldPosition."
owner_module = "OrbitWorld"
keywords = ["planet", "cube sphere", "tile", "face", "projection", "world position", "neighbor", "edge", "coordinates"]
sources = [
  "engine/world/include/orbit/world/CubeProjection.hpp",
  "engine/world/include/orbit/world/Planet.hpp",
  "engine/world/include/orbit/world/PlanetTileNeighborhood.hpp",
  "engine/world/include/orbit/world/WorldPosition.hpp",
  "engine/world/CMakeLists.txt",
]
symbols = ["CubeCoordinate", "TileGridOffset", "WorldPosition"]
invariants = [
  "Projection onto a specific cube face (not a canonical major-axis face) is required at cube edges and corners, where one physical direction legitimately belongs to two or three faces.",
  "Local page-grid convention for physical page exchange: north is the minimum-v / y-1 edge, east is maximum-u / x+1, south is maximum-v / y+1, west is minimum-u / x-1; these are page-local orientation names, not compass directions.",
  "Terrain invalidation neighbourhoods are computed on this tile grid at one level (/rendering/terrain/invalidation).",
]
related = ["/rendering/terrain/invalidation", "/world/universe"]
depends_on = ["/foundation/core", "/foundation/math"]
used_by = ["/apps/sandbox", "/editor/studio-session", "/rendering/lighting/radiance-cache", "/rendering/terrain/contracts", "/rendering/terrain/debug-fields", "/rendering/terrain/erosion", "/rendering/terrain/gpu-passes", "/rendering/terrain/hydrology", "/rendering/terrain/impacts", "/rendering/terrain/macro-geology", "/rendering/terrain/page-cache", "/rendering/terrain/regions", "/rendering/terrain/relief", "/rendering/terrain/scatter", "/rendering/terrain/streaming", "/rendering/terrain/water", "/rendering/water", "/world/surface-registry", "/world/terrain-constraints"]
verify = [
  "ctest -R Orbit.WorldCoordinates",
  "ctest -R Orbit.PlanetTileNeighborhood",
]
verified = "b0a0de7f"
+++


