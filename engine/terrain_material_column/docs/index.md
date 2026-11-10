+++
path = "/rendering/terrain/material-column"
title = "Physical material column and surface resolver (M08, M18)"
kind = "subsystem"
status = "stable"
summary = "The mutable physical terrain column that every process consumes: per cell debris, sand, soil, regolith and bedrock with bedrock identity and moisture (MaterialColumnPage); and the single exposed-surface resolver above it (SurfaceResolver). Sparse local 3D promotion for caves and overhangs (Local3DPromotion) was removed in 0.0.9 because no application used it."
owner_module = "OrbitTerrainMaterialColumn"
keywords = ["material column", "regolith", "soil", "sand", "debris", "bedrock", "exposed surface", "surface resolver", "mass accounting", "loose material"]
sources = [
  "engine/terrain_material_column/include/orbit/terrain_material_column/MaterialColumnPage.hpp",
  "engine/terrain_material_column/include/orbit/terrain_material_column/SurfaceResolver.hpp",
  "engine/terrain_material_column/CMakeLists.txt"]
symbols = ["MaterialColumnPage", "MaterialColumnCell", "GeologySample", "ExposedSurfaceState"]
invariants = [
  "CPU material-column state is the authority; the GPU page is a derived packed representation. The column order is debris, sand, soil, regolith, bedrock and erosion removes loose material top-down: loose material must be exhausted before bedrock can be cut.",
  "ExposedSurface() is the highest nonzero layer, so deposition covers exposed bedrock without rewriting geology; M07 ejecta, melt, breccia and connected resurfacing deposits enter the debris layer while the stable M02 RockTypeId stays the substrate authority.",
  "Exposed physical material is determined only by the actual topmost M08 material: biome state is never an input to the resolver and cannot choose underlying rock identity; a missing M02 material is an invalid physical page, not a biome fallback.",
  "M07 excavation removes the top-down material column; ejecta, impact melt, fault-generated tectonic breccia and resurfacing deposits are deposited into its debris layer, while ray intensity remains a separate scratch signal. The page builder calls the impact material operation whenever any compiled geological material channel is present, including renewal events without a primary crater.",
  "M08 stays authoritative for loose-layer thickness, bedrock identity, physical surface elevation and moisture; M02 stays authoritative for intrinsic rock properties."]
related = ["/rendering/terrain/geology", "/rendering/terrain/hydrology", "/rendering/terrain/erosion", "/rendering/terrain/scatter"]
depends_on = ["/foundation/core", "/rendering/rhi", "/rendering/terrain/clipmaps", "/rendering/terrain/contracts", "/rendering/terrain/geology", "/rendering/terrain/impacts", "/rendering/terrain/scatter"]
used_by = ["/editor/studio-session", "/rendering/terrain/biomes", "/rendering/terrain/debug-fields", "/rendering/terrain/erosion", "/rendering/terrain/gpu-passes", "/rendering/terrain/hydrology", "/rendering/terrain/regions", "/rendering/terrain/scatter", "/rendering/terrain/water", "/world/surface-composition"]
verify = [
  "ctest -R Orbit.TerrainMaterialColumn",
  "ctest -R Orbit.TerrainSurfaceResolver"]
verified = "b0a0de7f"
+++
