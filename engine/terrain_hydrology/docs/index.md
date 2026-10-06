+++
path = "/rendering/terrain/hydrology"
title = "Drainage and depression routing (M09)"
kind = "subsystem"
status = "stable"
summary = "DrainagePage derives flow routing from the physical M08 top surface: D8 direction, conditioned drainage elevation, depression-fill depth, contributing area, discharge, authored guidance and explicit outlet state, with halo exchange across page boundaries. The older regional HydrologyGrid and RiverGraph remain as compatibility tooling."
owner_module = "OrbitTerrainHydrology"
keywords = ["drainage", "hydrology", "flow direction", "d8", "depression fill", "discharge", "contributing area", "catchment", "outlet", "routing", "halo"]
sources = [
  "engine/terrain_hydrology/include/orbit/terrain_hydrology/DrainagePage.hpp",
  "engine/terrain_hydrology/include/orbit/terrain_hydrology/HydrologyGrid.hpp",
  "engine/terrain_hydrology/include/orbit/terrain_hydrology/RiverGraph.hpp",
  "engine/terrain_hydrology/CMakeLists.txt",
]
symbols = ["DrainagePage", "DrainageRoutingConfig", "DepressionRoutingPolicy", "HydrologyGrid", "RiverGraph"]
invariants = [
  "The drainage surface is the actual M08 top surface (bedrock + regolith + soil + sand + debris); the GPU path reads the M08 R32F and RGBA16F UAVs directly with no CPU round trip and creates no second authoritative heightfield.",
  "DepressionRoutingPolicy::PreserveClosed keeps true local sinks; FillToBoundary conditions only the derived routing surface and never modifies the M08 material column.",
  "M04 drainage guidance is a routing preference, not height authority: it scales candidate downhill slopes (authoredGuidanceWeight, default 0.20, kept below 1 so scores stay positive) but can never make an uphill neighbour eligible.",
  "DrainagePage::Revision() is the M09 drainage revision field and its stable fingerprint combines the full M08 PhysicalTerrainPageKey with the depression policy and drainage settings.",
  "M09 is the authoritative derived drainage state for flow direction, conditioned elevation, area, discharge and page-boundary flow; render pages, camera and cache residency never become terrain authority.",
  "The pre-M09 regional HydrologyGrid/RiverGraph keep a sampled authoritative surface for erosion and a separate monotonically drainable surface used only for routing; they are compatibility/regional tooling, not the V0.0.4 authority.",
]
related = ["/rendering/terrain/erosion", "/rendering/terrain/rivers", "/rendering/terrain/material-column", "/legacy/v0-0-4-m09-drainage"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain/contracts", "/rendering/terrain/material-column", "/world/planet-coordinates"]
used_by = ["/apps/sandbox", "/editor/studio-session", "/rendering/terrain/debug-fields", "/rendering/terrain/erosion", "/rendering/terrain/gpu-passes", "/rendering/terrain/regions", "/rendering/terrain/water"]
verify = [
  "ctest -R Orbit.TerrainDrainagePage",
  "ctest -R Orbit.TerrainHydrology",
]
verified = "b0a0de7f"
+++


