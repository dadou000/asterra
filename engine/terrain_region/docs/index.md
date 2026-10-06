+++
path = "/rendering/terrain/regions"
title = "Derived terrain regions and physical page boundary exchange (M25)"
kind = "subsystem"
status = "stable"
summary = "Two things: the regional derived-terrain pipeline (DerivedTerrainRegion tiles built on the CPU or GPU, cached and streamed, with finer-tile coverage replacing coarser) and the deterministic physical-neighbour boundary exchange of conservative water and M14 sediment between pages (SurfaceBoundaryExchange)."
owner_module = "OrbitTerrainRegion"
keywords = ["region", "derived terrain region", "streamer", "region cache", "boundary exchange", "halo", "ghost cell", "neighbor", "flux", "page boundary", "conservation"]
sources = [
  "engine/terrain_region/include/orbit/terrain_region/DerivedRegionTerrainSource.hpp",
  "engine/terrain_region/include/orbit/terrain_region/DerivedTerrainRegion.hpp",
  "engine/terrain_region/include/orbit/terrain_region/DerivedTerrainRegionCache.hpp",
  "engine/terrain_region/include/orbit/terrain_region/DerivedTerrainRegionGpu.hpp",
  "engine/terrain_region/include/orbit/terrain_region/DerivedTerrainRegionStreamer.hpp",
  "engine/terrain_region/include/orbit/terrain_region/SurfaceBoundaryExchange.hpp",
  "engine/terrain_region/CMakeLists.txt"]
symbols = ["SurfaceBoundaryFlux", "SurfaceBoundaryTransferBatch", "DerivedTerrainRegion", "DerivedTerrainRegionCache", "DerivedTerrainRegionStreamer", "DerivedRegionTerrainSource"]
invariants = [
  "Boundary exchange is one conservative transport layer: SurfaceBoundaryFlux carries water volume plus the volume-weighted velocity moment (kept as a moment so merges are exact) and the canonical M14 SedimentBoundaryFlux; no second sediment or water authority is created.",
  "Remapping across cube faces always uses the canonical PlanetTileNeighborhood mapping (target tile, target edge, sample-order reversal, tangent-vector rotation); corner packets use physical tile offsets and canonical corner directions, never a hard-coded cube-face table.",
  "Exchange is deterministic: input pages are sorted by stable physical address and results by receiving page, receiving edge/corner and source page, so caller iteration order and camera/cache residency cannot change it.",
  "Hydraulic and coastal solvers remain the owners of their water state: IncomingWaterBoundaryFlux returns the exact conservative packet entering a page and M25 duplicates no water authority.",
  "In the regional source, where a finer region cache has ready coverage it fully replaces the coarse cache's version for that segment or lake cell; both are never drawn together.",
  "When a GPU device is configured, region tiles build on the GPU: Request() only queues the tile and the caller's per-frame Flush() records the dispatch (same pattern as GpuElevationQuery)."]
related = ["/rendering/terrain/water", "/rendering/terrain/erosion", "/world/planet-coordinates"]
depends_on = ["/foundation/core", "/foundation/jobs", "/foundation/math", "/rendering/rhi", "/rendering/terrain/contracts", "/rendering/terrain/erosion", "/rendering/terrain/gpu-passes", "/rendering/terrain/hydrology", "/rendering/terrain/material-column", "/rendering/terrain/water", "/world/planet-coordinates"]
used_by = ["/apps/sandbox", "/rendering/terrain/debug-fields", "/rendering/water"]
verify = [
  "ctest -R Orbit.TerrainBoundaryExchange",
  "ctest -R Orbit.DerivedTerrainRegion",
  "ctest -R Orbit.DerivedTerrainRegionGpu",
  "ctest -R Orbit.DerivedTerrainStreaming"]
verified = "b0a0de7f"
+++


