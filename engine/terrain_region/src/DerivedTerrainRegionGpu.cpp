#include <orbit/terrain_region/DerivedTerrainRegionGpu.hpp>

#include <stdexcept>

namespace orbit::terrain_region
{
namespace
{
[[nodiscard]] i8 ClampFlowComponent(const i64 value) noexcept
{
    if (value < -1)
    {
        return -1;
    }

    if (value > 1)
    {
        return 1;
    }

    return static_cast<i8>(value);
}

[[nodiscard]] terrain_hydrology::HydrologyGrid BuildHydrologyGridFromGpu(
    const GpuHydrologyReadback& readback,
    const DerivedTerrainRegionConfig& config,
    const f64 halfExtentMeters)
{
    const u32 resolution = readback.resolution;
    const std::size_t cellCount =
        static_cast<std::size_t>(resolution) * resolution;

    if (readback.rawElevationMeters.size() != cellCount ||
        readback.drainageElevationMeters.size() != cellCount ||
        readback.accumulation.size() != cellCount ||
        readback.downstream.size() != cellCount ||
        readback.netElevationDeltaMeters.size() != cellCount)
    {
        throw std::invalid_argument(
            "Orbit GPU hydrology readback size does not match its own "
            "resolution.");
    }

    terrain_hydrology::HydrologyGrid grid{};
    grid.config = config.hydrology;
    grid.config.resolution = resolution;
    grid.config.halfExtentMeters = halfExtentMeters;
    grid.surfaceFrame = readback.surfaceFrame;
    grid.spacingMeters = readback.spacingMeters;
    grid.cells.resize(cellCount);

    constexpr u32 kNoDownstream = 0xFFFFFFFFU;

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(y) * resolution + x;

            terrain_hydrology::HydrologyCell& cell = grid.cells[index];

            const f32 raw = readback.rawElevationMeters[index];
            const f32 drainage = readback.drainageElevationMeters[index];

            cell.elevationMeters = raw;
            cell.drainageElevationMeters = drainage;
            cell.depressionFillMeters =
                drainage > raw ? drainage - raw : 0.0F;
            cell.runoffWeight = 1.0F;
            cell.flowAccumulation = readback.accumulation[index];
            cell.oceanWeight =
                drainage <= readback.seaLevelMeters ? 1.0F : 0.0F;

            const u32 downstreamIndex = readback.downstream[index];

            if (downstreamIndex == kNoDownstream)
            {
                cell.flowDx = 0;
                cell.flowDy = 0;
            }
            else
            {
                const i64 downstreamX =
                    static_cast<i64>(downstreamIndex % resolution);
                const i64 downstreamY =
                    static_cast<i64>(downstreamIndex / resolution);

                cell.flowDx = ClampFlowComponent(
                    downstreamX - static_cast<i64>(x));
                cell.flowDy = ClampFlowComponent(
                    downstreamY - static_cast<i64>(y));
            }
        }
    }

    return grid;
}
} // namespace

DerivedTerrainRegion BuildDerivedTerrainRegionFromGpuReadback(
    const DerivedTerrainRegionId& id,
    const f64 approximateTileWidthMeters,
    const f64 halfExtentMeters,
    const GpuHydrologyReadback& readback,
    const DerivedTerrainRegionConfig& config)
{
    terrain_hydrology::HydrologyGrid hydrology =
        BuildHydrologyGridFromGpu(readback, config, halfExtentMeters);

    // GPU erosion is already an internally-converged single pass (see
    // GpuErosion's own documentation) -- its net delta is used
    // directly as the "cumulative" delta the CPU
    // BuildRegionalElevationDeltaField expects, rather than looping
    // config.refinement.iterations times re-routing hydrology between
    // passes the way RefineHydrologyWithSediment does.
    const std::span<const f32> cumulativeElevationDeltaMeters =
        readback.netElevationDeltaMeters;

    auto rivers = terrain_hydrology::BuildRiverGraph(
        hydrology, config.minimumRiverDrainageAreaSquareMeters);

    auto elevationDelta = terrain_erosion::BuildRegionalElevationDeltaField(
        hydrology, cumulativeElevationDeltaMeters);

    auto carving = terrain_erosion::BuildRiverCarvingField(
        hydrology, rivers, config.carving);

    auto water = terrain_water::BuildRiverWaterNetwork(
        hydrology,
        rivers,
        carving,
        approximateTileWidthMeters * 0.5,
        config.water);

    auto lakes = terrain_water::BuildLakeWaterField(
        hydrology, approximateTileWidthMeters * 0.5, config.lakes);

    return {
        .id = id,
        .approximateTileWidthMeters = approximateTileWidthMeters,
        .halfExtentMeters = halfExtentMeters,
        .hydrology = std::move(hydrology),
        .elevationDelta = std::move(elevationDelta),
        .rivers = std::move(rivers),
        .carving = std::move(carving),
        .water = std::move(water),
        .lakes = std::move(lakes)
    };
}
} // namespace orbit::terrain_region
