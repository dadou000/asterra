#include <orbit/terrain_water/RiverWater.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace orbit::terrain_water
{
namespace
{
[[nodiscard]] f64 Distance(
    const math::Double2& a,
    const math::Double2& b) noexcept
{
    const f64 dx =
        a.x - b.x;

    const f64 dy =
        a.y - b.y;

    return std::sqrt(
        dx * dx +
        dy * dy);
}

[[nodiscard]] bool IsOwnedByCore(
    const math::Double2& a,
    const math::Double2& b,
    const f64 coreHalfExtentMeters) noexcept
{
    const math::Double2 midpoint{
        (a.x + b.x) * 0.5,
        (a.y + b.y) * 0.5
    };

    return
        std::abs(midpoint.x) <=
            coreHalfExtentMeters &&
        std::abs(midpoint.y) <=
            coreHalfExtentMeters;
}

void ValidateConfig(
    const RiverWaterConfig& config,
    const f64 coreHalfExtentMeters)
{
    if (!std::isfinite(
            coreHalfExtentMeters) ||
        coreHalfExtentMeters <= 0.0 ||
        !std::isfinite(
            config.channelFillFraction) ||
        config.channelFillFraction <= 0.0 ||
        config.channelFillFraction > 1.0 ||
        !std::isfinite(
            config.referenceDrainageAreaSquareMeters) ||
        config.referenceDrainageAreaSquareMeters <= 0.0 ||
        config.baseDepthMeters <= 0.0 ||
        config.minimumDepthMeters <= 0.0 ||
        config.maximumDepthMeters <
            config.minimumDepthMeters ||
        config.depthExponent < 0.0 ||
        config.bankClearanceMeters < 0.0 ||
        config.manningRoughness <= 0.0 ||
        config.minimumSlope < 0.0 ||
        config.minimumVelocityMetersPerSecond < 0.0 ||
        config.maximumVelocityMetersPerSecond <
            config.minimumVelocityMetersPerSecond)
    {
        throw std::invalid_argument(
            "Orbit river water configuration is invalid.");
    }
}

[[nodiscard]] f64 WaterDepth(
    const f64 drainageAreaSquareMeters,
    const RiverWaterConfig& config) noexcept
{
    const f64 areaRatio =
        std::max(
            drainageAreaSquareMeters /
                config.
                    referenceDrainageAreaSquareMeters,
            1.0e-6);

    return std::clamp(
        config.baseDepthMeters *
            std::pow(
                areaRatio,
                config.depthExponent),
        config.minimumDepthMeters,
        config.maximumDepthMeters);
}

[[nodiscard]] f64 ManningVelocity(
    const f64 fullWidthMeters,
    const f64 depthMeters,
    const f64 slope,
    const RiverWaterConfig& config) noexcept
{
    if (fullWidthMeters <= 0.0 ||
        depthMeters <= 0.0)
    {
        return
            config.
                minimumVelocityMetersPerSecond;
    }

    const f64 area =
        fullWidthMeters *
        depthMeters;

    const f64 wettedPerimeter =
        fullWidthMeters +
        2.0 *
            depthMeters;

    const f64 hydraulicRadius =
        area /
        std::max(
            wettedPerimeter,
            1.0e-6);

    const f64 velocity =
        (1.0 /
         config.manningRoughness) *
        std::pow(
            hydraulicRadius,
            2.0 / 3.0) *
        std::sqrt(
            std::max(
                slope,
                config.minimumSlope));

    return std::clamp(
        velocity,
        config.minimumVelocityMetersPerSecond,
        config.maximumVelocityMetersPerSecond);
}
} // namespace

RiverWaterNetwork BuildRiverWaterNetwork(
    const terrain_hydrology::HydrologyGrid& hydrology,
    const terrain_hydrology::RiverGraph& rivers,
    const terrain_erosion::RiverCarvingField& carving,
    const f64 coreHalfExtentMeters,
    const RiverWaterConfig config)
{
    ValidateConfig(
        config,
        coreHalfExtentMeters);

    if (rivers.nodes.size() !=
            carving.nodes.size() ||
        hydrology.config.resolution < 3)
    {
        throw std::invalid_argument(
            "Orbit river water requires matching river and carving topology.");
    }

    RiverWaterNetwork network{};

    network.surfaceFrame =
        hydrology.surfaceFrame;

    network.coreHalfExtentMeters =
        coreHalfExtentMeters;

    constexpr u32 invalidNode =
        std::numeric_limits<u32>::max();

    std::vector<u32> compactNodeForRiver(
        rivers.nodes.size(),
        invalidNode);

    const auto ensureNode =
        [&network,
         &compactNodeForRiver,
         &rivers,
         &carving,
         &config](
            const u32 riverNodeIndex) -> u32
        {
            if (riverNodeIndex >=
                rivers.nodes.size())
            {
                throw std::invalid_argument(
                    "Orbit river water segment references an invalid river node.");
            }

            u32& existing =
                compactNodeForRiver[
                    riverNodeIndex];

            if (existing !=
                invalidNode)
            {
                return existing;
            }

            const auto& riverNode =
                rivers.nodes[
                    riverNodeIndex];

            const auto& carvingNode =
                carving.nodes[
                    riverNodeIndex];

            if (carvingNode.sourceRiverNode !=
                riverNodeIndex)
            {
                throw std::invalid_argument(
                    "Orbit river water carving topology is not aligned with the river graph.");
            }

            const f64 depth =
                WaterDepth(
                    riverNode.
                        drainageAreaSquareMeters,
                    config);

            const f64 halfWidth =
                std::max(
                    static_cast<f64>(
                        carvingNode.
                            channelHalfWidthMeters) *
                        config.
                            channelFillFraction,
                    0.25);

            const f64 bed =
                static_cast<f64>(
                    carvingNode.
                        bedElevationMeters);

            const f64 bankLimit =
                static_cast<f64>(
                    carvingNode.
                        sourceElevationMeters) -
                config.
                    bankClearanceMeters;

            const f64 surface =
                std::max(
                    bed,
                    std::min(
                        bed + depth,
                        bankLimit));

            const f64 effectiveDepth =
                std::max(
                    surface - bed,
                    0.0);

            existing =
                static_cast<u32>(
                    network.nodes.size());

            network.nodes.push_back({
                .sourceRiverNode =
                    riverNodeIndex,
                .offsetMeters =
                    riverNode.offsetMeters,
                .bedElevationMeters =
                    static_cast<f32>(
                        bed),
                .surfaceElevationMeters =
                    static_cast<f32>(
                        surface),
                .halfWidthMeters =
                    static_cast<f32>(
                        halfWidth),
                .depthMeters =
                    static_cast<f32>(
                        effectiveDepth),
                .drainageAreaSquareMeters =
                    riverNode.
                        drainageAreaSquareMeters
            });

            return existing;
        };

    network.segments.reserve(
        rivers.segments.size());

    for (const auto& riverSegment :
         rivers.segments)
    {
        if (riverSegment.upstreamNode >=
                rivers.nodes.size() ||
            riverSegment.downstreamNode >=
                rivers.nodes.size())
        {
            throw std::invalid_argument(
                "Orbit river water encountered an invalid river segment.");
        }

        const auto& upstreamRiver =
            rivers.nodes[
                riverSegment.upstreamNode];

        const auto& downstreamRiver =
            rivers.nodes[
                riverSegment.downstreamNode];

        if (!IsOwnedByCore(
                upstreamRiver.offsetMeters,
                downstreamRiver.offsetMeters,
                coreHalfExtentMeters))
        {
            continue;
        }

        const u32 upstreamNode =
            ensureNode(
                riverSegment.
                    upstreamNode);

        const u32 downstreamNode =
            ensureNode(
                riverSegment.
                    downstreamNode);

        const RiverWaterNode& upstream =
            network.nodes[
                upstreamNode];

        const RiverWaterNode& downstream =
            network.nodes[
                downstreamNode];

        const f64 length =
            Distance(
                upstream.offsetMeters,
                downstream.offsetMeters);

        const f64 elevationDrop =
            static_cast<f64>(
                upstream.
                    bedElevationMeters) -
            static_cast<f64>(
                downstream.
                    bedElevationMeters);

        const f64 slope =
            length > 1.0e-6
                ? std::max(
                    elevationDrop /
                        length,
                    0.0)
                : 0.0;

        const f64 averageDepth =
            0.5 *
            (static_cast<f64>(
                 upstream.depthMeters) +
             static_cast<f64>(
                 downstream.depthMeters));

        const f64 averageFullWidth =
            static_cast<f64>(
                upstream.halfWidthMeters +
                downstream.halfWidthMeters);

        network.segments.push_back({
            .upstreamNode =
                upstreamNode,
            .downstreamNode =
                downstreamNode,
            .slope =
                static_cast<f32>(
                    slope),
            .velocityMetersPerSecond =
                static_cast<f32>(
                    ManningVelocity(
                        averageFullWidth,
                        averageDepth,
                        slope,
                        config))
        });
    }

    return network;
}
} // namespace orbit::terrain_water
