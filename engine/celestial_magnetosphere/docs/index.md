+++
path = "/celestial/magnetosphere"
title = "Magnetosphere and aurora"
kind = "subsystem"
status = "stable"
summary = "A replaceable semantic/runtime seam for magnetic-field and auroral presentation: body-fixed dipole axis and field strength, solar-wind drivers, magnetopause shape and the auroral oval, producing a MagnetosphereProduct with an aurora mesh."
owner_module = "OrbitCelestialMagnetosphere"
keywords = ["magnetosphere", "aurora", "dipole", "magnetopause", "solar wind", "auroral oval", "magnetic field"]
sources = [
  "engine/celestial_magnetosphere/include/orbit/celestial_magnetosphere/Magnetosphere.hpp",
  "engine/celestial_magnetosphere/CMakeLists.txt",
]
symbols = ["MagnetosphereParameters"]
invariants = [
  "Atmosphere is not the owner of magnetospheric state; the Magnetosphere / Aurora capability owns explicit body-fixed parameters (dipole axis, equatorial field, solar-wind direction and pressure, magnetopause standoff and flaring, auroral oval latitude/width and altitude interval, activity).",
  "This is a presentation seam, not magnetohydrodynamics.",
]
related = ["/celestial/magnetosphere-render", "/legacy/research-v006-magnetosphere-aurora"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain"]
used_by = ["/celestial/magnetosphere-render"]
verify = [
  "ctest -R Orbit.CelestialMagnetosphere",
]
verified = "b0a0de7f"
+++


