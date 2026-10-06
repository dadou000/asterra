+++
path = "/rendering/terrain/gpu-passes"
title = "Terrain GPU passes (field generation, hydrology and erosion on the GPU)"
kind = "subsystem"
status = "stable"
summary = "The compute passes behind terrain on the GPU: clipmap field generation and region deltas, async elevation queries, the region hydrology chain (depression fill, flow accumulation, erosion, lakes, rivers), and the stateful physical-page solvers (M09 drainage, M11 hydraulic, M12 thermal, M13 aeolian, M17 coastal, M22 scatter) over GPU mirrors of the M08 material column and M14 sediment."
owner_module = "OrbitTerrainGpu"
keywords = ["gpu passes", "compute", "gpu terrain", "drainage gpu", "hydraulic gpu", "thermal gpu", "aeolian gpu", "coastal gpu", "scatter gpu", "flow accumulation", "depression fill", "readback", "elevation query", "region delta", "jacobi", "ping pong", "determinism", "fence"]
sources = [
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuDrainagePage.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuFlowAccumulation.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuDepressionFill.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuLakeBasins.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuRiverGeometry.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuErosion.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuHydraulicErosionPage.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuThermalErosionPage.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuAeolianErosionPage.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuCoastalProcessPage.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuBiomeScatterPage.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuMaterialColumnResources.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuSedimentExchangeResources.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuElevationQuery.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuHydrologyRegion.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuRegionDelta.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuFieldGenerator.hpp",
]
symbols = ["GpuDrainagePage", "GpuFlowAccumulation", "GpuDepressionFill", "GpuLakeBasins", "GpuRiverGeometry", "GpuErosion", "GpuHydraulicErosionPage", "GpuThermalErosionPage", "GpuAeolianErosionPage", "GpuCoastalProcessPage", "GpuBiomeScatterPage", "GpuMaterialColumnResources", "GpuSedimentExchangeResources", "GpuElevationQuery", "GpuHydrologyRegion", "GpuRegionDelta", "GpuFieldGenerator"]
invariants = [
  "CPU state stays authority: GPU material and sediment pages are derived mirrors. Passes that mutate them (hydraulic, thermal, aeolian, coastal) provide RecordMaterialReadback() and ApplyMaterialReadbackToCpu() (plus water readback where relevant) and the caller must wait for the submission fence before applying a readback.",
  "Determinism by construction: thermal relaxation computes one steepest-neighbour proposal per source cell and then a gather pass applies outgoing and incoming material simultaneously, avoiding float atomics and iteration-order dependence; the M22 scatter page writes one 48-byte slot per planting-grid cell with no atomics or append order.",
  "Parity is explicit: depression filling and the erosion/sediment transport use GPU-parallel relaxations (ping-pong Jacobi passes) that reach the same kind of result as the CPU algorithms by a different route and are ACCEPTED NON-PARITY; river channel geometry is a single pure per-cell pass with the same formula and constants as the CPU (no non-parity); the field generator must match AnalyticTerrainSource (/rendering/terrain/clipmaps/generator-parity).",
  "Flow accumulation must be fed the depression-filled elevation (not raw elevation) so every non-outlet cell has a strictly lower neighbour; it is two passes (steepest-descent pointers, then Jacobi summation along the forest) over a fixed scratch pool sized for maxResolution at construction. Lake-basin labelling likewise needs raw and drainage elevation already in ShaderResource state.",
  "M09 GPU drainage runs over one M08 page plus a one-cell neighbouring boundary ring: conditioned-elevation input is (resolution+2)^2 and only its outer ring is read; outputs also carry the ring so flux can be exported to adjacent pages; accumulation uses a FastFlow-style rake-compress reduction over the deterministic steepest-descent forest.",
  "Shared buffer contracts: wind forcing is one float4 per cell (x east m/s, y north m/s, z surface resistance [0,1], w reserved) with airborne state float2 kg/m^2 (x sand, y soil/fines); the sediment mirror keeps three float4 buffers (sand/fines/coarse debris kg/m^2 in xyz) separate per medium so each pass binds only what it needs, and process-local ping-pong buffers are scratch, never persistent; hydraulic state is water depth, four virtual-pipe fluxes, horizontal velocity and suspended sediment (kg/m^2); coastal state is depth, momentum, wet mask and shoreline mask, with an empty CPU boundary encoded as four closed walls.",
  "GpuMaterialColumnResources owns the four mutable M08 texture lanes with persistent staging; after upload all four are in UnorderedAccess state ready for later M09+ compute passes.",
  "Elevation queries are asynchronous request/poll pairs backed by a small fixed pool of compute dispatches (a few frames of latency), replacing synchronous CPU Sample() calls for camera ground clamp, teleport and similar; the region hydrology pass chains field generation, depression fill, flow accumulation and erosion for a caller-sized tile and exposes every intermediate buffer.",
  "The physical-page composite fades from generated terrain to the page and can be disabled for comparison (/rendering/terrain/clipmaps/precision-and-pages).",
]
related = ["/rendering/terrain/gpu-cache", "/rendering/terrain/hydrology", "/rendering/terrain/erosion", "/rendering/terrain/material-column", "/rendering/terrain/clipmaps/generator-parity", "/rendering/rhi", "/legacy/v0-0-4-m26-persistent-gpu-terrain-cache"]
verify = [
  "ctest -R Orbit.TerrainGpuDrainageShaders",
  "ctest -R Orbit.TerrainGpuHydraulicShaders",
  "ctest -R Orbit.TerrainGpuThermalShaders",
  "ctest -R Orbit.TerrainGpuAeolianShaders",
  "ctest -R Orbit.TerrainGpuCoastalShaders",
  "ctest -R Orbit.TerrainGpuScatterShaders",
  "ctest -R Orbit.TerrainGpuField",
  "ctest -R Orbit.TerrainGpuElevationQuery",
  "ctest -R Orbit.HydrologyGpuDepressionFill",
  "ctest -R Orbit.HydrologyGpuFlowAccumulation",
  "ctest -R Orbit.HydrologyGpuErosion",
  "ctest -R Orbit.HydrologyGpuRiverGeometry",
  "ctest -R Orbit.HydrologyGpuLakeBasins",
]
verified = "b0a0de7f"
+++


