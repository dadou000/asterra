+++
path = "/foundation/math"
title = "Math types"
kind = "subsystem"
status = "stable"
summary = "Header-only vector, matrix and rigid-transform types (Vec2/Vec3/Vec4, Mat4, Double3x3, RigidTransformD) used across the engine; double precision is the norm for planet-scale positions (math::Double3, math::Double2) with float variants for GPU-facing data."
owner_module = "OrbitMath"
keywords = ["math", "vector", "matrix", "transform", "double3", "float3", "rigid transform"]
sources = [
  "engine/math/include/orbit/math/Matrix.hpp",
  "engine/math/include/orbit/math/RigidTransform.hpp",
  "engine/math/include/orbit/math/Vector.hpp",
  "engine/math/CMakeLists.txt",
]
symbols = ["Mat4", "Double3x3", "Vec2"]
invariants = [
  "RigidTransformD stores column vectors: the local X/Y/Z axes expressed in the parent frame.",
  "Multiply(lhs, rhs) returns lhs * rhs: a point is transformed by rhs first, then lhs.",
]
depends_on = ["/foundation/core"]
used_by = ["/apps/sandbox", "/authoring/schema", "/celestial/appearance", "/celestial/compact-objects", "/celestial/far-render", "/celestial/giants", "/celestial/globe", "/celestial/gravity", "/celestial/lighting", "/celestial/magnetosphere", "/celestial/ocean", "/celestial/orbits", "/celestial/radiometry", "/celestial/rings", "/celestial/rotation", "/celestial/small-bodies", "/celestial/stellar", "/editor/ui-toolkit", "/foundation/frames", "/foundation/platform", "/rendering/free-camera", "/rendering/lighting/radiance-cache", "/rendering/planet-map", "/rendering/render-view", "/rendering/rhi", "/rendering/shading", "/rendering/terrain/contracts", "/rendering/terrain/erosion", "/rendering/terrain/gpu-passes", "/rendering/terrain/hydrology", "/rendering/terrain/impacts", "/rendering/terrain/macro-geology", "/rendering/terrain/regions", "/rendering/terrain/relief", "/rendering/terrain/water", "/rendering/volumes/fields", "/rendering/volumes/render", "/rendering/volumes/representation", "/rendering/volumes/solver", "/rendering/water", "/world/fields", "/world/path-geometry", "/world/path-routing", "/world/paths", "/world/planet-coordinates", "/world/surface-registry", "/world/terrain-constraints", "/world/universe", "/world/world-model"]
verify = [
  "ctest -R Orbit.Camera",
]
verified = "b0a0de7f"
+++

No sources of its own (header-only). It depends only on `Core`.
