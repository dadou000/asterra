#include <orbit/path_geometry/PathSource.hpp>

#include <orbit/universe/ReferenceSurface.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace orbit::path_geometry
{
namespace
{
constexpr f64 kEpsilon = 1.0e-9;

[[nodiscard]] math::Double3
ReferenceNormal(
    const universe::BodyShape& shape,
    const math::Double3& point) noexcept
{
    return std::visit(
        [&](const auto& value)
        {
            using Shape =
                std::decay_t<
                    decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    universe::SphereShape>)
            {
                return math::Normalize(
                    point);
            }
            else
            {
                const f64 x =
                    std::max(
                        value.radiiMeters.x,
                        1.0e-9);
                const f64 y =
                    std::max(
                        value.radiiMeters.y,
                        1.0e-9);
                const f64 z =
                    std::max(
                        value.radiiMeters.z,
                        1.0e-9);

                return math::Normalize({
                    point.x / (x * x),
                    point.y / (y * y),
                    point.z / (z * z)
                });
            }
        },
        shape);
}

[[nodiscard]] PathUpResolver
ResolveUpProvider(
    const PathSourceRequest& request)
{
    if (request.upResolver)
    {
        return request.upResolver;
    }

    const auto* startSurface =
        std::get_if<paths::SurfaceAnchor>(
            &request.startNode.anchor);
    const auto* endSurface =
        std::get_if<paths::SurfaceAnchor>(
            &request.endNode.anchor);

    if (startSurface != nullptr &&
        endSurface != nullptr &&
        startSurface->body ==
            endSurface->body &&
        request.bodies != nullptr)
    {
        const auto* body =
            request.bodies->FindBody(
                startSurface->body);

        if (body != nullptr &&
            body->frame ==
                request.targetFrame)
        {
            const universe::BodyShape shape =
                body->shape;

            return
                [shape](
                    const math::Double3&
                        position)
                {
                    return ReferenceNormal(
                        shape,
                        position);
                };
        }
    }

    return
        [](const math::Double3&)
        {
            return math::Double3{
                0.0,
                1.0,
                0.0
            };
        };
}

[[nodiscard]] bool ValidRequest(
    const PathSourceRequest& request,
    std::string* failureReason)
{
    const auto fail =
        [failureReason](
            const char* message)
        {
            if (failureReason != nullptr)
            {
                *failureReason = message;
            }
            return false;
        };

    if (request.frames == nullptr ||
        request.bodies == nullptr)
    {
        return fail(
            "Path source requires frame and body registries.");
    }

    if (!request.targetFrame ||
        !request.frames->Contains(
            request.targetFrame))
    {
        return fail(
            "Path source target frame does not exist.");
    }

    if (request.edge.startNode !=
            request.startNode.id ||
        request.edge.endNode !=
            request.endNode.id)
    {
        return fail(
            "Path source endpoint records do not match the semantic edge.");
    }

    if (!std::isfinite(
            request.
                curveSampleSpacingMeters) ||
        request.curveSampleSpacingMeters <=
            0.0)
    {
        return fail(
            "Path curve sample spacing must be positive and finite.");
    }

    return true;
}
} // namespace

std::optional<PathCenterline>
BuildPathCenterline(
    const PathSourceRequest& request,
    std::string* failureReason)
{
    if (failureReason != nullptr)
    {
        failureReason->clear();
    }

    if (!ValidRequest(
            request,
            failureReason))
    {
        return std::nullopt;
    }

    const PathUpResolver up =
        ResolveUpProvider(request);

    PathCenterline centerline{
        .edge = request.edge.id,
        .frame = request.targetFrame,
        .sourceRevision =
            request.sourceRevision
    };

    if (request.edge.mode ==
        paths::EdgeMode::Routed)
    {
        if (request.routed == nullptr ||
            request.routed->edge !=
                request.edge.id)
        {
            if (failureReason != nullptr)
            {
                *failureReason =
                    "Routed path source requires the current route result.";
            }

            return std::nullopt;
        }

        centerline.sourceRevision =
            request.routed->
                dependencySignature;

        centerline.samples.reserve(
            request.routed->points.size());

        for (const auto& point :
             request.routed->points)
        {
            math::Double3 position =
                point.localMeters;

            if (request.routed->frame !=
                request.targetFrame)
            {
                const auto transformed =
                    request.frames->
                        TransformPoint(
                            {
                                .frame =
                                    request.routed->
                                        frame,
                                .localMeters =
                                    point.
                                        localMeters
                            },
                            request.targetFrame,
                            request.atTime);

                if (!transformed.has_value())
                {
                    if (failureReason !=
                        nullptr)
                    {
                        *failureReason =
                            "Routed result frame is disconnected from the requested target frame.";
                    }

                    return std::nullopt;
                }

                position =
                    transformed->
                        localMeters;
            }

            centerline.samples.push_back({
                .position = position,
                .up = up(position)
            });
        }

        if (centerline.samples.size() <
            2U)
        {
            if (failureReason != nullptr)
            {
                *failureReason =
                    "Routed result requires at least two points.";
            }

            return std::nullopt;
        }

        return centerline;
    }

    const auto start =
        paths::ResolveAnchor(
            request.startNode.anchor,
            request.targetFrame,
            request.atTime,
            *request.frames,
            *request.bodies,
            request.entityResolver);

    const auto end =
        paths::ResolveAnchor(
            request.endNode.anchor,
            request.targetFrame,
            request.atTime,
            *request.frames,
            *request.bodies,
            request.entityResolver);

    if (!start.has_value() ||
        !end.has_value())
    {
        if (failureReason != nullptr)
        {
            *failureReason =
                "Path endpoint anchor could not be resolved.";
        }

        return std::nullopt;
    }

    if (request.edge.mode ==
        paths::EdgeMode::Direct)
    {
        centerline.samples = {
            {
                .position =
                    start->localMeters,
                .up =
                    up(start->
                        localMeters)
            },
            {
                .position =
                    end->localMeters,
                .up =
                    up(end->
                        localMeters)
            }
        };

        return centerline;
    }

    const f64 controlLength =
        math::Length(
            request.edge.
                startHandleMeters) +
        math::Length(
            (end->localMeters +
             request.edge.
                 endHandleMeters) -
            (start->localMeters +
             request.edge.
                 startHandleMeters)) +
        math::Length(
            request.edge.
                endHandleMeters);

    const u32 segments =
        std::max<u32>(
            2U,
            static_cast<u32>(
                std::ceil(
                    std::max(
                        controlLength,
                        math::Length(
                            end->localMeters -
                            start->
                                localMeters)) /
                    request.
                        curveSampleSpacingMeters)));

    centerline.samples.reserve(
        static_cast<std::size_t>(
            segments) +
        1U);

    for (u32 segment = 0;
         segment <= segments;
         ++segment)
    {
        const f64 t =
            static_cast<f64>(segment) /
            static_cast<f64>(segments);

        const math::Double3 position =
            paths::EvaluateBezier(
                start->localMeters,
                end->localMeters,
                request.edge.
                    startHandleMeters,
                request.edge.
                    endHandleMeters,
                t);

        centerline.samples.push_back({
            .position = position,
            .up = up(position)
        });
    }

    return centerline;
}
} // namespace orbit::path_geometry
