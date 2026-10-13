#include <orbit/terrain_impacts/ImpactField.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace orbit::terrain_impacts
{
namespace
{
[[nodiscard]] u64 Mix64(u64 value) noexcept
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] f64 UnitFloat(const u64 value) noexcept
{
    constexpr f64 inverse53 =
        1.0 / static_cast<f64>(1ULL << 53U);
    return static_cast<f64>(value >> 11U) * inverse53;
}

[[nodiscard]] bool FinitePositive(const f64 value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool FiniteUnit(const f64 value) noexcept
{
    return std::isfinite(value) &&
        value >= 0.0 && value <= 1.0;
}

[[nodiscard]] f64 SmoothUnit(const f64 value) noexcept
{
    const f64 x = std::clamp(value, 0.0, 1.0);
    return x * x * x *
        (x * (x * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] math::Double3 DisplaceAcrossTectonicRenewal(
    const world::PlanetDefinition& planet,
    const ResurfacingRecord& event,
    const math::Double3& sourceDirection)
{
    if (event.kind != ResurfacingKind::TectonicRenewal ||
        event.displacementMeters == 0.0 ||
        math::LengthSquared(event.displacementUnitDirection) <= 1.0e-12)
        return sourceDirection;

    if (event.regionalPlateMotion)
    {
        f64 windingRadians = 0.0;
        for (std::size_t i = 1U; i < event.centerlineUnitDirections.size(); ++i)
        {
            math::Double3 a = event.centerlineUnitDirections[i - 1U] -
                sourceDirection * math::Dot(event.centerlineUnitDirections[i - 1U], sourceDirection);
            math::Double3 b = event.centerlineUnitDirections[i] -
                sourceDirection * math::Dot(event.centerlineUnitDirections[i], sourceDirection);
            if (math::LengthSquared(a) <= 1.0e-16 ||
                math::LengthSquared(b) <= 1.0e-16)
                return sourceDirection;
            a = math::Normalize(a);
            b = math::Normalize(b);
            windingRadians += std::atan2(
                math::Dot(sourceDirection, math::Cross(a, b)),
                math::Dot(a, b));
        }
        if (std::abs(windingRadians) <= std::numbers::pi_v<f64>)
            return sourceDirection;

        math::Double3 slip = event.displacementUnitDirection -
            sourceDirection * math::Dot(event.displacementUnitDirection, sourceDirection);
        if (math::LengthSquared(slip) <= 1.0e-12) return sourceDirection;
        slip = math::Normalize(slip);
        const f64 angle = event.displacementMeters / planet.radiusMeters;
        return math::Normalize(sourceDirection * std::cos(angle) + slip * std::sin(angle));
    }

    f64 closestDistance = std::numeric_limits<f64>::infinity();
    f64 closestSignedDistance = 0.0;
    for (std::size_t i = 1U; i < event.centerlineUnitDirections.size(); ++i)
    {
        const math::Double3& aDirection = event.centerlineUnitDirections[i - 1U];
        const math::Double3& bDirection = event.centerlineUnitDirections[i];
        const math::Double3 midpoint = math::Normalize(aDirection + bDirection);
        const world::SurfaceFrame frame = world::MakeSurfaceFrame(midpoint);
        const math::Double2 a = world::SurfaceOffsetBetweenDirections(
            planet, frame, aDirection);
        const math::Double2 b = world::SurfaceOffsetBetweenDirections(
            planet, frame, bDirection);
        const math::Double2 p = world::SurfaceOffsetBetweenDirections(
            planet, frame, sourceDirection);
        const math::Double2 edge = b - a;
        const f64 edgeLengthSquared = edge.x * edge.x + edge.y * edge.y;
        const f64 edgeLength = std::sqrt(edgeLengthSquared);
        const f64 t = edgeLengthSquared > 1.0e-9
            ? std::clamp(((p.x - a.x) * edge.x + (p.y - a.y) * edge.y) /
                edgeLengthSquared, 0.0, 1.0)
            : 0.5;
        const f64 nearestX = a.x + edge.x * t;
        const f64 nearestY = a.y + edge.y * t;
        const f64 dx = p.x - nearestX;
        const f64 dy = p.y - nearestY;
        const f64 distance = std::sqrt(dx * dx + dy * dy);
        if (distance < closestDistance)
        {
            closestDistance = distance;
            closestSignedDistance = edgeLength > 1.0e-6
                ? (edge.x * dy - edge.y * dx) / edgeLength
                : 0.0;
        }
    }

    const f64 halfWidth = event.widthMeters * 0.5;
    const f64 blend = std::max(halfWidth * 0.2, 1.0);
    const f64 fade = 1.0 - SmoothUnit(
        (closestDistance - halfWidth) / std::max(halfWidth * 2.0, 1.0));
    if (fade <= 0.0) return sourceDirection;
    const f64 side = std::tanh(closestSignedDistance / blend);
    const f64 offsetMeters = event.displacementMeters * 0.5 * side * fade;
    if (std::abs(offsetMeters) <= 1.0e-6) return sourceDirection;

    math::Double3 slip = event.displacementUnitDirection -
        sourceDirection * math::Dot(event.displacementUnitDirection, sourceDirection);
    if (math::LengthSquared(slip) <= 1.0e-12) return sourceDirection;
    slip = math::Normalize(slip);
    const f64 angle = offsetMeters / planet.radiusMeters;
    return math::Normalize(sourceDirection * std::cos(angle) + slip * std::sin(angle));
}

[[nodiscard]] f64 DistanceToPolylineMeters(
    const world::PlanetDefinition& planet,
    const ResurfacingRecord& event,
    const math::Double3& direction)
{
    f64 minimumDistance = std::numeric_limits<f64>::infinity();
    for (std::size_t i = 1U; i < event.centerlineUnitDirections.size(); ++i)
    {
        const math::Double3& aDirection = event.centerlineUnitDirections[i - 1U];
        const math::Double3& bDirection = event.centerlineUnitDirections[i];
        const math::Double3 midpoint = math::Normalize(aDirection + bDirection);
        const world::SurfaceFrame frame = world::MakeSurfaceFrame(midpoint);
        const math::Double2 a = world::SurfaceOffsetBetweenDirections(planet, frame, aDirection);
        const math::Double2 b = world::SurfaceOffsetBetweenDirections(planet, frame, bDirection);
        const math::Double2 p = world::SurfaceOffsetBetweenDirections(planet, frame, direction);
        const math::Double2 edge = b - a;
        const f64 lengthSquared = edge.x * edge.x + edge.y * edge.y;
        const f64 t = lengthSquared > 1.0e-9
            ? std::clamp(((p.x - a.x) * edge.x + (p.y - a.y) * edge.y) /
                lengthSquared, 0.0, 1.0)
            : 0.5;
        const f64 dx = p.x - (a.x + edge.x * t);
        const f64 dy = p.y - (a.y + edge.y * t);
        minimumDistance = std::min(minimumDistance, std::sqrt(dx * dx + dy * dy));
    }
    return minimumDistance;
}

[[nodiscard]] f64 FeatureWeight(
    const f64 diameterMeters,
    const f64 footprintMeters) noexcept
{
    if (footprintMeters <= 0.0)
    {
        return 1.0;
    }

    const f64 lower = footprintMeters * 2.0;
    const f64 upper = footprintMeters * 4.0;

    if (diameterMeters <= lower)
    {
        return 0.0;
    }

    if (diameterMeters >= upper)
    {
        return 1.0;
    }

    return SmoothUnit(
        (diameterMeters - lower) /
        (upper - lower));
}

[[nodiscard]] ImpactId ProceduralImpactId(
    const u64 seed,
    const u32 index) noexcept
{
    const u64 ordinal = static_cast<u64>(index) + 1ULL;
    return {
        .high = Mix64(
            seed ^
            (ordinal * 0xD6E8FEB86659FD93ULL) ^
            0x494D504143544849ULL),
        .low = Mix64(
            seed ^
            (ordinal * 0xA24BAED4963EE407ULL) ^
            0x494D504143544C4FULL)
    };
}

[[nodiscard]] ImpactId DerivedImpactId(
    const ImpactId parent,
    const u32 ordinal,
    const u64 salt) noexcept
{
    const u64 item = static_cast<u64>(ordinal) + 1ULL;
    return {
        .high = Mix64(parent.high ^ salt ^ item * 0xD6E8FEB86659FD93ULL),
        .low = Mix64(parent.low ^ salt ^ item * 0xA24BAED4963EE407ULL)
    };
}

[[nodiscard]] math::Double3 RandomDirection(
    const u64 seed,
    const u32 index) noexcept
{
    const u64 ordinal = static_cast<u64>(index) + 1ULL;

    const f64 u =
        UnitFloat(
            Mix64(seed ^
                ordinal * 0x9E3779B97F4A7C15ULL));
    const f64 v =
        UnitFloat(
            Mix64(seed ^
                ordinal * 0xBF58476D1CE4E5B9ULL));

    const f64 z = 1.0 - 2.0 * u;
    const f64 radial =
        std::sqrt(std::max(0.0, 1.0 - z * z));
    const f64 azimuth =
        2.0 * std::numbers::pi_v<f64> * v;

    return {
        radial * std::cos(azimuth),
        z,
        radial * std::sin(azimuth)
    };
}

[[nodiscard]] f64 SamplePowerLawRadius(
    const CraterSizeFrequencyDistribution& distribution,
    const u64 seed,
    const u32 index,
    const f64 maximumQuantile = 1.0) noexcept
{
    const f64 b = distribution.cumulativeExponent;
    const f64 minimum = distribution.minimumRadiusMeters;
    const f64 maximum = distribution.maximumRadiusMeters;

    const f64 random = UnitFloat(
            Mix64(
                seed ^
                (static_cast<u64>(index) + 1ULL) *
                    0x94D049BB133111EBULL ^
                0x5241444955534D37ULL));
    const f64 u = maximumQuantile >= 1.0
        ? random
        : 1.0 - maximumQuantile * (1.0 - random);

    const f64 minPower = std::pow(minimum, -b);
    const f64 maxPower = std::pow(maximum, -b);
    const f64 value =
        minPower +
        (maxPower - minPower) * u;

    return std::pow(
        std::max(value, maxPower),
        -1.0 / b);
}

[[nodiscard]] f64 PowerLawRadiusAtQuantile(
    const CraterSizeFrequencyDistribution& distribution,
    const f64 quantile) noexcept
{
    const f64 exponent = distribution.cumulativeExponent;
    const f64 minPower = std::pow(distribution.minimumRadiusMeters, -exponent);
    const f64 maxPower = std::pow(distribution.maximumRadiusMeters, -exponent);
    const f64 value = minPower + (maxPower - minPower) *
        std::clamp(quantile, 0.0, 1.0);
    return std::pow(std::max(value, maxPower), -1.0 / exponent);
}

[[nodiscard]] CraterProfileKind ResolveProfile(
    const ImpactRecord& impact,
    const ImpactFieldDefinition& definition) noexcept
{
    if (impact.profile != CraterProfileKind::Auto)
    {
        return impact.profile;
    }

    return impact.radiusMeters >=
            definition.complexTransitionRadiusMeters *
                std::sqrt(1.62 / definition.surfaceGravityMetersPerSecondSquared)
        ? CraterProfileKind::Complex
        : CraterProfileKind::Simple;
}

[[nodiscard]] f64 EjectaMassBalanceScale(
    const ImpactRecord& impact,
    const ImpactFieldDefinition& definition) noexcept
{
    // Integrate the excavation bowl and the analytic ejecta blanket in
    // dimensionless polar area. The rim and central rebound consume part of
    // the displaced volume; remaining material is normalized into the ejecta
    // distribution. This is a coarse local conservation model, not a ballistic
    // excavation simulation.
    constexpr u32 kIntegrationSteps = 192U;
    const CraterProfileKind profile = ResolveProfile(impact, definition);
    const f64 depthRatio = profile == CraterProfileKind::Complex
        ? impact.complexDepthRatio
        : impact.simpleDepthRatio;
    const f64 excavationVolumeNormalized = depthRatio / 6.0;

    f64 rimIntegral = 0.0;
    f64 ejectaIntegral = 0.0;
    const f64 maximumX = std::max(impact.ejectaExtentRadii, 1.0);
    const f64 step = maximumX / static_cast<f64>(kIntegrationSteps);
    for (u32 i = 0U; i < kIntegrationSteps; ++i)
    {
        const f64 x = (static_cast<f64>(i) + 0.5) * step;
        const f64 rimDistance = (x - 1.0) / 0.10;
        rimIntegral += x * std::exp(-0.5 * rimDistance * rimDistance) * step;
        if (x >= 1.0)
        {
            const f64 normalizedExtent = std::clamp(
                (x - 1.0) / std::max(impact.ejectaExtentRadii - 1.0, 1.0e-9),
                0.0, 1.0);
            ejectaIntegral += std::pow(x, -2.0) *
                (1.0 - SmoothUnit(normalizedExtent)) * step;
        }
    }
    const f64 rimVolumeNormalized = impact.rimHeightRatio * rimIntegral;
    f64 reboundVolumeNormalized = 0.0;
    if (profile == CraterProfileKind::Complex)
    {
        const f64 transitionRadius = definition.complexTransitionRadiusMeters *
            std::sqrt(1.62 / definition.surfaceGravityMetersPerSecondSquared);
        const f64 peakRatio = std::clamp(
            0.025 + 0.02 * impact.radiusMeters / std::max(transitionRadius, 1.0),
            0.025, 0.085);
        reboundVolumeNormalized = std::min(
            peakRatio, depthRatio * 0.85) * (0.28 * 0.28 / 12.0);
    }
    f64 meanRayGain = 0.0;
    if (impact.rayCount > 0U && impact.rayStrength > 0.0)
    {
        // Exact angular mean of max(cos(theta), 0)^8. For distorted rays this
        // remains the same approximate volume normalization used by the fit.
        meanRayGain = 35.0 / 256.0;
    }
    const f64 ejectaVolumeNormalized = impact.ejectaThicknessRatio *
        ejectaIntegral * (1.0 + impact.rayStrength * meanRayGain);
    if (ejectaVolumeNormalized <= 1.0e-12) return 1.0;
    const f64 remaining = std::max(
        excavationVolumeNormalized - rimVolumeNormalized - reboundVolumeNormalized,
        0.0);
    return std::clamp(remaining / ejectaVolumeNormalized, 0.0, 12.0);
}

[[nodiscard]] f64 RayModulation(
    const ImpactRecord& impact,
    const f64 phase,
    const math::Double2& offset,
    const f64 normalizedRadius) noexcept
{
    if (impact.rayStrength <= 0.0 ||
        impact.rayCount == 0)
    {
        return 0.0;
    }

    if (!std::isfinite(offset.x) ||
        !std::isfinite(offset.y))
    {
        return 0.0;
    }

    const f64 azimuth =
        std::atan2(offset.y, offset.x);
    // Smooth, periodic angular distortion avoids face seams and keeps the
    // ray curve coherent along its length instead of adding per-texel noise.
    const f64 distortion = impact.rayIrregularity * (
        0.65 * std::sin(3.0 * azimuth + normalizedRadius * 0.45 + phase) +
        0.35 * std::sin(5.0 * azimuth - normalizedRadius * 0.23 - phase));

    const f64 ray =
        std::max(
            0.0,
            std::cos(
                (azimuth + distortion) *
                    static_cast<f64>(impact.rayCount) +
                phase));

    return impact.rayStrength *
        std::pow(ray, 8.0);
}

[[nodiscard]] CraterProcessSample SampleImpact(
    const ImpactRecord& impact,
    const ImpactFieldDefinition& definition,
    const world::PlanetDefinition& planet,
    const PreparedImpactGeometry& prepared,
    const math::Double3& positionDirection,
    const f64 footprintDiameterMeters) noexcept
{
    CraterProcessSample result{};

    if (!impact.enabled)
    {
        return result;
    }

    const f64 spectralWeight =
        FeatureWeight(
            impact.radiusMeters * 2.0,
            footprintDiameterMeters);

    if (spectralWeight <= 0.0)
    {
        return result;
    }

    const world::SurfaceFrame& impactFrame = prepared.frame;
    const f64 impactPhase = prepared.phase;
    const math::Double3& center = impactFrame.up;

    const f64 cosine =
        std::clamp(
            math::Dot(
                center,
                positionDirection),
            -1.0,
            1.0);

    if (cosine < prepared.influenceCosine)
    {
        return result;
    }

    const math::Double2 offset =
        world::SurfaceOffsetBetweenDirections(
            planet, impactFrame, positionDirection);
    const f64 along = offset.x * prepared.azimuthCosine +
        offset.y * prepared.azimuthSine;
    const f64 across = -offset.x * prepared.azimuthSine +
        offset.y * prepared.azimuthCosine;
    const f64 elongation = prepared.elongation;
    f64 x = std::sqrt(
        (along / (impact.radiusMeters * elongation)) *
            (along / (impact.radiusMeters * elongation)) +
        (across / impact.radiusMeters) *
            (across / impact.radiusMeters));
    if (x > 1.0e-6 && impact.shapeIrregularity > 0.0)
    {
        const f64 shapeAngle = std::atan2(across, along);
        const f64 angularNoise =
            0.65 * std::cos(3.0 * shapeAngle + impactPhase) +
            0.35 * std::cos(5.0 * shapeAngle - impactPhase * 0.7);
        x /= std::max(
            1.0 + impact.shapeIrregularity * angularNoise,
            0.75);
    }

    if (x > impact.InfluenceExtentRadii())
    {
        return result;
    }

    f64 degradationRatePerMillionYears = 0.0012;
    switch (definition.environment)
    {
    case SurfaceEnvironment::Airless: degradationRatePerMillionYears = 0.0012; break;
    case SurfaceEnvironment::Wet: degradationRatePerMillionYears = 0.012; break;
    case SurfaceEnvironment::Icy: degradationRatePerMillionYears = 0.004; break;
    case SurfaceEnvironment::GeologicallyActive: degradationRatePerMillionYears = 0.05; break;
    }
    const f64 elapsedAgeYears = definition.surfaceAgeYears > 0.0
        ? std::max(definition.surfaceAgeYears - impact.formationAgeYears, 0.0)
        : impact.formationAgeYears;
    const f64 ageMyr = elapsedAgeYears / 1'000'000.0;
    const f64 agePreservation = std::exp(
        -degradationRatePerMillionYears * ageMyr);
    const f64 effectiveDegradation = 1.0 -
        (1.0 - impact.degradation) * agePreservation;
    const f64 preservation =
        (1.0 - effectiveDegradation) * spectralWeight;

    if (preservation <= 0.0)
    {
        return result;
    }

    const CraterProfileKind profile =
        ResolveProfile(impact, definition);

    f64 excavation = 0.0;
    f64 craterDelta = 0.0;

    if (x < 1.0)
    {
        const f64 bowl =
            std::max(0.0, 1.0 - x * x);
        const f64 bowlShape =
            bowl * bowl;

        const f64 depthRatio =
            profile == CraterProfileKind::Complex
                ? impact.complexDepthRatio
                : impact.simpleDepthRatio;

        excavation =
            impact.radiusMeters *
            depthRatio *
            bowlShape;

        craterDelta = -excavation;

        if (profile == CraterProfileKind::Complex)
        {
            const f64 peakRadius = 0.28;
            const f64 transitionRadius =
                definition.complexTransitionRadiusMeters *
                std::sqrt(1.62 / definition.surfaceGravityMetersPerSecondSquared);
            const f64 peakRatio = std::clamp(
                0.025 + 0.02 * impact.radiusMeters /
                    std::max(transitionRadius, 1.0),
                0.025,
                0.085);
            if (x < peakRadius)
            {
                const f64 peakX =
                    1.0 - x / peakRadius;
                const f64 centralPeak =
                    impact.radiusMeters *
                    std::min(peakRatio, depthRatio * 0.85) *
                    peakX * peakX;
                craterDelta += centralPeak;
            }

            if (x > 0.62)
            {
                const f64 terracePhase =
                    (x - 0.62) / 0.38;
                craterDelta +=
                    impact.radiusMeters *
                    0.012 * std::clamp(
                        std::sqrt(1.62 / definition.surfaceGravityMetersPerSecondSquared),
                        0.65,
                        1.4) *
                    std::sin(
                        terracePhase *
                        3.0 *
                        std::numbers::pi_v<f64>) *
                    (1.0 - terracePhase);
            }
        }
    }

    const f64 rimWidth = 0.10;
    const f64 rimDistance =
        (x - 1.0) / rimWidth;
    const f64 rim = x <= impact.ejectaExtentRadii ?
        impact.radiusMeters *
        impact.rimHeightRatio *
        std::exp(
            -0.5 *
            rimDistance *
            rimDistance) : 0.0;

    if (impact.multiringStrength > 0.0 &&
        impact.radiusMeters >=
            5.0 * definition.complexTransitionRadiusMeters &&
        x >= 1.0 && x <= impact.ejectaExtentRadii)
    {
        const f64 ringPhase = (x - 1.0) *
            (2.0 * std::numbers::pi_v<f64> / 1.35);
        craterDelta += impact.radiusMeters *
            impact.multiringStrength * 0.012 *
            std::cos(ringPhase) * std::exp(-(x - 1.0) * 0.45);
    }

    f64 ejecta = 0.0;
    f64 rayField = 0.0;
    const f64 angularRay = x >= 1.0
        ? RayModulation(impact, impactPhase, offset, x) : 0.0;
    if (impact.rayExtentRadii > 0.0 && x >= 1.0 && x <= impact.rayExtentRadii)
    {
        rayField = angularRay * (1.0 - SmoothUnit(
            (x - 1.0) / std::max(impact.rayExtentRadii - 1.0, 1.0e-9)));
    }

    if (x >= 1.0 &&
        x <= impact.ejectaExtentRadii)
    {
        const f64 normalizedExtent =
            std::clamp(
                (x - 1.0) /
                std::max(
                    impact.ejectaExtentRadii - 1.0,
                    1.0e-9),
                0.0,
                1.0);

        const f64 outerFade =
            1.0 - SmoothUnit(normalizedExtent);

        ejecta =
            impact.radiusMeters *
            impact.ejectaThicknessRatio *
            std::pow(std::max(x, 1.0), -3.0) *
            outerFade * prepared.ejectaMassBalanceScale;
        if (impact.rayExtentRadii == 0.0) rayField = angularRay;
        ejecta *= 1.0 + angularRay;
    }

    result.heightDeltaMeters =
        (craterDelta + rim + ejecta) *
        preservation;

    result.excavationCoverage = x < 1.0
        ? std::clamp(
              std::max(0.0, 1.0 - x * x) *
                  std::max(0.0, 1.0 - x * x) * preservation,
              0.0,
              1.0)
        : 0.0;
    result.formationAgeOrder = impact.ageOrder;
    result.formationAgeYears = impact.formationAgeYears;
    if (result.excavationCoverage > 0.01 || ejecta > 0.0 || rayField > 0.0)
    {
        result.exposureAgeOrder = impact.ageOrder;
        result.exposureAgeYears = elapsedAgeYears;
    }

    result.excavationDepthMeters =
        excavation * preservation;

    result.ejectaThicknessMeters =
        ejecta * preservation;

    result.meltThicknessMeters =
        excavation * impact.meltFraction * preservation;

    result.rayField =
        rayField * preservation;

    result.debrisField =
        std::clamp(
            (result.ejectaThicknessMeters +
             rim * preservation * 0.35) /
                std::max(
                    impact.radiusMeters *
                        std::max(
                            impact.ejectaThicknessRatio,
                            0.001),
                    1.0),
            0.0,
            1.0);
    result.brecciaField = std::clamp(
        result.debrisField * impact.brecciaFraction,
        0.0,
        1.0);

    result.affectingImpacts = 1;
    return result;
}

void Accumulate(
    CraterProcessSample& destination,
    const CraterProcessSample& contribution) noexcept
{
    // A later impact excavates the terrain already present. Attenuate the
    // earlier compiled surface inside its cavity before adding the new event;
    // this preserves chronological overlap without a planetary resimulation.
    const f64 retained = 1.0 - contribution.excavationCoverage;
    destination.heightDeltaMeters =
        destination.heightDeltaMeters * retained +
        contribution.heightDeltaMeters;
    destination.excavationDepthMeters =
        destination.excavationDepthMeters * retained +
        contribution.excavationDepthMeters;
    destination.ejectaThicknessMeters =
        destination.ejectaThicknessMeters * retained +
        contribution.ejectaThicknessMeters;
    destination.meltThicknessMeters =
        destination.meltThicknessMeters * retained +
        contribution.meltThicknessMeters;
    destination.brecciaField = std::clamp(
        destination.brecciaField * retained + contribution.brecciaField,
        0.0,
        1.0);
    destination.resurfacingThicknessMeters *= retained;
    destination.resurfacedMaterialFraction *= retained;
    destination.debrisField =
        std::clamp(
            destination.debrisField * retained +
                contribution.debrisField,
            0.0,
            1.0);
    destination.rayField =
        std::clamp(
            destination.rayField * retained +
                contribution.rayField,
            0.0,
            1.0);
    if (destination.affectingImpacts == 0U ||
        contribution.excavationCoverage >= 0.95)
    {
        destination.formationAgeOrder =
            contribution.formationAgeOrder;
        destination.formationAgeYears =
            contribution.formationAgeYears;
    }
    if (contribution.exposureAgeOrder != 0U ||
        contribution.excavationCoverage > 0.01 ||
        contribution.ejectaThicknessMeters > 0.0 || contribution.rayField > 0.0)
    {
        destination.exposureAgeOrder =
            contribution.exposureAgeOrder;
        destination.exposureAgeYears =
            contribution.exposureAgeYears;
    }
    destination.excavationCoverage =
        1.0 - (1.0 - destination.excavationCoverage) * retained;
    destination.affectingImpacts += contribution.affectingImpacts;
}
} // namespace

f64 ScaleImpactCraterRadiusMeters(const ImpactScalingInput& input)
{
    const auto positive = [](const f64 value)
    {
        return std::isfinite(value) && value > 0.0;
    };
    if (!positive(input.impactorDiameterMeters) ||
        !positive(input.impactVelocityMetersPerSecond) ||
        !positive(input.impactorDensityKgPerCubicMeter) ||
        !positive(input.targetDensityKgPerCubicMeter) ||
        !positive(input.surfaceGravityMetersPerSecondSquared) ||
        !std::isfinite(input.targetStrengthPascals) ||
        input.targetStrengthPascals < 0.0 ||
        !std::isfinite(input.impactAngleDegrees) ||
        input.impactAngleDegrees < 0.0 || input.impactAngleDegrees > 90.0)
    {
        throw std::invalid_argument("Impact pi-scaling inputs are not physical.");
    }

    const f64 diameter = input.impactorDiameterMeters;
    const f64 velocitySquared =
        input.impactVelocityMetersPerSecond *
        input.impactVelocityMetersPerSecond;
    const f64 gravityTerm =
        input.surfaceGravityMetersPerSecondSquared * diameter /
        velocitySquared;
    const f64 strengthTerm = input.targetStrengthPascals /
        (input.targetDensityKgPerCubicMeter * velocitySquared);
    const f64 angle = input.impactAngleDegrees *
        (std::numbers::pi_v<f64> / 180.0);
    const f64 incidence = std::pow(
        std::max(std::cos(angle), 0.05), 1.0 / 3.0);
    const f64 transientDiameter = 1.6 * diameter *
        std::cbrt(input.impactorDensityKgPerCubicMeter /
                  input.targetDensityKgPerCubicMeter) *
        std::pow(std::max(
            gravityTerm + std::sqrt(std::max(strengthTerm, 0.0)),
            1.0e-12), -0.22) * incidence;
    const f64 finalDiameter = transientDiameter * 1.25;
    if (!std::isfinite(finalDiameter) || finalDiameter <= 0.0)
    {
        throw std::invalid_argument("Impact pi-scaling result is not finite.");
    }
    return finalDiameter * 0.5;
}

std::optional<std::vector<GeologicalInfluenceCap>> ChangedAuthoredEventInfluenceCaps(
    const world::PlanetDefinition& planet,
    const ImpactFieldDefinition& before,
    const ImpactFieldDefinition& after)
{
    if (!(planet.radiusMeters > 0.0) || !std::isfinite(planet.radiusMeters))
        return std::nullopt;

    const auto baseSignature = [](ImpactFieldDefinition definition)
    {
        definition.authoredImpacts.clear();
        definition.resurfacingEvents.clear();
        return SerializeImpactFieldToml(definition);
    };
    if (baseSignature(before) != baseSignature(after)) return std::nullopt;

    using EventKey = std::pair<u64, u64>;
    std::map<EventKey, const ImpactRecord*> oldImpacts;
    std::map<EventKey, const ImpactRecord*> newImpacts;
    for (const ImpactRecord& event : before.authoredImpacts)
        oldImpacts[{event.id.high, event.id.low}] = &event;
    for (const ImpactRecord& event : after.authoredImpacts)
        newImpacts[{event.id.high, event.id.low}] = &event;

    std::vector<GeologicalInfluenceCap> caps;
    const auto impactSignature = [](const ImpactFieldDefinition& base,
                                    const ImpactRecord& event)
    {
        ImpactFieldDefinition single = base;
        single.resurfacingEvents.clear();
        single.authoredImpacts.assign(1U, event);
        return SerializeImpactFieldToml(single);
    };
    const auto appendImpactCap = [&](const ImpactFieldDefinition& recipe,
                                     const ImpactRecord& event)
    {
        const f64 radius = event.impactorDiameterMeters > 0.0
            ? ScaleImpactCraterRadiusMeters({
                .impactorDiameterMeters = event.impactorDiameterMeters,
                .impactVelocityMetersPerSecond = event.impactVelocityMetersPerSecond,
                .impactAngleDegrees = event.impactAngleDegrees,
                .impactorDensityKgPerCubicMeter = event.impactorDensityKgPerCubicMeter,
                .targetDensityKgPerCubicMeter = recipe.targetDensityKgPerCubicMeter,
                .surfaceGravityMetersPerSecondSquared = recipe.surfaceGravityMetersPerSecondSquared,
                .targetStrengthPascals = recipe.targetStrengthPascals})
            : event.radiusMeters;
        const f64 profileBound = 1.65 /
            std::max(1.0 - event.shapeIrregularity, 0.75) * 1.65;
        const f64 extent = event.InfluenceExtentRadii();
        const f64 main = profileBound * extent;
        const f64 binary = event.binarySeparationRadii + profileBound * extent *
            std::max(event.binaryCompanionRadiusRatio, 0.0);
        const f64 secondaries = event.secondaryCount > 0U
            ? 5.5 + 1.8 * profileBound *
                std::max(event.secondaryRadiusRatio, 0.0) * extent
            : 0.0;
        const f64 influenceRadius = radius * std::max({main, binary, secondaries});
        math::Double3 presentCenter = math::Normalize(event.centerUnitDirection);
        std::vector<const ResurfacingRecord*> laterRenewals;
        for (const ResurfacingRecord& renewal : recipe.resurfacingEvents)
        {
            const bool afterImpact = renewal.ageOrder > event.ageOrder ||
                (renewal.ageOrder == event.ageOrder &&
                 (renewal.id.high > event.id.high ||
                  (renewal.id.high == event.id.high && renewal.id.low > event.id.low)));
            if (afterImpact && renewal.kind == ResurfacingKind::TectonicRenewal &&
                renewal.displacementMeters != 0.0)
                laterRenewals.push_back(&renewal);
        }
        std::stable_sort(laterRenewals.begin(), laterRenewals.end(),
            [](const ResurfacingRecord* a, const ResurfacingRecord* b)
            {
                if (a->ageOrder != b->ageOrder) return a->ageOrder < b->ageOrder;
                if (a->id.high != b->id.high) return a->id.high < b->id.high;
                return a->id.low < b->id.low;
            });
        for (const ResurfacingRecord* renewal : laterRenewals)
            presentCenter = DisplaceAcrossTectonicRenewal(planet, *renewal, presentCenter);
        caps.push_back({presentCenter,
            std::min(influenceRadius / planet.radiusMeters, std::numbers::pi_v<f64>)});
    };
    for (const auto& [key, oldEvent] : oldImpacts)
    {
        const auto found = newImpacts.find(key);
        if (found != newImpacts.end() &&
            impactSignature(before, *oldEvent) == impactSignature(after, *found->second))
            continue;
        appendImpactCap(before, *oldEvent);
        if (found != newImpacts.end()) appendImpactCap(after, *found->second);
    }
    for (const auto& [key, newEvent] : newImpacts)
        if (!oldImpacts.contains(key)) appendImpactCap(after, *newEvent);

    std::map<EventKey, const ResurfacingRecord*> oldFlows;
    std::map<EventKey, const ResurfacingRecord*> newFlows;
    for (const ResurfacingRecord& event : before.resurfacingEvents)
        oldFlows[{event.id.high, event.id.low}] = &event;
    for (const ResurfacingRecord& event : after.resurfacingEvents)
        newFlows[{event.id.high, event.id.low}] = &event;
    const auto flowSignature = [](const ImpactFieldDefinition& base,
                                  const ResurfacingRecord& event)
    {
        ImpactFieldDefinition single = base;
        single.authoredImpacts.clear();
        single.resurfacingEvents.assign(1U, event);
        return SerializeImpactFieldToml(single);
    };
    const auto appendFlowCaps = [&](const ResurfacingRecord& event)
    {
        if (event.centerlineUnitDirections.empty()) return;
        const f64 widthAngle = std::min(
            event.widthMeters * 2.0 / planet.radiusMeters, std::numbers::pi_v<f64>);
        const auto append = [&](const math::Double3& center, const f64 segmentRadius)
        {
            caps.push_back({math::Normalize(center), std::min(
                widthAngle + segmentRadius, std::numbers::pi_v<f64>)});
        };
        for (const auto& direction : event.centerlineUnitDirections) append(direction, 0.0);
        for (std::size_t i = 1U; i < event.centerlineUnitDirections.size(); ++i)
        {
            const auto& a = event.centerlineUnitDirections[i - 1U];
            const auto& b = event.centerlineUnitDirections[i];
            const f64 segmentRadius = 0.5 * std::acos(std::clamp(
                math::Dot(math::Normalize(a), math::Normalize(b)), -1.0, 1.0));
            const math::Double3 midpoint = a + b;
            append(math::LengthSquared(midpoint) > 1.0e-12 ? midpoint : a,
                segmentRadius);
        }
    };
    for (const auto& [key, oldEvent] : oldFlows)
    {
        const auto found = newFlows.find(key);
        if (found != newFlows.end() &&
            flowSignature(before, *oldEvent) == flowSignature(after, *found->second))
            continue;
        appendFlowCaps(*oldEvent);
        if (found != newFlows.end()) appendFlowCaps(*found->second);
    }
    for (const auto& [key, newEvent] : newFlows)
        if (!oldFlows.contains(key)) appendFlowCaps(*newEvent);

    for (const auto& [key, oldEvent] : oldFlows)
    {
        const auto found = newFlows.find(key);
        const ResurfacingRecord* newEvent = found == newFlows.end()
            ? nullptr : found->second;
        const bool hasDisplacement =
            (oldEvent->kind == ResurfacingKind::TectonicRenewal &&
             oldEvent->displacementMeters != 0.0) ||
            (newEvent != nullptr && newEvent->kind == ResurfacingKind::TectonicRenewal &&
             newEvent->displacementMeters != 0.0);
        if (!hasDisplacement) continue;
        if (newEvent == nullptr || oldEvent->kind != newEvent->kind ||
            flowSignature(before, *oldEvent) != flowSignature(after, *newEvent) ||
            oldEvent->displacementMeters != newEvent->displacementMeters ||
            oldEvent->displacementUnitDirection.x != newEvent->displacementUnitDirection.x ||
            oldEvent->displacementUnitDirection.y != newEvent->displacementUnitDirection.y ||
            oldEvent->displacementUnitDirection.z != newEvent->displacementUnitDirection.z)
            return std::nullopt;
    }
    for (const auto& [key, newEvent] : newFlows)
        if (!oldFlows.contains(key) &&
            newEvent->kind == ResurfacingKind::TectonicRenewal &&
            newEvent->displacementMeters != 0.0)
            return std::nullopt;
    return caps;
}

bool CraterSizeFrequencyDistribution::IsValid() const noexcept
{
    return
        count <= 10'000'000U &&
        FinitePositive(minimumRadiusMeters) &&
        FinitePositive(maximumRadiusMeters) &&
        maximumRadiusMeters >= minimumRadiusMeters &&
        std::isfinite(cumulativeExponent) &&
        cumulativeExponent > 0.0;
}

bool ImpactRecord::IsValid() const noexcept
{
    const bool hasScaling = impactorDiameterMeters != 0.0 ||
        impactVelocityMetersPerSecond != 0.0 ||
        impactorDensityKgPerCubicMeter != 0.0;
    const bool validScaling = !hasScaling ||
        (FinitePositive(impactorDiameterMeters) &&
         FinitePositive(impactVelocityMetersPerSecond) &&
         FinitePositive(impactorDensityKgPerCubicMeter));
    return
        id.IsValid() &&
        std::isfinite(centerUnitDirection.x) &&
        std::isfinite(centerUnitDirection.y) &&
        std::isfinite(centerUnitDirection.z) &&
        math::LengthSquared(centerUnitDirection) > 0.0 &&
        (FinitePositive(radiusMeters) || hasScaling) &&
        validScaling &&
        FinitePositive(simpleDepthRatio) &&
        FinitePositive(complexDepthRatio) &&
        FinitePositive(rimHeightRatio) &&
        FinitePositive(ejectaThicknessRatio) &&
        std::isfinite(ejectaExtentRadii) &&
        ejectaExtentRadii >= 1.0 &&
        std::isfinite(rayStrength) &&
        rayStrength >= 0.0 &&
        rayStrength <= 4.0 &&
        rayCount <= 64U &&
        std::isfinite(rayExtentRadii) &&
        (rayExtentRadii == 0.0 ||
         (rayExtentRadii > 1.0 && rayExtentRadii <= 100.0)) &&
        FiniteUnit(rayIrregularity) &&
        FiniteUnit(degradation) &&
        std::isfinite(formationAgeYears) && formationAgeYears >= 0.0 &&
        std::isfinite(impactAngleDegrees) &&
        impactAngleDegrees >= 0.0 && impactAngleDegrees <= 89.0 &&
        std::isfinite(impactAzimuthRadians) &&
        std::isfinite(shapeIrregularity) &&
        shapeIrregularity >= 0.0 && shapeIrregularity <= 0.25 &&
        FiniteUnit(meltFraction) && FiniteUnit(brecciaFraction) &&
        FiniteUnit(multiringStrength) &&
        std::isfinite(binarySeparationRadii) &&
        binarySeparationRadii >= 0.0 && binarySeparationRadii <= 20.0 &&
        FiniteUnit(binaryCompanionRadiusRatio) &&
        (binarySeparationRadii == 0.0 || binaryCompanionRadiusRatio > 0.0) &&
        std::isfinite(binaryAzimuthRadians) &&
        secondaryCount <= 16U &&
        std::isfinite(secondaryRadiusRatio) &&
        secondaryRadiusRatio >= 0.01 && secondaryRadiusRatio <= 0.25 &&
        FiniteUnit(secondaryRayAlignment);
}

f64 ImpactRecord::InfluenceExtentRadii() const noexcept
{
    return std::max(std::max(ejectaExtentRadii, 1.0),
        rayStrength > 0.0 && rayCount > 0U ? rayExtentRadii : 0.0);
}

bool ResurfacingRecord::IsValid() const noexcept
{
    if (!id.IsValid() || centerlineUnitDirections.size() < 2U ||
        centerlineUnitDirections.size() > 4096U ||
        !FinitePositive(widthMeters) ||
        !std::isfinite(thicknessMeters) || thicknessMeters < 0.0 ||
        !std::isfinite(formationAgeYears) || formationAgeYears < 0.0 ||
        !std::isfinite(displacementUnitDirection.x) ||
        !std::isfinite(displacementUnitDirection.y) ||
        !std::isfinite(displacementUnitDirection.z) ||
        !std::isfinite(displacementMeters) || std::abs(displacementMeters) > 1.0e9 ||
        static_cast<u8>(kind) > static_cast<u8>(ResurfacingKind::TectonicRenewal))
    {
        return false;
    }
    if (std::abs(displacementMeters) > 0.0 &&
        (!std::isfinite(displacementUnitDirection.x) ||
         !std::isfinite(displacementUnitDirection.y) ||
         !std::isfinite(displacementUnitDirection.z) ||
         math::LengthSquared(displacementUnitDirection) <= 1.0e-12 ||
         kind != ResurfacingKind::TectonicRenewal))
        return false;
    if (regionalPlateMotion &&
        (kind != ResurfacingKind::TectonicRenewal || displacementMeters == 0.0 ||
         centerlineUnitDirections.size() < 4U ||
         math::Dot(centerlineUnitDirections.front(), centerlineUnitDirections.back()) < 0.999999))
        return false;
    return std::all_of(centerlineUnitDirections.begin(),
        centerlineUnitDirections.end(), [](const math::Double3& direction)
        {
            return std::isfinite(direction.x) && std::isfinite(direction.y) &&
                std::isfinite(direction.z) && math::LengthSquared(direction) > 0.0;
        });
}

bool ImpactFieldDefinition::IsValid() const noexcept
{
    if (!id.IsValid() ||
        !planet.IsValid() ||
        name.empty() ||
        !procedural.IsValid() ||
        !FinitePositive(complexTransitionRadiusMeters) ||
        !std::isfinite(surfaceAgeYears) || surfaceAgeYears < 0.0 ||
        !FinitePositive(surfaceGravityMetersPerSecondSquared) ||
        !FinitePositive(targetDensityKgPerCubicMeter) ||
        !std::isfinite(targetStrengthPascals) || targetStrengthPascals < 0.0 ||
        static_cast<u8>(environment) >
            static_cast<u8>(SurfaceEnvironment::GeologicallyActive) ||
        resurfacingEvents.size() > 100'000U)
    {
        return false;
    }

    std::unordered_set<ImpactId> ids;
    ids.reserve(authoredImpacts.size());

    if (iceFractures != nullptr && !iceFractures->IsValid())
    {
        return false;
    }

    for (const ImpactRecord& impact : authoredImpacts)
    {
        if (!impact.IsValid() ||
            !ids.insert(impact.id).second)
        {
            return false;
        }
    }

    u64 resurfacingPointCount = 0U;
    for (const ResurfacingRecord& event : resurfacingEvents)
    {
        resurfacingPointCount += event.centerlineUnitDirections.size();
        if (resurfacingPointCount > 1'000'000U) return false;
        if (!event.IsValid() || !ids.insert(event.id).second)
        {
            return false;
        }
    }

    return true;
}

ImpactField::ImpactField(
    const world::PlanetDefinition planet,
    ImpactFieldDefinition definition)
    : planet_(planet),
      definition_(std::move(definition))
{
    if (!planet_.id.IsValid() ||
        !FinitePositive(planet_.radiusMeters))
    {
        throw std::invalid_argument(
            "M07 ImpactField requires a valid planet.");
    }

    for (ImpactRecord& impact : definition_.authoredImpacts)
    {
        if (impact.impactorDiameterMeters > 0.0)
        {
            impact.radiusMeters = ScaleImpactCraterRadiusMeters({
                .impactorDiameterMeters = impact.impactorDiameterMeters,
                .impactVelocityMetersPerSecond = impact.impactVelocityMetersPerSecond,
                .impactAngleDegrees = impact.impactAngleDegrees,
                .impactorDensityKgPerCubicMeter = impact.impactorDensityKgPerCubicMeter,
                .targetDensityKgPerCubicMeter = definition_.targetDensityKgPerCubicMeter,
                .surfaceGravityMetersPerSecondSquared = definition_.surfaceGravityMetersPerSecondSquared,
                .targetStrengthPascals = definition_.targetStrengthPascals
            });
        }
    }

    if (!definition_.IsValid())
    {
        throw std::invalid_argument(
            "M07 ImpactFieldDefinition is invalid.");
    }

    if (definition_.planet != planet_.id)
    {
        throw std::invalid_argument(
            "M07 impact authority belongs to another planet.");
    }

    for (ResurfacingRecord& event : definition_.resurfacingEvents)
    {
        for (math::Double3& direction : event.centerlineUnitDirections)
        {
            direction = math::Normalize(direction);
        }
    }
    std::stable_sort(definition_.resurfacingEvents.begin(),
        definition_.resurfacingEvents.end(),
        [](const ResurfacingRecord& a, const ResurfacingRecord& b)
        {
            if (a.ageOrder != b.ageOrder) return a.ageOrder < b.ageOrder;
            if (a.id.high != b.id.high) return a.id.high < b.id.high;
            return a.id.low < b.id.low;
        });

    for (ResurfacingRecord& event : definition_.resurfacingEvents)
    {
        if (event.displacementMeters != 0.0)
            event.displacementUnitDirection =
                math::Normalize(event.displacementUnitDirection);
    }
    BuildResurfacingSpatialIndex();

    const u64 seed =
        definition_.seed != 0
            ? definition_.seed
            : Mix64(
                planet_.generationSeed ^
                0x494D504143544D37ULL);

    constexpr u32 maximumExplicitPopulation = 100'000U;
    const u32 explicitProceduralCount = std::min(
        definition_.procedural.count, maximumExplicitPopulation);
    statisticalMicroImpactCount_ =
        definition_.procedural.count - explicitProceduralCount;
    const f64 explicitQuantile = definition_.procedural.count > 0U
        ? static_cast<f64>(explicitProceduralCount) /
            static_cast<f64>(definition_.procedural.count)
        : 0.0;
    statisticalMicroImpactMaximumRadiusMeters_ =
        statisticalMicroImpactCount_ > 0U
            ? PowerLawRadiusAtQuantile(
                  definition_.procedural, 1.0 - explicitQuantile)
            : 0.0;
    resolvedImpacts_.reserve(
        static_cast<std::size_t>(explicitProceduralCount) +
        definition_.authoredImpacts.size());

    for (u32 index = 0;
         index < explicitProceduralCount;
         ++index)
    {
        const u64 random =
            Mix64(
                seed ^
                static_cast<u64>(index + 1U) *
                    0xD6E8FEB86659FD93ULL);

        const f64 degradation =
            UnitFloat(
                Mix64(
                    random ^
                    0x4445475241444537ULL)) *
            0.45;

        const f64 rayChoice =
            UnitFloat(
                Mix64(
                    random ^
                    0x52415943484F4943ULL));

        resolvedImpacts_.push_back({
            .id = ProceduralImpactId(seed, index),
            .centerUnitDirection =
                RandomDirection(seed, index),
            .radiusMeters =
                SamplePowerLawRadius(
                    definition_.procedural,
                    seed,
                    index,
                    explicitQuantile),
            .profile = CraterProfileKind::Auto,
            .simpleDepthRatio = 0.18,
            .complexDepthRatio = 0.075,
            .rimHeightRatio = 0.035,
            .ejectaThicknessRatio = 0.012,
            .ejectaExtentRadii = 3.0,
            .rayStrength =
                rayChoice > 0.72
                    ? 0.35 + rayChoice * 0.35
                    : 0.0,
            .rayCount =
                rayChoice > 0.72
                    ? 4U + static_cast<u32>(
                        random % 5ULL)
                    : 0U,
            .degradation = degradation,
            .ageOrder = static_cast<u64>(index),
            .formationAgeYears = definition_.surfaceAgeYears *
                ((static_cast<f64>(index) + 0.5) /
                 static_cast<f64>(explicitProceduralCount)),
            .enabled = true,
            .authored = false
        });
    }

    for (ImpactRecord impact : definition_.authoredImpacts)
    {
        impact.centerUnitDirection =
            math::Normalize(
                impact.centerUnitDirection);
        impact.authored = true;
        resolvedImpacts_.push_back(impact);

        const math::Double3 impactCenter = impact.centerUnitDirection;
        const world::SurfaceFrame impactFrame =
            world::MakeSurfaceFrame(impactCenter);
        if (impact.binarySeparationRadii > 0.0)
        {
            ImpactRecord companion = impact;
            companion.id = DerivedImpactId(
                impact.id, 0U, 0x42494E4152593031ULL);
            companion.centerUnitDirection = world::DirectionAtSurfaceOffset(
                planet_, impactFrame,
                {std::cos(impact.binaryAzimuthRadians) *
                     impact.radiusMeters * impact.binarySeparationRadii,
                 std::sin(impact.binaryAzimuthRadians) *
                     impact.radiusMeters * impact.binarySeparationRadii});
            companion.radiusMeters *= impact.binaryCompanionRadiusRatio;
            companion.binarySeparationRadii = 0.0;
            companion.secondaryCount = 0U;
            companion.rayStrength = 0.0;
            companion.rayCount = 0U;
            companion.authored = false;
            resolvedImpacts_.push_back(std::move(companion));
        }

        for (u32 secondaryIndex = 0U;
             secondaryIndex < impact.secondaryCount;
             ++secondaryIndex)
        {
            const u64 random = Mix64(
                impact.id.high ^ impact.id.low ^
                (static_cast<u64>(secondaryIndex) + 1ULL) *
                    0x9E3779B97F4A7C15ULL);
            const f64 alignment = impact.secondaryRayAlignment;
            const f64 rayAzimuth = impact.rayCount > 0U
                ? (2.0 * std::numbers::pi_v<f64> *
                   static_cast<f64>(secondaryIndex % impact.rayCount) /
                   static_cast<f64>(impact.rayCount))
                : impact.binaryAzimuthRadians;
            const f64 azimuth = rayAzimuth * alignment +
                (2.0 * std::numbers::pi_v<f64> * UnitFloat(random)) *
                    (1.0 - alignment);
            const f64 distance = impact.radiusMeters *
                (1.5 + 3.0 * UnitFloat(Mix64(random ^ 0x5345434F4E444152ULL)));

            ImpactRecord secondary = impact;
            secondary.id = DerivedImpactId(
                impact.id, secondaryIndex, 0x5345434F4E443031ULL);
            secondary.centerUnitDirection = world::DirectionAtSurfaceOffset(
                planet_, impactFrame,
                {std::cos(azimuth) * distance,
                 std::sin(azimuth) * distance});
            secondary.radiusMeters = impact.radiusMeters *
                impact.secondaryRadiusRatio *
                (0.7 + 0.6 * UnitFloat(Mix64(random ^ 0x5241444955533031ULL)));
            secondary.profile = CraterProfileKind::Simple;
            secondary.rimHeightRatio *= 0.7;
            secondary.ejectaThicknessRatio *= 0.35;
            secondary.ejectaExtentRadii = 1.8;
            secondary.rayStrength = 0.0;
            secondary.rayCount = 0U;
            secondary.binarySeparationRadii = 0.0;
            secondary.secondaryCount = 0U;
            secondary.impactAngleDegrees = 0.0;
            secondary.shapeIrregularity = 0.06;
            secondary.authored = false;
            resolvedImpacts_.push_back(std::move(secondary));
        }
    }

    std::unordered_set<ImpactId> resolvedIds;
    resolvedIds.reserve(resolvedImpacts_.size());
    for (const ImpactRecord& impact : resolvedImpacts_)
    {
        if (!resolvedIds.insert(impact.id).second)
        {
            throw std::invalid_argument(
                "M07 generated a duplicate stable impact identity.");
        }
    }

    std::stable_sort(
        resolvedImpacts_.begin(),
        resolvedImpacts_.end(),
        [](const ImpactRecord& a,
           const ImpactRecord& b)
        {
            if (a.ageOrder != b.ageOrder)
            {
                return a.ageOrder < b.ageOrder;
            }
            if (a.id.high != b.id.high)
            {
                return a.id.high < b.id.high;
            }
            return a.id.low < b.id.low;
        });

    f64 cumulativeTectonicDisplacementMeters = 0.0;
    for (const ResurfacingRecord& event : definition_.resurfacingEvents)
        if (event.enabled && event.kind == ResurfacingKind::TectonicRenewal)
            cumulativeTectonicDisplacementMeters +=
                std::abs(event.displacementMeters);
    if (cumulativeTectonicDisplacementMeters > 0.0)
    {
        ImpactQueryScratch scratch;
        const f64 angularReach = std::min(
            cumulativeTectonicDisplacementMeters / planet_.radiusMeters,
            std::numbers::pi_v<f64>);
        const f64 queryChordRadius = 2.0 * std::sin(angularReach * 0.5);
        for (ImpactRecord& impact : resolvedImpacts_)
        {
            QueryResurfacingSpatialIndex(
                impact.centerUnitDirection, scratch, queryChordRadius);
            for (const std::size_t eventIndex : scratch.candidates)
            {
                const ResurfacingRecord& event =
                    definition_.resurfacingEvents[eventIndex];
                const bool afterImpact = event.ageOrder > impact.ageOrder ||
                    (event.ageOrder == impact.ageOrder &&
                     (event.id.high > impact.id.high ||
                      (event.id.high == impact.id.high && event.id.low > impact.id.low)));
                if (afterImpact)
                    impact.centerUnitDirection = DisplaceAcrossTectonicRenewal(
                        planet_, event, impact.centerUnitDirection);
            }
        }
    }
    preparedImpactGeometries_.reserve(resolvedImpacts_.size());
    for (const ImpactRecord& impact : resolvedImpacts_)
    {
        const f64 angle = impact.impactAngleDegrees *
            (std::numbers::pi_v<f64> / 180.0);
        const f64 shapeBound = 1.65 /
            std::max(1.0 - impact.shapeIrregularity, 0.75);
        const f64 angularRadius = std::min(
            impact.radiusMeters * impact.InfluenceExtentRadii() *
                shapeBound / planet_.radiusMeters,
            std::numbers::pi_v<f64>);
        preparedImpactGeometries_.push_back({
            .frame = world::MakeSurfaceFrame(impact.centerUnitDirection),
            .phase = UnitFloat(Mix64(impact.id.high ^ impact.id.low)) *
                2.0 * std::numbers::pi_v<f64>,
            .ejectaMassBalanceScale = EjectaMassBalanceScale(impact, definition_),
            .azimuthCosine = std::cos(impact.impactAzimuthRadians),
            .azimuthSine = std::sin(impact.impactAzimuthRadians),
            .elongation = 1.0 + 0.65 * std::sin(angle) * std::sin(angle),
            .influenceCosine = std::cos(angularRadius),
            .influenceChordRadius = 2.0 * std::sin(angularRadius * 0.5)});
    }
    BuildSpatialIndex();
    BuildResurfacingSpatialIndex();
}

void ImpactField::BuildSpatialIndex()
{
    spatialOrder_.resize(resolvedImpacts_.size());
    std::iota(spatialOrder_.begin(), spatialOrder_.end(), 0U);
    spatialNodes_.clear();
    if (spatialOrder_.empty())
    {
        return;
    }

    constexpr std::size_t kLeafCapacity = 12U;
    const auto coordinate = [](const math::Double3& value, const u32 axis)
    {
        return axis == 0U ? value.x : axis == 1U ? value.y : value.z;
    };

    std::function<std::size_t(std::size_t, std::size_t)> build =
        [&](const std::size_t begin, const std::size_t end)
        {
            const std::size_t nodeIndex = spatialNodes_.size();
            spatialNodes_.push_back({});

            SpatialNode node{};
            node.minimum = {1.0, 1.0, 1.0};
            node.maximum = {-1.0, -1.0, -1.0};
            node.begin = begin;
            node.count = end - begin;
            for (std::size_t i = begin; i < end; ++i)
            {
                const ImpactRecord& impact = resolvedImpacts_[spatialOrder_[i]];
                const math::Double3 center =
                    math::Normalize(impact.centerUnitDirection);
                node.minimum.x = std::min(node.minimum.x, center.x);
                node.minimum.y = std::min(node.minimum.y, center.y);
                node.minimum.z = std::min(node.minimum.z, center.z);
                node.maximum.x = std::max(node.maximum.x, center.x);
                node.maximum.y = std::max(node.maximum.y, center.y);
                node.maximum.z = std::max(node.maximum.z, center.z);
                node.maximumChordRadius = std::max(
                    node.maximumChordRadius,
                    preparedImpactGeometries_[spatialOrder_[i]].influenceChordRadius);
            }

            if (end - begin <= kLeafCapacity)
            {
                node.leaf = true;
                spatialNodes_[nodeIndex] = node;
                return nodeIndex;
            }

            const math::Double3 extent = node.maximum - node.minimum;
            const u32 axis = extent.y > extent.x && extent.y >= extent.z
                ? 1U
                : extent.z > extent.x && extent.z > extent.y ? 2U : 0U;
            const std::size_t middle = begin + (end - begin) / 2U;
            std::nth_element(
                spatialOrder_.begin() + static_cast<std::ptrdiff_t>(begin),
                spatialOrder_.begin() + static_cast<std::ptrdiff_t>(middle),
                spatialOrder_.begin() + static_cast<std::ptrdiff_t>(end),
                [&](const std::size_t lhs, const std::size_t rhs)
                {
                    return coordinate(
                               resolvedImpacts_[lhs].centerUnitDirection, axis) <
                           coordinate(
                               resolvedImpacts_[rhs].centerUnitDirection, axis);
                });
            node.left = build(begin, middle);
            node.right = build(middle, end);
            spatialNodes_[nodeIndex] = node;
            return nodeIndex;
        };

    static_cast<void>(build(0U, spatialOrder_.size()));
}

void ImpactField::BuildResurfacingSpatialIndex()
{
    resurfacingSpatialNodes_.clear();
    const std::size_t count = definition_.resurfacingEvents.size();
    resurfacingCenters_.resize(count);
    resurfacingChordRadii_.resize(count);
    resurfacingOrder_.resize(count);
    std::iota(resurfacingOrder_.begin(), resurfacingOrder_.end(), 0U);
    for (std::size_t eventIndex = 0U; eventIndex < count; ++eventIndex)
    {
        const auto& event = definition_.resurfacingEvents[eventIndex];
        math::Double3 sum{};
        for (const auto& direction : event.centerlineUnitDirections) sum = sum + direction;
        math::Double3 center = math::LengthSquared(sum) > 1.0e-10
            ? math::Normalize(sum)
            : event.centerlineUnitDirections.front();
        f64 maximumAngle = 0.0;
        for (const auto& direction : event.centerlineUnitDirections)
        {
            maximumAngle = std::max(maximumAngle,
                std::acos(std::clamp(math::Dot(center, direction), -1.0, 1.0)));
        }
        resurfacingCenters_[eventIndex] = center;
        if (event.regionalPlateMotion)
            resurfacingChordRadii_[eventIndex] = 2.0;
        else
        {
            maximumAngle = std::min(maximumAngle +
                event.widthMeters / (2.0 * planet_.radiusMeters),
                std::numbers::pi_v<f64>);
            resurfacingChordRadii_[eventIndex] = 2.0 * std::sin(maximumAngle * 0.5);
        }
    }
    if (count == 0U) return;

    constexpr std::size_t leafCapacity = 8U;
    const auto coordinate = [](const math::Double3& value, const u32 axis)
    {
        return axis == 0U ? value.x : axis == 1U ? value.y : value.z;
    };
    std::function<std::size_t(std::size_t, std::size_t)> build =
        [&](const std::size_t begin, const std::size_t end)
        {
            const std::size_t index = resurfacingSpatialNodes_.size();
            resurfacingSpatialNodes_.push_back({});
            SpatialNode node{};
            node.minimum = {1.0, 1.0, 1.0};
            node.maximum = {-1.0, -1.0, -1.0};
            node.begin = begin;
            node.count = end - begin;
            for (std::size_t i = begin; i < end; ++i)
            {
                const std::size_t eventIndex = resurfacingOrder_[i];
                const auto& center = resurfacingCenters_[eventIndex];
                node.minimum.x = std::min(node.minimum.x, center.x);
                node.minimum.y = std::min(node.minimum.y, center.y);
                node.minimum.z = std::min(node.minimum.z, center.z);
                node.maximum.x = std::max(node.maximum.x, center.x);
                node.maximum.y = std::max(node.maximum.y, center.y);
                node.maximum.z = std::max(node.maximum.z, center.z);
                node.maximumChordRadius = std::max(node.maximumChordRadius,
                    resurfacingChordRadii_[eventIndex]);
            }
            if (end - begin <= leafCapacity)
            {
                node.leaf = true;
                resurfacingSpatialNodes_[index] = node;
                return index;
            }
            const auto extent = node.maximum - node.minimum;
            const u32 axis = extent.y > extent.x && extent.y >= extent.z
                ? 1U : extent.z > extent.x && extent.z > extent.y ? 2U : 0U;
            const std::size_t middle = begin + (end - begin) / 2U;
            std::nth_element(
                resurfacingOrder_.begin() + static_cast<std::ptrdiff_t>(begin),
                resurfacingOrder_.begin() + static_cast<std::ptrdiff_t>(middle),
                resurfacingOrder_.begin() + static_cast<std::ptrdiff_t>(end),
                [&](const std::size_t a, const std::size_t b)
                {
                    return coordinate(resurfacingCenters_[a], axis) <
                        coordinate(resurfacingCenters_[b], axis);
                });
            node.left = build(begin, middle);
            node.right = build(middle, end);
            resurfacingSpatialNodes_[index] = node;
            return index;
        };
    static_cast<void>(build(0U, count));
}

void ImpactField::QueryResurfacingSpatialIndex(
    const math::Double3& point,
    ImpactQueryScratch& scratch,
    const f64 queryChordRadius) const
{
    auto& candidates = scratch.candidates;
    candidates.clear();
    scratch.traversalOverflow.clear();
    if (resurfacingSpatialNodes_.empty()) return;
    std::array<std::size_t, 64U> localStack{};
    std::size_t stackSize = 1U;
    localStack[0] = 0U;
    const auto push = [&](const std::size_t value)
    {
        if (stackSize < localStack.size()) localStack[stackSize++] = value;
        else scratch.traversalOverflow.push_back(value);
    };
    while (stackSize > 0U || !scratch.traversalOverflow.empty())
    {
        std::size_t index = 0U;
        if (stackSize > 0U)
        {
            index = localStack[--stackSize];
        }
        else
        {
            index = scratch.traversalOverflow.back();
            scratch.traversalOverflow.pop_back();
        }
        const SpatialNode& node = resurfacingSpatialNodes_[index];
        const f64 dx = std::max({node.minimum.x - point.x, 0.0, point.x - node.maximum.x});
        const f64 dy = std::max({node.minimum.y - point.y, 0.0, point.y - node.maximum.y});
        const f64 dz = std::max({node.minimum.z - point.z, 0.0, point.z - node.maximum.z});
        const f64 candidateRadius = node.maximumChordRadius + queryChordRadius;
        if (dx * dx + dy * dy + dz * dz >
            candidateRadius * candidateRadius) continue;
        if (node.leaf)
        {
            for (std::size_t i = node.begin; i < node.begin + node.count; ++i)
            {
                const std::size_t eventIndex = resurfacingOrder_[i];
                const math::Double3 delta = resurfacingCenters_[eventIndex] - point;
                const f64 reach = resurfacingChordRadii_[eventIndex] + queryChordRadius;
                if (math::LengthSquared(delta) <= reach * reach)
                    candidates.push_back(eventIndex);
            }
        }
        else
        {
            push(node.left);
            push(node.right);
        }
    }
    scratch.traversalOverflow.clear();
    std::sort(candidates.begin(), candidates.end(), [&](const std::size_t a, const std::size_t b)
    {
        const auto& lhs = definition_.resurfacingEvents[a];
        const auto& rhs = definition_.resurfacingEvents[b];
        if (lhs.ageOrder != rhs.ageOrder) return lhs.ageOrder < rhs.ageOrder;
        if (lhs.id.high != rhs.id.high) return lhs.id.high < rhs.id.high;
        return lhs.id.low < rhs.id.low;
    });
}

void ImpactField::QuerySpatialIndex(
    const math::Double3& point,
    ImpactQueryScratch& scratch,
    const f64 queryChordRadius) const
{
    auto& candidates = scratch.candidates;
    candidates.clear();
    scratch.traversalOverflow.clear();
    if (spatialNodes_.empty())
    {
        return;
    }

    std::array<std::size_t, 64U> localStack{};
    std::size_t stackSize = 1U;
    localStack[0] = 0U;
    const auto push = [&](const std::size_t value)
    {
        if (stackSize < localStack.size())
        {
            localStack[stackSize++] = value;
        }
        else
        {
            scratch.traversalOverflow.push_back(value);
        }
    };

    while (stackSize > 0U || !scratch.traversalOverflow.empty())
    {
        std::size_t index = 0U;
        if (stackSize > 0U)
        {
            index = localStack[--stackSize];
        }
        else
        {
            index = scratch.traversalOverflow.back();
            scratch.traversalOverflow.pop_back();
        }
        const SpatialNode& node = spatialNodes_[index];
        const f64 dx = std::max({node.minimum.x - point.x, 0.0, point.x - node.maximum.x});
        const f64 dy = std::max({node.minimum.y - point.y, 0.0, point.y - node.maximum.y});
        const f64 dz = std::max({node.minimum.z - point.z, 0.0, point.z - node.maximum.z});
        const f64 candidateRadius = node.maximumChordRadius + queryChordRadius;
        if (dx * dx + dy * dy + dz * dz >
            candidateRadius * candidateRadius)
        {
            continue;
        }
        if (node.leaf)
        {
            for (std::size_t i = node.begin; i < node.begin + node.count; ++i)
            {
                const std::size_t impactIndex = spatialOrder_[i];
                const ImpactRecord& impact = resolvedImpacts_[impactIndex];
                const f64 reach =
                    preparedImpactGeometries_[impactIndex].influenceChordRadius + queryChordRadius;
                const math::Double3 delta = impact.centerUnitDirection - point;
                if (math::LengthSquared(delta) <= reach * reach)
                    candidates.push_back(impactIndex);
            }
        }
        else
        {
            push(node.left);
            push(node.right);
        }
    }

    scratch.traversalOverflow.clear();

    std::sort(
        candidates.begin(),
        candidates.end(),
        [&](const std::size_t lhs, const std::size_t rhs)
        {
            const ImpactRecord& a = resolvedImpacts_[lhs];
            const ImpactRecord& b = resolvedImpacts_[rhs];
            if (a.ageOrder != b.ageOrder) return a.ageOrder < b.ageOrder;
            if (a.id.high != b.id.high) return a.id.high < b.id.high;
            return a.id.low < b.id.low;
        });
}

std::size_t ImpactField::CandidateCount(
    const math::Double3& unitDirection) const
{
    const math::Double3 point = math::Normalize(unitDirection);
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(point.z) || math::LengthSquared(point) <= 0.0)
    {
        return 0U;
    }
    ImpactQueryScratch scratch;
    QuerySpatialIndex(point, scratch);
    return scratch.candidates.size();
}

CraterProcessSample ImpactField::Sample(
    const math::Double3& unitDirection,
    const f64 footprintDiameterMeters) const
{
    ImpactQueryScratch scratch;
    return Sample(unitDirection, footprintDiameterMeters, scratch);
}

CraterProcessSample ImpactField::Sample(
    const math::Double3& unitDirection,
    const f64 footprintDiameterMeters,
    ImpactQueryScratch& scratch) const
{
    const math::Double3 canonical = math::Normalize(unitDirection);

    if (!std::isfinite(canonical.x) || !std::isfinite(canonical.y) ||
        !std::isfinite(canonical.z) || math::LengthSquared(canonical) <= 0.0)
    {
        throw std::invalid_argument(
            "M07 crater sample position is invalid or belongs to another planet.");
    }

    if (!FinitePositive(footprintDiameterMeters))
    {
        throw std::invalid_argument(
            "M07 crater sample footprint must be finite and positive.");
    }

    QuerySpatialIndex(canonical, scratch);
    scratch.impactCandidates = scratch.candidates;
    QueryResurfacingSpatialIndex(canonical, scratch);
    return SampleCandidates(canonical, footprintDiameterMeters, scratch);
}

CraterProcessSample ImpactField::Sample(
    const math::Double3& unitDirection,
    const f64 footprintDiameterMeters,
    ImpactQueryScratch& scratch,
    const std::span<const GeologicalEventReference> batch) const
{
    const math::Double3 canonical = math::Normalize(unitDirection);
    if (!std::isfinite(canonical.x) || !std::isfinite(canonical.y) ||
        !std::isfinite(canonical.z) || math::LengthSquared(canonical) <= 0.0)
        throw std::invalid_argument("M07 crater sample position is invalid or belongs to another planet.");
    if (!FinitePositive(footprintDiameterMeters))
        throw std::invalid_argument("M07 crater sample footprint must be finite and positive.");

    scratch.impactCandidates.clear();
    scratch.candidates.clear();
    for (const GeologicalEventReference& event : batch)
    {
        if (event.kind == GeologicalEventKind::Impact)
        {
            if (event.index >= resolvedImpacts_.size() ||
                resolvedImpacts_[event.index].id != event.id ||
                resolvedImpacts_[event.index].ageOrder != event.ageOrder)
                throw std::invalid_argument("M07 impact batch contains a stale event reference.");
            scratch.impactCandidates.push_back(event.index);
        }
        else
        {
            if (event.index >= definition_.resurfacingEvents.size())
                throw std::invalid_argument("M07 resurfacing batch contains an invalid event reference.");
            const ResurfacingRecord& resurfacing = definition_.resurfacingEvents[event.index];
            const GeologicalEventKind expectedKind =
                resurfacing.kind == ResurfacingKind::LavaFlow
                    ? GeologicalEventKind::LavaFlow
                    : resurfacing.kind == ResurfacingKind::IceRenewal
                        ? GeologicalEventKind::IceRenewal
                        : GeologicalEventKind::TectonicRenewal;
            if (event.kind != expectedKind || resurfacing.id != event.id ||
                resurfacing.ageOrder != event.ageOrder)
                throw std::invalid_argument("M07 resurfacing batch contains a stale event reference.");
            scratch.candidates.push_back(event.index);
        }
    }
    return SampleCandidates(canonical, footprintDiameterMeters, scratch);
}

CraterProcessSample ImpactField::SampleCandidates(
    const math::Double3& canonical,
    const f64 footprintDiameterMeters,
    ImpactQueryScratch& scratch) const
{
    CraterProcessSample result{};
    std::erase_if(scratch.candidates, [&](const std::size_t index)
    {
        const ResurfacingRecord& event = definition_.resurfacingEvents[index];
        return event.regionalPlateMotion &&
            DistanceToPolylineMeters(planet_, event, canonical) >
                event.widthMeters * 0.5 + footprintDiameterMeters * 0.5;
    });

    // Impacts and connected resurfacing records share one local chronology.
    // Each spatial hierarchy still prunes its own event type, then the two
    // compact sorted candidate lists are merged by age and stable ID. This
    // lets an old flow be excavated by a younger crater without replaying
    // unrelated planetary events.
    const auto impactBeforeResurfacing = [&](const std::size_t impactIndex,
                                              const std::size_t resurfacingIndex)
    {
        const ImpactRecord& impact = resolvedImpacts_[impactIndex];
        const ResurfacingRecord& resurfacing =
            definition_.resurfacingEvents[resurfacingIndex];
        if (impact.ageOrder != resurfacing.ageOrder)
            return impact.ageOrder < resurfacing.ageOrder;
        if (impact.id.high != resurfacing.id.high)
            return impact.id.high < resurfacing.id.high;
        if (impact.id.low != resurfacing.id.low)
            return impact.id.low < resurfacing.id.low;
        return true; // Stable tie: impact first, then deposit.
    };

    const auto applyImpact = [&](const std::size_t index)
    {
        const ImpactRecord& impact = resolvedImpacts_[index];
        const PreparedImpactGeometry& prepared = preparedImpactGeometries_[index];
        Accumulate(result, SampleImpact(impact, definition_, planet_,
            prepared, canonical, footprintDiameterMeters));
    };
    const auto applyResurfacing = [&](const std::size_t eventIndex)
    {
        const ResurfacingRecord& event = definition_.resurfacingEvents[eventIndex];
        if (!event.enabled) return;
        f64 minimumDistanceMeters = std::numeric_limits<f64>::infinity();
        for (std::size_t i = 1U; i < event.centerlineUnitDirections.size(); ++i)
        {
            const math::Double3& aDirection = event.centerlineUnitDirections[i - 1U];
            const math::Double3& bDirection = event.centerlineUnitDirections[i];
            const math::Double3 midpoint = math::Normalize(aDirection + bDirection);
            const world::SurfaceFrame frame = world::MakeSurfaceFrame(midpoint);
            const math::Double2 a = world::SurfaceOffsetBetweenDirections(
                planet_, frame, aDirection);
            const math::Double2 b = world::SurfaceOffsetBetweenDirections(
                planet_, frame, bDirection);
            const math::Double2 p = world::SurfaceOffsetBetweenDirections(
                planet_, frame, canonical);
            const math::Double2 edge = b - a;
            const f64 edgeLengthSquared = edge.x * edge.x + edge.y * edge.y;
            const f64 t = edgeLengthSquared > 1.0e-9
                ? std::clamp(((p.x - a.x) * edge.x + (p.y - a.y) * edge.y) /
                    edgeLengthSquared, 0.0, 1.0)
                : 0.5;
            const f64 dx = p.x - (a.x + edge.x * t);
            const f64 dy = p.y - (a.y + edge.y * t);
            minimumDistanceMeters = std::min(minimumDistanceMeters,
                std::sqrt(dx * dx + dy * dy));
        }
        const f64 halfWidth = event.widthMeters * 0.5;
        const f64 normalizedEdge = std::clamp(
            (minimumDistanceMeters - halfWidth) / std::max(halfWidth, 1.0),
            0.0, 1.0);
        const f64 coverage = 1.0 - SmoothUnit(normalizedEdge);
        if (coverage <= 0.0) return;

        if (event.kind == ResurfacingKind::TectonicRenewal)
        {
            // A younger fault belt reactivates existing geology: it partly
            // disrupts old relief and deposits breccia, while preserving the
            // old formation age and the identity of material not excavated.
            // The narrow positive center and weak flanks make this a structural
            // overprint instead of lava-like blanket filling.
            const f64 coreWidth = std::max(halfWidth * 0.22, 1.0);
            const f64 shoulder = (minimumDistanceMeters - halfWidth * 0.62) /
                std::max(halfWidth * 0.12, 1.0);
            const f64 ridge = std::exp(-0.5 * std::pow(minimumDistanceMeters / coreWidth, 2.0));
            const f64 trough = std::exp(-0.5 * shoulder * shoulder);
            const f64 retained = 1.0 - 0.22 * coverage;
            result.heightDeltaMeters = result.heightDeltaMeters * retained +
                event.thicknessMeters * coverage * (ridge - 0.28 * trough);
            result.excavationDepthMeters *= retained;
            result.ejectaThicknessMeters *= retained;
            result.meltThicknessMeters *= retained;
            result.resurfacingThicknessMeters *= retained;
            result.rayField *= retained;
            result.brecciaField = std::clamp(
                result.brecciaField * retained + 0.3 * coverage, 0.0, 1.0);
            result.debrisField = std::clamp(
                result.debrisField * retained + 0.16 * coverage, 0.0, 1.0);
            result.ejectaThicknessMeters +=
                0.04 * event.thicknessMeters * coverage;
            result.excavationCoverage = std::max(
                result.excavationCoverage, 0.22 * coverage);
            if (event.ageOrder >= result.exposureAgeOrder)
            {
                result.exposureAgeOrder = event.ageOrder;
                result.exposureAgeYears = std::max(
                    0.0, definition_.surfaceAgeYears - event.formationAgeYears);
            }
            return;
        }

        const f64 retained = 1.0 - coverage;
        result.heightDeltaMeters = result.heightDeltaMeters * retained +
            event.thicknessMeters * coverage;
        result.excavationDepthMeters *= retained;
        result.ejectaThicknessMeters *= retained;
        result.meltThicknessMeters *= retained;
        result.brecciaField *= retained;
        result.debrisField *= retained;
        result.rayField *= retained;
        result.resurfacingThicknessMeters =
            result.resurfacingThicknessMeters * retained +
            event.thicknessMeters * coverage;
        result.resurfacedMaterialFraction = std::clamp(
            result.resurfacedMaterialFraction * retained + coverage, 0.0, 1.0);
        if (event.ageOrder >= result.exposureAgeOrder)
        {
            result.exposureAgeOrder = event.ageOrder;
            result.exposureAgeYears = std::max(
                0.0, definition_.surfaceAgeYears - event.formationAgeYears);
            if (coverage >= 0.95)
            {
                result.formationAgeOrder = event.ageOrder;
                result.formationAgeYears = event.formationAgeYears;
            }
        }
    };

    std::size_t impactCursor = 0U;
    std::size_t resurfacingCursor = 0U;
    while (impactCursor < scratch.impactCandidates.size() ||
           resurfacingCursor < scratch.candidates.size())
    {
        const bool takeImpact = resurfacingCursor >= scratch.candidates.size() ||
            (impactCursor < scratch.impactCandidates.size() &&
             impactBeforeResurfacing(scratch.impactCandidates[impactCursor],
                 scratch.candidates[resurfacingCursor]));
        if (takeImpact)
            applyImpact(scratch.impactCandidates[impactCursor++]);
        else
            applyResurfacing(scratch.candidates[resurfacingCursor++]);
    }

    // The sub-resolution population is statistical rather than an explicit
    // event list. Condition it on the exposure age left by the shared event
    // chronology so a recent lava flow does not inherit old microcratering.
    if (statisticalMicroImpactCount_ > 0U &&
        statisticalMicroImpactMaximumRadiusMeters_ > 0.0)
    {
        const f64 surfaceArea = 4.0 * std::numbers::pi_v<f64> *
            planet_.radiusMeters * planet_.radiusMeters;
        const f64 expectedAtFootprint =
            static_cast<f64>(statisticalMicroImpactCount_) *
            footprintDiameterMeters * footprintDiameterMeters /
            std::max(surfaceArea, 1.0);
        const bool hasExposure = result.exposureAgeOrder != 0U ||
            result.excavationCoverage > 0.01 ||
            result.ejectaThicknessMeters > 0.0 ||
            result.resurfacedMaterialFraction > 0.0 || result.rayField > 0.0;
        const f64 exposureAgeYears = hasExposure
            ? result.exposureAgeYears
            : definition_.surfaceAgeYears;
        const f64 ageFraction = definition_.surfaceAgeYears > 0.0
            ? std::clamp(exposureAgeYears / definition_.surfaceAgeYears, 0.0, 1.0)
            : 1.0;
        result.microImpactCoverage = 1.0 -
            std::exp(-expectedAtFootprint * ageFraction);
        const f64 representativeRadius = std::sqrt(
            definition_.procedural.minimumRadiusMeters *
            statisticalMicroImpactMaximumRadiusMeters_);
        result.microImpactRoughnessMeters = std::min(
            representativeRadius * 0.22 *
                std::sqrt(result.microImpactCoverage),
            footprintDiameterMeters * 0.12);
        const f64 residualWeight = FeatureWeight(
            representativeRadius * 2.0, footprintDiameterMeters);
        if (residualWeight > 0.0)
        {
            const u64 seed = definition_.seed != 0U
                ? definition_.seed
                : Mix64(planet_.generationSeed ^ 0x494D504143544D37ULL);
            const f64 phase = UnitFloat(Mix64(seed ^ 0x4D4943524F524553ULL)) *
                2.0 * std::numbers::pi_v<f64>;
            const f64 frequency = planet_.radiusMeters /
                std::max(representativeRadius * 2.0, 1.0);
            const f64 noise = 0.25 * (
                std::sin((canonical.x + canonical.y * 0.37) * frequency + phase) +
                std::sin((canonical.y + canonical.z * 0.41) * frequency - phase * 0.7) +
                std::cos((canonical.z + canonical.x * 0.29) * frequency + phase * 1.3) +
                std::cos((canonical.x - canonical.y + canonical.z) * frequency * 0.73));
            result.heightDeltaMeters += result.microImpactRoughnessMeters *
                noise * residualWeight;
        }
    }

    return result;
}

const ImpactFieldDefinition&
ImpactField::Definition() const noexcept
{
    return definition_;
}

const std::vector<ImpactRecord>&
ImpactField::ResolvedImpacts() const noexcept
{
    return resolvedImpacts_;
}

const std::vector<PreparedImpactGeometry>&
ImpactField::PreparedImpactGeometries() const noexcept
{
    return preparedImpactGeometries_;
}

std::vector<GeologicalEventReference>
ImpactField::ChronologicalEvents() const
{
    std::vector<GeologicalEventReference> events;
    events.reserve(resolvedImpacts_.size() + definition_.resurfacingEvents.size());
    for (std::size_t index = 0U; index < resolvedImpacts_.size(); ++index)
    {
        const ImpactRecord& impact = resolvedImpacts_[index];
        events.push_back({
            .kind = GeologicalEventKind::Impact,
            .index = index,
            .id = impact.id,
            .ageOrder = impact.ageOrder});
    }
    for (std::size_t index = 0U; index < definition_.resurfacingEvents.size(); ++index)
    {
        const ResurfacingRecord& resurfacing = definition_.resurfacingEvents[index];
        const GeologicalEventKind kind =
            resurfacing.kind == ResurfacingKind::LavaFlow
                ? GeologicalEventKind::LavaFlow
                : resurfacing.kind == ResurfacingKind::IceRenewal
                    ? GeologicalEventKind::IceRenewal
                    : GeologicalEventKind::TectonicRenewal;
        events.push_back({
            .kind = kind,
            .index = index,
            .id = resurfacing.id,
            .ageOrder = resurfacing.ageOrder});
    }
    const auto kindRank = [](const GeologicalEventKind kind)
    {
        return kind == GeologicalEventKind::Impact ? 0U : 1U;
    };
    std::sort(events.begin(), events.end(), [&](
        const GeologicalEventReference& lhs,
        const GeologicalEventReference& rhs)
    {
        if (lhs.ageOrder != rhs.ageOrder) return lhs.ageOrder < rhs.ageOrder;
        if (lhs.id.high != rhs.id.high) return lhs.id.high < rhs.id.high;
        if (lhs.id.low != rhs.id.low) return lhs.id.low < rhs.id.low;
        return kindRank(lhs.kind) < kindRank(rhs.kind);
    });
    return events;
}

std::vector<GeologicalEventReference>
ImpactField::EventsIntersectingCap(
    const math::Double3& centerDirection,
    const f64 angularRadiusRadians) const
{
    ImpactQueryScratch scratch;
    std::vector<GeologicalEventReference> events;
    CollectEventsIntersectingCap(
        centerDirection, angularRadiusRadians, scratch, events);
    return events;
}

void ImpactField::CollectEventsIntersectingCap(
    const math::Double3& centerDirection,
    const f64 angularRadiusRadians,
    ImpactQueryScratch& scratch,
    std::vector<GeologicalEventReference>& events) const
{
    if (!std::isfinite(centerDirection.x) || !std::isfinite(centerDirection.y) ||
        !std::isfinite(centerDirection.z) ||
        math::LengthSquared(centerDirection) <= 1.0e-20 ||
        !std::isfinite(angularRadiusRadians) || angularRadiusRadians < 0.0 ||
        angularRadiusRadians > std::numbers::pi_v<f64>)
    {
        throw std::invalid_argument("M07 event batch cap is invalid.");
    }
    const math::Double3 center = math::Normalize(centerDirection);
    const f64 queryChordRadius = 2.0 * std::sin(angularRadiusRadians * 0.5);
    QuerySpatialIndex(center, scratch, queryChordRadius);
    scratch.impactCandidates = scratch.candidates;
    QueryResurfacingSpatialIndex(center, scratch, queryChordRadius);
    std::erase_if(scratch.candidates, [&](const std::size_t index)
    {
        const ResurfacingRecord& event = definition_.resurfacingEvents[index];
        return event.regionalPlateMotion &&
            DistanceToPolylineMeters(planet_, event, center) >
                angularRadiusRadians * planet_.radiusMeters + event.widthMeters * 0.5;
    });

    events.clear();
    events.reserve(scratch.impactCandidates.size() + scratch.candidates.size());
    std::size_t impactCursor = 0U;
    std::size_t resurfacingCursor = 0U;
    const auto impactBeforeResurfacing = [&](const std::size_t impactIndex,
                                              const std::size_t resurfacingIndex)
    {
        const ImpactRecord& impact = resolvedImpacts_[impactIndex];
        const ResurfacingRecord& resurfacing =
            definition_.resurfacingEvents[resurfacingIndex];
        if (impact.ageOrder != resurfacing.ageOrder)
            return impact.ageOrder < resurfacing.ageOrder;
        if (impact.id.high != resurfacing.id.high)
            return impact.id.high < resurfacing.id.high;
        return impact.id.low < resurfacing.id.low;
    };
    while (impactCursor < scratch.impactCandidates.size() ||
           resurfacingCursor < scratch.candidates.size())
    {
        const bool takeImpact = resurfacingCursor >= scratch.candidates.size() ||
            (impactCursor < scratch.impactCandidates.size() &&
             impactBeforeResurfacing(scratch.impactCandidates[impactCursor],
                 scratch.candidates[resurfacingCursor]));
        if (takeImpact)
        {
            const std::size_t index = scratch.impactCandidates[impactCursor++];
            const ImpactRecord& impact = resolvedImpacts_[index];
            events.push_back({GeologicalEventKind::Impact, index,
                impact.id, impact.ageOrder});
        }
        else
        {
            const std::size_t index = scratch.candidates[resurfacingCursor++];
            const ResurfacingRecord& resurfacing = definition_.resurfacingEvents[index];
            const GeologicalEventKind kind =
                resurfacing.kind == ResurfacingKind::LavaFlow
                    ? GeologicalEventKind::LavaFlow
                    : resurfacing.kind == ResurfacingKind::IceRenewal
                        ? GeologicalEventKind::IceRenewal
                        : GeologicalEventKind::TectonicRenewal;
            events.push_back({kind, index, resurfacing.id, resurfacing.ageOrder});
        }
    }
}

u32 ImpactField::StatisticalMicroImpactCount() const noexcept
{
    return statisticalMicroImpactCount_;
}

f64 ImpactField::StatisticalMicroImpactMaximumRadiusMeters() const noexcept
{
    return statisticalMicroImpactMaximumRadiusMeters_;
}

bool IceFractureDefinition::IsValid() const noexcept
{
    const auto finiteVector = [](const math::Double3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z) && math::LengthSquared(value) > 0.0;
    };
    return finiteVector(tidalAxis) && finiteVector(spinAxis) &&
        std::isfinite(formationAgeYears) && formationAgeYears >= 0.0 &&
        std::isfinite(tidalStress) && tidalStress >= 0.0 &&
        std::isfinite(rotationalStress) && rotationalStress >= 0.0 &&
        FiniteUnit(tensileStrength) && fractureCount <= 4'096U &&
        segmentsPerFracture >= 2U && segmentsPerFracture <= 64U &&
        FinitePositive(maximumLengthMeters) && FinitePositive(widthMeters) &&
        std::isfinite(grooveDepthMeters) && grooveDepthMeters >= 0.0 &&
        std::isfinite(ridgeHeightMeters) && ridgeHeightMeters >= 0.0 &&
        FiniteUnit(branchProbability);
}

IceFractureField::IceFractureField(
    const world::PlanetDefinition planet,
    IceFractureDefinition definition)
    : planet_(planet), definition_(std::move(definition))
{
    if (!planet_.id.IsValid() || !FinitePositive(planet_.radiusMeters) ||
        !definition_.IsValid())
    {
        throw std::invalid_argument("M08 ice fracture field inputs are invalid.");
    }
    if (!definition_.enabled)
    {
        return;
    }

    definition_.tidalAxis = math::Normalize(definition_.tidalAxis);
    definition_.spinAxis = math::Normalize(definition_.spinAxis);
    const f64 stepLength = definition_.maximumLengthMeters /
        static_cast<f64>(definition_.segmentsPerFracture);
    segments_.reserve(static_cast<std::size_t>(definition_.fractureCount) *
        definition_.segmentsPerFracture * 2U);

    for (u32 fracture = 0U; fracture < definition_.fractureCount; ++fracture)
    {
        math::Double3 direction = RandomDirection(definition_.seed, fracture);
        const f64 tidalCosine = math::Dot(direction, definition_.tidalAxis);
        const f64 spinCosine = math::Dot(direction, definition_.spinAxis);
        const f64 tensileStress =
            definition_.tidalStress * (1.0 - 3.0 * tidalCosine * tidalCosine) +
            definition_.rotationalStress * (1.0 - 3.0 * spinCosine * spinCosine);
        if (tensileStress < definition_.tensileStrength)
        {
            continue;
        }

        math::Double3 tangent = definition_.tidalAxis -
            direction * tidalCosine;
        if (math::LengthSquared(tangent) < 1.0e-8)
        {
            tangent = definition_.spinAxis - direction * spinCosine;
        }
        tangent = math::Normalize(tangent);
        const world::SurfaceFrame initialFrame = world::MakeSurfaceFrame(direction);
        f64 heading = std::atan2(
            math::Dot(tangent, initialFrame.north),
            math::Dot(tangent, initialFrame.east));
        const u64 pathSeed = Mix64(
            definition_.seed ^ (static_cast<u64>(fracture) + 1ULL) *
                0x4943454652414354ULL);

        for (u32 step = 0U; step < definition_.segmentsPerFracture; ++step)
        {
            const u64 stepSeed = Mix64(pathSeed ^ step);
            const f64 bend = (UnitFloat(stepSeed) - 0.5) * 0.22;
            const f64 stepHeading = heading + bend;
            const world::SurfaceFrame frame = world::MakeSurfaceFrame(direction);
            const math::Double3 end = world::DirectionAtSurfaceOffset(
                planet_, frame,
                {std::cos(stepHeading) * stepLength,
                 std::sin(stepHeading) * stepLength});
            const math::Double3 midpoint = world::DirectionAtSurfaceOffset(
                planet_, frame,
                {std::cos(stepHeading) * stepLength * 0.5,
                 std::sin(stepHeading) * stepLength * 0.5});
            segments_.push_back({direction, end, midpoint, stepLength * 0.5});

            if (UnitFloat(Mix64(stepSeed ^ 0x4252414E43483031ULL)) <
                definition_.branchProbability)
            {
                const f64 branchHeading = stepHeading +
                    (UnitFloat(Mix64(stepSeed ^ 0x4252414E43483032ULL)) < 0.5
                        ? -0.9 : 0.9);
                const f64 branchLength = stepLength * 0.72;
                const math::Double3 branchEnd = world::DirectionAtSurfaceOffset(
                    planet_, frame,
                    {std::cos(branchHeading) * branchLength,
                     std::sin(branchHeading) * branchLength});
                const math::Double3 branchMidpoint = world::DirectionAtSurfaceOffset(
                    planet_, frame,
                    {std::cos(branchHeading) * branchLength * 0.5,
                     std::sin(branchHeading) * branchLength * 0.5});
                segments_.push_back({direction, branchEnd, branchMidpoint,
                    branchLength * 0.5});
            }

            heading = stepHeading;
            direction = end;
        }
    }
    BuildSpatialIndex();
}

void IceFractureField::BuildSpatialIndex()
{
    segmentOrder_.resize(segments_.size());
    std::iota(segmentOrder_.begin(), segmentOrder_.end(), 0U);
    if (segmentOrder_.empty()) return;

    constexpr std::size_t leafCapacity = 12U;
    const auto coordinate = [](const math::Double3& value, const u32 axis)
    {
        return axis == 0U ? value.x : axis == 1U ? value.y : value.z;
    };
    std::function<std::size_t(std::size_t, std::size_t)> build =
        [&](const std::size_t begin, const std::size_t end)
        {
            const std::size_t index = spatialNodes_.size();
            spatialNodes_.push_back({});
            SpatialNode node{};
            node.minimum = {1.0, 1.0, 1.0};
            node.maximum = {-1.0, -1.0, -1.0};
            node.begin = begin;
            node.count = end - begin;
            for (std::size_t i = begin; i < end; ++i)
            {
                const IceFractureSegment& segment = segments_[segmentOrder_[i]];
                const auto& center = segment.midpointDirection;
                node.minimum.x = std::min(node.minimum.x, center.x);
                node.minimum.y = std::min(node.minimum.y, center.y);
                node.minimum.z = std::min(node.minimum.z, center.z);
                node.maximum.x = std::max(node.maximum.x, center.x);
                node.maximum.y = std::max(node.maximum.y, center.y);
                node.maximum.z = std::max(node.maximum.z, center.z);
                const f64 angle = std::min(
                    (segment.halfLengthMeters + 4.0 * definition_.widthMeters) /
                        planet_.radiusMeters,
                    std::numbers::pi_v<f64>);
                node.maximumChordRadius = std::max(
                    node.maximumChordRadius, 2.0 * std::sin(angle * 0.5));
            }
            if (end - begin <= leafCapacity)
            {
                node.leaf = true;
                spatialNodes_[index] = node;
                return index;
            }
            const auto extent = node.maximum - node.minimum;
            const u32 axis = extent.y > extent.x && extent.y >= extent.z
                ? 1U : extent.z > extent.x && extent.z > extent.y ? 2U : 0U;
            const std::size_t middle = begin + (end - begin) / 2U;
            std::nth_element(
                segmentOrder_.begin() + static_cast<std::ptrdiff_t>(begin),
                segmentOrder_.begin() + static_cast<std::ptrdiff_t>(middle),
                segmentOrder_.begin() + static_cast<std::ptrdiff_t>(end),
                [&](const std::size_t a, const std::size_t b)
                {
                    return coordinate(segments_[a].midpointDirection, axis) <
                        coordinate(segments_[b].midpointDirection, axis);
                });
            node.left = build(begin, middle);
            node.right = build(middle, end);
            spatialNodes_[index] = node;
            return index;
        };
    static_cast<void>(build(0U, segmentOrder_.size()));
}

void IceFractureField::QuerySpatialIndex(
    const math::Double3& point,
    ImpactQueryScratch& scratch,
    const f64 queryChordRadius) const
{
    auto& candidates = scratch.candidates;
    candidates.clear();
    scratch.traversalOverflow.clear();
    if (spatialNodes_.empty()) return;

    std::array<std::size_t, 64U> localStack{};
    std::size_t stackSize = 1U;
    localStack[0] = 0U;
    const auto push = [&](const std::size_t index)
    {
        if (stackSize < localStack.size()) localStack[stackSize++] = index;
        else scratch.traversalOverflow.push_back(index);
    };
    const f64 queryAngle = 2.0 * std::asin(
        std::clamp(queryChordRadius * 0.5, 0.0, 1.0));
    while (stackSize > 0U || !scratch.traversalOverflow.empty())
    {
        std::size_t index = 0U;
        if (stackSize > 0U)
        {
            index = localStack[--stackSize];
        }
        else
        {
            index = scratch.traversalOverflow.back();
            scratch.traversalOverflow.pop_back();
        }
        const SpatialNode& node = spatialNodes_[index];
        const f64 dx = std::max({node.minimum.x - point.x, 0.0, point.x - node.maximum.x});
        const f64 dy = std::max({node.minimum.y - point.y, 0.0, point.y - node.maximum.y});
        const f64 dz = std::max({node.minimum.z - point.z, 0.0, point.z - node.maximum.z});
        const f64 candidateRadius = node.maximumChordRadius + queryChordRadius;
        if (dx * dx + dy * dy + dz * dz >
            candidateRadius * candidateRadius) continue;
        if (node.leaf)
        {
            for (std::size_t i = node.begin; i < node.begin + node.count; ++i)
            {
                const std::size_t segmentIndex = segmentOrder_[i];
                const IceFractureSegment& segment = segments_[segmentIndex];
                const f64 influenceAngle = std::min(
                    (segment.halfLengthMeters + 4.0 * definition_.widthMeters) /
                        planet_.radiusMeters,
                    std::numbers::pi_v<f64>);
                const f64 influenceChord = 2.0 * std::sin(
                    std::min(influenceAngle + queryAngle, std::numbers::pi_v<f64>) * 0.5);
                if (math::LengthSquared(segment.midpointDirection - point) <=
                    influenceChord * influenceChord)
                    candidates.push_back(segmentIndex);
            }
        }
        else
        {
            push(node.left);
            push(node.right);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    scratch.traversalOverflow.clear();
}

IceFractureSample IceFractureField::Sample(
    const math::Double3& unitDirection,
    const f64 footprintDiameterMeters,
    ImpactQueryScratch& scratch) const
{
    const math::Double3 canonical = math::Normalize(unitDirection);
    if (!std::isfinite(canonical.x) || !std::isfinite(canonical.y) ||
        !std::isfinite(canonical.z) || math::LengthSquared(canonical) <= 0.0 ||
        !FinitePositive(footprintDiameterMeters))
    {
        throw std::invalid_argument("M08 ice fracture sample request is invalid.");
    }
    IceFractureSample result{};
    if (!definition_.enabled || segments_.empty()) return result;
    QuerySpatialIndex(canonical, scratch);
    return SampleSegments(canonical, footprintDiameterMeters, scratch.candidates);
}

IceFractureSample IceFractureField::Sample(
    const math::Double3& unitDirection,
    const f64 footprintDiameterMeters,
    const std::span<const std::size_t> segmentBatch) const
{
    const math::Double3 canonical = math::Normalize(unitDirection);
    if (!std::isfinite(canonical.x) || !std::isfinite(canonical.y) ||
        !std::isfinite(canonical.z) || math::LengthSquared(canonical) <= 0.0 ||
        !FinitePositive(footprintDiameterMeters))
        throw std::invalid_argument("M08 ice fracture sample request is invalid.");
    return SampleSegments(canonical, footprintDiameterMeters, segmentBatch);
}

IceFractureSample IceFractureField::SampleSegments(
    const math::Double3& canonical,
    const f64 footprintDiameterMeters,
    const std::span<const std::size_t> segmentIndices) const
{
    IceFractureSample result{};
    if (!definition_.enabled || segments_.empty()) return result;
    const f64 featureWeight = FeatureWeight(
        definition_.widthMeters * 4.0, footprintDiameterMeters);
    if (featureWeight <= 0.0) return result;

    for (const std::size_t index : segmentIndices)
    {
        if (index >= segments_.size())
            throw std::invalid_argument("M08 fracture batch contains an invalid segment reference.");
        const IceFractureSegment& segment = segments_[index];
        const world::SurfaceFrame frame = world::MakeSurfaceFrame(segment.midpointDirection);
        const math::Double2 a = world::SurfaceOffsetBetweenDirections(
            planet_, frame, segment.startDirection);
        const math::Double2 b = world::SurfaceOffsetBetweenDirections(
            planet_, frame, segment.endDirection);
        const math::Double2 p = world::SurfaceOffsetBetweenDirections(
            planet_, frame, canonical);
        const math::Double2 edge = b - a;
        const f64 edgeLengthSquared = edge.x * edge.x + edge.y * edge.y;
        const f64 t = edgeLengthSquared > 1.0e-9
            ? std::clamp(((p.x - a.x) * edge.x + (p.y - a.y) * edge.y) /
                edgeLengthSquared, 0.0, 1.0)
            : 0.5;
        const f64 dx = p.x - (a.x + edge.x * t);
        const f64 dy = p.y - (a.y + edge.y * t);
        const f64 distance = std::sqrt(dx * dx + dy * dy);
        if (distance > 4.0 * definition_.widthMeters)
            continue;
        const f64 normalized = distance / definition_.widthMeters;
        const f64 groove = std::exp(-0.5 * normalized * normalized);
        const f64 ridgeOffset = normalized - 2.2;
        const f64 ridge = std::exp(-0.5 * ridgeOffset * ridgeOffset);
        result.heightDeltaMeters += featureWeight *
            (-definition_.grooveDepthMeters * groove +
             definition_.ridgeHeightMeters * ridge);
        result.damage = std::max(result.damage,
            std::clamp(groove * featureWeight, 0.0, 1.0));
        ++result.nearbySegments;
    }
    result.fractureCoverage = result.damage;
    if (result.damage > 0.0)
    {
        result.ageOrder = definition_.ageOrder;
        result.formationAgeYears = definition_.formationAgeYears;
    }
    return result;
}

const IceFractureDefinition& IceFractureField::Definition() const noexcept
{
    return definition_;
}

std::size_t IceFractureField::SegmentCount() const noexcept
{
    return segments_.size();
}

const std::vector<IceFractureSegment>& IceFractureField::Segments() const noexcept
{
    return segments_;
}

void IceFractureField::CollectSegmentsIntersectingCap(
    const math::Double3& centerDirection,
    const f64 angularRadiusRadians,
    ImpactQueryScratch& scratch,
    std::vector<std::size_t>& output) const
{
    if (!std::isfinite(centerDirection.x) || !std::isfinite(centerDirection.y) ||
        !std::isfinite(centerDirection.z) ||
        math::LengthSquared(centerDirection) <= 1.0e-20 ||
        !std::isfinite(angularRadiusRadians) || angularRadiusRadians < 0.0 ||
        angularRadiusRadians > std::numbers::pi_v<f64>)
    {
        throw std::invalid_argument("M08 fracture batch cap is invalid.");
    }
    const math::Double3 center = math::Normalize(centerDirection);
    const f64 queryChordRadius = 2.0 * std::sin(angularRadiusRadians * 0.5);
    QuerySpatialIndex(center, scratch, queryChordRadius);
    output.assign(scratch.candidates.begin(), scratch.candidates.end());
}

ImpactFieldDefinition MakeMoonLikeImpactPreset(
    const world::PlanetId planet,
    const ImpactFieldId fieldId,
    const u64 seed)
{
    ImpactFieldDefinition definition{
        .id = fieldId,
        .planet = planet,
        .name = "Moon-like crater field",
        .seed = seed,
        .procedural = {
            .count = 768,
            .minimumRadiusMeters = 2'000.0,
            .maximumRadiusMeters = 240'000.0,
            .cumulativeExponent = 1.85
        },
        .complexTransitionRadiusMeters = 18'000.0
    };

    if (!definition.IsValid())
    {
        throw std::invalid_argument(
            "M07 moon-like preset requires valid planet and field IDs.");
    }

    return definition;
}
} // namespace orbit::terrain_impacts
