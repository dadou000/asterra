#include <orbit/terrain_macro_geology/MacroGeologyField.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_macro_geology
{
namespace
{
[[nodiscard]] u64 SplitMix64(u64 value) noexcept
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] f64 UnitFloat(const u64 value) noexcept
{
    constexpr f64 inverse =
        1.0 / static_cast<f64>(1ULL << 53U);
    return static_cast<f64>(value >> 11U) * inverse;
}

[[nodiscard]] math::Double3 AxisFromSeed(
    const u64 seed,
    const u32 octave) noexcept
{
    const u64 base =
        SplitMix64(seed ^ (0xD6E8FEB86659FD93ULL *
                           static_cast<u64>(octave + 1U)));

    math::Double3 axis{
        UnitFloat(SplitMix64(base ^ 0xA24BAED4963EE407ULL)) * 2.0 - 1.0,
        UnitFloat(SplitMix64(base ^ 0x9FB21C651E98DF25ULL)) * 2.0 - 1.0,
        UnitFloat(SplitMix64(base ^ 0xC13FA9A902A6328FULL)) * 2.0 - 1.0
    };

    if (math::LengthSquared(axis) <= 1.0e-12)
    {
        axis = {1.0, 0.0, 0.0};
    }

    return math::Normalize(axis);
}

[[nodiscard]] f64 PhaseFromSeed(
    const u64 seed,
    const u32 octave) noexcept
{
    const u64 value =
        SplitMix64(seed ^
                   (0x94D049BB133111EBULL *
                    static_cast<u64>(octave + 1U)));

    return UnitFloat(value) *
        (2.0 * std::numbers::pi_v<f64>);
}

[[nodiscard]] f64 CollisionScale(
    const terrain::GlobalTerrainFieldSample& tectonic,
    const MacroGeologyDesc& desc) noexcept
{
    if (tectonic.nearestPlateContinental &&
        tectonic.secondPlateContinental)
    {
        return desc.continentalCollisionScale;
    }

    if (tectonic.nearestPlateContinental ||
        tectonic.secondPlateContinental)
    {
        return desc.mixedCollisionScale;
    }

    return desc.oceanicCollisionScale;
}

[[nodiscard]] bool FiniteNonNegative(
    const f64 value) noexcept
{
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] bool FinitePositive(
    const f64 value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}
} // namespace

bool MacroGeologyDesc::IsValid() const noexcept
{
    return
        FiniteNonNegative(convergenceUpliftMeters) &&
        FiniteNonNegative(continentalCollisionScale) &&
        FiniteNonNegative(mixedCollisionScale) &&
        FiniteNonNegative(oceanicCollisionScale) &&
        FiniteNonNegative(divergenceSubsidenceMeters) &&
        std::isfinite(distortionAmplitude) &&
        distortionAmplitude >= 0.0 &&
        distortionAmplitude <= 1.0 &&
        FinitePositive(distortionWavelengthMeters) &&
        distortionOctaves >= 1U &&
        distortionOctaves <= 8U;
}

u64 MacroGeologyRevisionFingerprint(
    const MacroGeologyRevisionInputs& revisions) noexcept
{
    u64 value = SplitMix64(
        revisions.geology ^ 0x47454F4C4F475931ULL);
    value ^= SplitMix64(
        revisions.authoring ^ 0x415554484F523031ULL);
    return SplitMix64(value);
}

MacroGeologyField::MacroGeologyField(
    const world::PlanetDefinition planet,
    const terrain::GlobalTerrainFields& globalFields,
    const surface_authoring::TerrainConstraintSet* authoredConstraints,
    MacroGeologyDesc desc)
    : planet_(planet),
      globalFields_(&globalFields),
      authoredConstraints_(authoredConstraints),
      desc_(std::move(desc)),
      resolvedSeed_(
          desc_.seed != 0
              ? desc_.seed
              : SplitMix64(
                    planet.generationSeed ^
                    0x4D4143524F47454FULL))
{
    if (!planet_.id.IsValid() ||
        !std::isfinite(planet_.radiusMeters) ||
        planet_.radiusMeters <= 0.0)
    {
        throw std::invalid_argument(
            "M05 MacroGeologyField requires a valid planet.");
    }

    if (!desc_.IsValid())
    {
        throw std::invalid_argument(
            "M05 MacroGeologyDesc contains invalid values.");
    }

    if (authoredConstraints_ != nullptr)
    {
        if (!authoredConstraints_->IsValid())
        {
            throw std::invalid_argument(
                "M05 authored terrain constraints are invalid.");
        }

        if (authoredConstraints_->planet != planet_.id)
        {
            throw std::invalid_argument(
                "M05 authored terrain constraints belong to another planet.");
        }
    }
}

