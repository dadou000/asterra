+++
path = "/rendering/terrain/geology"
title = "Geological materials and stratigraphy (M02, M03)"
kind = "subsystem"
status = "stable"
summary = "The permanent authored substrate layer: GeologicalMaterial records (hardness, cohesion, erodibilities, permeability, weatherability, fracture tendency, density) with a validated GeologicalMaterialLibrary, and virtual StratigraphyProfiles that say which rock is exposed at which depth without voxelising the planet."
owner_module = "OrbitTerrainGeology"
keywords = ["geology", "rock", "material", "stratigraphy", "hardness", "erodibility", "basalt", "granite", "sandstone", "limestone", "ash", "substrate", "rock type id"]
sources = [
  "engine/terrain_geology/include/orbit/terrain_geology/GeologicalMaterial.hpp",
  "engine/terrain_geology/include/orbit/terrain_geology/Stratigraphy.hpp",
  "engine/terrain_geology/CMakeLists.txt",
]
symbols = ["GeologicalMaterial", "GeologicalMaterialLibrary", "GeologicalMaterialGpuTable", "StratigraphyProfile", "CompiledStratigraphyProfile"]
invariants = [
  "Geological materials are substrate physics: not biome definitions and not renderer/PBR materials.",
  "RockTypeId is a 128-bit StrongId: authored/persisted identity, never derived from a GPU table index, clipmap location, camera state or terrain page; the stable reference IDs live in terrain_geology::reference_rock and their numeric values must not be repurposed.",
  "The seven non-density coefficients are normalised authoring values in [0, 1]; density is bulk density in kg/m^3 and must be finite and positive; invalid IDs, empty names, out-of-range or non-finite values are rejected before they enter library authority.",
  "The reference rocks (basalt, granite, sandstone, limestone, unconsolidated volcanic ash) are real .orbitgeologicalmaterial authority files under engine/terrain_geology/assets/reference/; their values are data, not C++ defaults or shader constants, and are physically ordered defaults rather than laboratory calibration.",
  "A material edit increments the library's monotonic Revision(); consumers map it to the geology revision domain and invalidate derived pages without making derived tables authoritative.",
  "Shaders receive a compact derived GPU table, never strings or 128-bit rock IDs per sample.",
  "A StratigraphyProfile selects which authored RockTypeId is exposed at a depth but cannot redefine that rock's hardness, cohesion or erosion properties; finite layers are ordered top to bottom with one basement material below, and every RockTypeId is checked against the library when the profile is compiled.",
  "Stratigraphy is virtual: no full rock stack is copied into each terrain page.",
]
related = ["/rendering/terrain/material-column", "/rendering/terrain/macro-geology", "/legacy/v0-0-4-m02-geological-materials", "/legacy/v0-0-4-m03-virtual-stratigraphy"]
depends_on = ["/foundation/core", "/rendering/terrain/contracts"]
used_by = ["/rendering/terrain/biomes", "/rendering/terrain/debug-fields", "/rendering/terrain/erosion", "/rendering/terrain/gpu-passes", "/rendering/terrain/material-column", "/rendering/terrain/scatter", "/rendering/terrain/water", "/world/surface-composition", "/world/terrain-constraints"]
verify = [
  "ctest -R Orbit.TerrainGeologyMaterials",
  "ctest -R Orbit.TerrainStratigraphy",
]
verified = "b0a0de7f"
+++


