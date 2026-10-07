+++
path = "/celestial/small-bodies"
title = "Small body appearance (asteroids, comets, moonlets)"
kind = "subsystem"
status = "stable"
summary = "Irregular small-body shape and regolith appearance: a disposable directional radius multiplier on the Reference Shape (axis scale, low-frequency lobes, craters) sampled on a cube sphere, plus rough-surface photometry."
owner_module = "OrbitCelestialSmallBodies"
keywords = ["asteroid", "comet", "small body", "irregular", "regolith", "crater", "triaxial", "photometry"]
sources = [
  "engine/celestial_small_bodies/include/orbit/celestial_small_bodies/SmallBodyAppearance.hpp",
  "engine/celestial_small_bodies/CMakeLists.txt",
]
symbols = ["SmallBodyParameters"]
invariants = [
  "Small bodies are ordinary celestial bodies with a specialised derived appearance/shape capability: no new body class and no requirement for the planetary toroidal terrain stack.",
  "The semantic Reference Shape remains the physical scale authority; the radial product is derived, fingerprinted, disposable and cube-sphere sampled so later mesh or ray representations can share one deterministic source.",
]
related = ["/celestial", "/legacy/tree-history-research-v006-small-body-rendering"]
depends_on = ["/celestial/appearance", "/foundation/core", "/foundation/math", "/rendering/terrain/contracts"]
used_by = ["/editor/studio-session", "/editor/studio-ui", "/world/world-model"]
verify = [
  "ctest -R Orbit.CelestialSmallBodies",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"
+++


