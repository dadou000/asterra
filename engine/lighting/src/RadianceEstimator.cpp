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
    const VisibilityRegistry* const visibility,
    const RadianceEstimateSettings& settings,
    const std::span<const EmissiveVolumeSource> emissiveVolumes,
    const std::span<const EmissiveSampledEmitter> emissiveSurfaces)
{
    DirectionalIrradianceL1 result;

    const f32 transport =
        std::max(
            settings.diffuseTransportScale,
            0.0F);

    const bool hasSkySummary =
        settings.skyIrradianceLinear.x > 0.0F ||
        settings.skyIrradianceLinear.y > 0.0F ||
        settings.skyIrradianceLinear.z > 0.0F;

    const f32 fallbackAmbient =
        std::max(
            settings.ambientIrradianceScale,
            0.0F);

    const math::Float3 ambientEnergy =
        hasSkySummary
            ? math::Float3{
                  std::max(settings.skyIrradianceLinear.x, 0.0F),
                  std::max(settings.skyIrradianceLinear.y, 0.0F),
                  std::max(settings.skyIrradianceLinear.z, 0.0F)}
            : math::Float3{
                  fallbackAmbient,
                  fallbackAmbient,
                  fallbackAmbient};

    result.l0 = {
        ambientEnergy.x * transport,
        ambientEnergy.y * transport,
        ambientEnergy.z * transport
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

    const auto cellCenterFrame =
        RadianceCellCenterInFrame(
            key,
            config);

    bool stellarVisible = true;

    if (visibility != nullptr)
    {
        VisibilityQuery query{
            .purpose =
                VisibilityPurpose::DiffuseGi,
            .frame = view.frame,
            .body = view.body,
            .originInFrameMeters =
                cellCenterFrame,
            .direction =
                stellar.directionToLight,
            .minimumDistanceMeters = 0.05F,
            .maximumDistanceMeters =
                std::max(
                    view.farPlaneMeters,
                    10'000.0F),
            .importance = 1.0F,
            .requirements = {
                .requireOffscreenCoverage = true
            }
        };

        const auto visibilityResult =
            visibility->TraceNearest(query);

        stellarVisible =
            visibilityResult.resolution !=
                VisibilityResolution::Hit;
    }

    if (stellarVisible)
    {
        AddDirectionalLobe(
            result,
            stellar.directionToLight,
            stellarEnergy);
    }

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

    for (const auto& source :
         emissiveVolumes)
    {
        const math::Double3 deltaD =
            source.centerInFrameMeters -
            cellCenterFrame;

        const f64 centerDistanceD =
            math::Length(deltaD);

        const f32 radius =
            std::max(
                source.radiusMeters,
                0.01F);

        const f32 centerDistance =
            static_cast<f32>(
                std::max(
                    centerDistanceD,
                    0.0));

        const f32 surfaceDistance =
            std::max(
                centerDistance -
                radius,
                0.0F);

        const f32 automaticRange =
            std::max(
                radius * 12.0F,
                radius + 1.0F);

        const f32 range =
            source.influenceRangeMeters > 0.0F
                ? source.influenceRangeMeters
                : automaticRange;

        if (surfaceDistance >= range)
        {
            continue;
        }

        math::Float3 direction{
            0.0F, 1.0F, 0.0F};

        if (centerDistanceD > 1.0e-8)
        {
            const auto directionD =
                deltaD /
                centerDistanceD;

            direction = {
                static_cast<f32>(directionD.x),
                static_cast<f32>(directionD.y),
                static_cast<f32>(directionD.z)
            };
        }

        bool visible = true;

        if (visibility != nullptr &&
            surfaceDistance > 0.05F)
        {
            VisibilityQuery query{
                .purpose =
                    VisibilityPurpose::DiffuseGi,
                .frame = view.frame,
                .body = view.body,
                .originInFrameMeters =
                    cellCenterFrame,
                .direction =
                    direction,
                .minimumDistanceMeters = 0.05F,
                .maximumDistanceMeters =
                    std::max(
                        surfaceDistance,
                        0.05F),
                .importance = 1.0F,
                .requirements = {
                    .requireOffscreenCoverage = true
                }
            };

            const auto visibilityResult =
                visibility->TraceNearest(query);

            visible =
                visibilityResult.resolution !=
                    VisibilityResolution::Hit;
        }

        if (!visible)
        {
            continue;
        }

        const f32 normalized =
            std::clamp(
                surfaceDistance /
                std::max(range, 0.01F),
                0.0F,
                1.0F);

        const f32 fade =
            (1.0F - normalized) *
            (1.0F - normalized);

        // Approximate projected solid-angle coverage of the bounded emitter.
        // This intentionally stays low-frequency: the cache carries broad
        // radiance while screen-space/future volumetric passes preserve detail.
        const f32 coverage =
            centerDistance <= radius
                ? 1.0F
                : std::clamp(
                      (radius * radius) /
                      std::max(
                          centerDistance *
                              centerDistance,
                          radius * radius),
                      0.0F,
                      1.0F);

        const f32 sourceScale =
            std::max(
                source.intensityScale,
                0.0F) *
            fade *
            coverage *
            transport;

        const math::Float3 energy{
            std::max(
                source.emissionLinear.x,
                0.0F) *
                sourceScale,
            std::max(
                source.emissionLinear.y,
                0.0F) *
                sourceScale,
            std::max(
                source.emissionLinear.z,
                0.0F) *
                sourceScale
        };

        AddDirectionalLobe(
            result,
            direction,
            energy);
    }

    for (const auto& emitter :
         emissiveSurfaces)
    {
        const math::Double3 deltaD =
            emitter.positionInFrameMeters -
            cellCenterFrame;

        const f64 distanceD =
            math::Length(deltaD);

        if (!std::isfinite(distanceD) ||
            distanceD <= 1.0e-5)
        {
            continue;
        }

        const auto directionD =
            deltaD /
            distanceD;

        const math::Float3 direction{
            static_cast<f32>(directionD.x),
            static_cast<f32>(directionD.y),
            static_cast<f32>(directionD.z)
        };

        const f64 emitterFacing =
            std::max(
                math::Dot(
                    emitter.normalInFrame,
                    directionD * -1.0),
                0.0);

        if (emitterFacing <= 0.0 ||
            emitter.areaMetersSquared <= 0.0)
        {
            continue;
        }

        bool visible = true;

        if (visibility != nullptr)
        {
            VisibilityQuery query{
                .purpose =
                    VisibilityPurpose::DiffuseGi,
                .frame = view.frame,
                .body = view.body,
                .originInFrameMeters =
                    cellCenterFrame,
                .direction = direction,
                .minimumDistanceMeters = 0.05F,
                .maximumDistanceMeters =
                    static_cast<f32>(
                        std::max(
                            distanceD - 0.05,
                            0.05)),
                .importance =
                    static_cast<f32>(
                        std::clamp(
                            emitter.samplingProbability,
                            0.0F,
                            1.0F)),
                .requirements = {
                    .requireOffscreenCoverage = true
                }
            };

            const auto visibilityResult =
                visibility->TraceNearest(query);

            visible =
                visibilityResult.resolution !=
                    VisibilityResolution::Hit;
        }

        if (!visible)
        {
            continue;
        }

        // Far-field area-emitter irradiance approximation:
        // E ~= L * A * cos(theta_emitter) / r^2.
        const f64 geometry =
            emitter.areaMetersSquared *
            emitterFacing /
            std::max(
                distanceD * distanceD,
                1.0e-6);

        const f32 scale =
            static_cast<f32>(
                geometry) *
            transport;

        const math::Float3 energy{
            std::max(
                emitter.averageRadiance.x,
                0.0F) *
                scale,
            std::max(
                emitter.averageRadiance.y,
                0.0F) *
                scale,
            std::max(
                emitter.averageRadiance.z,
                0.0F) *
                scale
        };

        AddDirectionalLobe(
            result,
            direction,
            energy);
    }

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

        bool visible = true;

        if (visibility != nullptr)
        {
            VisibilityQuery query{
                .purpose =
                    VisibilityPurpose::DiffuseGi,
                .frame = view.frame,
                .body = view.body,
                .originInFrameMeters =
                    cellCenterFrame,
                .direction = direction,
                .minimumDistanceMeters = 0.05F,
                .maximumDistanceMeters =
                    std::max(
                        distance - 0.05F,
                        0.05F),
                .importance = 1.0F,
                .requirements = {
                    .requireOffscreenCoverage = true
                }
            };

            const auto visibilityResult =
                visibility->TraceNearest(query);

            visible =
                visibilityResult.resolution !=
                    VisibilityResolution::Hit;
        }

        if (!visible)
        {
            continue;
        }

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
