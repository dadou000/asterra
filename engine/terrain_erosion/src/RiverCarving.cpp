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

[[nodiscard]] u32 SpatialCoordinate(
    const f64 coordinate,
    const f64 halfExtent,
    const f64 cellSize,
    const u32 resolution) noexcept
{
    if (resolution == 0 ||
        cellSize <= 0.0)
    {
        return 0;
    }

    const f64 normalized =
        (coordinate +
         halfExtent) /
        cellSize;

    const i64 raw =
        static_cast<i64>(
            std::floor(
                normalized));

    return static_cast<u32>(
        std::clamp<i64>(
            raw,
            0,
            static_cast<i64>(
                resolution - 1U)));
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
            0.0 ||
        config.spatialIndexResolution == 0)
    {
        throw std::invalid_argument(
            "Orbit river carving configuration is invalid.");
    }
}

[[nodiscard]] RiverCarvingSample
SampleSegment(
    const RiverCarvingField& field,
    const RiverCarvingSegment& segment,
    const math::Double2& offsetMeters) noexcept
{
    if (segment.upstreamNode >=
            field.nodes.size() ||
        segment.downstreamNode >=
            field.nodes.size())
    {
        return {};
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
        return {};
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
        return {};
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

    return {
        .active = true,
        .targetElevationMeters =
            Lerp(
                bedElevation,
                sourceElevation,
                bankBlend),
        .distanceToCenterMeters =
            distance,
        .influence =
            1.0 -
            bankBlend,
        .channelHalfWidthMeters =
            channelHalfWidth,
        .valleyHalfWidthMeters =
            valleyHalfWidth
    };
}

void BuildSpatialIndex(
    RiverCarvingField& field,
    const u32 resolution)
{
    field.spatialResolution =
        resolution;

    f64 maximumValleyHalfWidth =
        0.0;

    for (const RiverCarvingNode& node :
         field.nodes)
    {
        maximumValleyHalfWidth =
            std::max(
                maximumValleyHalfWidth,
                static_cast<f64>(
                    node.
                        valleyHalfWidthMeters));
    }

    field.spatialHalfExtentMeters =
        field.halfExtentMeters +
        maximumValleyHalfWidth;

    field.spatialCellSizeMeters =
        field.spatialHalfExtentMeters *
        2.0 /
        static_cast<f64>(
            resolution);

    const std::size_t cellCount =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    std::vector<
        std::vector<u32>>
        cells(
            cellCount);

    for (u32 segmentIndex = 0;
         segmentIndex <
            static_cast<u32>(
                field.segments.size());
         ++segmentIndex)
    {
        const RiverCarvingSegment& segment =
            field.segments[
                segmentIndex];

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

        const f64 envelope =
            std::max(
                static_cast<f64>(
                    upstream.
                        valleyHalfWidthMeters),
                static_cast<f64>(
                    downstream.
                        valleyHalfWidthMeters));

        const f64 minimumX =
            std::min(
                upstream.offsetMeters.x,
                downstream.offsetMeters.x) -
            envelope;

        const f64 maximumX =
            std::max(
                upstream.offsetMeters.x,
                downstream.offsetMeters.x) +
            envelope;

        const f64 minimumY =
            std::min(
                upstream.offsetMeters.y,
                downstream.offsetMeters.y) -
            envelope;

        const f64 maximumY =
            std::max(
                upstream.offsetMeters.y,
                downstream.offsetMeters.y) +
            envelope;

        const u32 minX =
            SpatialCoordinate(
                minimumX,
                field.
                    spatialHalfExtentMeters,
                field.
                    spatialCellSizeMeters,
                resolution);

        const u32 maxX =
            SpatialCoordinate(
                maximumX,
                field.
                    spatialHalfExtentMeters,
                field.
                    spatialCellSizeMeters,
                resolution);

        const u32 minY =
            SpatialCoordinate(
                minimumY,
                field.
                    spatialHalfExtentMeters,
                field.
                    spatialCellSizeMeters,
                resolution);

        const u32 maxY =
            SpatialCoordinate(
                maximumY,
                field.
                    spatialHalfExtentMeters,
                field.
                    spatialCellSizeMeters,
                resolution);

        for (u32 y = minY;
             y <= maxY;
             ++y)
        {
            for (u32 x = minX;
                 x <= maxX;
                 ++x)
            {
                cells[
                    static_cast<std::size_t>(
                        y) *
                        resolution +
                    x].
                    push_back(
                        segmentIndex);
            }
        }
    }

    field.spatialCellOffsets.resize(
        cellCount + 1U);

    std::size_t totalIndices = 0;

    for (std::size_t cellIndex = 0;
         cellIndex < cellCount;
         ++cellIndex)
    {
        field.spatialCellOffsets[
            cellIndex] =
                static_cast<u32>(
                    totalIndices);

        totalIndices +=
            cells[cellIndex].
                size();
    }

    field.spatialCellOffsets[
        cellCount] =
            static_cast<u32>(
                totalIndices);

    field.spatialSegmentIndices.
        reserve(
            totalIndices);

    for (const auto& cell :
         cells)
    {
        field.spatialSegmentIndices.
            insert(
                field.
                    spatialSegmentIndices.
                    end(),
                cell.begin(),
                cell.end());
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

    BuildSpatialIndex(
        field,
        config.
            spatialIndexResolution);

    return field;
}

RiverCarvingSample SampleRiverCarving(
    const RiverCarvingField& field,
    const math::Double2& offsetMeters) noexcept
{
    RiverCarvingSample best{};
    f64 bestTargetElevation = 0.0;

    if (field.spatialResolution == 0 ||
        field.spatialCellSizeMeters <=
            0.0 ||
        field.spatialCellOffsets.size() !=
            static_cast<std::size_t>(
                field.
                    spatialResolution) *
                field.
                    spatialResolution +
            1U)
    {
        return best;
    }

    if (std::abs(
            offsetMeters.x) >
            field.
                spatialHalfExtentMeters ||
        std::abs(
            offsetMeters.y) >
            field.
                spatialHalfExtentMeters)
    {
        return best;
    }

    const u32 cellX =
        SpatialCoordinate(
            offsetMeters.x,
            field.
                spatialHalfExtentMeters,
            field.
                spatialCellSizeMeters,
            field.
                spatialResolution);

    const u32 cellY =
        SpatialCoordinate(
            offsetMeters.y,
            field.
                spatialHalfExtentMeters,
            field.
                spatialCellSizeMeters,
            field.
                spatialResolution);

    const std::size_t cellIndex =
        static_cast<std::size_t>(
            cellY) *
            field.
                spatialResolution +
        cellX;

    const u32 begin =
        field.spatialCellOffsets[
            cellIndex];

    const u32 end =
        field.spatialCellOffsets[
            cellIndex + 1U];

    if (begin > end ||
        end >
            field.
                spatialSegmentIndices.
                size())
    {
        return best;
    }

    for (u32 candidate = begin;
         candidate < end;
         ++candidate)
    {
        const u32 segmentIndex =
            field.spatialSegmentIndices[
                candidate];

        if (segmentIndex >=
            field.segments.size())
        {
            continue;
        }

        const RiverCarvingSample sample =
            SampleSegment(
                field,
                field.segments[
                    segmentIndex],
                offsetMeters);

        if (!sample.active)
        {
            continue;
        }

        if (!best.active ||
            sample.
                targetElevationMeters <
                bestTargetElevation)
        {
            best = sample;

            bestTargetElevation =
                sample.
                    targetElevationMeters;
        }
    }

    return best;
}
} // namespace orbit::terrain_erosion
