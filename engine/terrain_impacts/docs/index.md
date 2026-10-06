+++
path = "/rendering/terrain/impacts"
title = "Impact craters (M07)"
kind = "subsystem"
status = "stable"
summary = "ImpactField generates and stores impacts as process state: a deterministic size-frequency distribution plus authored impact records produce crater channels (signed height delta, excavation depth, ejecta thickness, debris, rays) that feed the material column."
owner_module = "OrbitTerrainImpacts"
keywords = ["impact", "crater", "ejecta", "size frequency distribution", "sfd", "excavation", "rays", "moon", "regolith", "ImpactRecord"]
sources = [
  "engine/terrain_impacts/include/orbit/terrain_impacts/ImpactField.hpp",
  "engine/terrain_impacts/CMakeLists.txt",
]
symbols = ["ImpactField", "ImpactFieldDefinition", "ImpactRecord", "CraterSizeFrequencyDistribution", "CraterProcessSample"]
invariants = [
  "Craters are process state, not biome decoration and not protected height stamps; generated crater terrain is disposable derived state.",
  "Procedural impacts follow a cumulative power law N(>R) ~ R^-b defined by count, minimum radius, maximum radius and exponent; radius sampling is deterministic from the planet generation seed and impact ordinal.",
  "Placement uses deterministic equal-area sphere sampling and never cube-face UVs, clipmap rings, viewport state or cache slots; stable ImpactIds derive from seed plus ordinal so regeneration reproduces identity as well as placement.",
  "Explicit crater placements persist as .orbitimpacts project-authority records.",
]
related = ["/rendering/terrain/material-column", "/rendering/terrain/clipmaps/generator-parity", "/legacy/v0-0-4-m07-impact-craters"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain/contracts", "/world/planet-coordinates"]
used_by = ["/rendering/terrain/material-column"]
verify = [
  "ctest -R Orbit.TerrainImpacts",
]
verified = "b0a0de7f"
+++


