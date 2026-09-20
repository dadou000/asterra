#include <orbit/studio_ui/StudioViewportCamera.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::studio_ui
{
std::optional<render_view::CameraState>
ComposeViewportCamera(
    const studio_session::ViewportTargetState& view,
    const u64 universeGeneration)
{
    if (!view.target.has_value())
    {
        return std::nullopt;
    }

    const auto& target = *view.target;

    if (target.universeGeneration != universeGeneration)
    {
        throw std::logic_error(
            "Studio viewport target was resolved against a stale universe generation.");
    }

    const f64 radius =
        std::max(
            target.referenceRadiusMeters,
            1.0);

    render_view::CameraState camera{};
    camera.frame = target.frame;
    camera.nearPlaneMeters =
        static_cast<f32>(
            std::max(
                radius * 1.0e-6,
                0.01));
    camera.farPlaneMeters =
        static_cast<f32>(
            std::max(
                radius * 12.0,
                static_cast<f64>(
                    camera.nearPlaneMeters) *
                    100.0));

    switch (view.mode)
    {
    case studio_session::ViewportMode::Perspective:
        camera.localPositionMeters = {
            0.0,
            0.0,
            -radius * 3.2
        };
        camera.forward = {
            0.0F,
            0.0F,
            1.0F
        };
        camera.up = {
            0.0F,
            1.0F,
            0.0F
        };
        camera.verticalFovRadians =
            1.22173048F;
        break;

    case studio_session::ViewportMode::BodyMap:
        camera.localPositionMeters = {
            0.0,
            radius * 3.2,
            0.0
        };
        camera.forward = {
            0.0F,
            -1.0F,
            0.0F
        };
        camera.up = {
            0.0F,
            0.0F,
            1.0F
        };
        camera.verticalFovRadians =
            1.04719755F;
        break;

    case studio_session::ViewportMode::Debug:
        camera.localPositionMeters = {
            -radius * 2.4,
            radius * 1.8,
            -radius * 2.4
        };
        camera.forward = {
            0.624695F,
            -0.468521F,
            0.624695F
        };
        camera.up = {
            0.0F,
            1.0F,
            0.0F
        };
        camera.verticalFovRadians =
            1.30899694F;
        break;
    }

    return camera;
}

render_view::CameraState
ComposeTerrainViewportCamera(
    const studio_session::ViewportTargetState& view,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain)
{
    if (!view.target.has_value())
    {
        throw std::invalid_argument(
            "Terrain viewport camera requires a resolved body target.");
    }

    if (view.mode !=
            studio_session::ViewportMode::Perspective ||
        view.target->body != terrain.body ||
        view.target->semanticObject !=
            terrain.semanticBody ||
        view.target->universeGeneration !=
            terrain.universeGeneration)
    {
        throw std::logic_error(
            "Terrain viewport camera target does not match the current production terrain runtime.");
    }

    const math::Double3 direction =
        math::Normalize(
            terrain.observer.meters);

    if (math::LengthSquared(direction) <=
        1.0e-20)
    {
        throw std::logic_error(
            "Terrain viewport observer has no valid body-space direction.");
    }

    const world::SurfaceFrame frame =
        world::MakeSurfaceFrame(
            direction);

    math::Float3 localForward{
        0.0F,
        -0.28F,
        1.0F
    };
    localForward =
        math::Normalize(
            localForward);

    const math::Double3 bodyForward =
        frame.east *
            static_cast<f64>(
                localForward.x) +
        frame.up *
            static_cast<f64>(
                localForward.y) +
        frame.north *
            static_cast<f64>(
                localForward.z);

    const f64 observerRadius =
        math::Length(
            terrain.observer.meters);

    const f64 altitude =
        std::max(
            observerRadius -
                terrain.planet.radiusMeters,
            0.0);

    render_view::CameraState camera{};
    camera.frame =
        view.target->frame;
    camera.localPositionMeters =
        terrain.observer.meters;
    camera.forward = {
        static_cast<f32>(
            bodyForward.x),
        static_cast<f32>(
            bodyForward.y),
        static_cast<f32>(
            bodyForward.z)
    };
    camera.forward =
        math::Normalize(
            camera.forward);
    camera.up = {
        static_cast<f32>(
            frame.up.x),
        static_cast<f32>(
            frame.up.y),
        static_cast<f32>(
            frame.up.z)
    };
    camera.verticalFovRadians =
        1.22173048F;
    camera.nearPlaneMeters =
        static_cast<f32>(
            std::clamp(
                altitude * 0.001,
                0.05,
                10.0));
    camera.farPlaneMeters =
        static_cast<f32>(
            std::max(
                terrain.planet.radiusMeters *
                    2.0,
                static_cast<f64>(
                    camera.nearPlaneMeters) *
                    100.0));

    return camera;
}

std::optional<StudioPhysicalPageSelection>
PhysicalPageAtViewportPoint(
    const studio_session::ViewportTargetState& view,
    const render_view::CameraState& camera,
    const u32 width,
    const u32 height,
    const f32 u,
    const f32 v,
    const u8 physicalTileLevel) noexcept
{
    if (!view.target.has_value() ||
        view.target->referenceRadiusMeters <= 0.0)
    {
        return std::nullopt;
    }

    const auto ray =
        render_view::ViewportRay(
            camera,
            width,
            height,
            u,
            v);

    if (!ray.has_value())
    {
        return std::nullopt;
    }

    const f64 radius =
        view.target->referenceRadiusMeters;
    const f64 projection =
        math::Dot(
            ray->origin,
            ray->direction);
    const f64 radial =
        math::Dot(
            ray->origin,
            ray->origin) -
        radius * radius;
    const f64 discriminant =
        projection * projection -
        radial;

    if (discriminant < 0.0)
    {
        return std::nullopt;
    }

    const f64 root =
        std::sqrt(
            std::max(
                discriminant,
                0.0));

    f64 distance =
        -projection - root;

    if (distance < 0.0)
    {
        distance =
            -projection + root;
    }

    if (distance < 0.0)
    {
        return std::nullopt;
    }

    const math::Double3 hit =
        ray->origin +
        ray->direction * distance;
    const math::Double3 direction =
        math::Normalize(hit);

    if (math::LengthSquared(direction) <= 1.0e-20)
    {
        return std::nullopt;
    }

    // SurfaceRegistry preserves BodyId bits as PlanetId, so this matches the
    // stable physical planet identity used by terrain pages.
    const world::PlanetId planet{
        .high = view.target->body.high,
        .low = view.target->body.low
    };

    return StudioPhysicalPageSelection{
        .address = {
            .planet = planet,
            .tile =
                world::TileForDirection(
                    direction,
                    physicalTileLevel)
        },
        .surfaceDirection = direction
    };
}

void ApplyViewportCamera(
    render_view::RenderView& view,
    const render_view::CameraState& camera) noexcept
{
    view.Camera() = camera;
}
} // namespace orbit::studio_ui
