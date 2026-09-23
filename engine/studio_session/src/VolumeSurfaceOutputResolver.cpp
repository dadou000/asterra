#include <orbit/studio_session/VolumeSurfaceOutputResolver.hpp>

#include <cmath>
#include <optional>
#include <variant>

namespace orbit::studio_session
{
namespace
{
[[nodiscard]] std::optional<universe::BodyId> FindOwningBody(
    const scene::ObjectStore& objects,
    const world_model::UniverseComposition& universe,
    const scene::ObjectId volume)
{
    auto current = objects.Find(volume);

    while (current.has_value())
    {
        if (const auto body =
                universe.BodyForObject(current->id);
            body.has_value())
        {
            return body;
        }

        if (!current->parent.has_value())
        {
            break;
        }

        current = objects.Find(*current->parent);
    }

    return std::nullopt;
}

[[nodiscard]] f64 NormalizedRadiusSquared(
    const universe::BodyShape& shape,
    const math::Double3& point) noexcept
{
    return std::visit(
        [&point](const auto& value) -> f64
        {
            using Shape = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<Shape, universe::SphereShape>)
            {
                const f64 radius =
                    std::max(value.radiusMeters, 1.0e-12);
                return
                    (point.x * point.x +
                     point.y * point.y +
                     point.z * point.z) /
                    (radius * radius);
            }
            else
            {
                const f64 rx = std::max(value.radiiMeters.x, 1.0e-12);
                const f64 ry = std::max(value.radiiMeters.y, 1.0e-12);
                const f64 rz = std::max(value.radiiMeters.z, 1.0e-12);
                return
                    point.x * point.x / (rx * rx) +
                    point.y * point.y / (ry * ry) +
                    point.z * point.z / (rz * rz);
            }
        },
        shape);
}

[[nodiscard]] f64 LengthSquared(
    const math::Double3& value) noexcept
{
    return
        value.x * value.x +
        value.y * value.y +
        value.z * value.z;
}
} // namespace

void VolumeSurfaceOutputResolver::Resolve(
    const scene::ObjectStore& objects,
    const world_model::UniverseComposition& universe,
    const surface_model::SurfaceComposition& surfaces,
    const std::span<
        const volume_representation::VolumeSurfaceQueuedRequest>
        requests)
{
    resolved_.clear();
    diagnostics_ = {};
    diagnostics_.submitted = static_cast<u32>(requests.size());
    resolved_.reserve(requests.size());

    for (const auto& queued : requests)
    {
        const auto bodyId =
            FindOwningBody(
                objects,
                universe,
                queued.volume);

        if (!bodyId.has_value())
        {
            ++diagnostics_.missingOwningBody;
            continue;
        }

        const auto* body =
            universe.Bodies().FindBody(*bodyId);

        if (body == nullptr)
        {
            ++diagnostics_.missingRuntimeBody;
            continue;
        }

        // A BodyShape is also used as a conservative envelope for bodies that
        // have no solid surface. Require a real composed terrain/surface
        // capability before accepting deposition.
        if (!surfaces.TerrainObjectForBody(*bodyId).has_value())
        {
            ++diagnostics_.missingSurfaceCapability;
            continue;
        }

        const auto& request = queued.request;
        const auto& sample = request.samplePositionMeters;

        if (LengthSquared(sample) <= 1.0e-18)
        {
            ++diagnostics_.invalidProjectionDirection;
            continue;
        }

        const bool outside =
            NormalizedRadiusSquared(body->shape, sample) >= 1.0;

        const math::Double3 direction{
            outside ? -sample.x : sample.x,
            outside ? -sample.y : sample.y,
            outside ? -sample.z : sample.z
        };

        const auto hit =
            universe::IntersectReferenceSurfaceRay(
                body->shape,
                sample,
                direction);

        if (!hit.has_value())
        {
            ++diagnostics_.projectionMiss;
            continue;
        }

        const math::Double3 delta{
            hit->x - sample.x,
            hit->y - sample.y,
            hit->z - sample.z
        };
        const f64 projectionDistance =
            std::sqrt(LengthSquared(delta));

        if (projectionDistance >
            std::max(request.maximumProjectionDistanceMeters, 0.0) +
                1.0e-9)
        {
            ++diagnostics_.projectionOutOfRange;
            continue;
        }

        const auto coordinate =
            universe::ReferenceSurfaceCoordinate(
                body->shape,
                *hit);

        if (!coordinate.has_value())
        {
            ++diagnostics_.coordinateFailure;
            continue;
        }

        resolved_.push_back({
            .volume = queued.volume,
            .body = *bodyId,
            .frame = body->frame,
            .coordinate = *coordinate,
            .bodyLocalSurfacePointMeters = *hit,
            .request = request
        });
        ++diagnostics_.resolved;
    }
}

std::span<const ResolvedVolumeSurfaceDeposit>
VolumeSurfaceOutputResolver::Resolved() const noexcept
{
    return resolved_;
}

const VolumeSurfaceOutputResolverDiagnostics&
VolumeSurfaceOutputResolver::Diagnostics() const noexcept
{
    return diagnostics_;
}

void VolumeSurfaceOutputResolver::Clear() noexcept
{
    resolved_.clear();
    diagnostics_ = {};
}
} // namespace orbit::studio_session
