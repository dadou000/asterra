#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <vector>

namespace orbit::celestial_compact_objects
{
inline constexpr f64 kSpeedOfLightMetersPerSecond =
    299792458.0;

enum class CompactObjectModel : u8
{
    SchwarzschildBaseline
};

struct CompactObjectParameters
{
    CompactObjectModel model{
        CompactObjectModel::SchwarzschildBaseline};

    // Semantic gravitational parameter GM. This keeps the capability compatible
    // with Orbit's gravity authority without requiring a solid reference shape.
    f64 gravitationalParameterM3PerS2{
        1.3271645321e20};

    // Reserved semantic seam for a future Kerr solver. The Schwarzschild
    // baseline requires |a*| == 0 but the schema need not break later.
    f64 dimensionlessSpin{0.0};

    math::Double3 spinAxis{0.0, 0.0, 1.0};

    // Presentation controls remain dimensionless and explicit.
    f64 shadowScale{1.0};
    f64 lensingStrength{1.0};
    f64 photonRingIntensity{1.0};
};

struct AccretionFlowParameters
{
    // Radii are expressed in gravitational radii rg = GM/c^2 so flows scale
    // naturally with compact-object mass.
    f64 innerRadiusRg{6.0};
    f64 outerRadiusRg{40.0};

    math::Double3 axis{0.0, 0.0, 1.0};

    // Scene-linear baseline emissive appearance. A future radiative-transfer
    // solver can replace this without changing semantic ownership.
    math::Double3 colorLinear{1.4, 0.42, 0.08};
    f64 intensity{1.0};
    f64 temperatureKelvin{8.0e6};
    f64 radialFalloffExponent{2.0};
    f64 thicknessRatio{0.08};
    f64 dopplerStrength{0.45};
    u64 seed{1U};
};

struct CompactObjectScales
{
    f64 gravitationalRadiusMeters{0.0};
    f64 schwarzschildRadiusMeters{0.0};
    f64 photonSphereRadiusMeters{0.0};
    f64 iscoRadiusMeters{0.0};
    f64 criticalImpactParameterMeters{0.0};
};

struct CompactObjectPresentation
{
    u64 fingerprint{0U};
    CompactObjectScales scales{};
    f64 shadowRadiusMeters{0.0};
    f64 photonRingRadiusMeters{0.0};
};

struct AccretionSample
{
    f64 radiusRg{0.0};
    f64 normalizedEmission{0.0};
};

struct AccretionFlowProduct
{
    u64 fingerprint{0U};
    f64 innerRadiusMeters{0.0};
    f64 outerRadiusMeters{0.0};
    std::vector<AccretionSample> radialProfile;
};

[[nodiscard]] u64 CompactObjectFingerprint(
    const CompactObjectParameters& parameters);

[[nodiscard]] CompactObjectScales ResolveScales(
    const CompactObjectParameters& parameters);

[[nodiscard]] CompactObjectPresentation
BuildCompactObjectPresentation(
    const CompactObjectParameters& parameters);

[[nodiscard]] f64 WeakFieldDeflectionRadians(
    const CompactObjectParameters& parameters,
    f64 impactParameterMeters) noexcept;

[[nodiscard]] u64 AccretionFlowFingerprint(
    const AccretionFlowParameters& parameters,
    const CompactObjectParameters& compact);

[[nodiscard]] AccretionFlowProduct
BuildAccretionFlowProduct(
    const AccretionFlowParameters& parameters,
    const CompactObjectParameters& compact,
    u32 radialSamples = 128U);

} // namespace orbit::celestial_compact_objects
