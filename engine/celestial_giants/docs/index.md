+++
path = "/celestial/giants"
title = "Gas and ice giant appearance"
kind = "subsystem"
status = "stable"
summary = "Procedural cloud-top appearance for gas and ice giants (GiantClass, GiantAppearanceParameters) without inventing terrain or a solid surface."
owner_module = "OrbitCelestialGiants"
keywords = ["gas giant", "ice giant", "cloud tops", "jupiter", "bands", "giant appearance"]
sources = [
  "engine/celestial_giants/include/orbit/celestial_giants/GiantAppearance.hpp",
  "engine/celestial_giants/CMakeLists.txt",
]
symbols = ["GiantAppearanceParameters"]
invariants = [
  "A giant is an ordinary Celestial Body with ordinary shape, mass, orbit, rotation, gravity and optional atmosphere capabilities; Giant Appearance owns only procedural cloud-top parameters.",
  "Giant Appearance does not own shape, mass/gravity, rotation/orbit, atmospheric density and scattering (the Atmosphere capability stays the physical authority), radiative emission, ring systems or a terrain surface.",
]
related = ["/rendering/atmosphere", "/celestial", "/legacy/research-v006-gas-ice-giants"]
depends_on = ["/celestial/appearance", "/foundation/core", "/foundation/math", "/rendering/terrain/contracts"]
used_by = ["/editor/studio-session", "/editor/studio-ui", "/world/world-model"]
verify = [
  "ctest -R Orbit.CelestialGiants",
]
verified = "b0a0de7f"
+++


