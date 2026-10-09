+++
path = "/celestial/rotation"
title = "Rotation and orientation providers"
kind = "subsystem"
status = "stable"
summary = "OrientationProvider implementations for fixed, uniform-spin and synchronous orientation. Body-fixed orientation is independent of orbital translation and is combined with it only when the FrameGraph evaluates the body's parent transform."
owner_module = "OrbitCelestialRotation"
keywords = ["rotation", "orientation", "spin", "pole", "axial tilt", "synchronous", "tidally locked", "orientation provider"]
sources = [
  "engine/celestial_rotation/include/orbit/celestial_rotation/OrientationState.hpp",
  "engine/celestial_rotation/CMakeLists.txt",
]
symbols = ["OrientationState"]
invariants = [
  "parentFromBody(t) combines orientationProvider(t) with orbitStateProvider(t).position; both providers stay independent and are composed only when the FrameGraph asks.",
  "The authored pole vector defines the body's +Z axis in the parent frame: the pole is normalised, a stable orthonormal equatorial X/Y basis is built perpendicular to it, X/Y rotate about it by phase and +Z is preserved exactly - tilt has a direct geometric meaning, not a generic axis-angle about the parent identity basis.",
  "OrientationProvider is the extension seam for later precession, nutation, libration and higher-order models.",
]
related = ["/celestial/orbits", "/world/universe", "/legacy/tree-history-research-v006-rotation-orientation"]
depends_on = ["/celestial/orbits", "/foundation/core", "/foundation/math", "/foundation/time"]
used_by = ["/world/universe", "/world/world-model"]
verify = [
  "ctest -R Orbit.CelestialRotation",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"
+++


