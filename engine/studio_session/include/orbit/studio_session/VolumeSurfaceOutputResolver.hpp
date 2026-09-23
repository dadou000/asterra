#pragma once

#include <orbit/frames/FrameGraph.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/surface_model/SurfaceComposition.hpp>
#include <orbit/universe/ReferenceSurface.hpp>
#include <orbit/volume_representation/VolumeOutputRuntime.hpp>
#include <orbit/world_model/UniverseComposition.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace orbit::studio_session
{
struct ResolvedVolumeSurfaceDeposit
{
    scene::ObjectId volume{};
    universe::BodyId body{};
    frames::FrameId frame{};
    universe::SurfaceCoordinate coordinate{};
    math::Double3 bodyLocalSurfacePointMeters{};
    volume_representation::VolumeSurfaceDepositRequest request{};
};

struct VolumeSurfaceOutputResolverDiagnostics
{
    u32 submitted{0U};
    u32 resolved{0U};
    u32 missingOwningBody{0U};
    u32 missingRuntimeBody{0U};
    u32 missingSurfaceCapability{0U};
    u32 invalidProjectionDirection{0U};
    u32 projectionMiss{0U};
    u32 projectionOutOfRange{0U};
    u32 coordinateFailure{0U};
};

// Resolves M38 transport-neutral surface requests against the authoritative
// composed world. Volume coordinates are interpreted in the owning body's
// body-fixed frame, matching Orbit's body-local surface/terrain semantics.
// Projection is radial with respect to the body shape, never a global -Y ray.
class VolumeSurfaceOutputResolver
{
public:
    void Resolve(
        const scene::ObjectStore& objects,
        const world_model::UniverseComposition& universe,
        const surface_model::SurfaceComposition& surfaces,
        const std::span<
            const volume_representation::VolumeSurfaceQueuedRequest>
            requests)
    {
        resolved_.clear();
        diagnostics_ = {};
        diagnostics_.submitted =
            static_cast<u32>(requests.size());
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

            // BodyShape is also a conservative envelope for non-solid bodies.
            // Require an authored/composed Surface capability before deposits
            // can become physical-surface work.
            if (!surfaces.
                    TerrainObjectForBody(*bodyId).
                    has_value())
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
                NormalizedRadiusSquared(
                    body->shape,
                    sample) >= 1.0;

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
                std::max(
                    request.maximumProjectionDistanceMeters,
                    0.0) +
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

    [[nodiscard]] std::span<const ResolvedVolumeSurfaceDeposit>
    Resolved() const noexcept
    {
        return resolved_;
    }

    [[nodiscard]] const VolumeSurfaceOutputResolverDiagnostics&
    Diagnostics() const noexcept
    {
        return diagnostics_;
    }

    void Clear() noexcept
    {
        resolved_.clear();
        diagnostics_ = {};
    }

private:
    [[nodiscard]] static std::optional<universe::BodyId>
    FindOwningBody(
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

    [[nodiscard]] static f64 NormalizedRadiusSquared(
        const universe::BodyShape& shape,
        const math::Double3& point) noexcept
    {
        return std::visit(
            [&point](const auto& value) -> f64
            {
                using Shape =
                    std::decay_t<decltype(value)>;

                if constexpr (
                    std::is_same_v<
                        Shape,
                        universe::SphereShape>)
                {
                    const f64 radius =
                        std::max(
                            value.radiusMeters,
                            1.0e-12);
                    return
                        (point.x * point.x +
                         point.y * point.y +
                         point.z * point.z) /
                        (radius * radius);
                }
                else
                {
                    const f64 rx =
                        std::max(
                            value.radiiMeters.x,
                            1.0e-12);
                    const f64 ry =
                        std::max(
                            value.radiiMeters.y,
                            1.0e-12);
                    const f64 rz =
                        std::max(
                            value.radiiMeters.z,
                            1.0e-12);
                    return
                        point.x * point.x / (rx * rx) +
                        point.y * point.y / (ry * ry) +
                        point.z * point.z / (rz * rz);
                }
            },
            shape);
    }

    [[nodiscard]] static f64 LengthSquared(
        const math::Double3& value) noexcept
    {
        return
            value.x * value.x +
            value.y * value.y +
            value.z * value.z;
    }

    std::vector<ResolvedVolumeSurfaceDeposit> resolved_;
    VolumeSurfaceOutputResolverDiagnostics diagnostics_{};
};

[[nodiscard]] inline VolumeSurfaceOutputResolver&
VolumeSurfaceOutputs() noexcept
{
    static VolumeSurfaceOutputResolver resolver;
    return resolver;
}
} // namespace orbit::studio_session
