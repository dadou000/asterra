#pragma once

#include <orbit/lighting/Visibility.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::lighting
{
struct SkyVisibilitySettings
{
    f32 minimumDistanceMeters{0.10F};
    f32 maximumDistanceMeters{2'000.0F};
};

struct SkyVisibilityEstimate
{
    f32 visibleFraction{1.0F};

    // Weighted direction toward the visible portion of the hemisphere.
    // Zero means visibility is isotropic or fully occluded.
    math::Float3 openDirection{};
};

[[nodiscard]] SkyVisibilityEstimate EstimateSkyVisibility(
    const VisibilityRegistry& visibility,
    frames::FrameId frame,
    universe::BodyId body,
    const math::Double3& originInFrameMeters,
    math::Float3 upDirection,
    const SkyVisibilitySettings& settings = {});
} // namespace orbit::lighting
