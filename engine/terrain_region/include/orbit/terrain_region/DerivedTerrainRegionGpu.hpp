#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain_region/DerivedTerrainRegion.hpp>
#include <orbit/world/Planet.hpp>

#include <span>

namespace orbit::terrain_region
{
// Everything a GPU-computed hydrology pass (see
// engine/terrain_gpu/GpuHydrologyRegion.hpp) produces for one region
// tile, read back to the CPU. Each span holds `resolution * resolution`
// row-major (index = y * resolution + x) elements matching
// GpuHydrologyRegion::Dispatch's five output buffers exactly.
struct GpuHydrologyReadback
{
    u32 resolution{0};
    f64 spacingMeters{0.0};
    f32 seaLevelMeters{0.0F};
    world::SurfaceFrame surfaceFrame{};

    std::span<const f32> rawElevationMeters;
    std::span<const f32> drainageElevationMeters;
    std::span<const f32> accumulation;
    // 0xFFFFFFFF ("no downstream") for grid-boundary/outlet cells,
    // otherwise a flat index into the same resolution*resolution grid.
    std::span<const u32> downstream;
    std::span<const f32> netElevationDeltaMeters;
};

// Builds a complete DerivedTerrainRegion the same way
// BuildDerivedTerrainRegion (DerivedTerrainRegion.cpp) does, except the
// raw sampling + depression-filling + flow-accumulation + erosion steps
// -- CPU BuildHydrologyGrid/RefineHydrologyWithSediment there -- are
// replaced with `readback`'s already-GPU-computed results (see the GPU
// terrain generation plan's Milestone 4). Every step after that --
// BuildRiverGraph, BuildRegionalElevationDeltaField,
// BuildRiverCarvingField, BuildRiverWaterNetwork, BuildLakeWaterField --
// is the *same, unchanged* CPU code BuildDerivedTerrainRegion calls,
// just fed a GPU-populated terrain_hydrology::HydrologyGrid instead of
// a CPU-computed one.
//
// Non-parity, by design: the GPU relaxation passes converge to the same
// *kind* of result the CPU priority-flood/sequential-sweep algorithms
// do, not a bit-identical one (see GpuDepressionFill/GpuErosion's own
// documentation). Also, this always applies erosion once (GPU erosion
// is already an internally-converged single pass), rather than
// following `config.refinement.iterations`'s literal outer-loop count
// -- the CPU config's default of 2 iterations re-routes hydrology
// between passes, which this doesn't reproduce.
[[nodiscard]] DerivedTerrainRegion BuildDerivedTerrainRegionFromGpuReadback(
    const DerivedTerrainRegionId& id,
    f64 approximateTileWidthMeters,
    f64 halfExtentMeters,
    const GpuHydrologyReadback& readback,
    const DerivedTerrainRegionConfig& config = {});
} // namespace orbit::terrain_region
