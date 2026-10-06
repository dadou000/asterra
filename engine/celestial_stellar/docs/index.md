+++
path = "/celestial/stellar"
title = "Stellar appearance"
kind = "subsystem"
status = "stable"
summary = "How a radiative photosphere looks: limb darkening, granulation, activity, chromosphere, corona and glare parameters (StellarAppearanceParameters) on top of the radiometric authority."
owner_module = "OrbitCelestialStellar"
keywords = ["star", "stellar", "limb darkening", "granulation", "corona", "chromosphere", "glare", "photosphere", "sun appearance"]
sources = [
  "engine/celestial_stellar/include/orbit/celestial_stellar/StellarAppearance.hpp",
  "engine/celestial_stellar/CMakeLists.txt",
]
symbols = ["StellarAppearanceParameters"]
invariants = [
  "Radiometry owns luminosity, radius, temperature, emissivity, irradiance and scene scaling; stellar appearance owns only the visual representation of the photosphere.",
  "Screen-space corona, glare and diffraction are never light sources and never feed back into the luminosity solve.",
  "The existing Photosphere capability is the only stellar surface authoring object: no new Star subclass or separate stellar editor exists; its radius and effective temperature are shared with radiometry.",
]
related = ["/celestial/radiometry", "/legacy/research-v006-stellar-rendering"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain"]
verify = [
  "ctest -R Orbit.CelestialStellar",
]
verified = "b0a0de7f"
+++


