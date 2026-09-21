#include <orbit/celestial_compact_objects/CompactObject.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbit::celestial_compact_objects
{
namespace
{
void ValidateCompact(
    const CompactObjectParameters& p)
{
    if (!std::isfinite(
            p.gravitationalParameterM3PerS2) ||
        p.gravitationalParameterM3PerS2 <= 0.0 ||
        !std::isfinite(p.dimensionlessSpin) ||
        std::abs(p.dimensionlessSpin) > 1.0 ||
        !std::isfinite(p.shadowScale) ||
        p.shadowScale <= 0.0 ||
        !std::isfinite(p.lensingStrength) ||
        p.lensingStrength < 0.0 ||
        !std::isfinite(p.photonRingIntensity) ||
        p.photonRingIntensity < 0.0 ||
        math::LengthSquared(p.spinAxis) <= 1.0e-24)
    {
        throw std::invalid_argument(
            "Compact object parameters are invalid.");
    }

    if (p.model ==
            CompactObjectModel::
                SchwarzschildBaseline &&
        std::abs(p.dimensionlessSpin) >
            1.0e-12)
    {
        throw std::invalid_argument(
            "Schwarzschild baseline requires zero dimensionless spin.");
    }
}

void ValidateAccretion(
    const AccretionFlowParameters& p)
{
    if (!std::isfinite(p.innerRadiusRg) ||
        !std::isfinite(p.outerRadiusRg) ||
        p.innerRadiusRg <= 0.0 ||
        p.outerRadiusRg <= p.innerRadiusRg ||
        math::LengthSquared(p.axis) <=
            1.0e-24 ||
        !std::isfinite(p.intensity) ||
        p.intensity < 0.0 ||
        !std::isfinite(p.temperatureKelvin) ||
        p.temperatureKelvin <= 0.0 ||
        !std::isfinite(
            p.radialFalloffExponent) ||
        p.radialFalloffExponent <= 0.0 ||
        !std::isfinite(p.thicknessRatio) ||
        p.thicknessRatio < 0.0 ||
        p.thicknessRatio > 1.0 ||
        !std::isfinite(p.dopplerStrength) ||
        p.dopplerStrength < 0.0 ||
        p.dopplerStrength > 1.0 ||
        !std::isfinite(p.colorLinear.x) ||
        !std::isfinite(p.colorLinear.y) ||
        !std::isfinite(p.colorLinear.z) ||
        p.colorLinear.x < 0.0 ||
        p.colorLinear.y < 0.0 ||
        p.colorLinear.z < 0.0)
    {
        throw std::invalid_argument(
            "Accretion flow parameters are invalid.");
    }
}
} // namespace

u64 CompactObjectFingerprint(
    const CompactObjectParameters& p)
{
    ValidateCompact(p);

    u64 hash =
        0x4d3330434f4d5041ULL;

    const auto add =
        [&](const u64 value)
        {
            hash =
                terrain::StableCombine64(
                    hash,
                    value);
        };

    const auto addf =
        [&](const f64 value)
        {
            add(std::bit_cast<u64>(value));
        };

    add(static_cast<u64>(p.model));
    addf(p.gravitationalParameterM3PerS2);
    addf(p.dimensionlessSpin);
    addf(p.spinAxis.x);
    addf(p.spinAxis.y);
    addf(p.spinAxis.z);
    addf(p.shadowScale);
    addf(p.lensingStrength);
    addf(p.photonRingIntensity);

    return hash;
}

CompactObjectScales ResolveScales(
    const CompactObjectParameters& p)
{
    ValidateCompact(p);

    const f64 c2 =
        kSpeedOfLightMetersPerSecond *
        kSpeedOfLightMetersPerSecond;

    const f64 rg =
        p.gravitationalParameterM3PerS2 /
        c2;

    return {
        .gravitationalRadiusMeters = rg,
        .schwarzschildRadiusMeters =
            2.0 * rg,
        .photonSphereRadiusMeters =
            3.0 * rg,
        .iscoRadiusMeters =
            6.0 * rg,
        .criticalImpactParameterMeters =
            3.0 *
            std::sqrt(3.0) *
            rg
    };
}

CompactObjectPresentation
BuildCompactObjectPresentation(
    const CompactObjectParameters& p)
{
    const auto scales =
        ResolveScales(p);

    return {
        .fingerprint =
            CompactObjectFingerprint(p),
        .scales = scales,
        .shadowRadiusMeters =
            scales.
                criticalImpactParameterMeters *
            p.shadowScale,
        .photonRingRadiusMeters =
            scales.
                criticalImpactParameterMeters
    };
}

f64 WeakFieldDeflectionRadians(
    const CompactObjectParameters& p,
    const f64 impactParameterMeters) noexcept
{
    if (impactParameterMeters <= 0.0 ||
        !std::isfinite(
            impactParameterMeters) ||
        p.gravitationalParameterM3PerS2 <=
            0.0)
    {
        return 0.0;
    }

    const f64 c2 =
        kSpeedOfLightMetersPerSecond *
        kSpeedOfLightMetersPerSecond;

    const f64 alpha =
        4.0 *
        p.gravitationalParameterM3PerS2 /
        (impactParameterMeters * c2);

    return std::max(
        alpha *
            std::max(
                p.lensingStrength,
                0.0),
        0.0);
}

u64 AccretionFlowFingerprint(
    const AccretionFlowParameters& p,
    const CompactObjectParameters& compact)
{
    ValidateCompact(compact);
    ValidateAccretion(p);

    u64 hash =
        terrain::StableCombine64(
            CompactObjectFingerprint(
                compact),
            0x414343524554494fULL);

    const auto add =
        [&](const u64 value)
        {
            hash =
                terrain::StableCombine64(
                    hash,
                    value);
        };

    const auto addf =
        [&](const f64 value)
        {
            add(std::bit_cast<u64>(value));
        };

    addf(p.innerRadiusRg);
    addf(p.outerRadiusRg);
    addf(p.axis.x);
    addf(p.axis.y);
    addf(p.axis.z);
    addf(p.colorLinear.x);
    addf(p.colorLinear.y);
    addf(p.colorLinear.z);
    addf(p.intensity);
    addf(p.temperatureKelvin);
    addf(p.radialFalloffExponent);
    addf(p.thicknessRatio);
    addf(p.dopplerStrength);
    add(p.seed);

    return hash;
}

AccretionFlowProduct
BuildAccretionFlowProduct(
    const AccretionFlowParameters& p,
    const CompactObjectParameters& compact,
    const u32 radialSamples)
{
    ValidateCompact(compact);
    ValidateAccretion(p);

    if (radialSamples < 2U)
    {
        throw std::invalid_argument(
            "Accretion flow requires at least two radial samples.");
    }

    const auto scales =
        ResolveScales(compact);

    AccretionFlowProduct result{
        .fingerprint =
            AccretionFlowFingerprint(
                p,
                compact),
        .innerRadiusMeters =
            p.innerRadiusRg *
            scales.gravitationalRadiusMeters,
        .outerRadiusMeters =
            p.outerRadiusRg *
            scales.gravitationalRadiusMeters
    };

    result.radialProfile.reserve(
        radialSamples);

    for (u32 i = 0U;
         i < radialSamples;
         ++i)
    {
        const f64 t =
            static_cast<f64>(i) /
            static_cast<f64>(
                radialSamples - 1U);

        const f64 radiusRg =
            std::lerp(
                p.innerRadiusRg,
                p.outerRadiusRg,
                t);

        const f64 normalizedRadius =
            radiusRg /
            p.innerRadiusRg;

        const f64 emission =
            p.intensity *
            std::pow(
                normalizedRadius,
                -p.radialFalloffExponent) *
            std::max(
                1.0 -
                    std::sqrt(
                        p.innerRadiusRg /
                        radiusRg),
                0.0);

        result.radialProfile.push_back({
            .radiusRg = radiusRg,
            .normalizedEmission = emission
        });
    }

    return result;
}
} // namespace orbit::celestial_compact_objects
