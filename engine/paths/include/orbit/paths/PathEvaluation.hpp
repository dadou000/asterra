#pragma once

#include <orbit/frames/FrameGraph.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/time/SimulationTime.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <functional>
#include <optional>
#include <string_view>

namespace orbit::paths
{
using EntitySocketResolver =
    std::function<std::optional<frames::FramePoint>(
        scene::ObjectId entity,
        std::string_view socket,
        math::Double3 localMeters)>;

// Resolves an authored semantic anchor into targetFrame at an explicit
// simulation time. Surface anchors use the body's reference shape/frame;
// entity/socket anchors are delegated to the owning runtime/entity system.
[[nodiscard]] std::optional<frames::FramePoint>
ResolveAnchor(
    const PathAnchor& anchor,
    frames::FrameId targetFrame,
    time::SimulationTime atTime,
    const frames::FrameGraph& frames,
    const universe::BodyRegistry& bodies,
    const EntitySocketResolver& entityResolver = {});

[[nodiscard]] math::Double3 EvaluateDirect(
    const math::Double3& start,
    const math::Double3& end,
    f64 t) noexcept;

// Handles are endpoint-relative vectors in the edge evaluation frame.
[[nodiscard]] math::Double3 EvaluateBezier(
    const math::Double3& start,
    const math::Double3& end,
    const math::Double3& startHandle,
    const math::Double3& endHandle,
    f64 t) noexcept;

[[nodiscard]] math::Double3 EvaluateBezierTangent(
    const math::Double3& start,
    const math::Double3& end,
    const math::Double3& startHandle,
    const math::Double3& endHandle,
    f64 t) noexcept;

[[nodiscard]] f64 EvaluateBezierCurvature(
    const math::Double3& start,
    const math::Double3& end,
    const math::Double3& startHandle,
    const math::Double3& endHandle,
    f64 t) noexcept;
} // namespace orbit::paths
