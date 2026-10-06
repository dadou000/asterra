+++
path = "/celestial/appearance"
title = "Planetary appearance product"
kind = "subsystem"
status = "stable"
summary = "A disposable, revisioned far-view appearance product (PlanetaryAppearanceProduct and its GPU form) derived from terrain, climate, biome and water outputs so planets read correctly from orbit."
owner_module = "OrbitCelestialAppearance"
keywords = ["appearance", "planetary appearance", "far view", "albedo", "color", "orbital texture", "derived", "revision"]
sources = [
  "engine/celestial_appearance/include/orbit/celestial_appearance/PlanetaryAppearance.hpp",
  "engine/celestial_appearance/CMakeLists.txt",
]
symbols = ["AppearanceConfig"]
invariants = [
  "The product is derived from existing TerrainSource outputs (elevation, coarse elevation, climate temperature, biome classification and weights, standing-water depth, generation revisions); there is no second surface database, material hierarchy or persistent orbital texture authority.",
  "The product is disposable and revisioned: it is rebuilt when the TerrainSource or generation revisions change.",
]
related = ["/celestial/globe", "/celestial/far-render", "/rendering/terrain", "/legacy/research-v006-planetary-appearance"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/rhi", "/rendering/terrain"]
used_by = ["/celestial/far-render", "/celestial/giants", "/celestial/globe", "/celestial/ocean", "/celestial/small-bodies"]
verify = [
  "ctest -R Orbit.PlanetaryAppearance",
]
verified = "b0a0de7f"
+++


