+++
path = "/celestial/lighting"
title = "Eclipses, transits and reflected light"
kind = "subsystem"
status = "stable"
summary = "Render-LOD-independent celestial lighting geometry: finite apparent discs (alpha = asin(radius / distance)), single and multiple occultation with exact and sampled overlap, direct irradiance at a receiver and reflected light from lit bodies."
owner_module = "OrbitCelestialLighting"
keywords = ["eclipse", "transit", "occultation", "penumbra", "annular", "reflected light", "albedo", "apparent disc", "irradiance", "moonlight"]
sources = [
  "engine/celestial_lighting/include/orbit/celestial_lighting/CelestialLighting.hpp",
  "engine/celestial_lighting/CMakeLists.txt",
]
symbols = ["ApparentDisc"]
invariants = [
  "The service works on authoritative body positions, radii and semantic radiative emitters and does not depend on the macro-globe, impostor or point-proxy choice.",
  "Source and occluder are finite apparent discs; an occluder contributes only when its centre is closer to the receiver than the source centre; single-occluder overlap uses the analytic circle-intersection area.",
  "Independent visibility fractions are never multiplied: several occluders are evaluated as the union of projected discs over the source disc with a deterministic equal-solid-angle golden-angle sample set.",
  "Results expose obscured and visible fractions plus front/overlap, total-eclipse and annular-eclipse states.",
]
related = ["/celestial/radiometry", "/rendering/atmosphere", "/legacy/tree-history-research-v006-eclipse-reflected-light"]
depends_on = ["/celestial/radiometry", "/foundation/core", "/foundation/math"]
used_by = ["/celestial/far-render", "/editor/studio-session", "/world/world-model"]
verify = [
  "ctest -R Orbit.CelestialLighting",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"
+++


