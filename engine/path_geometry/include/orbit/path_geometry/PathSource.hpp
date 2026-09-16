#pragma once

#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>
#include <orbit/paths/PathEvaluation.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/time/SimulationTime.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <functional>
#include <optional>
#include <string>

namespace orbit::path_geometry
{
using PathUpResolver =
    std::function<math::Double3(
        const math::Double3& position)>;

struct PathSourceRequest
{
    paths::PathEdgeRecord edge;
    paths::PathNodeRecord startNode;
    paths::PathNodeRecord endNode;

    frames::FrameId targetFrame{};
    time::SimulationTime atTime{};
    const frames::FrameGraph* frames{nullptr};
    const universe::BodyRegistry* bodies{nullptr};

    // Required only for EdgeMode::Routed. The routed product remains derived
    // input; it is never copied into semantic path authority.
    const path_routing::RouteResult* routed{nullptr};

    paths::EntitySocketResolver entityResolver;
    PathUpResolver upResolver;

    f64 curveSampleSpacingMeters{4.0};
    u64 sourceRevision{0};
};

[[nodiscard]] std::optional<PathCenterline>
BuildPathCenterline(
    const PathSourceRequest& request,
    std::string* failureReason = nullptr);
} // namespace orbit::path_geometry
