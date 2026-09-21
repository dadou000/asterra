#include <orbit/lighting/SurfaceData.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] bool Finite3(
    const math::Float3& value) noexcept
{
    return
        std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z);
}

[[nodiscard]] math::Float3 SafeNormal(
    const math::Float3& value,
    const math::Float3& fallback) noexcept
{
    if (!Finite3(value))
    {
        return fallback;
    }

    const f32 lengthSquared =
        math::LengthSquared(value);

    if (!std::isfinite(lengthSquared) ||
        lengthSquared <= 1.0e-12F)
    {
        return fallback;
    }

    return math::Normalize(value);
}

[[nodiscard]] f32 FiniteClamp01(
    const f32 value,
    const f32 fallback) noexcept
{
    if (!std::isfinite(value))
    {
        return fallback;
    }

    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] f32 NonNegativeFinite(
    const f32 value) noexcept
{
    if (!std::isfinite(value) ||
        value <= 0.0F)
    {
        return 0.0F;
    }

    return value;
}
} // namespace

bool IsFinite(
    const SurfaceData& surface) noexcept
{
    return
        Finite3(surface.positionCameraRelativeMeters) &&
        Finite3(surface.geometricNormal) &&
        Finite3(surface.shadingNormal) &&
        Finite3(surface.baseColorLinear) &&
        std::isfinite(surface.roughness) &&
        std::isfinite(surface.metallic) &&
        Finite3(surface.emissionRadianceSceneLinear) &&
        std::isfinite(surface.emissionGiScale);
}

SurfaceData Canonicalize(
    const SurfaceData& surface) noexcept
{
    SurfaceData result = surface;

    result.positionCameraRelativeMeters = {
        std::isfinite(surface.positionCameraRelativeMeters.x)
            ? surface.positionCameraRelativeMeters.x
            : 0.0F,
        std::isfinite(surface.positionCameraRelativeMeters.y)
            ? surface.positionCameraRelativeMeters.y
            : 0.0F,
        std::isfinite(surface.positionCameraRelativeMeters.z)
            ? surface.positionCameraRelativeMeters.z
            : 0.0F
    };

    result.geometricNormal =
        SafeNormal(
            surface.geometricNormal,
            {0.0F, 1.0F, 0.0F});

    result.shadingNormal =
        SafeNormal(
            surface.shadingNormal,
            result.geometricNormal);

    result.baseColorLinear = {
        FiniteClamp01(surface.baseColorLinear.x, 0.18F),
        FiniteClamp01(surface.baseColorLinear.y, 0.18F),
        FiniteClamp01(surface.baseColorLinear.z, 0.18F)
    };

    result.roughness =
        FiniteClamp01(surface.roughness, 0.8F);
    result.metallic =
        FiniteClamp01(surface.metallic, 0.0F);

    result.emissionRadianceSceneLinear = {
        NonNegativeFinite(
            surface.emissionRadianceSceneLinear.x),
        NonNegativeFinite(
            surface.emissionRadianceSceneLinear.y),
        NonNegativeFinite(
            surface.emissionRadianceSceneLinear.z)
    };

    result.emissionGiScale =
        NonNegativeFinite(
            surface.emissionGiScale);

    return result;
}
} // namespace orbit::lighting
