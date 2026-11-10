+++
path = "/celestial/gravity"
title = "Gravity service"
kind = "subsystem"
status = "stable"
summary = "A reusable gravity query service: AccelerationFrom(source, point, time) and TotalAcceleration(point, time) over semantic gravity sources, with pluggable GravityModel evaluated in the source's own frame (point-mass baseline a = -mu r / |r|^3, G = 6.67430e-11)."
owner_module = "OrbitCelestialGravity"
keywords = ["gravity", "acceleration", "gravity model", "point mass", "j2", "spherical harmonics", "gravity source"]
sources = [
  "engine/celestial_gravity/include/orbit/celestial_gravity/GravityService.hpp",
  "engine/celestial_gravity/CMakeLists.txt",
]
symbols = ["GravityModel"]
invariants = [
  "Gravity is a query service separate from orbital propagation and N-body integration: it does not own transforms, body identities or integration state.",
  "A model evaluates acceleration in its own source/body frame (AccelerationLocal(sourceToPointMeters)); the service transforms the query point into the source frame, evaluates, then rotates the acceleration back into the caller's frame - required so J2 and spherical-harmonic fields, which depend on the body's axis, evaluate correctly.",
  "Coordinates come from the existing FrameGraph and semantic gravity sources.",
]
related = ["/celestial/orbits", "/foundation/frames", "/legacy/tree-history-research-v006-gravity-capability"]
depends_on = ["/foundation/core", "/foundation/frames", "/foundation/math", "/foundation/time"]
used_by = ["/world/world-model"]
verify = [
  "ctest -R Orbit.GravityService",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"
+++


