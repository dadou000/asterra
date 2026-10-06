+++
path = "/celestial/compact-objects"
title = "Compact objects and accretion flows"
kind = "subsystem"
status = "stable"
summary = "Compact Object and Accretion Flow body capabilities (GM, model selection, spin, shadow presentation scale) with a replaceable Schwarzschild baseline; they require neither Surface nor Reference Shape authority."
owner_module = "OrbitCelestialCompactObjects"
keywords = ["compact object", "black hole", "neutron star", "accretion", "schwarzschild", "shadow", "kerr"]
sources = [
  "engine/celestial_compact_objects/include/orbit/celestial_compact_objects/CompactObject.hpp",
  "engine/celestial_compact_objects/CMakeLists.txt",
]
symbols = ["CompactObjectParameters"]
invariants = [
  "A celestial body is not assumed to be a solid-surface object: Compact Object and Accretion Flow are ordinary body capabilities needing no Surface or Reference Shape authority.",
  "The runtime model is a deliberately replaceable Schwarzschild baseline that fixes the semantic contracts for later Kerr, ray-integrated lensing and radiative transfer; it does not claim to be full general relativity.",
]
related = ["/celestial/compact-render", "/legacy/research-v006-compact-objects"]
depends_on = ["/celestial/representation", "/foundation/core", "/foundation/math", "/rendering/terrain"]
used_by = ["/celestial/compact-render"]
verify = [
  "ctest -R Orbit.CelestialCompactObjects",
]
verified = "b0a0de7f"
+++