f64 MacroGeologyField::DistortionSignal(
    const math::Double3& unitDirection) const noexcept
{
    const math::Double3 direction =
        math::Normalize(unitDirection);

    if (math::LengthSquared(direction) <= 0.0)
    {
        return 0.0;
    }

    f64 sum = 0.0;
    f64 weight = 1.0;
    f64 weightSum = 0.0;
    f64 wavelength = desc_.distortionWavelengthMeters;

    for (u32 octave = 0;
         octave < desc_.distortionOctaves;
         ++octave)
    {
        const math::Double3 axis =
            AxisFromSeed(resolvedSeed_, octave);
        const f64 phase =
            PhaseFromSeed(resolvedSeed_, octave);

        const f64 angularFrequency =
            (2.0 * std::numbers::pi_v<f64> *
             planet_.radiusMeters) /
            std::max(wavelength, 1.0);

        const f64 sample =
            std::sin(
                math::Dot(direction, axis) *
                    angularFrequency +
                phase);

        sum += sample * weight;
        weightSum += weight;

        weight *= 0.5;
        wavelength *= 0.5;
    }

    return weightSum > 0.0
        ? std::clamp(sum / weightSum, -1.0, 1.0)
        : 0.0;
}

MacroGeologySample MacroGeologyField::Sample(
    const terrain::PlanetSurfacePosition& position) const
{
    const terrain::PlanetSurfacePosition canonical =
        terrain::CanonicalizeSurfacePosition(position);

    if (!canonical.IsValid() ||
        canonical.planet != planet_.id)
    {
        throw std::invalid_argument(
            "M05 macro geology sample position belongs to another or invalid planet.");
    }

    const terrain::TerrainQuery query{
        .unitDirection = canonical.unitDirection,
        .footprintMeters =
            desc_.distortionWavelengthMeters,
        .planet = canonical.planet,
        .radialOffsetMeters =
            canonical.radialOffsetMeters
    };

    const terrain::GlobalTerrainFieldSample global =
        globalFields_->Sample(query);

    const f64 collisionScale =
        CollisionScale(global, desc_);

    const f64 tectonicUplift =
        std::max(global.convergenceMask, 0.0) *
        desc_.convergenceUpliftMeters *
        collisionScale;

    const f64 tectonicSubsidence =
        std::max(global.divergenceMask, 0.0) *
        desc_.divergenceSubsidenceMeters;

    const f64 hotspotUplift =
        std::max(global.hotspotElevationMeters, 0.0);

    const f64 distortion =
        DistortionSignal(canonical.unitDirection);

    const f64 distortionMultiplier =
        std::max(
            0.0,
            1.0 +
                distortion *
                desc_.distortionAmplitude);

    const f64 baselineUplift =
        (tectonicUplift - tectonicSubsidence) *
            distortionMultiplier +
        hotspotUplift;

    surface_authoring::TerrainConstraintSample authored{
        .heightMeters = 0.0,
        .gradient = {},
        .upliftMeters = baselineUplift,
        .material = {},
        .protection = 0.0,
        .drainage = 0.0
    };

    if (authoredConstraints_ != nullptr)
    {
        authored =
            surface_authoring::EvaluateTerrainConstraintSet(
                *authoredConstraints_,
                planet_,
                canonical,
                {
                    .heightMeters = 0.0,
                    .gradient = {},
                    .upliftMeters = baselineUplift,
                    .material = {},
                    .protection = 0.0,
                    .drainage = 0.0
                });
    }

    return {
        .tectonicUpliftMeters = tectonicUplift,
        .tectonicSubsidenceMeters = tectonicSubsidence,
        .hotspotUpliftMeters = hotspotUplift,
        .distortionSignal = distortion,
        .upliftMeters = authored.upliftMeters,
        .authoredHeightMeters = authored.heightMeters,
        .gradientGuidance = authored.gradient,
        .drainageGuidance = authored.drainage,
        .protection = authored.protection
    };
}

const MacroGeologyDesc&
MacroGeologyField::Description() const noexcept
{
    return desc_;
}

surface_authoring::ScalarTerrainConstraint
MakeMountainBeltConstraint(
    const surface_authoring::TerrainConstraintId id,
    surface_authoring::SplineConstraintPrimitive spline,
    const f64 upliftMeters,
    const f64 opacity)
{
    if (!id.IsValid() ||
        !spline.IsValid() ||
        !std::isfinite(upliftMeters) ||
        upliftMeters <= 0.0 ||
        !std::isfinite(opacity) ||
        opacity < 0.0 ||
        opacity > 1.0)
    {
        throw std::invalid_argument(
            "Invalid M05 mountain-belt constraint.");
    }

    return {
        .id = id,
        .mode =
            surface_authoring::ConstraintCompositionMode::Add,
        .primitive = std::move(spline),
        .value = upliftMeters,
        .opacity = opacity,
        .enabled = true
    };
}

surface_authoring::ScalarTerrainConstraint
MakeBasinConstraint(
    const surface_authoring::TerrainConstraintId id,
    surface_authoring::TerrainConstraintPrimitive primitive,
    const f64 subsidenceMeters,
    const f64 opacity)
{
    if (!id.IsValid() ||
        !std::isfinite(subsidenceMeters) ||
        subsidenceMeters <= 0.0 ||
        !std::isfinite(opacity) ||
        opacity < 0.0 ||
        opacity > 1.0)
    {
        throw std::invalid_argument(
            "Invalid M05 basin constraint.");
    }

    return {
        .id = id,
        .mode =
            surface_authoring::ConstraintCompositionMode::Add,
        .primitive = std::move(primitive),
        .value = -subsidenceMeters,
        .opacity = opacity,
        .enabled = true
    };
}
} // namespace orbit::terrain_macro_geology
