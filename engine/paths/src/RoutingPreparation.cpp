#include <orbit/paths/Routing.hpp>

#include <utility>

namespace orbit::paths
{
namespace
{
[[nodiscard]] u64 MixRevision(
    u64 state,
    const u64 value) noexcept
{
    state ^= value +
        0x9e3779b97f4a7c15ULL +
        (state << 6U) +
        (state >> 2U);
    return state;
}
} // namespace

u64 RouteDependencySignature(
    const RouteDependencyRevisions& revisions) noexcept
{
    u64 result =
        0x4f52424954524f55ULL;
    result = MixRevision(
        result,
        revisions.terrain);
    result = MixRevision(
        result,
        revisions.profile);
    result = MixRevision(
        result,
        revisions.costFields);
    result = MixRevision(
        result,
        revisions.endpoints);
    return result;
}

std::optional<RouteSolveRequest>
PrepareRouteSolveRequest(
    const PathNetworkService& paths,
    const scene::ObjectId edge,
    const frames::FrameId targetFrame,
    const time::SimulationTime atTime,
    const frames::FrameGraph& frameGraph,
    const universe::BodyRegistry& bodies,
    PathProfile profile,
    RouteCostSource costSource,
    const RouteDependencyRevisions dependencies,
    const f64 cellSizeMeters,
    const f64 corridorHalfWidthMeters,
    const EntitySocketResolver& entityResolver,
    std::string* failureReason)
{
    const auto fail =
        [failureReason](std::string message)
            -> std::optional<RouteSolveRequest>
        {
            if (failureReason != nullptr)
            {
                *failureReason =
                    std::move(message);
            }
            return std::nullopt;
        };

    if (!targetFrame ||
        !frameGraph.Contains(targetFrame))
    {
        return fail(
            "Routed edge target frame is not available.");
    }

    const auto edgeRecord =
        paths.FindEdge(edge);

    if (!edgeRecord.has_value())
    {
        return fail(
            "Routed edge does not exist.");
    }

    if (edgeRecord->mode !=
        EdgeMode::Routed)
    {
        return fail(
            "Route preparation requires an edge in Routed mode.");
    }

    const auto startNode =
        paths.FindNode(
            edgeRecord->startNode);
    const auto endNode =
        paths.FindNode(
            edgeRecord->endNode);

    if (!startNode.has_value() ||
        !endNode.has_value())
    {
        return fail(
            "Routed edge references a missing path node.");
    }

    const auto start =
        ResolveAnchor(
            startNode->anchor,
            targetFrame,
            atTime,
            frameGraph,
            bodies,
            entityResolver);
    const auto end =
        ResolveAnchor(
            endNode->anchor,
            targetFrame,
            atTime,
            frameGraph,
            bodies,
            entityResolver);

    if (!start.has_value() ||
        !end.has_value())
    {
        return fail(
            "Routed edge endpoint could not be resolved into the solve frame.");
    }

    if (!costSource)
    {
        return fail(
            "Routed edge has no cost source.");
    }

    return RouteSolveRequest{
        .start = start->localMeters,
        .end = end->localMeters,
        .up = {0.0, 1.0, 0.0},
        .profile = std::move(profile),
        .cellSizeMeters = cellSizeMeters,
        .corridorHalfWidthMeters =
            corridorHalfWidthMeters,
        .dependencyRevision =
            RouteDependencySignature(
                dependencies),
        .costSource =
            std::move(costSource)
    };
}
} // namespace orbit::paths
