#include <orbit/lighting/RadianceEstimator.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::lighting
{
namespace
{
void AddDirectionalLobe(
    DirectionalIrradianceL1& result,
    const math::Float3 direction,
    const math::Float3 energy) noexcept
{
    const auto dir =
        math::Normalize(direction);

    // First-order fit of a broad cosine-like lobe. The constant term carries
    // hemispherical energy while the linear terms preserve dominant direction.
    constexpr f32 kConstant = 0.5F;
    constexpr f32 kLinear = 0.5F;

    result.l0 = {
        result.l0.x + energy.x * kConstant,
        result.l0.y + energy.y * kConstant,
        result.l0.z + energy.z * kConstant
    };

    result.l1x = {
        result.l1x.x + energy.x * dir.x * kLinear,
        result.l1x.y + energy.y * dir.x * kLinear,
        result.l1x.z + energy.z * dir.x * kLinear
    };

    result.l1y = {
        result.l1y.x + energy.x * dir.y * kLinear,
        result.l1y.y + energy.y * dir.y * kLinear,
        result.l1y.z + energy.z * dir.y * kLinear
    };

    result.l1z = {
        result.l1z.x + energy.x * dir.z * kLinear,
        result.l1z.y + energy.y * dir.z * kLinear,
        result.l1z.z + energy.z * dir.z * kLinear
    };
}

[[nodiscard]] f32 RangeAttenuation(
    const f32 distanceMeters,
    const f32 rangeMeters) noexcept
{
    if (distanceMeters <= 1.0e-4F ||
        rangeMeters <= 1.0e-4F ||
        distanceMeters >= rangeMeters)
    {
        return 0.0F;
    }

    const f32 normalized =
        distanceMeters /
        rangeMeters;

    const f32 quartic =
        normalized *
        normalized *
        normalized *
        normalized;

    const f32 smooth =
        std::clamp(
            1.0F - quartic,
            0.0F,
            1.0F);

    return smooth * smooth;
}
} // namespace

DirectionalIrradianceL1
EstimateRadianceCell(
    const RadianceCellKey& key,
    const RadianceClipmapConfig& config,
    const LightingView& view,
    const DirectionalLight& stellar,
    const std::span<const ResolvedLocalLight> localLights,
    const RadianceEstimateSettings& settings)
{
    DirectionalIrradianceL1 result;

    const f32 transport =
        std::max(
            settings.diffuseTransportScale,
            0.0F);

    const f32 ambient =
        std::max(
            settings.ambientIrradianceScale,
            0.0F) *
        transport;

    result.l0 = {
        ambient,
        ambient,
        ambient
    };

    const math::Float3 stellarEnergy{
        std::max(
            stellar.colorLinear.x,
            0.0F) *
            std::max(
                stellar.irradianceScale,
                0.0F) *
            transport,
        std::max(
            stellar.colorLinear.y,
            0.0F) *
            std::max(
                stellar.irradianceScale,
                0.0F) *
            transport,
        std::max(
            stellar.colorLinear.z,
            0.0F) *
            std::max(
                stellar.irradianceScale,
                0.0F) *
            transport
    };

    AddDirectionalLobe(
        result,
        stellar.directionToLight,
        stellarEnergy);

    const auto cellCenter =
        RadianceCellGpuCenter(
            key,
            config,
            view);

    const f32 luminousEfficacy =
        std::max(
            settings.photopicLuminousEfficacy,
            1.0F);

    const f32 referenceIrradiance =
        std::max(
            settings.solarReferenceIrradiance,
            1.0e-5F);

    for (const auto& light :
         localLights)
    {
        const math::Float3 delta{
            light.positionCameraRelativeMeters.x -
                cellCenter.x,
            light.positionCameraRelativeMeters.y -
                cellCenter.y,
            light.positionCameraRelativeMeters.z -
                cellCenter.z
        };

        const f32 distance =
            math::Length(delta);

        const f32 range =
            std::max(
                light.rangeMeters,
                0.0F);

        const f32 rangeAttenuation =
            RangeAttenuation(
                distance,
                range);

        if (rangeAttenuation <= 0.0F)
        {
            continue;
        }

        const math::Float3 direction =
            delta /
            std::max(distance, 1.0e-5F);

        f32 solidAngle =
            4.0F *
            3.14159265358979323846F;

        f32 angular = 1.0F;

        if (light.type ==
            LocalLightType::Spot)
        {
            solidAngle =
                std::max(
                    2.0F *
                        3.14159265358979323846F *
                        (1.0F -
                         light.outerConeCosine),
                    1.0e-4F);

            const f32 spotCos =
                math::Dot(
                    direction * -1.0F,
                    math::Normalize(
                        light.direction));

            const f32 denominator =
                std::max(
                    light.innerConeCosine -
                        light.outerConeCosine,
                    1.0e-5F);

            angular =
                std::clamp(
                    (spotCos -
                     light.outerConeCosine) /
                        denominator,
                    0.0F,
                    1.0F);

            angular =
                angular * angular *
                (3.0F - 2.0F * angular);
        }

        if (angular <= 0.0F)
        {
            continue;
        }

        const f32 radiantWatts =
            std::max(
                light.luminousFluxLumens,
                0.0F) /
            luminousEfficacy;

        const f32 radiantIntensity =
            radiantWatts /
            solidAngle;

        const f32 normalizedIrradiance =
            radiantIntensity /
            std::max(
                distance * distance,
                0.0025F) *
            rangeAttenuation *
            angular /
            referenceIrradiance *
            transport;

        const math::Float3 energy{
            std::max(light.colorLinear.x, 0.0F) *
                normalizedIrradiance,
            std::max(light.colorLinear.y, 0.0F) *
                normalizedIrradiance,
            std::max(light.colorLinear.z, 0.0F) *
                normalizedIrradiance
        };

        AddDirectionalLobe(
            result,
            direction,
            energy);
    }

    return result;
}
} // namespace orbit::lighting
