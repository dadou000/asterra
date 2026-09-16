#include <orbit/path_routing/RoutePlanner.hpp>

#include <orbit/universe/ReferenceSurface.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace orbit::path_routing
{
namespace
{
[[nodiscard]] u64 Mix(
    u64 hash,
    const u64 value) noexcept
{
    hash ^=
        value +
        0x9e3779b97f4a7c15ULL +
        (hash << 6U) +
        (hash >> 2U);
    return hash;
}

[[nodiscard]] u64 HashString(
    const std::string_view text) noexcept
{
    u64 hash =
        0xcbf29ce484222325ULL;

    for (const unsigned char character :
         text)
    {
        hash ^=
            static_cast<u64>(
                character);
        hash *=
            0x100000001b3ULL;
    }

    return hash;
}

[[nodiscard]] u64 HashDouble(
    const f64 value) noexcept
{
    return std::bit_cast<u64>(value);
}

[[nodiscard]] u64 HashPoint(
    u64 hash,
    const math::Double3& value) noexcept
{
    hash = Mix(hash, HashDouble(value.x));
    hash = Mix(hash, HashDouble(value.y));
    hash = Mix(hash, HashDouble(value.z));
    return hash;
}

[[nodiscard]] u64 HashProfile(
    const paths::PathProfile& profile) noexcept
{
    u64 hash =
        Mix(
            0x7f4a7c159e3779b9ULL,
            HashString(profile.name));

    hash = Mix(
        hash,
        static_cast<u64>(
            profile.kind));
    hash = Mix(
        hash,
        HashDouble(
            profile.widthMeters));
    hash = Mix(
        hash,
        profile.lanes);
    hash = Mix(
        hash,
        HashDouble(
            profile.minimumRadiusMeters));
    hash = Mix(
        hash,
        HashDouble(
            profile.maximumGrade));
    hash = Mix(
        hash,
        profile.allowBridge ? 1U : 0U);
    hash = Mix(
        hash,
        profile.allowTunnel ? 1U : 0U);
    hash = Mix(
        hash,
        HashDouble(
            profile.terrainCutCost));
    hash = Mix(
        hash,
        HashDouble(
            profile.terrainFillCost));
    hash = Mix(
        hash,
        HashDouble(
            profile.waterCrossingCost));

    for (const std::string& field :
         profile.preferredCostFields)
    {
        hash =
            Mix(
                hash,
                HashString(field));
    }

    return hash;
}

[[nodiscard]] u64 HashSearch(
    const RouteSearchConfig& search) noexcept
{
    u64 hash =
        Mix(
            0xd6e8feb86659fd93ULL,
            HashDouble(
                search.spacingMeters));

    hash = Mix(
        hash,
        HashDouble(
            search.
                corridorHalfWidthMeters));
    hash = Mix(
        hash,
        search.maximumAlongSamples);
    hash = Mix(
        hash,
        search.maximumLateralSamples);
    hash = Mix(
        hash,
        search.maximumGridCells);
    return hash;
}

void ValidateSearch(
    const RouteSearchConfig& search)
{
    if (!std::isfinite(
            search.spacingMeters) ||
        search.spacingMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Route spacing must be positive and finite.");
    }

    if (!std::isfinite(
            search.corridorHalfWidthMeters) ||
        search.corridorHalfWidthMeters <
            0.0)
    {
        throw std::invalid_argument(
            "Route corridor width must be finite and non-negative.");
    }

    if (search.maximumAlongSamples < 2 ||
        search.maximumLateralSamples < 1 ||
        search.maximumGridCells < 2)
    {
        throw std::invalid_argument(
            "Route grid limits are too small.");
    }
}

[[nodiscard]] std::optional<
    frames::FramePoint>
NativeAnchor(
    const paths::PathAnchor& anchor,
    const universe::BodyRegistry& bodies,
    const paths::EntitySocketResolver&
        entityResolver)
{
    return std::visit(
        [&](const auto& value)
            -> std::optional<
                frames::FramePoint>
        {
            using Anchor =
                std::decay_t<
                    decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Anchor,
                    paths::FramePointAnchor>)
            {
                return frames::FramePoint{
                    .frame = value.frame,
                    .localMeters =
                        value.localMeters
                };
            }
            else if constexpr (
                std::is_same_v<
                    Anchor,
                    paths::SurfaceAnchor>)
            {
                const auto* body =
                    bodies.FindBody(
                        value.body);

                if (body == nullptr)
                {
                    return std::nullopt;
                }

                return frames::FramePoint{
                    .frame = body->frame,
                    .localMeters =
                        universe::
                            ReferenceSurfacePoint(
                                body->shape,
                                {
                                    .latitudeRadians =
                                        value.
                                            coordinate.
                                            x,
                                    .longitudeRadians =
                                        value.
                                            coordinate.
                                            y,
                                    .offsetMeters =
                                        value.
                                            coordinate.
                                            z
                                })
                };
            }
            else
            {
                if (!entityResolver)
                {
                    return std::nullopt;
                }

                return entityResolver(
                    value.entity,
                    value.socket,
                    value.localMeters);
            }
        },
        anchor);
}

struct GridPoint
{
    bool evaluated{false};
    bool valid{false};
    RouteProjectedPoint projected{};
    f64 externalCostPerMeter{0.0};
};

struct OpenNode
{
    f64 score{0.0};
    u32 index{0};

    [[nodiscard]] bool operator<(
        const OpenNode& other)
        const noexcept
    {
        return score >
            other.score;
    }
};

[[nodiscard]] f64 TurnRadius(
    const math::Double3& previous,
    const math::Double3& current,
    const math::Double3& next)
    noexcept
{
    const math::Double3 incoming =
        current - previous;
    const math::Double3 outgoing =
        next - current;

    const f64 incomingLength =
        math::Length(incoming);
    const f64 outgoingLength =
        math::Length(outgoing);

    if (incomingLength <= 1.0e-9 ||
        outgoingLength <= 1.0e-9)
    {
        return std::numeric_limits<f64>::
            infinity();
    }

    const f64 cosine =
        std::clamp(
            math::Dot(
                incoming / incomingLength,
                outgoing / outgoingLength),
            -1.0,
            1.0);

    const f64 angle =
        std::acos(cosine);

    if (angle <= 1.0e-6)
    {
        return std::numeric_limits<f64>::
            infinity();
    }

    const f64 sine =
        std::sin(angle * 0.5);

    if (sine <= 1.0e-9)
    {
        return std::numeric_limits<f64>::
            infinity();
    }

    return
        std::min(
            incomingLength,
            outgoingLength) /
        (2.0 * sine);
}
} // namespace

