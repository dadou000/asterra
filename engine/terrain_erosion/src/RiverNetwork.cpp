#include <orbit/terrain_erosion/RiverNetwork.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orbit::terrain_erosion
{
namespace
{
constexpr u64 kRiverNodeDomain =
    0x4F5242564E4F4445ULL; // "ORBVNODE"
constexpr u64 kRiverSegmentDomain =
    0x4F5242565345474DULL; // "ORBVSEGM"
constexpr u64 kRiverBasinDomain =
    0x4F5242564241534EULL; // "ORVBASN"
constexpr u64 kRiverCutoffDomain =
    0x4F52425643555446ULL; // "ORBVCUTF"

[[nodiscard]] std::size_t Index(
    const u32 resolution,
    const u32 x,
    const u32 y) noexcept
{
    return
        static_cast<std::size_t>(y) *
            resolution +
        x;
}

[[nodiscard]] bool Inside(
    const i32 x,
    const i32 y,
    const u32 resolution) noexcept
{
    return
        x >= 0 &&
        y >= 0 &&
        x <
            static_cast<i32>(resolution) &&
        y <
            static_cast<i32>(resolution);
}

[[nodiscard]] f64 Length(
    const math::Double2& value) noexcept
{
    return
        std::hypot(
            value.x,
            value.y);
}

[[nodiscard]] math::Double2 Add(
    const math::Double2& a,
    const math::Double2& b) noexcept
{
    return {
        a.x + b.x,
        a.y + b.y
    };
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

[[nodiscard]] math::Double2 Scale(
    const math::Double2& value,
    const f64 scale) noexcept
{
    return {
        value.x * scale,
        value.y * scale
    };
}

[[nodiscard]] f64 Dot(
    const math::Double2& a,
    const math::Double2& b) noexcept
{
    return
        a.x * b.x +
        a.y * b.y;
}

[[nodiscard]] math::Double2 Normalize(
    const math::Double2& value) noexcept
{
    const f64 length =
        Length(value);

    if (length <= 1.0e-12)
    {
        return {};
    }

    return {
        value.x / length,
        value.y / length
    };
}

[[nodiscard]] f64 Distance(
    const math::Double2& a,
    const math::Double2& b) noexcept
{
    return
        Length(
            Subtract(
                a,
                b));
}

[[nodiscard]] f64 DistanceToSegment(
    const math::Double2& point,
    const math::Double2& a,
    const math::Double2& b,
    f64* const tOut = nullptr) noexcept
{
    const math::Double2 ab =
        Subtract(
            b,
            a);

    const f64 lengthSquared =
        Dot(
            ab,
            ab);

    f64 t = 0.0;

    if (lengthSquared > 1.0e-18)
    {
        t =
            std::clamp(
                Dot(
                    Subtract(
                        point,
                        a),
                    ab) /
                    lengthSquared,
                0.0,
                1.0);
    }

    if (tOut != nullptr)
    {
        *tOut = t;
    }

    return
        Distance(
            point,
            Add(
                a,
                Scale(
                    ab,
                    t)));
}

[[nodiscard]] u64 StableAddressFingerprint(
    const terrain::PhysicalTerrainPageAddress& address) noexcept
{
    u64 value =
        0x4F52425650414745ULL; // "ORBVPAGE"

    value =
        terrain::StableCombine64(
            value,
            address.planet.high);

    value =
        terrain::StableCombine64(
            value,
            address.planet.low);

    value =
        terrain::StableCombine64(
            value,
            static_cast<u64>(
                address.tile.face));

    value =
        terrain::StableCombine64(
            value,
            address.tile.level);

    value =
        terrain::StableCombine64(
            value,
            address.tile.x);

    value =
        terrain::StableCombine64(
            value,
            address.tile.y);

    return value;
}

template <typename Id>
[[nodiscard]] Id MakeStableId(
    const u64 domain,
    const terrain::PhysicalTerrainPageAddress& address,
    const u64 a,
    const u64 b = 0ULL) noexcept
{
    const u64 page =
        StableAddressFingerprint(
            address);

    return {
        .high =
            terrain::StableCombine64(
                domain,
                page),
        .low =
            terrain::StableCombine64(
                terrain::StableCombine64(
                    page,
                    a),
                b)
    };
}

[[nodiscard]] RiverNodeId MakeNodeId(
    const terrain::PhysicalTerrainPageAddress& address,
    const u32 x,
    const u32 y) noexcept
{
    return MakeStableId<RiverNodeId>(
        kRiverNodeDomain,
        address,
        x,
        y);
}

[[nodiscard]] RiverBasinId MakeBasinId(
    const terrain::PhysicalTerrainPageAddress& address,
    const u32 terminalX,
    const u32 terminalY,
    const i8 exitDx,
    const i8 exitDy) noexcept
{
    const u64 packedTerminal =
        (static_cast<u64>(
             terminalX) <<
         32U) |
        terminalY;

    const u64 packedExit =
        (static_cast<u64>(
             static_cast<u8>(
                 exitDx)) <<
         8U) |
        static_cast<u8>(
            exitDy);

    return MakeStableId<RiverBasinId>(
        kRiverBasinDomain,
        address,
        packedTerminal,
        packedExit);
}

[[nodiscard]] RiverSegmentId MakeSegmentId(
    const RiverNodeId upstream,
    const RiverNodeId downstream,
    const u64 domain =
        kRiverSegmentDomain) noexcept
{
    return {
        .high =
            terrain::StableCombine64(
                domain,
                upstream.high ^
                    downstream.high),
        .low =
            terrain::StableCombine64(
                upstream.low,
                downstream.low)
    };
}

[[nodiscard]] bool QualifiesAsRiver(
    const terrain_hydrology::DrainageCell& cell,
    const RiverNetworkConfig& config) noexcept
{
    return
        cell.drainageAreaSquareMeters >=
            config.
                minimumDrainageAreaSquareMeters &&
        cell.dischargeCubicMetersPerSecond >=
            config.
                minimumDischargeCubicMetersPerSecond;
}

[[nodiscard]] math::Double2 CellOffset(
    const u32 resolution,
    const f64 spacing,
    const u32 x,
    const u32 y) noexcept
{
    const f64 half =
        static_cast<f64>(
            resolution - 1U) *
        0.5;

    return {
        (static_cast<f64>(x) -
         half) *
            spacing,
        (static_cast<f64>(y) -
         half) *
            spacing
    };
}

[[nodiscard]] RiverBasinId ResolveBasin(
    const terrain_hydrology::DrainagePage& drainage,
    const u32 startX,
    const u32 startY)
{
    const u32 resolution =
        drainage.Resolution();

    u32 x = startX;
    u32 y = startY;

    for (u64 step = 0ULL;
         step <
             static_cast<u64>(
                 resolution) *
                 resolution +
             1ULL;
         ++step)
    {
        const auto& cell =
            drainage.At(
                x,
                y);

        if (!cell.flow.HasDownstream())
        {
            return MakeBasinId(
                drainage.SourcePage().
                    address,
                x,
                y,
                0,
                0);
        }

        const i32 nx =
            static_cast<i32>(x) +
            cell.flow.dx;

        const i32 ny =
            static_cast<i32>(y) +
            cell.flow.dy;

        if (cell.flow.exitsPage ||
            !Inside(
                nx,
                ny,
                resolution))
        {
            return MakeBasinId(
                drainage.SourcePage().
                    address,
                x,
                y,
                cell.flow.dx,
                cell.flow.dy);
        }

        x =
            static_cast<u32>(nx);

        y =
            static_cast<u32>(ny);
    }

    throw std::logic_error(
        "Orbit M16 drainage tracing encountered a cycle.");
}

[[nodiscard]] f64 ConstraintInfluence(
    const RiverConstraint& constraint,
    const math::Double2& position) noexcept
{
    if (!constraint.enabled ||
        constraint.radiusMeters <= 0.0)
    {
        return 0.0;
    }

    const f64 distance =
        Distance(
            constraint.centerMeters,
            position);

    if (distance >=
        constraint.radiusMeters)
    {
        return 0.0;
    }

    const f64 t =
        1.0 -
        distance /
            constraint.radiusMeters;

    return
        t * t *
        (3.0 - 2.0 * t);
}

void ApplyConstraint(
    math::Double2& displacement,
    const RiverConstraint& constraint,
    const math::Double2& position,
    const f64 influence)
{
    if (influence <= 0.0)
    {
        return;
    }

    const f64 strength =
        constraint.strength *
        influence;

    switch (constraint.kind)
    {
    case RiverConstraintKind::Attract:
    {
        const math::Double2 direction =
            Normalize(
                Subtract(
                    constraint.
                        centerMeters,
                    position));

        displacement =
            Add(
                displacement,
                Scale(
                    direction,
                    strength));
        break;
    }

    case RiverConstraintKind::Repel:
    {
        const math::Double2 direction =
            Normalize(
                Subtract(
                    position,
                    constraint.
                        centerMeters));

        displacement =
            Add(
                displacement,
                Scale(
                    direction,
                    strength));
        break;
    }

    case RiverConstraintKind::Trajectory:
    {
        const math::Double2 direction =
            Normalize(
                constraint.
                    directionMeters);

        if (Length(direction) <=
            1.0e-12)
        {
            return;
        }

        const math::Double2 relative =
            Subtract(
                position,
                constraint.
                    centerMeters);

        const math::Double2 projected =
            Add(
                constraint.
                    centerMeters,
                Scale(
                    direction,
                    Dot(
                        relative,
                        direction)));

        const math::Double2 correction =
            Subtract(
                projected,
                position);

        displacement =
            Add(
                displacement,
                Scale(
                    correction,
                    strength /
                        std::max(
                            constraint.
                                radiusMeters,
                            1.0)));
        break;
    }
    }
}

[[nodiscard]] u64 HashDouble(
    const f64 value) noexcept
{
    return
        terrain::StableMix64(
            std::bit_cast<u64>(
                value));
}

[[nodiscard]] u64 HashConstraint(
    const RiverConstraint& constraint) noexcept
{
    u64 value =
        0x4F524256434F4E53ULL; // "ORBVCONS"

    value =
        terrain::StableCombine64(
            value,
            constraint.id.high);

    value =
        terrain::StableCombine64(
            value,
            constraint.id.low);

    value =
        terrain::StableCombine64(
            value,
            constraint.revision);

    value =
        terrain::StableCombine64(
            value,
            static_cast<u64>(
                constraint.kind));

    value =
        terrain::StableCombine64(
            value,
            HashDouble(
                constraint.
                    centerMeters.x));

    value =
        terrain::StableCombine64(
            value,
            HashDouble(
                constraint.
                    centerMeters.y));

    value =
        terrain::StableCombine64(
            value,
            HashDouble(
                constraint.
                    directionMeters.x));

    value =
        terrain::StableCombine64(
            value,
            HashDouble(
                constraint.
                    directionMeters.y));

    value =
        terrain::StableCombine64(
            value,
            HashDouble(
                constraint.
                    radiusMeters));

    value =
        terrain::StableCombine64(
            value,
            HashDouble(
                constraint.
                    strength));

    return value;
}

[[nodiscard]] u64 HashRiverConfig(
    const RiverNetworkConfig& config) noexcept
{
    u64 value =
        0x4F52425643464731ULL; // "ORRVCFG1"

    const auto addDouble =
        [&value](const f64 number)
        {
            value =
                terrain::StableCombine64(
                    value,
                    HashDouble(
                        number));
        };

    addDouble(
        config.
            minimumDrainageAreaSquareMeters);

    addDouble(
        config.
            minimumDischargeCubicMetersPerSecond);

    addDouble(
        config.
            referenceDischargeCubicMetersPerSecond);

    addDouble(
        config.
            baseChannelWidthMeters);

    addDouble(
        config.
            minimumChannelWidthMeters);

    addDouble(
        config.
            maximumChannelWidthMeters);

    addDouble(
        config.
            widthDischargeExponent);

    addDouble(
        config.
            baseChannelDepthMeters);

    addDouble(
        config.
            minimumChannelDepthMeters);

    addDouble(
        config.
            maximumChannelDepthMeters);

    addDouble(
        config.
            depthDischargeExponent);

    value =
        terrain::StableCombine64(
            value,
            config.enableMeanders
                ? 1ULL
                : 0ULL);

    value =
        terrain::StableCombine64(
            value,
            config.
                meanderIterations);

    addDouble(
        config.
            meanderTimeStep);

    addDouble(
        config.
            curvatureMigrationRate);

    addDouble(
        config.
            deterministicSeedMigrationRate);

    addDouble(
        config.
            maximumCenterlineOffsetWidths);

    value =
        terrain::StableCombine64(
            value,
            config.enableCutoffs
                ? 1ULL
                : 0ULL);

    value =
        terrain::StableCombine64(
            value,
            config.
                minimumCutoffPathNodes);

    addDouble(
        config.
            cutoffDistanceWidths);

    return value;
}

void RebuildBasinCounts(
    RiverNetwork& network)
{
    for (auto& basin :
         network.basins)
    {
        basin.nodeCount = 0U;
        basin.activeSegmentCount = 0U;
    }

    for (const auto& node :
         network.nodes)
    {
        for (auto& basin :
             network.basins)
        {
            if (basin.id ==
                node.basin)
            {
                ++basin.nodeCount;
                break;
            }
        }
    }

    for (const auto& segment :
         network.segments)
    {
        if (!segment.active)
        {
            continue;
        }

        for (auto& basin :
             network.basins)
        {
            if (basin.id ==
                segment.basin)
            {
                ++basin.activeSegmentCount;
                break;
            }
        }
    }
}

[[nodiscard]] std::vector<u32>
DirectedPathSegments(
    const RiverNetwork& network,
    const u32 startNode,
    const u32 targetNode)
{
    std::vector<u32> result;

    u32 current =
        startNode;

    for (std::size_t guard = 0U;
         guard <=
             network.nodes.size();
         ++guard)
    {
        if (current ==
            targetNode)
        {
            return result;
        }

        u32 nextSegment =
            std::numeric_limits<u32>::max();

        for (u32 segmentIndex = 0U;
             segmentIndex <
                 static_cast<u32>(
                     network.
                         segments.
                         size());
             ++segmentIndex)
        {
            const auto& segment =
                network.
                    segments[
                        segmentIndex];

            if (segment.active &&
                segment.upstreamNode ==
                    current)
            {
                nextSegment =
                    segmentIndex;
                break;
            }
        }

        if (nextSegment ==
            std::numeric_limits<u32>::max())
        {
            return {};
        }

        result.push_back(
            nextSegment);

        current =
            network.
                segments[
                    nextSegment].
                downstreamNode;
    }

    return {};
}

void EvolveMeanders(
    RiverNetwork& network,
    const std::span<const RiverConstraint> constraints,
    const RiverNetworkConfig& config)
{
    if (!config.enableMeanders ||
        config.meanderIterations == 0U ||
        network.nodes.empty())
    {
        return;
    }

    std::vector<std::vector<u32>>
        incoming(
            network.nodes.size());

    std::vector<std::vector<u32>>
        outgoing(
            network.nodes.size());

    for (u32 segmentIndex = 0U;
         segmentIndex <
             static_cast<u32>(
                 network.
                     segments.
                     size());
         ++segmentIndex)
    {
        const auto& segment =
            network.
                segments[
                    segmentIndex];

        if (!segment.active)
        {
            continue;
        }

        outgoing[
            segment.
                upstreamNode].
            push_back(
                segmentIndex);

        incoming[
            segment.
                downstreamNode].
            push_back(
                segmentIndex);
    }

    std::vector<math::Double2>
        next(
            network.nodes.size());

    for (u32 iteration = 0U;
         iteration <
             config.meanderIterations;
         ++iteration)
    {
        for (std::size_t i = 0U;
             i <
                 network.nodes.size();
             ++i)
        {
            next[i] =
                network.nodes[i].
                    channelOffsetMeters;
        }

        for (u32 nodeIndex = 0U;
             nodeIndex <
                 static_cast<u32>(
                     network.nodes.size());
             ++nodeIndex)
        {
            auto& node =
                network.nodes[
                    nodeIndex];

            math::Double2 displacement{};

            if (incoming[nodeIndex].size() ==
                    1U &&
                outgoing[nodeIndex].size() ==
                    1U)
            {
                const u32 upstream =
                    network.segments[
                        incoming[nodeIndex][0U]].
                        upstreamNode;

                const u32 downstream =
                    network.segments[
                        outgoing[nodeIndex][0U]].
                        downstreamNode;

                const math::Double2 a =
                    network.nodes[
                        upstream].
                        channelOffsetMeters;

                const math::Double2 b =
                    node.
                        channelOffsetMeters;

                const math::Double2 c =
                    network.nodes[
                        downstream].
                        channelOffsetMeters;

                const math::Double2 tangent =
                    Normalize(
                        Subtract(
                            c,
                            a));

                const math::Double2 normal{
                    -tangent.y,
                    tangent.x
                };

                const math::Double2 midpoint =
                    Scale(
                        Add(
                            a,
                            c),
                        0.5);

                const f64 signedCurvature =
                    Dot(
                        Subtract(
                            midpoint,
                            b),
                        normal);

                const f64 deterministic =
                    static_cast<f64>(
                        static_cast<i64>(
                            node.id.low &
                            0xFFFFULL) -
                        32'768LL) /
                    32'768.0;

                const f64 migrationMeters =
                    (config.
                         curvatureMigrationRate *
                         signedCurvature +
                     config.
                         deterministicSeedMigrationRate *
                         deterministic *
                         static_cast<f64>(
                             node.
                                 channelWidthMeters)) *
                    config.
                        meanderTimeStep;

                displacement =
                    Add(
                        displacement,
                        Scale(
                            normal,
                            migrationMeters));
            }

            for (const auto& constraint :
                 constraints)
            {
                if (!constraint.enabled ||
                    constraint.targetBasin !=
                        node.basin)
                {
                    continue;
                }

                ApplyConstraint(
                    displacement,
                    constraint,
                    node.
                        channelOffsetMeters,
                    ConstraintInfluence(
                        constraint,
                        node.
                            channelOffsetMeters));
            }

            math::Double2 candidate =
                Add(
                    node.
                        channelOffsetMeters,
                    displacement);

            const math::Double2 fromDrainage =
                Subtract(
                    candidate,
                    node.
                        drainageOffsetMeters);

            const f64 maximumOffset =
                config.
                    maximumCenterlineOffsetWidths *
                std::max(
                    static_cast<f64>(
                        node.
                            channelWidthMeters),
                    1.0);

            const f64 offsetLength =
                Length(
                    fromDrainage);

            if (offsetLength >
                    maximumOffset &&
                offsetLength > 0.0)
            {
                candidate =
                    Add(
                        node.
                            drainageOffsetMeters,
                        Scale(
                            fromDrainage,
                            maximumOffset /
                                offsetLength));
            }

            next[nodeIndex] =
                candidate;
        }

        for (std::size_t i = 0U;
             i <
                 network.nodes.size();
             ++i)
        {
            network.nodes[i].
                channelOffsetMeters =
                    next[i];
        }
    }
}

void DetectCutoffs(
    RiverNetwork& network,
    const RiverNetworkConfig& config)
{
    if (!config.enableCutoffs ||
        config.minimumCutoffPathNodes <
            2U)
    {
        return;
    }

    bool changed = true;

    while (changed)
    {
        changed = false;

        for (u32 a = 0U;
             a <
                 static_cast<u32>(
                     network.nodes.size()) &&
             !changed;
             ++a)
        {
            for (u32 b = a + 1U;
                 b <
                     static_cast<u32>(
                         network.nodes.size()) &&
                 !changed;
                 ++b)
            {
                const auto& nodeA =
                    network.nodes[a];

                const auto& nodeB =
                    network.nodes[b];

                if (nodeA.basin !=
                    nodeB.basin)
                {
                    continue;
                }

                std::vector<u32> path =
                    DirectedPathSegments(
                        network,
                        a,
                        b);

                u32 upstream = a;
                u32 downstream = b;

                if (path.empty())
                {
                    path =
                        DirectedPathSegments(
                            network,
                            b,
                            a);

                    upstream = b;
                    downstream = a;
                }

                if (path.size() <
                    config.
                        minimumCutoffPathNodes)
                {
                    continue;
                }

                const f64 threshold =
                    config.
                        cutoffDistanceWidths *
                    std::max(
                        static_cast<f64>(
                            network.nodes[
                                upstream].
                                channelWidthMeters),
                        static_cast<f64>(
                            network.nodes[
                                downstream].
                                channelWidthMeters));

                if (Distance(
                        network.nodes[
                            upstream].
                            channelOffsetMeters,
                        network.nodes[
                            downstream].
                            channelOffsetMeters) >
                    threshold)
                {
                    continue;
                }

                RiverCutoffEvent event{
                    .upstreamNode =
                        network.nodes[
                            upstream].
                            id,
                    .downstreamNode =
                        network.nodes[
                            downstream].
                            id
                };

                for (const u32 segmentIndex :
                     path)
                {
                    auto& segment =
                        network.segments[
                            segmentIndex];

                    segment.active = false;

                    event.oxbowSegments.
                        push_back(
                            segment.id);
                }

                const RiverSegmentId cutoffId =
                    MakeSegmentId(
                        network.nodes[
                            upstream].
                            id,
                        network.nodes[
                            downstream].
                            id,
                        kRiverCutoffDomain);

                network.segments.push_back({
                    .id =
                        cutoffId,
                    .basin =
                        network.nodes[
                            upstream].
                            basin,
                    .upstreamNode =
                        upstream,
                    .downstreamNode =
                        downstream,
                    .active = true,
                    .cutoffSegment = true
                });

                event.newSegment =
                    cutoffId;

                network.cutoffEvents.
                    push_back(
                        std::move(
                            event));

                changed = true;
            }
        }
    }
}

[[nodiscard]] f64 SmoothStep01(
    const f64 value) noexcept
{
    const f64 t =
        std::clamp(
            value,
            0.0,
            1.0);

    return
        t * t *
        (3.0 - 2.0 * t);
}
} // namespace

bool RiverConstraint::IsValid() const noexcept
{
    const bool directionRequired =
        kind ==
        RiverConstraintKind::Trajectory;

    return
        id.IsValid() &&
        targetBasin.IsValid() &&
        std::isfinite(
            centerMeters.x) &&
        std::isfinite(
            centerMeters.y) &&
        std::isfinite(
            directionMeters.x) &&
        std::isfinite(
            directionMeters.y) &&
        (!directionRequired ||
         Length(
             directionMeters) >
             1.0e-12) &&
        std::isfinite(
            radiusMeters) &&
        radiusMeters > 0.0 &&
        std::isfinite(
            strength);
}

bool RiverNetworkConfig::IsValid() const noexcept
{
    const auto positive =
        [](const f64 value)
        {
            return
                std::isfinite(value) &&
                value > 0.0;
        };

    const auto nonnegative =
        [](const f64 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0;
        };

    return
        nonnegative(
            minimumDrainageAreaSquareMeters) &&
        nonnegative(
            minimumDischargeCubicMetersPerSecond) &&
        positive(
            referenceDischargeCubicMetersPerSecond) &&
        positive(
            baseChannelWidthMeters) &&
        positive(
            minimumChannelWidthMeters) &&
        positive(
            maximumChannelWidthMeters) &&
        minimumChannelWidthMeters <=
            maximumChannelWidthMeters &&
        nonnegative(
            widthDischargeExponent) &&
        positive(
            baseChannelDepthMeters) &&
        positive(
            minimumChannelDepthMeters) &&
        positive(
            maximumChannelDepthMeters) &&
        minimumChannelDepthMeters <=
            maximumChannelDepthMeters &&
        nonnegative(
            depthDischargeExponent) &&
        meanderIterations > 0U &&
        positive(
            meanderTimeStep) &&
        nonnegative(
            curvatureMigrationRate) &&
        nonnegative(
            deterministicSeedMigrationRate) &&
        positive(
            maximumCenterlineOffsetWidths) &&
        minimumCutoffPathNodes >=
            2U &&
        positive(
            cutoffDistanceWidths);
}

const RiverNetworkNode*
RiverNetwork::FindNode(
    const RiverNodeId id) const noexcept
{
    const auto it =
        std::find_if(
            nodes.begin(),
            nodes.end(),
            [id](const RiverNetworkNode& node)
            {
                return node.id == id;
            });

    return
        it != nodes.end()
            ? &*it
            : nullptr;
}

const RiverNetworkSegment*
RiverNetwork::FindSegment(
    const RiverSegmentId id) const noexcept
{
    const auto it =
        std::find_if(
            segments.begin(),
            segments.end(),
            [id](const RiverNetworkSegment& segment)
            {
                return
                    segment.id == id;
            });

    return
        it != segments.end()
            ? &*it
            : nullptr;
}

const RiverBasinState*
RiverNetwork::FindBasin(
    const RiverBasinId id) const noexcept
{
    const auto it =
        std::find_if(
            basins.begin(),
            basins.end(),
            [id](const RiverBasinState& basin)
            {
                return basin.id == id;
            });

    return
        it != basins.end()
            ? &*it
            : nullptr;
}

u64 RiverBasinConstraintRevision(
    const RiverBasinId basin,
    const std::span<const RiverConstraint> constraints) noexcept
{
    u64 value =
        0x4F52425642525631ULL; // "ORBVBRV1"

    value =
        terrain::StableCombine64(
            value,
            basin.high);

    value =
        terrain::StableCombine64(
            value,
            basin.low);

    for (const auto& constraint :
         constraints)
    {
        if (!constraint.enabled ||
            constraint.targetBasin !=
                basin)
        {
            continue;
        }

        value =
            terrain::StableCombine64(
                value,
                HashConstraint(
                    constraint));
    }

    return value;
}

RiverNetwork BuildRiverNetwork(
    const terrain_hydrology::DrainagePage& drainage,
    const std::span<const RiverConstraint> constraints,
    const RiverNetworkConfig& config)
{
    if (!config.IsValid() ||
        drainage.Resolution() == 0U ||
        drainage.SpacingMeters() <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit M16 river network configuration or drainage page is invalid.");
    }

    for (const auto& constraint :
         constraints)
    {
        if (!constraint.IsValid())
        {
            throw std::invalid_argument(
                "Orbit M16 river authoring contains an invalid constraint.");
        }
    }

    RiverNetwork result{
        .sourcePage =
            drainage.SourcePage(),
        .drainageRevision =
            drainage.Revision(),
        .resolution =
            drainage.Resolution(),
        .spacingMeters =
            drainage.SpacingMeters()
    };

    const u32 resolution =
        result.resolution;

    constexpr u32 invalidNode =
        std::numeric_limits<u32>::max();

    std::vector<u32> nodeForCell(
        static_cast<std::size_t>(
            resolution) *
            resolution,
        invalidNode);

    const auto ensureNode =
        [&](
            const u32 x,
            const u32 y) -> u32
        {
            const std::size_t cellIndex =
                Index(
                    resolution,
                    x,
                    y);

            if (nodeForCell[cellIndex] !=
                invalidNode)
            {
                return
                    nodeForCell[cellIndex];
            }

            const auto& source =
                drainage.At(
                    x,
                    y);

            const f64 discharge =
                std::max(
                    source.
                        dischargeCubicMetersPerSecond,
                    0.0);

            const f64 ratio =
                std::max(
                    discharge /
                        config.
                            referenceDischargeCubicMetersPerSecond,
                    1.0e-9);

            const f64 width =
                std::clamp(
                    config.
                        baseChannelWidthMeters *
                        std::pow(
                            ratio,
                            config.
                                widthDischargeExponent),
                    config.
                        minimumChannelWidthMeters,
                    config.
                        maximumChannelWidthMeters);

            const f64 depth =
                std::clamp(
                    config.
                        baseChannelDepthMeters *
                        std::pow(
                            ratio,
                            config.
                                depthDischargeExponent),
                    config.
                        minimumChannelDepthMeters,
                    config.
                        maximumChannelDepthMeters);

            const math::Double2 offset =
                CellOffset(
                    resolution,
                    result.spacingMeters,
                    x,
                    y);

            const RiverBasinId basin =
                ResolveBasin(
                    drainage,
                    x,
                    y);

            const u32 nodeIndex =
                static_cast<u32>(
                    result.nodes.size());

            result.nodes.push_back({
                .id =
                    MakeNodeId(
                        result.sourcePage.
                            address,
                        x,
                        y),
                .basin =
                    basin,
                .sourceX = x,
                .sourceY = y,
                .drainageOffsetMeters =
                    offset,
                .channelOffsetMeters =
                    offset,
                .surfaceHeightMeters =
                    source.
                        surfaceHeightMeters,
                .drainageElevationMeters =
                    source.
                        drainageElevationMeters,
                .drainageAreaSquareMeters =
                    source.
                        drainageAreaSquareMeters,
                .dischargeCubicMetersPerSecond =
                    discharge,
                .channelWidthMeters =
                    static_cast<f32>(
                        width),
                .channelDepthMeters =
                    static_cast<f32>(
                        depth),
                .drainageFlowDx =
                    source.flow.dx,
                .drainageFlowDy =
                    source.flow.dy,
                .exitsPage =
                    source.flow.exitsPage,
                .outlet =
                    source.outlet
            });

            nodeForCell[cellIndex] =
                nodeIndex;

            return nodeIndex;
        };

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& cell =
                drainage.At(
                    x,
                    y);

            if (!QualifiesAsRiver(
                    cell,
                    config))
            {
                continue;
            }

            static_cast<void>(
                ensureNode(
                    x,
                    y));

            if (!cell.flow.HasDownstream() ||
                cell.flow.exitsPage)
            {
                continue;
            }

            const i32 nx =
                static_cast<i32>(x) +
                cell.flow.dx;

            const i32 ny =
                static_cast<i32>(y) +
                cell.flow.dy;

            if (Inside(
                    nx,
                    ny,
                    resolution))
            {
                static_cast<void>(
                    ensureNode(
                        static_cast<u32>(nx),
                        static_cast<u32>(ny)));
            }
        }
    }

    for (u32 nodeIndex = 0U;
         nodeIndex <
             static_cast<u32>(
                 result.nodes.size());
         ++nodeIndex)
    {
        const auto& node =
            result.nodes[
                nodeIndex];

        const auto& cell =
            drainage.At(
                node.sourceX,
                node.sourceY);

        if (!cell.flow.HasDownstream())
        {
            continue;
        }

        const i32 nx =
            static_cast<i32>(
                node.sourceX) +
            cell.flow.dx;

        const i32 ny =
            static_cast<i32>(
                node.sourceY) +
            cell.flow.dy;

        if (cell.flow.exitsPage ||
            !Inside(
                nx,
                ny,
                resolution))
        {
            result.boundaryLinks.push_back({
                .upstreamNode =
                    node.id,
                .basin =
                    node.basin,
                .flowDx =
                    cell.flow.dx,
                .flowDy =
                    cell.flow.dy,
                .targetX = nx,
                .targetY = ny
            });

            continue;
        }

        const u32 downstream =
            nodeForCell[
                Index(
                    resolution,
                    static_cast<u32>(nx),
                    static_cast<u32>(ny))];

        if (downstream ==
            invalidNode)
        {
            continue;
        }

        const auto& downstreamNode =
            result.nodes[
                downstream];

        result.segments.push_back({
            .id =
                MakeSegmentId(
                    node.id,
                    downstreamNode.id),
            .basin =
                node.basin,
            .upstreamNode =
                nodeIndex,
            .downstreamNode =
                downstream
        });
    }

    for (const auto& node :
         result.nodes)
    {
        const auto existing =
            std::find_if(
                result.basins.begin(),
                result.basins.end(),
                [&node](
                    const RiverBasinState& basin)
                {
                    return
                        basin.id ==
                        node.basin;
                });

        if (existing ==
            result.basins.end())
        {
            const u64 constraintRevision =
                RiverBasinConstraintRevision(
                    node.basin,
                    constraints);

            u64 revision =
                terrain::StableCombine64(
                    drainage.Revision(),
                    HashRiverConfig(
                        config));

            revision =
                terrain::StableCombine64(
                    revision,
                    constraintRevision);

            result.basins.push_back({
                .id =
                    node.basin,
                .drainageRevision =
                    drainage.Revision(),
                .constraintRevision =
                    constraintRevision,
                .revision =
                    revision
            });
        }
    }

    EvolveMeanders(
        result,
        constraints,
        config);

    DetectCutoffs(
        result,
        config);

    RebuildBasinCounts(
        result);

    return result;
}

RiverSelection SelectNearestRiver(
    const RiverNetwork& network,
    const math::Double2& localMeters,
    const f64 maximumDistanceMeters)
{
    RiverSelection result{
        .distanceMeters =
            maximumDistanceMeters
    };

    if (!std::isfinite(
            localMeters.x) ||
        !std::isfinite(
            localMeters.y) ||
        !std::isfinite(
            maximumDistanceMeters) ||
        maximumDistanceMeters < 0.0)
    {
        return result;
    }

    f64 best =
        maximumDistanceMeters;

    for (const auto& node :
         network.nodes)
    {
        const f64 distance =
            Distance(
                localMeters,
                node.
                    channelOffsetMeters);

        if (distance <=
            best)
        {
            best = distance;

            result = {
                .node =
                    node.id,
                .basin =
                    node.basin,
                .distanceMeters =
                    distance
            };
        }
    }

    for (const auto& segment :
         network.segments)
    {
        if (!segment.active ||
            segment.upstreamNode >=
                network.nodes.size() ||
            segment.downstreamNode >=
                network.nodes.size())
        {
            continue;
        }

        const auto& a =
            network.nodes[
                segment.
                    upstreamNode];

        const auto& b =
            network.nodes[
                segment.
                    downstreamNode];

        const f64 distance =
            DistanceToSegment(
                localMeters,
                a.channelOffsetMeters,
                b.channelOffsetMeters);

        if (distance <
            best)
        {
            best = distance;

            result = {
                .segment =
                    segment.id,
                .basin =
                    segment.basin,
                .distanceMeters =
                    distance
            };
        }
    }

    return result;
}

bool RiverIncisionConfig::IsValid() const noexcept
{
    return
        std::isfinite(
            valleyWidthMultiplier) &&
        valleyWidthMultiplier >=
            1.0 &&
        std::isfinite(
            maximumIncisionMetersPerBake) &&
        maximumIncisionMetersPerBake >=
            0.0;
}

RiverIncisionResult ApplyRiverNetworkIncision(
    terrain_material_column::MaterialColumnPage& material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    SedimentExchangePage& sediment,
    const RiverNetwork& network,
    const RiverIncisionConfig& config)
{
    if (!config.IsValid() ||
        material.Resolution() !=
            network.resolution ||
        sediment.Resolution() !=
            network.resolution ||
        std::abs(
            material.SpacingMeters() -
            network.spacingMeters) >
            1.0e-9 ||
        std::abs(
            sediment.SpacingMeters() -
            network.spacingMeters) >
            1.0e-9)
    {
        throw std::invalid_argument(
            "Orbit M16 river incision inputs do not share one physical page.");
    }

    RiverIncisionResult result{};

    const f64 half =
        static_cast<f64>(
            network.resolution - 1U) *
        0.5;

    for (u32 y = 0U;
         y <
             network.resolution;
         ++y)
    {
        for (u32 x = 0U;
             x <
                 network.resolution;
             ++x)
        {
            const math::Double2 point{
                (static_cast<f64>(x) -
                 half) *
                    network.spacingMeters,
                (static_cast<f64>(y) -
                 half) *
                    network.spacingMeters
            };

            f64 desiredDepth = 0.0;

            for (const auto& segment :
                 network.segments)
            {
                if (!segment.active)
                {
                    continue;
                }

                const auto& a =
                    network.nodes[
                        segment.
                            upstreamNode];

                const auto& b =
                    network.nodes[
                        segment.
                            downstreamNode];

                f64 t = 0.0;

                const f64 distance =
                    DistanceToSegment(
                        point,
                        a.channelOffsetMeters,
                        b.channelOffsetMeters,
                        &t);

                const f64 width =
                    std::lerp(
                        static_cast<f64>(
                            a.channelWidthMeters),
                        static_cast<f64>(
                            b.channelWidthMeters),
                        t);

                const f64 depth =
                    std::lerp(
                        static_cast<f64>(
                            a.channelDepthMeters),
                        static_cast<f64>(
                            b.channelDepthMeters),
                        t);

                const f64 channelHalfWidth =
                    std::max(
                        width * 0.5,
                        0.5);

                const f64 valleyHalfWidth =
                    channelHalfWidth *
                    config.
                        valleyWidthMultiplier;

                if (distance >=
                    valleyHalfWidth)
                {
                    continue;
                }

                f64 profile = 1.0;

                if (distance >
                    channelHalfWidth)
                {
                    const f64 normalized =
                        (distance -
                         channelHalfWidth) /
                        std::max(
                            valleyHalfWidth -
                                channelHalfWidth,
                            1.0e-9);

                    profile =
                        1.0 -
                        SmoothStep01(
                            normalized);
                }

                desiredDepth =
                    std::max(
                        desiredDepth,
                        depth *
                            profile);
            }

            desiredDepth =
                std::min(
                    desiredDepth,
                    config.
                        maximumIncisionMetersPerBake);

            if (desiredDepth <=
                0.0)
            {
                continue;
            }

            const SedimentMass removed =
                sediment.PickupFromColumn(
                    material,
                    geology,
                    x,
                    y,
                    desiredDepth,
                    SedimentSourceProcess::
                        Hydraulic,
                    SedimentTransportMedium::
                        Waterborne);

            if (removed.Empty())
            {
                continue;
            }

            result.erodedMassKg +=
                removed.TotalKg();

            result.erodedDepthMeters +=
                desiredDepth;

            ++result.affectedCells;
        }
    }

    result.finalWaterborneSedimentKg =
        sediment.TotalMobileMass().
            TotalKg();

    return result;
}
} // namespace orbit::terrain_erosion
