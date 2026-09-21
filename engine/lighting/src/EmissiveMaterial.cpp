#include <orbit/lighting/EmissiveMaterial.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] f32 SafeNonNegative(
    const f32 value) noexcept
{
    return
        std::isfinite(value)
            ? std::max(value, 0.0F)
            : 0.0F;
}

[[nodiscard]] math::Float3 SafeNonNegative(
    const math::Float3 value) noexcept
{
    return {
        SafeNonNegative(value.x),
        SafeNonNegative(value.y),
        SafeNonNegative(value.z)
    };
}
} // namespace

PhysicalEmissionEvaluation EvaluatePhysicalEmission(
    const PhysicalEmissionMaterial& material,
    const math::Float3 emissiveTextureLinear,
    const f32 photopicLuminousEfficacy,
    const f32 solarReferenceIrradiance) noexcept
{
    const auto color =
        SafeNonNegative(material.colorLinear);
    const auto texture =
        SafeNonNegative(emissiveTextureLinear);

    const math::Float3 modulation{
        color.x * texture.x,
        color.y * texture.y,
        color.z * texture.z
    };

    constexpr f32 kRedLuminance = 0.2126F;
    constexpr f32 kGreenLuminance = 0.7152F;
    constexpr f32 kBlueLuminance = 0.0722F;

    const f32 modulationLuminance =
        modulation.x * kRedLuminance +
        modulation.y * kGreenLuminance +
        modulation.z * kBlueLuminance;

    const f32 luminanceNits =
        SafeNonNegative(material.luminanceNits);

    const f32 efficacy =
        std::max(
            SafeNonNegative(
                photopicLuminousEfficacy),
            1.0e-6F);

    const f32 reference =
        std::max(
            SafeNonNegative(
                solarReferenceIrradiance),
            1.0e-6F);

    math::Float3 visible{};

    if (modulationLuminance > 1.0e-8F &&
        luminanceNits > 0.0F)
    {
        // cd/m^2 -> W/(sr*m^2) using the photopic efficacy approximation,
        // then normalize against Orbit's reference solar irradiance.
        const f32 normalizedRadiance =
            (luminanceNits / efficacy) /
            reference;

        const f32 chromaScale =
            normalizedRadiance /
            modulationLuminance;

        visible = {
            modulation.x * chromaScale,
            modulation.y * chromaScale,
            modulation.z * chromaScale
        };
    }

    const f32 giScale =
        material.contributesToGi
            ? SafeNonNegative(
                  material.giScale)
            : 0.0F;

    return {
        .visibleRadianceSceneLinear =
            visible,
        .giRadianceSceneLinear = {
            visible.x * giScale,
            visible.y * giScale,
            visible.z * giScale
        }
    };
}
} // namespace orbit::lighting