struct RoutePlanner::ResolvedRequest
{
    scene::ObjectId edge{};
    frames::FrameId frame{};
    math::Double3 start{};
    math::Double3 end{};
    paths::PathProfile profile{};
    u64 profileRevision{1};
    RouteEnvironment environment{};
    bool crossFrameEndpoints{false};
    u64 semanticHash{0};
};

struct RoutePlanner::PendingBuild
{
    std::mutex mutex;
    u64 generation{0};
    u64 signature{0};
    std::optional<RouteResult> result;
    std::string error;
};

struct RoutePlanner::Entry
{
    RouteState state{RouteState::Missing};
    u64 generation{0};
    u64 committedRevision{0};
    u64 manualRevision{0};
    u64 desiredSignature{0};
    bool crossFrameEndpoints{false};
    std::optional<ResolvedRequest> desired;
    std::optional<RouteResult> result;
    std::string error;
    std::shared_ptr<PendingBuild> pending;
    std::unique_ptr<jobs::JobGroup> jobGroup;
};

class RoutePlanner::Impl
{
public:
    std::unordered_map<
        scene::ObjectId,
        Entry>
        entries;
};

namespace
{
[[nodiscard]] RouteResult SolveRoute(
    const RoutePlanner::ResolvedRequest&
        request,
    const u64 generation,
    const u64 signature)
{
    const math::Double3 line =
        request.end -
        request.start;
    const f64 distance =
        math::Length(line);

    RouteProjector projector =
        request.environment.projector;

    if (!projector)
    {
        projector =
            [](const math::Double3& point,
               const f64)
                -> std::optional<
                    RouteProjectedPoint>
            {
                return RouteProjectedPoint{
                    .localMeters = point
                };
            };
    }

    const f64 footprint =
        std::max(
            request.profile.widthMeters,
            request.environment.search.
                spacingMeters);

    if (distance <= 1.0e-9)
    {
        const auto point =
            projector(
                request.start,
                footprint);

        if (!point.has_value())
        {
            throw std::runtime_error(
                "Route domain rejected the coincident endpoint.");
        }

        return RouteResult{
            .edge = request.edge,
            .frame = request.frame,
            .points = {
                {
                    .localMeters =
                        point->localMeters,
                    .elevationMeters =
                        point->elevationMeters,
                    .waterDepthMeters =
                        point->waterDepthMeters
                }
            },
            .totalCost = 0.0,
            .dependencySignature =
                signature,
            .generation = generation,
            .crossFrameEndpoints =
                request.crossFrameEndpoints
        };
    }

    const math::Double3 forward =
        line / distance;

    math::Double3 referenceUp{
        0.0,
        1.0,
        0.0
    };

    if (std::abs(
            math::Dot(
                forward,
                referenceUp)) >
        0.95)
    {
        referenceUp = {
            1.0,
            0.0,
            0.0
        };
    }

    const math::Double3 lateral =
        math::Normalize(
            math::Cross(
                forward,
                referenceUp));

    if (math::LengthSquared(lateral) <=
        1.0e-20)
    {
        throw std::runtime_error(
            "Route corridor basis is degenerate.");
    }

    const RouteSearchConfig& search =
        request.environment.search;

    const f64 effectiveSpacing =
        std::max(
            search.spacingMeters,
            distance /
                static_cast<f64>(
                    search.
                        maximumAlongSamples -
                    1U));

    u32 alongCount =
        static_cast<u32>(
            std::ceil(
                distance /
                effectiveSpacing)) +
        1U;

    alongCount =
        std::clamp(
            alongCount,
            2U,
            search.maximumAlongSamples);

    u32 lateralSteps =
        static_cast<u32>(
            std::floor(
                search.
                    corridorHalfWidthMeters /
                effectiveSpacing));

    const u32 maximumLateralSteps =
        (search.maximumLateralSamples -
         1U) /
        2U;

    lateralSteps =
        std::min(
            lateralSteps,
            maximumLateralSteps);

    if (alongCount >
        search.maximumGridCells)
    {
        throw std::runtime_error(
            "Route corridor exceeds maximum grid cells.");
    }

    const u32 maximumByCellBudget =
        std::max(
            1U,
            search.maximumGridCells /
                alongCount);

    u32 lateralCount =
        lateralSteps * 2U +
        1U;

    lateralCount =
        std::min(
            lateralCount,
            maximumByCellBudget);

    if ((lateralCount & 1U) == 0U)
    {
        --lateralCount;
    }

    lateralCount =
        std::max(
            lateralCount,
            1U);

    const u32 centerY =
        lateralCount / 2U;
    const u32 cellCount =
        alongCount *
        lateralCount;

    std::vector<GridPoint> grid(
        cellCount);

    const auto indexOf =
        [lateralCount](
            const u32 x,
            const u32 y)
        {
            return x *
                lateralCount +
                y;
        };

    const auto coordinates =
        [lateralCount](
            const u32 index)
        {
            return std::pair<u32, u32>{
                index / lateralCount,
                index % lateralCount
            };
        };

    const auto rawCandidate =
        [&](const u32 x,
            const u32 y)
        {
            const f64 fraction =
                static_cast<f64>(x) /
                static_cast<f64>(
                    alongCount - 1U);

            const f64 side =
                (static_cast<f64>(y) -
                 static_cast<f64>(
                     centerY)) *
                effectiveSpacing;

            return
                request.start +
                line * fraction +
                lateral * side;
        };

    const auto evaluateCell =
        [&](const u32 index)
            -> GridPoint&
        {
            GridPoint& cell =
                grid[index];

            if (cell.evaluated)
            {
                return cell;
            }

            cell.evaluated = true;

            const auto [x, y] =
                coordinates(index);

            const auto projected =
                projector(
                    rawCandidate(x, y),
                    footprint);

            if (!projected.has_value() ||
                !std::isfinite(
                    projected->
                        localMeters.x) ||
                !std::isfinite(
                    projected->
                        localMeters.y) ||
                !std::isfinite(
                    projected->
                        localMeters.z))
            {
                return cell;
            }

            f64 externalCost = 0.0;

            for (const RouteCostSource& source :
                 request.environment.costSources)
            {
                if (!source.evaluateCostPerMeter)
                {
                    continue;
                }

                const auto sampled =
                    source.evaluateCostPerMeter(
                        *projected,
                        footprint);

                if (!sampled.has_value() ||
                    !std::isfinite(*sampled) ||
                    *sampled < 0.0)
                {
                    return cell;
                }

                externalCost +=
                    *sampled;
            }

            cell.valid = true;
            cell.projected =
                *projected;
            cell.externalCostPerMeter =
                externalCost;
            return cell;
        };

    const u32 startIndex =
        indexOf(
            0U,
            centerY);
    const u32 endIndex =
        indexOf(
            alongCount - 1U,
            centerY);

    if (!evaluateCell(startIndex).valid ||
        !evaluateCell(endIndex).valid)
    {
        throw std::runtime_error(
            "Route domain rejected one or both endpoints.");
    }

    const f64 infinity =
        std::numeric_limits<f64>::
            infinity();

    std::vector<f64> cost(
        cellCount,
        infinity);
    std::vector<i64> parent(
        cellCount,
        -1);
    std::vector<bool> closed(
        cellCount,
        false);

    std::priority_queue<OpenNode>
        open;

    cost[startIndex] = 0.0;

    open.push({
        .score =
            math::Length(
                evaluateCell(startIndex).
                    projected.
                    localMeters -
                evaluateCell(endIndex).
                    projected.
                    localMeters),
        .index = startIndex
    });

    constexpr i32 kOffsets[8][2]{
        {-1, -1},
        {-1,  0},
        {-1,  1},
        { 0, -1},
        { 0,  1},
        { 1, -1},
        { 1,  0},
        { 1,  1}
    };

    while (!open.empty())
    {
        const u32 current =
            open.top().index;
        open.pop();

        if (closed[current])
        {
            continue;
        }

        closed[current] = true;

        if (current == endIndex)
        {
            break;
        }

        const auto [currentX, currentY] =
            coordinates(current);

        GridPoint& currentPoint =
            evaluateCell(current);

        if (!currentPoint.valid)
        {
            continue;
        }

        for (const auto& offset :
             kOffsets)
        {
            const i32 nextX =
                static_cast<i32>(
                    currentX) +
                offset[0];
            const i32 nextY =
                static_cast<i32>(
                    currentY) +
                offset[1];

            if (nextX < 0 ||
                nextY < 0 ||
                nextX >=
                    static_cast<i32>(
                        alongCount) ||
                nextY >=
                    static_cast<i32>(
                        lateralCount))
            {
                continue;
            }

            const u32 next =
                indexOf(
                    static_cast<u32>(
                        nextX),
                    static_cast<u32>(
                        nextY));

            if (closed[next])
            {
                continue;
            }

            GridPoint& nextPoint =
                evaluateCell(next);

            if (!nextPoint.valid)
            {
                continue;
            }

            const math::Double3 delta =
                nextPoint.
                    projected.
                    localMeters -
                currentPoint.
                    projected.
                    localMeters;

            const f64 segmentLength =
                math::Length(delta);

            if (segmentLength <= 1.0e-9)
            {
                continue;
            }

            const f64 elevationDelta =
                nextPoint.
                    projected.
                    elevationMeters -
                currentPoint.
                    projected.
                    elevationMeters;

            const f64 grade =
                std::abs(
                    elevationDelta) /
                segmentLength;

            f64 stepCost =
                segmentLength;

            if (grade >
                request.profile.maximumGrade)
            {
                if (!request.profile.
                        allowBridge &&
                    !request.profile.
                        allowTunnel)
                {
                    continue;
                }

                const f64 excess =
                    grade -
                    request.profile.
                        maximumGrade;

                stepCost +=
                    excess *
                    segmentLength *
                    (1.0 +
                     request.profile.
                         terrainCutCost +
                     request.profile.
                         terrainFillCost) *
                    20.0;
            }

            stepCost +=
                std::abs(
                    elevationDelta) *
                0.5 *
                (request.profile.
                     terrainCutCost +
                 request.profile.
                     terrainFillCost);

            const f64 waterDepth =
                std::max(
                    currentPoint.
                        projected.
                        waterDepthMeters,
                    nextPoint.
                        projected.
                        waterDepthMeters);

            if (waterDepth > 1.0e-3)
            {
                if (!request.profile.
                        allowBridge &&
                    !request.profile.
                        allowTunnel)
                {
                    continue;
                }

                const f64 structureFactor =
                    request.profile.
                            allowBridge
                        ? 1.0
                        : 2.0;

                stepCost +=
                    segmentLength *
                    request.profile.
                        waterCrossingCost *
                    structureFactor *
                    (1.0 +
                     std::min(
                         waterDepth,
                         100.0));
            }

            stepCost +=
                segmentLength *
                0.5 *
                (currentPoint.
                     externalCostPerMeter +
                 nextPoint.
                     externalCostPerMeter);

            if (request.profile.
                    minimumRadiusMeters >
                    0.0 &&
                parent[current] >= 0)
            {
                const GridPoint& previous =
                    evaluateCell(
                        static_cast<u32>(
                            parent[current]));

                const f64 radius =
                    TurnRadius(
                        previous.projected.
                            localMeters,
                        currentPoint.
                            projected.
                            localMeters,
                        nextPoint.
                            projected.
                            localMeters);

                if (radius <
                    request.profile.
                        minimumRadiusMeters)
                {
                    continue;
                }
            }

            const f64 candidate =
                cost[current] +
                stepCost;

            if (candidate >=
                cost[next])
            {
                continue;
            }

            cost[next] =
                candidate;
            parent[next] =
                static_cast<i64>(
                    current);

            const f64 heuristic =
                math::Length(
                    nextPoint.
                        projected.
                        localMeters -
                    evaluateCell(endIndex).
                        projected.
                        localMeters);

            open.push({
                .score =
                    candidate +
                    heuristic,
                .index = next
            });
        }
    }

    if (!std::isfinite(
            cost[endIndex]))
    {
        throw std::runtime_error(
            "No valid routed path exists inside the configured corridor.");
    }

    std::vector<RoutePoint> reversed;

    for (i64 cursor =
             static_cast<i64>(
                 endIndex);
         cursor >= 0;)
    {
        const GridPoint& point =
            evaluateCell(
                static_cast<u32>(
                    cursor));

        reversed.push_back({
            .localMeters =
                point.projected.
                    localMeters,
            .elevationMeters =
                point.projected.
                    elevationMeters,
            .waterDepthMeters =
                point.projected.
                    waterDepthMeters
        });

        if (cursor ==
            static_cast<i64>(
                startIndex))
        {
            break;
        }

        cursor =
            parent[
                static_cast<u32>(
                    cursor)];

        if (cursor < 0)
        {
            throw std::runtime_error(
                "Routed path parent chain is incomplete.");
        }
    }

    std::ranges::reverse(
        reversed);

    return RouteResult{
        .edge = request.edge,
        .frame = request.frame,
        .points = std::move(reversed),
        .totalCost =
            cost[endIndex],
        .dependencySignature =
            signature,
        .generation = generation,
        .crossFrameEndpoints =
            request.crossFrameEndpoints
    };
}
} // namespace

