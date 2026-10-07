+++
path = "/celestial/orbits"
title = "Orbit state providers (fixed, conic, ephemeris, N-body)"
kind = "subsystem"
status = "stable"
summary = "The time-dependent orbital contract EvaluateState(time) -> position, velocity, quality, with providers for fixed orbits, analytic conics (perifocal plane rotated by argument of periapsis, inclination and longitude of ascending node), imported ephemerides (cubic Hermite) and dynamically promoted N-body domains."
owner_module = "OrbitCelestialOrbits"
keywords = ["orbit", "orbit state", "kepler", "conic", "ephemeris", "n-body", "velocity verlet", "perifocal", "provider", "orbit state provider"]
sources = [
  "engine/celestial_orbits/include/orbit/celestial_orbits/ImportedEphemeris.hpp",
  "engine/celestial_orbits/include/orbit/celestial_orbits/NBodyDomain.hpp",
  "engine/celestial_orbits/include/orbit/celestial_orbits/OrbitState.hpp",
  "engine/celestial_orbits/CMakeLists.txt",
]
symbols = ["EphemerisSample", "NBodyMemberSeed", "OrbitState"]
invariants = [
  "WorldModel and FrameGraph consume the orbit contract but do not own orbital mathematics; FrameGraph remains the only spatial hierarchy.",
  "The provider interface is the single seam: fixed, analytic conic, imported ephemeris and dynamic N-body all implement OrbitStateProvider.",
  "Analytic conics are evaluated in the standard perifocal plane and rotated into the parent frame by argument of periapsis, then inclination, then longitude of ascending node; the state is relative to the semantic parent frame.",
  "No third-party ephemeris, astronomy-library, SPICE, Horizons or file-format type crosses the runtime boundary: importers convert external data to Orbit-native samples (time in integer microseconds, position in metres, velocity in m/s, already in the parent frame).",
  "Imported ephemerides are persistent scene authority (Ephemeris Asset with Ephemeris Sample children bound through the schema Source Object reference), not an opaque runtime cache; they interpolate with cubic Hermite because both position and velocity exist.",
  "N-body promotion is a simulation-LOD layer: it snapshots each member's existing provider at the domain epoch (any provider kind), integrates with fixed-step velocity Verlet and exposes the result through the same provider interface; it creates no new body identity or transform hierarchy.",
]
related = ["/world/universe", "/celestial/rotation", "/celestial/gravity", "/legacy/tree-history-research-v006-analytic-orbits", "/legacy/tree-history-research-v006-imported-ephemeris", "/legacy/tree-history-research-v006-dynamic-nbody"]
depends_on = ["/foundation/core", "/foundation/math", "/foundation/time"]
used_by = ["/celestial/rotation", "/world/universe", "/world/world-model"]
verify = [
  "ctest -R Orbit.AnalyticConics",
  "ctest -R Orbit.ImportedEphemeris",
  "ctest -R Orbit.NBodyDomain",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"
+++


