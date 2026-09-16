#include <orbit/paths/PathEvaluation.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace orbit::paths
{
namespace
{
[[nodiscard]] math::Double3 SurfacePoint(
    const universe::BodyShape& shape,
    const math::Double3& coordinate) noexcept
{
    const f64 latitude = coordinate.x;
    const f64 longitude = coordinate.y;
    const f64 offset = coordinate.z;

    universe::EllipsoidShape ellipsoid =
        std::visit(
            [](const auto& value)
            {
                using Shape =
                    std::decay_t<decltype(value)>;

                if constexpr (
                    std::is_same_v<
                        Shape,
                        universe::SphereShape>)
                {
                    return universe::EllipsoidShape{
                        .radiiMeters = {
                            value.radiusMeters,
                            value.radiusMeters,
                            value.radiusMeters
                        }
                    };
                }
                else
                {
                    return value;
                }
            },
            shape);

    const f64 cosLatitude =
        std::cos(latitude);
    const f64 sinLatitude =
        std::sin(latitude);
    const f64 cosLongitude =
        std::cos(longitude);
    const f64 sinLongitude =
        std::sin(longitude);

    math::Double3 point{
        ellipsoid.radiiMeters.x *
            cosLatitude * cosLongitude,
        ellipsoid.radiiMeters.y *
            sinLatitude,
        ellipsoid.radiiMeters.z *
            cosLatitude * sinLongitude
    };

    math::Double3 normal{
        point.x /
            (ellipsoid.radiiMeters.x *
             ellipsoid.radiiMeters.x),
        point.y /
            (ellipsoid.radiiMeters.y *
             ellipsoid.radiiMeters.y),
        point.z /
            (ellipsoid.radiiMeters.z *
             ellipsoid.radiiMeters.z)
    };

    normal = math::Normalize(normal);
    return point + normal * offset;
}

[[nodiscard]] f64 ClampT(
    const f64 t) noexcept
{
    return std::clamp(t, 0.0, 1.0);
}
} // namespace

std::optional<frames::FramePoint>
ResolveAnchor(
    const PathAnchor& anchor,
    const frames::FrameId targetFrame,
    const time::SimulationTime atTime,
    const frames::FrameGraph& frameGraph,
    const universe::BodyRegistry& bodies,
    const EntitySocketResolver& entityResolver)
{
    return std::visit(
        [&](const auto& value)
            -> std::optional<frames::FramePoint>
        {
            using Anchor =
                std::decay_t<decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Anchor,
                    FramePointAnchor>)
            {
                return frameGraph.TransformPoint(
                    {
                        .frame = value.frame,
                        .localMeters =
                            value.localMeters
                    },
                    targetFrame,
                    atTime);
            }
            else if constexpr (
                std::is_same_v<
                    Anchor,
                    SurfaceAnchor>)
            {
                const auto* body =
                    bodies.FindBody(
                        value.body);

                if (body == nullptr)
                {
                    return std::nullopt;
                }

                return frameGraph.TransformPoint(
                    {
                        .frame = body->frame,
                        .localMeters =
                            SurfacePoint(
                                body->shape,
                                value.coordinate)
                    },
                    targetFrame,
                    atTime);
            }
            else
            {
                if (!entityResolver)
                {
                    return std::nullopt;
                }

                const auto point =
                    entityResolver(
                        value.entity,
                        value.socket,
                        value.localMeters);

                if (!point.has_value())
                {
                    return std::nullopt;
                }

                return frameGraph.TransformPoint(
                    *point,
                    targetFrame,
                    atTime);
            }
        },
        anchor);
}

math::Double3 EvaluateDirect(
    const math::Double3& start,
    const math::Double3& end,
    const f64 t) noexcept
{
    const f64 u = ClampT(t);
    return start * (1.0 - u) +
        end * u;
}

math::Double3 EvaluateBezier(
    const math::Double3& start,
    const math::Double3& end,
    const math::Double3& startHandle,
    const math::Double3& endHandle,
    const f64 t) noexcept
{
    const f64 u = ClampT(t);
    const f64 oneMinus = 1.0 - u;
    const math::Double3 control1 =
        start + startHandle;
    const math::Double3 control2 =
        end + endHandle;

    return
        start *
            (oneMinus * oneMinus * oneMinus) +
        control1 *
            (3.0 * oneMinus * oneMinus * u) +
        control2 *
            (3.0 * oneMinus * u * u) +
        end *
            (u * u * u);
}

math::Double3 EvaluateBezierTangent(
    const math::Double3& start,
    const math::Double3& end,
    const math::Double3& startHandle,
    const math::Double3& endHandle,
    const f64 t) noexcept
{
    const f64 u = ClampT(t);
    const f64 oneMinus = 1.0 - u;
    const math::Double3 control1 =
        start + startHandle;
    const math::Double3 control2 =
        end + endHandle;

    return
        (control1 - start) *
            (3.0 * oneMinus * oneMinus) +
        (control2 - control1) *
            (6.0 * oneMinus * u) +
        (end - control2) *
            (3.0 * u * u);
}

f64 EvaluateBezierCurvature(
    const math::Double3& start,
    const math::Double3& end,
    const math::Double3& startHandle,
    const math::Double3& endHandle,
    const f64 t) noexcept
{
    const f64 u = ClampT(t);
    const math::Double3 control1 =
        start + startHandle;
    const math::Double3 control2 =
        end + endHandle;

    const math::Double3 first =
        EvaluateBezierTangent(
            start,
            end,
            startHandle,
            endHandle,
            u);

    const math::Double3 second =
        (control2 - control1 * 2.0 + start) *
            (6.0 * (1.0 - u)) +
        (end - control2 * 2.0 + control1) *
            (6.0 * u);

    const f64 speed =
        math::Length(first);

    if (speed <= 1.0e-12)
    {
        return 0.0;
    }

    return math::Length(
               math::Cross(
                   first,
                   second)) /
        (speed * speed * speed);
}
} // namespace orbit::paths