RoutePlanner::RoutePlanner(
    jobs::JobSystem& jobs,
    const frames::FrameGraph& frames,
    const universe::BodyRegistry& bodies)
    : jobs_(jobs),
      frames_(frames),
      bodies_(bodies),
      impl_(std::make_unique<Impl>())
{
}

RoutePlanner::~RoutePlanner() = default;

RoutePlanner::ResolvedRequest
RoutePlanner::Resolve(
    RoutePlanRequest request) const
{
    if (request.edge.mode !=
        paths::EdgeMode::Routed)
    {
        throw std::invalid_argument(
            "RoutePlanner requires a routed PathEdge.");
    }

    if (request.edge.startNode !=
            request.startNode.id ||
        request.edge.endNode !=
            request.endNode.id)
    {
        throw std::invalid_argument(
            "Route request node records do not match the edge endpoints.");
    }

    ValidateSearch(
        request.environment.search);

    const auto nativeStart =
        NativeAnchor(
            request.startNode.anchor,
            bodies_,
            request.entityResolver);
    const auto nativeEnd =
        NativeAnchor(
            request.endNode.anchor,
            bodies_,
            request.entityResolver);

    if (!nativeStart.has_value() ||
        !nativeEnd.has_value())
    {
        throw std::runtime_error(
            "Route endpoint anchor could not be resolved.");
    }

    const frames::FrameId routeFrame =
        request.environment.frame
            ? request.environment.frame
            : nativeStart->frame;

    if (!frames_.Contains(routeFrame))
    {
        throw std::invalid_argument(
            "Route environment frame does not exist.");
    }

    const auto start =
        frames_.TransformPoint(
            *nativeStart,
            routeFrame,
            request.atTime);
    const auto end =
        frames_.TransformPoint(
            *nativeEnd,
            routeFrame,
            request.atTime);

    if (!start.has_value() ||
        !end.has_value())
    {
        throw std::runtime_error(
            "Route endpoints do not share a connected frame graph.");
    }

    u64 semanticHash =
        Mix(
            0x6a09e667f3bcc909ULL,
            request.edge.id.high);

    semanticHash =
        Mix(
            semanticHash,
            request.edge.id.low);
    semanticHash =
        Mix(
            semanticHash,
            routeFrame.high);
    semanticHash =
        Mix(
            semanticHash,
            routeFrame.low);
    semanticHash =
        HashPoint(
            semanticHash,
            start->localMeters);
    semanticHash =
        HashPoint(
            semanticHash,
            end->localMeters);
    semanticHash =
        Mix(
            semanticHash,
            HashProfile(
                request.profile));
    semanticHash =
        Mix(
            semanticHash,
            HashSearch(
                request.environment.
                    search));
    semanticHash =
        Mix(
            semanticHash,
            HashString(
                request.environment.
                    domainKey));

    return ResolvedRequest{
        .edge = request.edge.id,
        .frame = routeFrame,
        .start = start->localMeters,
        .end = end->localMeters,
        .profile =
            std::move(
                request.profile),
        .profileRevision =
            request.profileRevision,
        .environment =
            std::move(
                request.environment),
        .crossFrameEndpoints =
            nativeStart->frame !=
            nativeEnd->frame,
        .semanticHash =
            semanticHash
    };
}

