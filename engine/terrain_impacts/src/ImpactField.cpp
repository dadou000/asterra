#include <orbit/terrain_impacts/ImpactField.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
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
    const u32 index) noexcept
{
    const f64 b = distribution.cumulativeExponent;
    const f64 minimum = distribution.minimumRadiusMeters;
    const f64 maximum = distribution.maximumRadiusMeters;

    const f64 u =
        UnitFloat(
            Mix64(
                seed ^
                (static_cast<u64>(index) + 1ULL) *
                    0x94D049BB133111EBULL ^
                0x5241444955534D37ULL));

    const f64 minPower = std::pow(minimum, -b);
    const f64 maxPower = std::pow(maximum, -b);
    const f64 value =
        minPower +
        (maxPower - minPower) * u;

    return std::pow(
        std::max(value, maxPower),
        -1.0 / b);
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
            definition.complexTransitionRadiusMeters
        ? CraterProfileKind::Complex
        : CraterProfileKind::Simple;
}

[[nodiscard]] f64 RayModulation(
    const ImpactRecord& impact,
    const world::PlanetDefinition& planet,
    const terrain::PlanetSurfacePosition& samplePosition) noexcept
{
    if (impact.rayStrength <= 0.0 ||
        impact.rayCount == 0)
    {
        return 0.0;
    }

    const terrain::PlanetSurfacePosition center{
        .planet = samplePosition.planet,
        .unitDirection = impact.centerUnitDirection,
        .radialOffsetMeters = 0.0
    };

    const world::SurfaceFrame frame =
        terrain::SurfaceTangentFrame(center);

    const math::Double2 offset =
        terrain::SurfaceOffsetBetweenPositions(
            planet,
            center,
            frame,
            samplePosition);

    if (!std::isfinite(offset.x) ||
        !std::isfinite(offset.y))
    {
        return 0.0;
    }

    const f64 azimuth =
        std::atan2(offset.y, offset.x);

    const f64 phase =
        UnitFloat(Mix64(impact.id.high ^ impact.id.low)) *
        2.0 * std::numbers::pi_v<f64>;

    const f64 ray =
        std::max(
            0.0,
            std::cos(
                azimuth *
                    static_cast<f64>(impact.rayCount) +
                phase));

    return impact.rayStrength *
        std::pow(ray, 8.0);
}

[[nodiscard]] CraterProcessSample SampleImpact(
    const ImpactRecord& impact,
    const ImpactFieldDefinition& definition,
    const world::PlanetDefinition& planet,
    const terrain::PlanetSurfacePosition& position,
    const terrain::TerrainSampleFootprint& footprint) noexcept
{
    CraterProcessSample result{};

    if (!impact.enabled)
    {
        return result;
    }

    const f64 spectralWeight =
        FeatureWeight(
            impact.radiusMeters * 2.0,
            footprint.diameterMeters);

    if (spectralWeight <= 0.0)
    {
        return result;
    }

    const math::Double3 center =
        math::Normalize(impact.centerUnitDirection);

    const f64 maximumRadius =
        impact.radiusMeters *
        std::max(1.0, impact.ejectaExtentRadii);

    const f64 maximumAngle =
        std::min(
            maximumRadius / planet.radiusMeters,
            std::numbers::pi_v<f64>);

    const f64 cosine =
        std::clamp(
            math::Dot(
                center,
                position.unitDirection),
            -1.0,
            1.0);

    if (cosine < std::cos(maximumAngle))
    {
        return result;
    }

    const f64 distanceMeters =
        std::acos(cosine) *
        planet.radiusMeters;

    const f64 x =
        distanceMeters / impact.radiusMeters;

    if (x > impact.ejectaExtentRadii)
    {
        return result;
    }

    const f64 preservation =
        (1.0 - impact.degradation) *
        spectralWeight;

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
            if (x < peakRadius)
            {
                const f64 peakX =
                    1.0 - x / peakRadius;
                const f64 centralPeak =
                    impact.radiusMeters *
                    0.045 *
                    peakX * peakX;
                craterDelta += centralPeak;
            }

            if (x > 0.62)
            {
                const f64 terracePhase =
                    (x - 0.62) / 0.38;
                craterDelta +=
                    impact.radiusMeters *
                    0.012 *
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
    const f64 rim =
        impact.radiusMeters *
        impact.rimHeightRatio *
        std::exp(
            -0.5 *
            rimDistance *
            rimDistance);

    f64 ejecta = 0.0;
    f64 rayField = 0.0;

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
            outerFade;

        rayField =
            RayModulation(
                impact,
                planet,
                position);

        ejecta *= 1.0 + rayField;
    }

    result.heightDeltaMeters =
        (craterDelta + rim + ejecta) *
        preservation;

    result.excavationDepthMeters =
        excavation * preservation;

    result.ejectaThicknessMeters =
        ejecta * preservation;

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

    result.affectingImpacts = 1;
    return result;
}

