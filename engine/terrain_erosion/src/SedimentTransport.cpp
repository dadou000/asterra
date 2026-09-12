#include <orbit/terrain_erosion/SedimentTransport.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace orbit::terrain_erosion
{
namespace
{
[[nodiscard]] std::size_t CellIndex(
    const u32 resolution,
    const u32 x,
    const u32 y) noexcept
{
    return
        static_cast<std::size_t>(y) *
            resolution +
        x;
}

[[nodiscard]] bool IsInside(
    const i32 coordinate,
    const u32 resolution) noexcept
{
    return
        coordinate >= 0 &&
        coordinate <
            static_cast<i32>(
                resolution);
}

void ValidateConfig(
    const SedimentTransportConfig& config)
{
    if (config.
            referenceDrainageAreaSquareMeters <=
            0.0 ||
        config.erosionScaleMeters <
            0.0 ||
        config.maximumErosionMeters <
            0.0 ||
        config.drainageAreaExponent <
            0.0 ||
        config.slopeExponent <
            0.0 ||
        config.depositionSlopeThreshold <
            0.0 ||
        config.maximumLandDepositionFraction <
            0.0 ||
        config.maximumLandDepositionFraction >
            1.0 ||
        config.oceanDepositionFraction <
            0.0 ||
        config.oceanDepositionFraction >
            1.0 ||
        config.maximumDepositionMeters <
            0.0)
    {
        throw std::invalid_argument(
            "Orbit sediment transport configuration is invalid.");
    }
}
} // namespace

SedimentCell& SedimentTransportGrid::At(
    const u32 x,
    const u32 y)
{
    if (x >= resolution ||
        y >= resolution)
    {
        throw std::out_of_range(
            "Orbit sediment grid coordinate is out of range.");
    }

    return cells[
        CellIndex(
            resolution,
            x,
            y)];
}

const SedimentCell&
SedimentTransportGrid::At(
    const u32 x,
    const u32 y) const
{
    if (x >= resolution ||
        y >= resolution)
    {
        throw std::out_of_range(
            "Orbit sediment grid coordinate is out of range.");
    }

    return cells[
        CellIndex(
            resolution,
            x,
            y)];
}

SedimentTransportGrid
BuildSedimentTransport(
    const terrain_hydrology::HydrologyGrid& hydrology,
    const SedimentTransportConfig config)
{
    ValidateConfig(config);

    const u32 resolution =
        hydrology.config.resolution;

    if (resolution < 3 ||
        hydrology.cells.size() !=
            static_cast<std::size_t>(
                resolution) *
            resolution ||
        hydrology.spacingMeters <=
            0.0)
    {
        throw std::invalid_argument(
            "Orbit sediment transport requires a valid routed hydrology grid.");
    }

    SedimentTransportGrid result{};
    result.resolution =
        resolution;

    result.spacingMeters =
        hydrology.spacingMeters;

    result.cells.resize(
        hydrology.cells.size());

    std::vector<u32> order(
        hydrology.cells.size());

    std::iota(
        order.begin(),
        order.end(),
        0U);

    std::stable_sort(
        order.begin(),
        order.end(),
        [&hydrology](
            const u32 a,
            const u32 b)
        {
            return
                hydrology.cells[a].
                    drainageElevationMeters >
                hydrology.cells[b].
                    drainageElevationMeters;
        });

    const f64 cellArea =
        hydrology.spacingMeters *
        hydrology.spacingMeters;

    for (const u32 index :
         order)
    {
        const u32 x =
            index %
            resolution;

        const u32 y =
            index /
            resolution;

        const terrain_hydrology::HydrologyCell&
            hydroCell =
                hydrology.cells[
                    index];

        SedimentCell& cell =
            result.cells[
                index];

        const i32 downstreamX =
            static_cast<i32>(x) +
            hydroCell.flowDx;

        const i32 downstreamY =
            static_cast<i32>(y) +
            hydroCell.flowDy;

        const bool hasDownstream =
            (hydroCell.flowDx != 0 ||
             hydroCell.flowDy != 0) &&
            IsInside(
                downstreamX,
                resolution) &&
            IsInside(
                downstreamY,
                resolution);

        f64 slope = 0.0;

        if (hasDownstream)
        {
            const terrain_hydrology::HydrologyCell&
                downstream =
                    hydrology.At(
                        static_cast<u32>(
                            downstreamX),
                        static_cast<u32>(
                            downstreamY));

            const f64 dx =
                static_cast<f64>(
                    hydroCell.flowDx);

            const f64 dy =
                static_cast<f64>(
                    hydroCell.flowDy);

            const f64 distance =
                hydrology.spacingMeters *
                std::sqrt(
                    dx * dx +
                    dy * dy);

            if (distance > 0.0)
            {
                slope =
                    std::max(
                        (static_cast<f64>(
                             hydroCell.
                                 drainageElevationMeters) -
                         static_cast<f64>(
                             downstream.
                                 drainageElevationMeters)) /
                            distance,
                        0.0);
            }
        }

        const f64 drainageArea =
            std::max(
                static_cast<f64>(
                    hydroCell.
                        flowAccumulation) *
                    cellArea,
                0.0);

        const f64 areaFactor =
            std::pow(
                drainageArea /
                    config.
                        referenceDrainageAreaSquareMeters,
                config.
                    drainageAreaExponent);

        const f64 slopeFactor =
            std::pow(
                std::max(
                    slope,
                    0.0),
                config.
                    slopeExponent);

        const f64 erosion =
            hydroCell.oceanWeight >=
                    0.5F
                ? 0.0
                : std::clamp(
                    config.
                        erosionScaleMeters *
                        areaFactor *
                        slopeFactor,
                    0.0,
                    config.
                        maximumErosionMeters);

        cell.erosionMeters =
            static_cast<f32>(
                erosion);

        const f64 availableSediment =
            std::max(
                static_cast<f64>(
                    cell.
                        incomingSediment) +
                    erosion,
                0.0);

        f64 depositionFraction = 0.0;

        if (hydroCell.oceanWeight >=
            0.5F)
        {
            depositionFraction =
                config.
                    oceanDepositionFraction;
        }
        else if (config.
                     depositionSlopeThreshold >
                 0.0)
        {
            const f64 lowSlope =
                std::clamp(
                    1.0 -
                        slope /
                        config.
                            depositionSlopeThreshold,
                    0.0,
                    1.0);

            depositionFraction =
                lowSlope *
                config.
                    maximumLandDepositionFraction;
        }

        const f64 deposition =
            std::min(
                availableSediment *
                    depositionFraction,
                config.
                    maximumDepositionMeters);

        const f64 outgoing =
            std::max(
                availableSediment -
                    deposition,
                0.0);

        cell.depositionMeters =
            static_cast<f32>(
                deposition);

        cell.netElevationDeltaMeters =
            static_cast<f32>(
                deposition -
                erosion);

        cell.outgoingSediment =
            static_cast<f32>(
                outgoing);

        if (hasDownstream)
        {
            const u32 downstreamIndex =
                static_cast<u32>(
                    downstreamY) *
                    resolution +
                static_cast<u32>(
                    downstreamX);

            result.cells[
                downstreamIndex].
                incomingSediment +=
                    static_cast<f32>(
                        outgoing);
        }
        else
        {
            result.exportedSediment +=
                outgoing;
        }
    }

    return result;
}
} // namespace orbit::terrain_erosion