u64 RoutePlanner::CurrentSignature(
    const ResolvedRequest& request,
    const u64 manualRevision) const
{
    u64 hash =
        Mix(
            request.semanticHash,
            request.profileRevision);

    hash =
        Mix(
            hash,
            manualRevision);

    if (request.environment.
            domainRevision)
    {
        hash =
            Mix(
                hash,
                request.environment.
                    domainRevision());
    }

    for (const RouteCostSource& source :
         request.environment.costSources)
    {
        hash =
            Mix(
                hash,
                HashString(source.key));

        hash =
            Mix(
                hash,
                source.revision
                    ? source.revision()
                    : 0U);
    }

    return hash;
}

bool RoutePlanner::Request(
    RoutePlanRequest request)
{
    ResolvedRequest resolved =
        Resolve(
            std::move(request));

    Entry& entry =
        impl_->entries[
            resolved.edge];

    const u64 signature =
        CurrentSignature(
            resolved,
            entry.manualRevision);

    const bool changed =
        !entry.desired.has_value() ||
        entry.desiredSignature !=
            signature;

    if (!changed)
    {
        return false;
    }

    entry.desired =
        std::move(resolved);
    entry.desiredSignature =
        signature;
    entry.crossFrameEndpoints =
        entry.desired->
            crossFrameEndpoints;
    ++entry.generation;
    entry.error.clear();

    if (entry.state !=
        RouteState::Building)
    {
        entry.state =
            RouteState::Dirty;
        Schedule(
            entry.desired->edge);
    }

    return true;
}

