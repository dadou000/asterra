+++
path = "/world/terrain-constraints"
title = "Authored terrain constraints"
kind = "subsystem"
status = "stable"
summary = "TerrainConstraints defines authored terrain intent as sets of constraint primitives (point, brush, spline, polygon, raster mask) with a composition mode (Add, Subtract, Replace, Multiply, Min, Max) that the terrain stack composes with procedural and simulated terrain."
owner_module = "OrbitSurfaceAuthoring"
keywords = ["terrain constraints", "authored terrain", "brush", "spline", "polygon", "raster mask", "composition mode", "canyon", "constraint set"]
sources = [
  "engine/surface_authoring/include/orbit/surface_authoring/TerrainConstraints.hpp",
  "engine/surface_authoring/CMakeLists.txt",
]
symbols = ["PointConstraintPrimitive"]
invariants = [
  "Authored constraints are project authority; the resulting terrain is derived state (V0.0.4 M04).",
  "Constraint edits invalidate terrain through the dependency graph as TerrainAuthoring changes (/rendering/terrain/invalidation).",
]
related = ["/rendering/terrain/invalidation", "/legacy/v0-0-4-m04-authored-terrain-constraints"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain", "/world/planet-coordinates"]
used_by = ["/world/surface-composition"]
verify = [
  "ctest -R Orbit.TerrainAuthoredConstraints",
]
verified = "b0a0de7f"
+++


