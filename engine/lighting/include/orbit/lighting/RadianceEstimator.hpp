#pragma once

#include <orbit/lighting/DirectLighting.hpp>
#include <orbit/lighting/LocalLightRegistry.hpp>
#include <orbit/lighting/RadianceClipmapResidency.hpp>

#include <span>

namespace orbit::lighting
{
struct RadianceEstimateSettings
{
    // Fraction of incident direct energy retained as broad one-bounce
    // irradiance by this first-order estimator. M14/M21+ may replace the
    // estimator without changing cache residency/sampling contracts.
    f32 diffuseTransportScale{0.18F};

    // Shared sky/ambient floor until sky visibility is integrated in M21.
    f32 ambientIrradianceScale{0.035F};

    f32 photopicLuminousEfficacy{683.0F};
    f32 solarReferenceIrradiance{1361.0F};
};

[[nodiscard]] DirectionalIrradianceL1
EstimateRadianceCell(
    const RadianceCellKey& key,
    const RadianceClipmapConfig& config,
    const LightingView& view,
    const DirectionalLight& stellar,
    std::span<const ResolvedLocalLight> localLights,
    const RadianceEstimateSettings& settings = {});
} // namespace orbit::lighting