void RoutePlanner::RefreshDependencies()
{
    for (auto& [edge, entry] :
         impl_->entries)
    {
        static_cast<void>(edge);

        if (!entry.desired.has_value())
        {
            continue;
        }

        const u64 signature =
            CurrentSignature(
                *entry.desired,
                entry.manualRevision);

        if (signature ==
            entry.desiredSignature)
        {
            continue;
        }

        entry.desiredSignature =
            signature;
        ++entry.generation;
        entry.error.clear();

        if (entry.state !=
            RouteState::Building)
        {
            entry.state =
                RouteState::Dirty;
        }
    }
}

void RoutePlanner::FinalizeCompleted()
{
    for (auto& [edge, entry] :
         impl_->entries)
    {
        static_cast<void>(edge);

        if (entry.state !=
                RouteState::Building ||
            entry.jobGroup == nullptr ||
            !entry.jobGroup->
                IsComplete())
        {
            continue;
        }

        std::optional<RouteResult>
            result;
        std::string error;
        u64 generation = 0;
        u64 signature = 0;

        {
            std::scoped_lock lock(
                entry.pending->mutex);

            result =
                std::move(
                    entry.pending->
                        result);
            error =
                std::move(
                    entry.pending->
                        error);
            generation =
                entry.pending->
                    generation;
            signature =
                entry.pending->
                    signature;
        }

        entry.pending.reset();
        entry.jobGroup.reset();

        const bool stale =
            generation !=
                entry.generation ||
            signature !=
                entry.desiredSignature;

        if (stale)
        {
            entry.state =
                RouteState::Dirty;
            continue;
        }

        if (!error.empty() ||
            !result.has_value())
        {
            entry.state =
                RouteState::Failed;
            entry.error =
                error.empty()
                    ? "Route job returned no result."
                    : std::move(error);
            continue;
        }

        entry.result =
            std::move(result);
        ++entry.committedRevision;
        entry.state =
            RouteState::Ready;
        entry.error.clear();
    }
}

