#include <orbit/lighting/LightingView.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::lighting
{
math::Float3 ToLightingCameraRelative(
    const math::Double3& pointInFrameMeters,
    const LightingView& view) noexcept
{
    const math::Double3 delta =
        pointInFrameMeters -
        view.gpuOriginInFrameMeters;

    return {
        static_cast<f32>(delta.x),
        static_cast<f32>(delta.y),
        static_cast<f32>(delta.z)
    };
}

StableLightingCell StableCellForPoint(
    const math::Double3& pointInFrameMeters,
    const f64 cellSizeMeters,
    const LightingView& view)
{
    if (!std::isfinite(cellSizeMeters) ||
        cellSizeMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Lighting cell size must be finite and positive.");
    }

    const auto quantize =
        [cellSizeMeters](const f64 coordinate)
            -> i64
        {
            if (!std::isfinite(coordinate))
            {
                throw std::invalid_argument(
                    "Lighting cell coordinate must be finite.");
            }

            return static_cast<i64>(
                std::floor(
                    coordinate /
                    cellSizeMeters));
        };

    return {
        .frame = view.frame,
        .body = view.body,
        .x = quantize(pointInFrameMeters.x),
        .y = quantize(pointInFrameMeters.y),
        .z = quantize(pointInFrameMeters.z)
    };
}

LightingViewChange ClassifyLightingViewChange(
    const LightingView& previous,
    const LightingView& current,
    const bool explicitCameraCut,
    const bool explicitProjectionChange) noexcept
{
    LightingViewChange result =
        LightingViewChange::None;

    if (previous.frame != current.frame)
    {
        result |=
            LightingViewChange::FrameChanged;
    }

    if (previous.body != current.body)
    {
        result |=
            LightingViewChange::BodyChanged;
    }

    if (previous.gpuOriginRevision !=
        current.gpuOriginRevision)
    {
        result |=
            LightingViewChange::GpuOriginRebase;
    }

    if (explicitCameraCut)
    {
        result |=
            LightingViewChange::CameraCut;
    }

    if (explicitProjectionChange ||
        previous.verticalFovRadians !=
            current.verticalFovRadians ||
        previous.nearPlaneMeters !=
            current.nearPlaneMeters ||
        previous.farPlaneMeters !=
            current.farPlaneMeters)
    {
        result |=
            LightingViewChange::ProjectionChanged;
    }

    return result;
}
} // namespace orbit::lighting
