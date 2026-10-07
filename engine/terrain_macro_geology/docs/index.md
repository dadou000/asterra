+++
path = "/rendering/terrain/macro-geology"
title = "Macro geology and uplift field (M05)"
kind = "subsystem"
status = "stable"
summary = "MacroGeologyField turns tectonic state (convergence and divergence masks, plate continental identity, hotspots) and authored uplift constraints into body-wide uplift forcing and guidance fields that drainage and stream-power erosion consume; the final mountain height is never stored as authority."
owner_module = "OrbitTerrainMacroGeology"
keywords = ["macro geology", "uplift", "tectonic", "plates", "convergence", "divergence", "hotspot", "mountain belt", "basin", "subsidence", "rift"]
sources = [
  "engine/terrain_macro_geology/include/orbit/terrain_macro_geology/MacroGeologyField.hpp",
  "engine/terrain_macro_geology/CMakeLists.txt"]
symbols = ["MacroGeologyField", "MacroGeologyDesc", "MacroGeologySample", "MakeMountainBeltConstraint", "MakeBasinConstraint"]
invariants = [
  "Uplift is authoritative input while the final mountain/valley surface remains derived: M05 does not encode final height.",
  "Baseline uplift reuses the existing deterministic tectonic state of GlobalTerrainFields (convergence/divergence masks, nearest and secondary plate continental identity, hotspot elevation) so 2D/global and 3D terrain never invent separate plate systems.",
  "Convergent boundaries give positive uplift forcing and divergent boundaries subsidence/rift forcing; continental/continental, mixed and oceanic/oceanic collisions have separate authored scales.",
  "No new authority types: MakeMountainBeltConstraint and MakeBasinConstraint create ordinary M04 uplift constraints, and imported rasters use the M04 RasterMaskConstraintPrimitive, so the editor, serializer and procedural graph keep one terrain-authoring model; no renderer texture or raster ownership path is added."]
related = ["/world/terrain-constraints", "/rendering/terrain/erosion"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain/contracts", "/world/planet-coordinates", "/world/terrain-constraints"]
used_by = ["/editor/studio-session", "/rendering/terrain/debug-fields", "/rendering/terrain/erosion"]
verify = [
  "ctest -R Orbit.TerrainMacroGeology"]
verified = "b0a0de7f"
+++


