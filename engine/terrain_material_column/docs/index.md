+++
path = "/rendering/terrain/material-column"
title = "Physical material column, surface resolver and local 3D (M08, M18, M24)"
kind = "subsystem"
status = "stable"
summary = "The mutable physical terrain column that every process consumes: per cell debris, sand, soil, regolith and bedrock with bedrock identity and moisture (MaterialColumnPage); the single exposed-surface resolver above it (SurfaceResolver); and sparse local 3D promotion for caves and overhangs (Local3DPromotion)."
owner_module = "OrbitTerrainMaterialColumn"
keywords = ["material column", "regolith", "soil", "sand", "debris", "bedrock", "exposed surface", "surface resolver", "mass accounting", "local 3d", "cave", "overhang", "promotion", "loose material"]
sources = [
  "engine/terrain_material_column/include/orbit/terrain_material_column/Local3DPromotion.hpp",
  "engine/terrain_material_column/include/orbit/terrain_material_column/MaterialColumnPage.hpp",
  "engine/terrain_material_column/include/orbit/terrain_material_column/SurfaceResolver.hpp",
  "engine/terrain_material_column/CMakeLists.txt",
]
symbols = ["MaterialColumnPage", "MaterialColumnCell", "GeologySample", "ExposedSurfaceState", "Local3DPromotionRegion", "Local3DPromotionPolicy"]
invariants = [
  "CPU material-column state is the authority; the GPU page is a derived packed representation. The column order is debris, sand, soil, regolith, bedrock and erosion removes loose material top-down: loose material must be exhausted before bedrock can be cut.",
  "ExposedSurface() is the highest nonzero layer, so deposition covers exposed bedrock without rewriting geology; the stable M02 RockTypeId stays the substrate authority.",
  "Exposed physical material is determined only by the actual topmost M08 material: biome state is never an input to the resolver and cannot choose underlying rock identity; a missing M02 material is an invalid physical page, not a biome fallback.",
  "M08 stays authoritative for loose-layer thickness, bedrock identity, physical surface elevation and moisture; M02 stays authoritative for intrinsic rock properties.",
  "Local 3D promotion is sparse: MaterialColumnPage is unchanged for ordinary terrain and a Local3DPromotionRegion exists only when TryPromoteLocal3D accepts an explicit request or a process request that meets ALL evidence thresholds (minimum void height, undercut distance, spatial persistence, process confidence).",
  "A guard ring of cells initialised from the exact M08 surface cannot be carved or extended by local 3D operations, and BoundaryMatches verifies it still has one span and the same top surface, giving a deterministic handoff to ordinary terrain.",
  "Local3DPromotionFingerprint depends on the page address, source/promotion revisions, reason/evidence and policy; camera, clipmap, frame and GPU residency are absent. Gaps between solid spans are real voids and the span count per cell is bounded.",
]
related = ["/rendering/terrain/geology", "/rendering/terrain/hydrology", "/rendering/terrain/erosion", "/rendering/terrain/scatter", "/legacy/v0-0-4-m08-material-column", "/legacy/v0-0-4-m18-surface-resolver", "/legacy/v0-0-4-m24-local-3d-promotion"]
depends_on = ["/foundation/core", "/rendering/rhi", "/rendering/terrain/clipmaps", "/rendering/terrain/contracts", "/rendering/terrain/geology", "/rendering/terrain/impacts", "/rendering/terrain/scatter"]
used_by = ["/editor/studio-session", "/rendering/terrain/biomes", "/rendering/terrain/debug-fields", "/rendering/terrain/erosion", "/rendering/terrain/hydrology", "/rendering/terrain/regions", "/rendering/terrain/scatter", "/rendering/terrain/water", "/world/surface-composition"]
verify = [
  "ctest -R Orbit.TerrainMaterialColumn",
  "ctest -R Orbit.TerrainSurfaceResolver",
  "ctest -R Orbit.TerrainLocal3DPromotion",
]
verified = "b0a0de7f"
+++