void RoutePlanner::Schedule(
    const scene::ObjectId edge)
{
    auto found =
        impl_->entries.find(edge);

    if (found ==
            impl_->entries.end() ||
        !found->second.desired.
            has_value())
    {
        return;
    }

    Entry& entry =
        found->second;

    if (entry.state ==
        RouteState::Building)
    {
        return;
    }

    const ResolvedRequest request =
        *entry.desired;
    const u64 generation =
        entry.generation;
    const u64 signature =
        entry.desiredSignature;

    auto pending =
        std::make_shared<
            PendingBuild>();

    pending->generation =
        generation;
    pending->signature =
        signature;

    entry.pending =
        pending;
    entry.jobGroup =
        std::make_unique<
            jobs::JobGroup>();
    entry.state =
        RouteState::Building;
    entry.error.clear();

    jobs_.Submit(
        *entry.jobGroup,
        jobs::JobPriority::Normal,
        [pending,
         request,
         generation,
         signature]
        {
            std::optional<RouteResult>
                result;
            std::string error;

            try
            {
                result =
                    SolveRoute(
                        request,
                        generation,
                        signature);
            }
            catch (const std::exception&
                       exception)
            {
                error =
                    exception.what();
            }
            catch (...)
            {
                error =
                    "Unknown route planning error.";
            }

            std::scoped_lock lock(
                pending->mutex);

            pending->result =
                std::move(result);
            pending->error =
                std::move(error);
        });
}

