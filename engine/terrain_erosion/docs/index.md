+++
path = "/rendering/terrain/erosion"
title = "Erosion processes and shared sediment (M10-M15, M23)"
kind = "subsystem"
status = "stable"
summary = "The terrain process solvers over the M08 material column: macro stream-power incision, local hydraulic (virtual-pipe shallow water), thermal/gravity slope failure, aeolian wind transport, glacial ice flow and erosion, all exchanging material through one typed mobile-sediment contract, plus the multi-scale process tiers."
owner_module = "OrbitTerrainErosion"
keywords = ["erosion", "stream power", "hydraulic", "thermal", "aeolian", "glacial", "sediment", "angle of repose", "wind", "ice", "deposition", "multi scale", "process"]
sources = [
  "engine/terrain_erosion/include/orbit/terrain_erosion/StreamPowerErosion.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/HydraulicErosion.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/ThermalErosion.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/AeolianErosion.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/GlacialErosion.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/SedimentExchange.hpp",
  "engine/terrain_erosion/include/orbit/terrain_erosion/MultiScaleTerrain.hpp",
  "engine/terrain_erosion/CMakeLists.txt"]
symbols = ["StreamPowerErosion", "HydraulicErosionConfig", "ThermalErosion", "AeolianErosionConfig", "GlacialErosionConfig", "SedimentExchangePage", "MultiScaleTerrainPlanner"]
invariants = [
  "M08 is the only physical terrain authority; every process reads and writes the same material column and never creates its own heightfield. Mobile material is process state that always returns to M08 layers when deposited.",
  "Stream power (M10) is the large-scale incision/equilibrium solver E = K Q^m S^n erodibility erosionAllowance, not droplet erosion; each iteration rebuilds M09 drainage, applies M05 uplift and the M04 equilibrium correction, incises, displaces M08 bedrock and then erodes top-down.",
  "Hydraulic erosion (M11) is a shallow-water/material-column virtual-pipe solver, not droplet erosion; suspended sediment is kilograms per cell on the CPU and kilograms per square metre on the GPU so the numerical scale does not depend on resolution.",
  "Thermal erosion (M12) fails loose material at its own critical repose angle (configurable defaults: sand 33, regolith 37, debris 40, soil 46 degrees) and treats exposed bedrock separately with a rock failure threshold modulated by M02 hardness, cohesion and fracture tendency.",
  "Aeolian erosion (M13) consumes one coarse wind/process sample per cell (east and north wind in the page-local tangent frame plus a generic surfaceResistance in [0, 1]) and owns two transient airborne lanes (sand, soil/fines); it owns no vegetation assets.",
  "Glacial erosion (M15) eligibility = cold-temperature factor x snowfall factor x process mask, clamped to [0, 1]; a zero mask or zero eligibility is a hard boundary: no retained ice, accumulation or flow.",
  "Sediment exchange (M14) is the sole mobile-sediment authority: three classes (sand, fines, coarse debris) map to M08 sand, soil and debris (removed regolith is classified as fines while mobile) and move in three media (waterborne, airborne, surface-mobile).",
  "Multi-scale terrain (M23) tiers are simulation/amplification scales, not render clipmap levels; their numeric values are stable array indices and the process mask says which families are eligible at a tier, not which must be enabled."]
related = ["/rendering/terrain/hydrology", "/rendering/terrain/rivers", "/rendering/terrain/material-column", "/rendering/terrain/water"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain/contracts", "/rendering/terrain/geology", "/rendering/terrain/hydrology", "/rendering/terrain/macro-geology", "/rendering/terrain/material-column", "/world/planet-coordinates", "/world/terrain-constraints"]
used_by = ["/apps/sandbox", "/rendering/terrain/debug-fields", "/rendering/terrain/gpu-passes", "/rendering/terrain/regions", "/rendering/terrain/water", "/world/surface-composition"]
verify = [
  "ctest -R Orbit.TerrainStreamPower",
  "ctest -R Orbit.TerrainHydraulicErosion",
  "ctest -R Orbit.TerrainThermalErosion",
  "ctest -R Orbit.TerrainAeolianErosion",
  "ctest -R Orbit.TerrainSedimentExchange",
  "ctest -R Orbit.TerrainGlacialErosion",
  "ctest -R Orbit.TerrainMultiScale"]
verified = "b0a0de7f"
+++