void Accumulate(
    CraterProcessSample& destination,
    const CraterProcessSample& contribution) noexcept
{
    destination.heightDeltaMeters +=
        contribution.heightDeltaMeters;
    destination.excavationDepthMeters +=
        contribution.excavationDepthMeters;
    destination.ejectaThicknessMeters +=
        contribution.ejectaThicknessMeters;
    destination.debrisField =
        std::clamp(
            destination.debrisField +
                contribution.debrisField,
            0.0,
            1.0);
    destination.rayField =
        std::clamp(
            destination.rayField +
                contribution.rayField,
            0.0,
            1.0);
    destination.affectingImpacts +=
        contribution.affectingImpacts;
}
} // namespace

bool CraterSizeFrequencyDistribution::IsValid() const noexcept
{
    return
        count <= 100'000U &&
        FinitePositive(minimumRadiusMeters) &&
        FinitePositive(maximumRadiusMeters) &&
        maximumRadiusMeters >= minimumRadiusMeters &&
        std::isfinite(cumulativeExponent) &&
        cumulativeExponent > 0.0;
}

bool ImpactRecord::IsValid() const noexcept
{
    return
        id.IsValid() &&
        std::isfinite(centerUnitDirection.x) &&
        std::isfinite(centerUnitDirection.y) &&
        std::isfinite(centerUnitDirection.z) &&
        math::LengthSquared(centerUnitDirection) > 0.0 &&
        FinitePositive(radiusMeters) &&
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
        FiniteUnit(degradation);
}

bool ImpactFieldDefinition::IsValid() const noexcept
{
    if (!id.IsValid() ||
        !planet.IsValid() ||
        name.empty() ||
        !procedural.IsValid() ||
        !FinitePositive(complexTransitionRadiusMeters))
    {
        return false;
    }

    std::unordered_set<ImpactId> ids;
    ids.reserve(authoredImpacts.size());

    for (const ImpactRecord& impact : authoredImpacts)
    {
        if (!impact.IsValid() ||
            !ids.insert(impact.id).second)
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

    const u64 seed =
        definition_.seed != 0
            ? definition_.seed
            : Mix64(
                planet_.generationSeed ^
                0x494D504143544D37ULL);

    resolvedImpacts_.reserve(
        static_cast<std::size_t>(
            definition_.procedural.count) +
        definition_.authoredImpacts.size());

    for (u32 index = 0;
         index < definition_.procedural.count;
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
                    index),
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
            .enabled = true,
            .authored = false
        });
    }

    for (ImpactRecord impact :
         definition_.authoredImpacts)
    {
        impact.centerUnitDirection =
            math::Normalize(
                impact.centerUnitDirection);
        impact.authored = true;
        resolvedImpacts_.push_back(
            std::move(impact));
    }

    std::stable_sort(
        resolvedImpacts_.begin(),
        resolvedImpacts_.end(),
        [](const ImpactRecord& a,
           const ImpactRecord& b)
        {
            return a.ageOrder < b.ageOrder;
        });
}

CraterProcessSample ImpactField::Sample(
    const terrain::PlanetSurfacePosition& position,
    const terrain::TerrainSampleFootprint& footprint) const
{
    const terrain::PlanetSurfacePosition canonical =
        terrain::CanonicalizeSurfacePosition(position);

    if (!canonical.IsValid() ||
        canonical.planet != planet_.id)
    {
        throw std::invalid_argument(
            "M07 crater sample position is invalid or belongs to another planet.");
    }

    if (!footprint.IsValid())
    {
        throw std::invalid_argument(
            "M07 crater sample footprint must be finite and positive.");
    }

    CraterProcessSample result{};

    for (const ImpactRecord& impact :
         resolvedImpacts_)
    {
        Accumulate(
            result,
            SampleImpact(
                impact,
                definition_,
                planet_,
                canonical,
                footprint));
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