void RoutePlanner::Poll()
{
    RefreshDependencies();
    FinalizeCompleted();

    for (auto& [edge, entry] :
         impl_->entries)
    {
        if (entry.state ==
            RouteState::Dirty)
        {
            Schedule(edge);
        }
    }
}

void RoutePlanner::Invalidate(
    const scene::ObjectId edge)
{
    auto found =
        impl_->entries.find(edge);

    if (found ==
        impl_->entries.end())
    {
        return;
    }

    Entry& entry =
        found->second;

    ++entry.manualRevision;
    ++entry.generation;

    if (entry.desired.has_value())
    {
        entry.desiredSignature =
            CurrentSignature(
                *entry.desired,
                entry.manualRevision);
    }

    entry.error.clear();

    if (entry.state !=
        RouteState::Building)
    {
        entry.state =
            RouteState::Dirty;
    }
}

void RoutePlanner::Erase(
    const scene::ObjectId edge) noexcept
{
    impl_->entries.erase(edge);
}

std::optional<RouteStatus>
RoutePlanner::Status(
    const scene::ObjectId edge) const
{
    const auto found =
        impl_->entries.find(edge);

    if (found ==
        impl_->entries.end())
    {
        return std::nullopt;
    }

    const Entry& entry =
        found->second;

    return RouteStatus{
        .state = entry.state,
        .generation =
            entry.generation,
        .committedRevision =
            entry.committedRevision,
        .dependencySignature =
            entry.desiredSignature,
        .crossFrameEndpoints =
            entry.crossFrameEndpoints,
        .error = entry.error
    };
}

const RouteResult*
RoutePlanner::Result(
    const scene::ObjectId edge) const
    noexcept
{
    const auto found =
        impl_->entries.find(edge);

    if (found ==
            impl_->entries.end() ||
        !found->second.result.
            has_value())
    {
        return nullptr;
    }

    return &*found->second.result;
}

bool RoutePlanner::BuildBlocking(
    RoutePlanRequest request)
{
    const scene::ObjectId edge =
        request.edge.id;

    static_cast<void>(
        Request(
            std::move(request)));

    while (true)
    {
        Poll();

        const auto status =
            Status(edge);

        if (!status.has_value())
        {
            return false;
        }

        if (status->state ==
            RouteState::Ready)
        {
            return true;
        }

        if (status->state ==
            RouteState::Failed)
        {
            return false;
        }

        jobs_.WaitIdle();
    }
}
} // namespace orbit::path_routing
