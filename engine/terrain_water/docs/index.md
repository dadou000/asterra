+++
path = "/rendering/terrain/water"
title = "WaterService, coastal process, rivers and lakes (M17)"
kind = "subsystem"
status = "stable"
summary = "WaterService owns fluid definitions, water pages, domains, hull masks and the read-only handoff terrain processes consume; the coastal process is a boundary-aware shallow-water solver on the M08 bed; RiverWater and LakeWater provide sampled river segments and lake basins for rendering and queries."
owner_module = "OrbitTerrainWater"
keywords = ["water", "water service", "coastal", "shoreline", "shallow water", "lake", "river water", "wet dry", "manning", "cfl", "fluid", "hull", "flooding"]
sources = [
  "engine/terrain_water/include/orbit/terrain_water/CoastalProcess.hpp",
  "engine/terrain_water/include/orbit/terrain_water/LakeWater.hpp",
  "engine/terrain_water/include/orbit/terrain_water/RiverWater.hpp",
  "engine/terrain_water/include/orbit/terrain_water/WaterService.hpp",
  "engine/terrain_water/CMakeLists.txt",
]
symbols = ["WaterService", "CoastalWaterPage", "CoastalShallowWaterConfig", "LakeWaterField", "RiverWaterNetwork"]
invariants = [
  "The permanent authority chain is geology -> physical material column -> drainage/macro geomorphology -> WaterService -> terrain processes -> exposed surface -> biome -> persistent GPU caches -> rendering.",
  "WaterService state is a read-only authority handoff for erosion/sediment systems: terrain processes may react to it but cannot mutate WaterService geology or manufacture a second water authority.",
  "M17 water depth, momentum, wet/dry state and shoreline masks are derived solver state: M08 stays the only terrain authority and M14 the only mobile-sediment authority; disabling the process returns M08 and M14 unchanged.",
  "The coastal solver is a positivity-clamped finite-volume shallow-water update with hydrostatic reconstruction across changing bed elevation, a CFL-bounded time step, Manning friction and an explicit velocity ceiling; when M08 changes, water depth is resynchronised preserving the free-surface elevation.",
  "Pages are advanced serially and share one solver scratch buffer.",
  "Standing lake water is sampled in O(1) from a dense support field (overlap plus a dry bank); no per-frame cell mesh or visibility budget is needed.",
]
related = ["/rendering/water", "/rendering/terrain/erosion", "/rendering/terrain/regions", "/legacy/standing-water-rendering", "/legacy/v0-0-4-water-service", "/legacy/v0-0-4-m17-coastal-process"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain/erosion", "/rendering/terrain/geology", "/rendering/terrain/hydrology", "/rendering/terrain/material-column", "/world/planet-coordinates", "/world/universe"]
used_by = ["/rendering/terrain/gpu-passes", "/rendering/terrain/regions", "/rendering/water", "/world/surface-composition"]
verify = [
  "ctest -R Orbit.TerrainCoastalProcess",
  "ctest -R Orbit.WaterService",
  "ctest -R Orbit.RiverWater",
  "ctest -R Orbit.LakeWater",
]
verified = "b0a0de7f"
+++


