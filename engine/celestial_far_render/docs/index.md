+++
path = "/celestial/far-render"
title = "Far body renderer (smooth globe, disc impostors, point proxies)"
kind = "subsystem"
status = "stable"
summary = "FarBodyRenderer draws the lowest-fidelity rungs of the representation ladder: smooth globe, analytic and cached disc impostors and point/stellar proxies."
owner_module = "OrbitCelestialFarRender"
keywords = ["far render", "impostor", "disc", "point proxy", "smooth globe", "stellar point", "cached disc", "distant body"]
sources = [
  "engine/celestial_far_render/include/orbit/celestial_far_render/FarBodyRenderer.hpp",
  "engine/celestial_far_render/CMakeLists.txt",
]
symbols = ["AppearanceSummary"]
invariants = [
  "All far representation products remain derived and disposable; none becomes semantic body authority.",
  "Terrain-capable bodies derive appearance from the planetary appearance product; non-terrain bodies take their reference shape from BodyRegistry and select stellar behaviour from ordinary capabilities such as RadiativeEmitter.",
]
related = ["/celestial/representation", "/celestial/appearance", "/legacy/research-v006-far-celestial-representations"]
depends_on = ["/celestial/appearance", "/celestial/lighting", "/celestial/representation", "/foundation/core", "/foundation/math", "/rendering/render-view", "/rendering/rhi", "/rendering/terrain", "/world/universe"]
verify = [
  "ctest -R Orbit.CelestialFarRender",
]
verified = "b0a0de7f"
+++


