+++
path = "/celestial/radiometry"
title = "Radiometry (emitters, scene scale, exposure)"
kind = "subsystem"
status = "stable"
summary = "SI radiometry for celestial emitters: BlackbodyEmitter/RadiativeState from bolometric luminosity, effective temperature, emissivity and radius, plus the scene encoding and ExposureSettings/ExposureState shared with the HDR display boundary."
owner_module = "OrbitCelestialRadiometry"
keywords = ["radiometry", "luminosity", "blackbody", "stefan boltzmann", "emissivity", "irradiance", "exposure", "hdr", "scene units", "star brightness"]
sources = [
  "engine/celestial_radiometry/include/orbit/celestial_radiometry/Radiometry.hpp",
  "engine/celestial_radiometry/CMakeLists.txt",
]
symbols = ["BlackbodyEmitter"]
invariants = [
  "Stellar brightness is explicit SI radiometry, never an arbitrary multiplier: M = emissivity * sigma * T^4, L = 4 pi R^2 M, radiance = M / pi with sigma = 5.670374419e-8 W m^-2 K^-4.",
  "RadiativeEmitter and Photosphere remain ordinary semantic capabilities; luminosity can be derived from the photosphere radius and temperature via an explicit toggle.",
  "Appearance layers (corona, glare, diffraction) never become light sources and never feed back into the luminosity solve.",
]
related = ["/celestial/stellar", "/celestial/lighting", "/rendering/lighting/eye-adaptation", "/legacy/research-v006-radiometry-hdr-exposure"]
depends_on = ["/foundation/core", "/foundation/math"]
used_by = ["/celestial/lighting"]
verify = [
  "ctest -R Orbit.CelestialRadiometry",
]
verified = "b0a0de7f"
+++


