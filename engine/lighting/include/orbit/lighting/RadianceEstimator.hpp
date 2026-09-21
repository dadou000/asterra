#pragma once

#include <orbit/lighting/DirectLighting.hpp>
#include <orbit/lighting/EmissiveSampling.hpp>
#include <orbit/lighting/LocalLightRegistry.hpp>
#include <orbit/lighting/RadianceClipmapResidency.hpp>
#include <orbit/lighting/Visibility.hpp>

#include <span>

namespace orbit::lighting
{
struct EmissiveVolumeSource
{
    // Stable source center in LightingView::frame coordinates.
    math::Double3 centerInFrameMeters{};

    // Conservative emitting bound. The estimator treats the source as a
    // radiance-bearing volume, not as an opaque surface or point light.
    f32 radiusMeters{1.0F};

    // Scene-linear emitted radiance color and dimensionless transport scale.
    math::Float3 emissionLinear{1.0F, 1.0F, 1.0F};
    f32 intensityScale{1.0F};

    // Maximum distance at which this low-frequency source contributes to the
    // radiance cache. Zero means radius-derived automatic range.
    f32 influenceRangeMeters{0.0F};

    u64 stableId{0U};
};

struct RadianceEstimateSettings
{
    // Fraction of incident direct energy retained as broad one-bounce
    // irradiance by this first-order estimator. M14/M21+ may replace the
    // estimator without changing cache residency/sampling contracts.
    f32 diffuseTransportScale{0.18F};

    // Legacy scalar fallback used when no physical sky summary is supplied.
    f32 ambientIrradianceScale{0.035F};

    // Low-frequency scene-linear sky irradiance summary. When non-zero this
    // replaces the scalar fallback and may carry atmospheric color.
    math::Float3 skyIrradianceLinear{};

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
    const VisibilityRegistry* visibility = nullptr,
    const RadianceEstimateSettings& settings = {},
    std::span<const EmissiveVolumeSource> emissiveVolumes = {},
    std::span<const EmissiveSampledEmitter> emissiveSurfaces = {});
} // namespace orbit::lighting
