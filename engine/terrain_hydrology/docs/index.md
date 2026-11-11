+++
path = "/rendering/terrain/hydrology"
title = "Drainage and depression routing (M09)"
kind = "subsystem"
status = "stable"
summary = "M09 D8 drainage and depression routing over the M08 surface, including conditioned elevation, contributing area, discharge, outlets and exchanged cardinal boundaries. Regional legacy solvers remain compatibility tooling."
owner_module = "OrbitTerrainHydrology"
keywords = ["drainage", "hydrology", "flow direction", "d8", "depression fill", "discharge", "contributing area", "catchment", "outlet", "routing", "halo"]
sources = [
  "engine/terrain_hydrology/include/orbit/terrain_hydrology/DrainagePage.hpp",
  "engine/terrain_hydrology/src/DrainagePage.cpp",
  "engine/terrain_hydrology/include/orbit/terrain_hydrology/HydrologyGrid.hpp",
  "engine/terrain_hydrology/include/orbit/terrain_hydrology/RiverGraph.hpp",
  "engine/terrain_hydrology/CMakeLists.txt"]
symbols = ["DrainagePage", "DrainageRoutingConfig", "DepressionRoutingPolicy", "HydrologyGrid", "RiverGraph"]
invariants = [
  "The drainage surface is the actual M08 top surface (bedrock + regolith + soil + sand + debris); the GPU path reads the M08 R32F and RGBA16F UAVs directly with no CPU round trip and creates no second authoritative heightfield.",
  "DepressionRoutingPolicy::PreserveClosed keeps true local sinks; FillToBoundary conditions only the derived routing surface and never modifies the M08 material column.",
  "M04 drainage guidance is a routing preference, not height authority: it scales candidate downhill slopes (authoredGuidanceWeight, default 0.20, kept below 1 so scores stay positive) but can never make an uphill neighbour eligible.",
  "DrainagePage::Revision() is the M09 drainage revision field and its stable fingerprint combines the full M08 PhysicalTerrainPageKey with the depression policy and drainage settings.",
  "M09 is the authoritative derived drainage state for flow direction, conditioned elevation, area, discharge and page-boundary flow; render pages, camera and cache residency never become terrain authority.",
  "The Studio page service no longer exchanges boundaries between resident pages: its halo is the terrain source sampled outside the page (flow zero, conditioned == surface), so pages are independent and basin-scale discharge is applied from the baked river graph (ApplyBakedRiverDischarge). The exchange payload types (DrainageBoundaryCell, BoundaryCell, CellAsBoundary, twins) remain for callers that do exchange boundaries and for the unit tests.",
  "Physical pages share their edge row/column, so a neighbor's edge cell is the SAME physical point as the page's own. DrainagePageHalo therefore carries two layers per side: north/east/south/west hold the neighbor's cell one step beyond the shared edge (the real outward neighbor, and the only valid drain), and twinNorth/East/South/West plus twinCorners hold the neighbor's copy of the shared edge itself. Twin conditioned height is a floor for the page's copy (both copies converge on one level), and a twin whose flow crosses into the page injects its area/discharge at its flow target; an outward cell whose published flow drains into the page is upstream of the edge and is not seeded as a drain. Corner ring cells are walls unless a diagonal page supplies them. With all twin vectors empty the halo keeps the legacy single-layer behavior used by independently generated pages and unit tests.",
  "If boundaries are exchanged between pages, the exchange only reaches a fixed point when edge-adjacent pages rebuild one after another (simultaneous rebuilds are bistable around lakes that span a seam); the 2x2 grid test in DrainagePageTests (TestGridSeamExchangeConverges) exchanges sequentially and must converge and conserve area. Studio avoids the problem by not exchanging at all.",
  "The pre-M09 regional HydrologyGrid/RiverGraph keep a sampled authoritative surface for erosion and a separate monotonically drainable surface used only for routing; they are compatibility/regional tooling, not the V0.0.4 authority."]
related = ["/rendering/terrain/erosion", "/rendering/terrain/rivers", "/rendering/terrain/material-column"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain/contracts", "/rendering/terrain/material-column", "/world/planet-coordinates"]
used_by = ["/editor/studio-session", "/rendering/terrain/debug-fields", "/rendering/terrain/erosion", "/rendering/terrain/gpu-passes", "/rendering/terrain/regions", "/rendering/terrain/water"]
verify = [
  "ctest -R Orbit.TerrainDrainagePage",
  "ctest -R Orbit.TerrainHydrology"]
verified = "b0a0de7f"
+++
