+++
path = "/rendering/terrain/gpu-passes"
title = "Terrain GPU passes (field generation, hydrology and erosion on the GPU)"
kind = "subsystem"
status = "stable"
summary = "The compute passes behind terrain on the GPU: clipmap field generation and region deltas, async elevation queries, the physical-page composite, and the region hydrology chain (depression fill, flow accumulation, erosion). The stateful physical-page solvers (drainage, hydraulic, thermal, aeolian, coastal, scatter, lake basins, river geometry and the material/sediment GPU mirrors) were removed in 0.0.9 because no application linked them."
owner_module = "OrbitTerrainGpu"
keywords = ["gpu passes", "compute", "gpu terrain", "flow accumulation", "depression fill", "readback", "elevation query", "region delta", "physical page composite", "jacobi", "ping pong", "determinism", "fence"]
sources = [
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuFlowAccumulation.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuDepressionFill.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuErosion.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuElevationQuery.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuHydrologyRegion.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuPhysicalPageComposite.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuRegionDelta.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuFieldGenerator.hpp"]
symbols = ["GpuFlowAccumulation", "GpuDepressionFill", "GpuErosion", "GpuElevationQuery", "GpuHydrologyRegion", "GpuPhysicalPageComposite", "GpuRegionDelta", "GpuFieldGenerator"]
invariants = [
  "CPU state stays authority: GPU pages are derived mirrors, and any pass that mutates one must provide a readback the caller applies only after waiting for the submission fence.",
  "Parity is explicit: depression filling and the erosion/sediment transport use GPU-parallel relaxations (ping-pong Jacobi passes) that reach the same kind of result as the CPU algorithms by a different route and are ACCEPTED NON-PARITY; the field generator must match AnalyticTerrainSource (/rendering/terrain/clipmaps/generator-parity).",
  "Flow accumulation must be fed the depression-filled elevation (not raw elevation) so every non-outlet cell has a strictly lower neighbour; it is two passes (steepest-descent pointers, then Jacobi summation along the forest) over a fixed scratch pool sized for maxResolution at construction.",
  "Elevation queries are asynchronous request/poll pairs backed by a small fixed pool of compute dispatches (a few frames of latency), replacing synchronous CPU Sample() calls for camera ground clamp, teleport and similar; the region hydrology pass chains field generation, depression fill, flow accumulation and erosion for a caller-sized tile and exposes every intermediate buffer.",
  "The physical-page composite fades from generated terrain to the page and can be disabled for comparison (/rendering/terrain/clipmaps/precision-and-pages)."]
related = ["/rendering/terrain/gpu-cache", "/rendering/terrain/hydrology", "/rendering/terrain/erosion", "/rendering/terrain/material-column", "/rendering/terrain/clipmaps/generator-parity", "/rendering/rhi"]
verify = [
  "ctest -R Orbit.TerrainGpuField",
  "ctest -R Orbit.TerrainGpuElevationQuery",
  "ctest -R Orbit.HydrologyGpuDepressionFill",
  "ctest -R Orbit.HydrologyGpuFlowAccumulation",
  "ctest -R Orbit.HydrologyGpuErosion"]
verified = "b0a0de7f"
+++
