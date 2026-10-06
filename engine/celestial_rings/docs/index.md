+++
path = "/celestial/rings"
title = "Ring systems"
kind = "subsystem"
status = "stable"
summary = "One semantic Ring System per body with arbitrary authored radial bands (RingBand), and disposable near (mesh) and far (profile sampling) render products with ring lighting."
owner_module = "OrbitCelestialRings"
keywords = ["rings", "ring system", "ring band", "saturn", "ring mesh", "far ring", "radial profile"]
sources = [
  "engine/celestial_rings/include/orbit/celestial_rings/RingSystem.hpp",
  "engine/celestial_rings/CMakeLists.txt",
]
symbols = ["RingBand"]
invariants = [
  "There is one ring-system authority per body with child Ring Bands and no authoring band-count cap.",
  "Ring geometry is never persisted as celestial authority; near and far products are derived and disposable.",
]
related = ["/celestial", "/legacy/research-v006-ring-system"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/render-view", "/rendering/rhi", "/rendering/shader-compiler", "/rendering/terrain/contracts"]
used_by = ["/editor/studio-ui", "/world/world-model"]
verify = [
  "ctest -R Orbit.CelestialRings",
]
verified = "b0a0de7f"
+++


