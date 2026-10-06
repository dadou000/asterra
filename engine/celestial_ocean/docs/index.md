+++
path = "/celestial/ocean"
title = "Orbital ocean optics"
kind = "subsystem"
status = "stable"
summary = "OceanOpticalParameters make standing planetary water physically recognisable from orbit by deriving appearance from the same terrain authority that renders the ground ocean."
owner_module = "OrbitCelestialOcean"
keywords = ["ocean", "orbital ocean", "water optics", "ocean color", "shoreline", "sea level"]
sources = [
  "engine/celestial_ocean/include/orbit/celestial_ocean/OceanOptics.hpp",
  "engine/celestial_ocean/CMakeLists.txt",
]
symbols = ["OceanOpticalParameters"]
invariants = [
  "The Ocean celestial capability contains optical/rendering parameters only: it owns no coastline, wet/dry mask or sea level.",
  "Ground and orbit share one authority: TerrainSample.elevationMeters is the ground/bed and standingWaterDepthMeters the canonical exterior standing-water depth; ground terrain renders bed plus standing water on the same clipmap triangles.",
  "The legacy detached OceanRenderer is not reintroduced into the production planetary path.",
]
related = ["/rendering/water", "/celestial/appearance", "/legacy/research-v006-orbital-ocean", "/legacy/standing-water-rendering"]
depends_on = ["/celestial/appearance", "/foundation/core", "/foundation/math", "/rendering/terrain"]
verify = [
  "ctest -R Orbit.CelestialOcean",
]
verified = "b0a0de7f"
+++


