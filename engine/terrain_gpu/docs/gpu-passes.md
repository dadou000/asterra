+++
path = "/rendering/terrain/gpu-passes"
title = "Terrain GPU passes (field generation, hydrology, geology and erosion on the GPU)"
kind = "subsystem"
status = "stable"
summary = "The compute passes behind terrain on the GPU: clipmap field generation and region deltas, chronological geological event tile compilation, the physical-page composite, and the region hydrology chain (depression fill, flow accumulation, erosion). The stateful physical-page solvers (drainage, hydraulic, thermal, aeolian, coastal, scatter, lake basins, river geometry and the material/sediment GPU mirrors) were removed in 0.0.9 because no application linked them."
owner_module = "OrbitTerrainGpu"
keywords = ["gpu passes", "compute", "gpu terrain", "geology compiler", "impact batch", "fracture batch", "flow accumulation", "depression fill", "readback", "elevation query", "region delta", "physical page composite", "jacobi", "ping pong", "determinism", "fence"]
sources = [
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuFlowAccumulation.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuDepressionFill.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuErosion.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuHydrologyRegion.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuPhysicalPageComposite.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuRegionDelta.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuFieldGenerator.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuGeologyCompiler.hpp",
  "engine/terrain_gpu/src/GeologyCompute.hpp",
  "engine/terrain_gpu/src/GpuGeologyCompiler.cpp",
  "tests/TerrainGpuGeologyCompilerTests.cpp"]
symbols = ["GpuFlowAccumulation", "GpuDepressionFill", "GpuErosion", "GpuHydrologyRegion", "GpuPhysicalPageComposite", "GpuRegionDelta", "GpuFieldGenerator", "GpuGeologyCompiler", "BuildCpuReference"]
invariants = [
  "CPU state stays authority: GPU pages are derived mirrors, and any pass that mutates one must provide a readback the caller applies only after waiting for the submission fence.",
  "Parity is explicit: depression filling and the erosion/sediment transport use GPU-parallel relaxations (ping-pong Jacobi passes) that reach the same kind of result as the CPU algorithms by a different route and are ACCEPTED NON-PARITY; the field generator must match AnalyticTerrainSource (/rendering/terrain/clipmaps/generator-parity).",
  "Flow accumulation must be fed the depression-filled elevation (not raw elevation) so every non-outlet cell has a strictly lower neighbour; it is two passes (steepest-descent pointers, then Jacobi summation along the forest) over a fixed scratch pool sized for maxResolution at construction.",
  "GpuGeologyCompiler obtains per-tile candidates from ImpactField and IceFractureField, packs their canonical age order and prepared impact geometry, evaluates impact/resurfacing/fracture samples with a compute dispatch, reads the base rasters back after a fence, then builds the validated shared BakedGeologyRasters scale-space product. Cancellation waits for submitted queue work before discarding the candidate; TerrainBakeService activates only a recipe- and resolution-matched product.",
  "Prepared impact packets reuse CPU-derived azimuth sine/cosine, elongation and influence cosine. Long material rays use the same finite support, radial fade and periodic angular distortion as CPU ImpactField; they occupy existing event packet slots, so no new descriptor or packet stride is required. All 64-bit geological age comparisons compare the high word before the low word.",
  "Geology bake diagnostics report recomputed tiles (also the regional cache-miss/invalidation count), raster and event-record throughput, dispatch count, estimated GPU input/readback transfer bytes, CPU fence-wait time and RHI timestamped GPU queue time when timestamp queries are supported; status includes an availability flag. Hardware occupancy is reported unavailable because the device-neutral RHI has no occupancy counter; transfer bytes describe explicit buffers, not internal device memory traffic.",
  "The geological compute kernel uses float32; Orbit.TerrainGpuGeologyCompiler compiles and dispatches it on Vulkan and compares relief, ice, chronology/counts and process channels against a CPU reference with bounded tolerances. Fracture tile candidates conservatively include the requested spherical cap plus each segment's influence radius; CPU and GPU then evaluate the same four-width finite support. The CPU tile compiler remains available as a reference and fallback. Local stable-ID crater and flow edits still use the bounded CPU tile rebake path.",
  "The region hydrology pass chains field generation, depression fill, flow accumulation and erosion for a caller-sized tile and exposes every intermediate buffer. The asynchronous GpuElevationQuery (camera ground clamp) was removed in 0.0.9 because only the sandbox app used it.",
  "The physical-page composite fades from generated terrain to the page and can be disabled for comparison (/rendering/terrain/clipmaps/precision-and-pages)."]
related = ["/rendering/terrain/gpu-cache", "/rendering/terrain/hydrology", "/rendering/terrain/erosion", "/rendering/terrain/material-column", "/rendering/terrain/clipmaps/generator-parity", "/rendering/rhi"]
verify = [
  "ctest -R Orbit.TerrainGpuField",
  "ctest -R Orbit.TerrainGpuGeologyCompiler",
  "ctest -R Orbit.HydrologyGpuDepressionFill",
  "ctest -R Orbit.HydrologyGpuFlowAccumulation",
  "ctest -R Orbit.HydrologyGpuErosion"]
verified = "707c225d50c50f01b9ef797e67903447c1a66d5f"
+++
