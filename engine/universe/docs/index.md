+++
path = "/world/universe"
title = "Universe: bodies, shapes and body transforms"
kind = "subsystem"
status = "stable"
summary = "BodyRegistry holds systems and bodies with a sphere or ellipsoid shape, mass properties and a body transform model (fixed, uniform rotation, orbit-driven rotation, or independent orbit and orientation providers composed only when the FrameGraph asks); ReferenceSurface converts latitude/longitude/offset."
owner_module = "OrbitUniverse"
keywords = ["universe", "body", "system", "sphere", "ellipsoid", "mass", "rotation", "orbit", "reference surface", "latitude", "longitude"]
sources = [
  "engine/universe/include/orbit/universe/BodyRegistry.hpp",
  "engine/universe/include/orbit/universe/ReferenceSurface.hpp",
  "engine/universe/CMakeLists.txt"]
symbols = ["SphereShape", "SurfaceCoordinate"]
invariants = [
  "Orbital translation and body-fixed orientation are independent providers, composed only when the FrameGraph asks for parentFromBody (the M06 permanent composition path); the orbit-driven uniform rotation is a compatibility bridge.",
  "SurfaceCoordinate uses radians for latitude/longitude and metres along the reference ellipsoid normal for offset."]
related = ["/foundation/frames", "/foundation/time", "/celestial"]
depends_on = ["/celestial/orbits", "/celestial/rotation", "/foundation/core", "/foundation/frames", "/foundation/math", "/foundation/time"]
used_by = ["/apps/studio", "/celestial/far-render", "/celestial/globe", "/editor/ui-toolkit", "/rendering/lighting/radiance-cache", "/rendering/terrain/biomes", "/rendering/terrain/water", "/world/fields", "/world/path-geometry", "/world/path-routing", "/world/paths", "/world/surface-composition", "/world/surface-registry", "/world/world-model"]
verify = [
  "ctest -R Orbit.BodyRegistry"]
verified = "b0a0de7f"
+++


