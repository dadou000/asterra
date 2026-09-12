#include <orbit/terrain_erosion/RiverCarving.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace orbit::terrain_erosion
{
namespace
{
[[nodiscard]] f64 Clamp01(
    const f64 value) noexcept
{
    return std::clamp(
        value,
        0.0,
        1.0);
}

[[nodiscard]] f64 SmoothStep01(
    const f64 value) noexcept
{
    const f64 t =
        Clamp01(value);

    return
        t * t *
        (3.0 - 2.0 * t);
}

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

[[nodiscard]] f64 Dot(
    const math::Double2& a,
    const math::Double2& b) noexcept
{
    return
        a.x * b.x +
        a.y * b.y;
}

[[nodiscard]] math::Double2 Subtract(
    const math::Double2& a,
    const math::Double2& b) noexcept
{
    return {
        a.x - b.x,
        a.y - b.y
    };
}

[[nodiscard]] math::Double2 AddScaled(
    const math::Double2& a,
    const math::Double2& direction,
    const f64 scale) noexcept
{
    return {
        a.x +
            direction.x *
                scale,
        a.y +
            direction.y *
                scale
    };
}

[[nodiscard]] f64 Lerp(
    const f64 a,
    const f64 b,
    const f64 t) noexcept
{
    return
        a +
        (b - a) *
            t;
}

void ValidateConfig(
    const RiverCarvingConfig& config)
{
    if (config.
            referenceDrainageAreaSquareMeters <=
            0.0 ||
        config.baseChannelHalfWidthMeters <=
            0.0 ||
        config.minimumChannelHalfWidthMeters <=
            0.0 ||
        config.maximumChannelHalfWidthMeters <
            config.
                minimumChannelHalfWidthMeters ||
        config.baseDepthMeters <=
            0.0 ||
        config.minimumDepthMeters <=
            0.0 ||
        config.maximumDepthMeters <
            config.minimumDepthMeters ||
        config.valleyWidthMultiplier <
            1.0 ||
        config.minimumBedSlope <
            0.0 ||
        config.maximumIncisionMeters <=
            0.0)
    {
        throw std::invalid_argument(
            "Orbit river carving configuration is invalid.");
    }
}
} // namespace

RiverCarvingField BuildRiverCarvingField(
    const terrain_hydrology::HydrologyGrid& hydrology,
    const terrain_hydrology::RiverGraph& rivers,
    const RiverCarvingConfig config)
{
    ValidateConfig(config);

    RiverCarvingField field{};
    field.surfaceFrame =
        hydrology.surfaceFrame;

    field.halfExtentMeters =
        hydrology.config.
            halfExtentMeters;

    field.nodes.reserve(
        rivers.nodes.size());

    for (u32 nodeIndex = 0;
         nodeIndex <
            static_cast<u32>(
                rivers.nodes.size());
         ++nodeIndex)
    {
        const terrain_hydrology::RiverNode&
            riverNode =
                rivers.nodes[
                    nodeIndex];

        const f64 areaRatio =
            std::max(
                riverNode.
                    drainageAreaSquareMeters /
                    config.
                        referenceDrainageAreaSquareMeters,
                1.0e-6);

        const f64 channelHalfWidth =
            std::clamp(
                config.
                    baseChannelHalfWidthMeters *
                    std::pow(
                        areaRatio,
                        config.widthExponent),
                config.
                    minimumChannelHalfWidthMeters,
                config.
                    maximumChannelHalfWidthMeters);

        const f64 depth =
            std::clamp(
                config.baseDepthMeters *
                    std::pow(
                        areaRatio,
                        config.depthExponent),
                config.minimumDepthMeters,
                config.maximumDepthMeters);

        const f64 sourceElevation =
            static_cast<f64>(
                riverNode.
                    elevationMeters);

        const f64 minimumBedElevation =
            sourceElevation -
            config.
                maximumIncisionMeters;

        const f64 initialBedElevation =
            std::max(
                sourceElevation -
                    depth,
                minimumBedElevation);

        field.nodes.push_back({
            .sourceRiverNode =
                nodeIndex,
            .offsetMeters =
                riverNode.offsetMeters,
            .sourceElevationMeters =
                riverNode.elevationMeters,
            .drainageElevationMeters =
                riverNode.
                    drainageElevationMeters,
            .bedElevationMeters =
                static_cast<f32>(
                    initialBedElevation),
            .channelHalfWidthMeters =
                static_cast<f32>(
                    channelHalfWidth),
            .valleyHalfWidthMeters =
                static_cast<f32>(
                    channelHalfWidth *
                    config.
                        valleyWidthMultiplier),
            .drainageAreaSquareMeters =
                riverNode.
                    drainageAreaSquareMeters
        });
    }

    field.segments.reserve(
        rivers.segments.size());

    for (const terrain_hydrology::RiverSegment&
         segment :
         rivers.segments)
    {
        if (segment.upstreamNode >=
                field.nodes.size() ||
            segment.downstreamNode >=
                field.nodes.size())
        {
            throw std::invalid_argument(
                "Orbit river graph contains an invalid segment.");
        }

        field.segments.push_back({
            .upstreamNode =
                segment.upstreamNode,
            .downstreamNode =
                segment.downstreamNode
        });
    }

    std::vector<std::vector<u32>>
        outgoing(
            field.nodes.size());

    for (u32 segmentIndex = 0;
         segmentIndex <
            static_cast<u32>(
                field.segments.size());
         ++segmentIndex)
    {
        const RiverCarvingSegment& segment =
            field.segments[
                segmentIndex];

        outgoing[
            segment.upstreamNode].
            push_back(
                segmentIndex);
    }

    std::vector<u32> order(
        field.nodes.size());

    std::iota(
        order.begin(),
        order.end(),
        0U);

    std::stable_sort(
        order.begin(),
        order.end(),
        [&field](
            const u32 a,
            const u32 b)
        {
            return
                field.nodes[a].
                    drainageElevationMeters >
                field.nodes[b].
                    drainageElevationMeters;
        });

    for (const u32 upstreamIndex :
         order)
    {
        RiverCarvingNode& upstream =
            field.nodes[
                upstreamIndex];

        for (const u32 segmentIndex :
             outgoing[
                 upstreamIndex])
        {
            const RiverCarvingSegment& segment =
                field.segments[
                    segmentIndex];

            RiverCarvingNode& downstream =
                field.nodes[
                    segment.
                        downstreamNode];

            const f64 segmentLength =
                Distance(
                    upstream.
                        offsetMeters,
                    downstream.
                        offsetMeters);

            const f64 maximumDownstreamBed =
                static_cast<f64>(
                    upstream.
                        bedElevationMeters) -
                config.
                    minimumBedSlope *
                segmentLength;

            const f64 minimumDownstreamBed =
                static_cast<f64>(
                    downstream.
                        sourceElevationMeters) -
                config.
                    maximumIncisionMeters;

            const f64 adjustedDownstreamBed =
                std::max(
                    std::min(
                        static_cast<f64>(
                            downstream.
                                bedElevationMeters),
                        maximumDownstreamBed),
                    minimumDownstreamBed);

            downstream.bedElevationMeters =
                static_cast<f32>(
                    adjustedDownstreamBed);
        }
    }

    return field;
}

RiverCarvingSample SampleRiverCarving(
    const RiverCarvingField& field,
    const math::Double2& offsetMeters) noexcept
{
    RiverCarvingSample best{};

    f64 bestTargetElevation = 0.0;

    for (const RiverCarvingSegment& segment :
         field.segments)
    {
        if (segment.upstreamNode >=
                field.nodes.size() ||
            segment.downstreamNode >=
                field.nodes.size())
        {
            continue;
        }

        const RiverCarvingNode& upstream =
            field.nodes[
                segment.upstreamNode];

        const RiverCarvingNode& downstream =
            field.nodes[
                segment.downstreamNode];

        const math::Double2 direction =
            Subtract(
                downstream.
                    offsetMeters,
                upstream.
                    offsetMeters);

        const f64 lengthSquared =
            Dot(
                direction,
                direction);

        if (lengthSquared <=
            1.0e-12)
        {
            continue;
        }

        const math::Double2 fromUpstream =
            Subtract(
                offsetMeters,
                upstream.
                    offsetMeters);

        const f64 t =
            Clamp01(
                Dot(
                    fromUpstream,
                    direction) /
                lengthSquared);

        const math::Double2 nearest =
            AddScaled(
                upstream.
                    offsetMeters,
                direction,
                t);

        const f64 distance =
            Distance(
                offsetMeters,
                nearest);

        const f64 channelHalfWidth =
            Lerp(
                upstream.
                    channelHalfWidthMeters,
                downstream.
                    channelHalfWidthMeters,
                t);

        const f64 valleyHalfWidth =
            Lerp(
                upstream.
                    valleyHalfWidthMeters,
                downstream.
                    valleyHalfWidthMeters,
                t);

        if (distance >
            valleyHalfWidth)
        {
            continue;
        }

        const f64 bedElevation =
            Lerp(
                upstream.
                    bedElevationMeters,
                downstream.
                    bedElevationMeters,
                t);

        const f64 sourceElevation =
            Lerp(
                upstream.
                    sourceElevationMeters,
                downstream.
                    sourceElevationMeters,
                t);

        const f64 bankWidth =
            std::max(
                valleyHalfWidth -
                    channelHalfWidth,
                1.0e-6);

        const f64 bankT =
            (distance -
             channelHalfWidth) /
            bankWidth;

        const f64 bankBlend =
            SmoothStep01(
                bankT);

        const f64 targetElevation =
            Lerp(
                bedElevation,
                sourceElevation,
                bankBlend);

        const f64 influence =
            1.0 -
            bankBlend;

        if (!best.active ||
            targetElevation <
                bestTargetElevation)
        {
            best.active = true;

            best.targetElevationMeters =
                targetElevation;

            best.distanceToCenterMeters =
                distance;

            best.influence =
                influence;

            bestTargetElevation =
                targetElevation;
        }
    }

    return best;
}
} // namespace orbit::terrain_erosion
