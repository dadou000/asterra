#include <orbit/studio_ui/StudioViewportCamera.hpp>

#include <algorithm>
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

void ApplyViewportCamera(
    render_view::RenderView& view,
    const render_view::CameraState& camera) noexcept
{
    view.Camera() = camera;
}
} // namespace orbit::studio_ui
