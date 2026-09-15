#include "TectonicField.hpp"

#include "ProceduralNoise.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbit::terrain::detail
{
namespace
{
constexpr f64 kGoldenAngle =
    std::numbers::pi * (3.0 - 2.2360679774997896964);

// Evenly distributes `count` directions over the sphere (golden-angle
// spiral), then perturbs each with a bounded vector-noise domain warp for
// organic, non-convex cell shapes instead of a perfectly regular lattice.
[[nodiscard]] math::Double3 SpiralSeedDirection(
    const u32 index,
    const u32 count,
    const u64 warpSeed,
    const f64 irregularity) noexcept
{
    const f64 y =
        1.0 - 2.0 * (static_cast<f64>(index) + 0.5) / static_cast<f64>(count);

    const f64 radius = std::sqrt(std::max(0.0, 1.0 - y * y));

    const math::Double3 base{
        radius * std::cos(kGoldenAngle * static_cast<f64>(index)),
        y,
        radius * std::sin(kGoldenAngle * static_cast<f64>(index))
    };

    const f64 warpFrequency = std::sqrt(static_cast<f64>(count)) * 0.6;
    const math::Double3 warp =
        VectorNoise3D(base * warpFrequency, warpSeed);
    const f64 jitter = irregularity * (2.0 / std::sqrt(static_cast<f64>(count)));

    const math::Double3 warped = math::Normalize(base + warp * jitter);
    return math::LengthSquared(warped) > 0.0 ? warped : base;
}

[[nodiscard]] math::Double3 HashedUnitVector(
    const u32 index,
    const u64 salt,
    const u64 seed) noexcept
{
    const math::Double3 raw{
        HashValue(index, 0, 0, seed ^ salt),
        HashValue(index, 1, 0, seed ^ salt),
        HashValue(index, 2, 0, seed ^ salt)
    };

    const math::Double3 unit = math::Normalize(raw);
    return math::LengthSquared(unit) > 0.0 ? unit : math::Double3{0.0, 1.0, 0.0};
}
} // namespace

TectonicField::TectonicField(
    const f64 planetRadiusMeters,
    const TectonicFieldDesc& desc)
    : desc_(desc),
      planetRadiusMeters_(planetRadiusMeters > 0.0 ? planetRadiusMeters : 1.0)
{
    if (desc.plateCount < 1 || desc.plateCount > kMaxTectonicPlates)
    {
        throw std::invalid_argument(
            "Orbit tectonic plate count must be between 1 and kMaxTectonicPlates.");
    }
    if (desc.hotspotCount > kMaxTectonicHotspots)
    {
        throw std::invalid_argument(
            "Orbit tectonic hotspot count exceeds kMaxTectonicHotspots.");
    }
    if (desc.hotspotAgeSteps > kMaxTectonicHotspotAgeSteps)
    {
        throw std::invalid_argument(
            "Orbit tectonic hotspot age steps exceed kMaxTectonicHotspotAgeSteps.");
    }

    const u64 seed = desc.seed;
    plateCount_ = desc.plateCount;
    hotspotCount_ = desc.hotspotCount;
    hotspotAgeSteps_ = desc.hotspotAgeSteps;

    for (u32 i = 0; i < plateCount_; ++i)
    {
        Plate& plate = plates_[i];

        plate.seedDirection = SpiralSeedDirection(
            i, plateCount_, seed ^ 0x504C41544553ULL, desc.plateIrregularity);

        const f64 continentalRoll =
            HashValue(i, 100, 0, seed ^ 0x434C415353ULL) * 0.5 + 0.5;
        plate.isContinental = continentalRoll < desc.continentalPlateFraction;
        plate.continentalBiasMeters = plate.isContinental
            ? desc.continentalPlateBiasMeters
            : desc.oceanicPlateBiasMeters;

        const math::Double3 axis =
            HashedUnitVector(i, 0x45554C4552ULL, seed);
        const f64 speedT =
            HashValue(i, 300, 0, seed ^ 0x53504545ULL) * 0.5 + 0.5;
        const f64 speed =
            Lerp(desc.minPlateAngularSpeed, desc.maxPlateAngularSpeed, speedT);
        plate.eulerVector = axis * speed;

        plate.sizeBiasDot =
            HashValue(i, 500, 0, seed ^ 0x53495A45ULL) *
            desc.plateSizeVarianceDot;
    }

    for (u32 h = 0; h < hotspotCount_; ++h)
    {
        Hotspot& hotspot = hotspots_[h];

        hotspot.mantlePosition = SpiralSeedDirection(
            h, hotspotCount_, seed ^ 0x484F5453504F5453ULL, 0.6);

        u32 nearestPlate = 0;
        f64 best = -2.0;
        for (u32 i = 0; i < plateCount_; ++i)
        {
            const f64 d = math::Dot(hotspot.mantlePosition, plates_[i].seedDirection);
            if (d > best)
            {
                best = d;
                nearestPlate = i;
            }
        }

        const math::Double3 plateVelocity =
            math::Cross(plates_[nearestPlate].eulerVector, hotspot.mantlePosition);

        math::Double3 trailDirection = math::Normalize(plateVelocity * -1.0);
        if (math::LengthSquared(trailDirection) <= 0.0)
        {
            // Degenerate (near-zero local plate velocity) -- fall back to
            // an arbitrary stable tangent so the chain still has a
            // direction to trail off in.
            const math::Double3 arbitrary =
                std::abs(hotspot.mantlePosition.y) < 0.99
                    ? math::Double3{0.0, 1.0, 0.0}
                    : math::Double3{1.0, 0.0, 0.0};
            trailDirection = math::Normalize(
                math::Cross(arbitrary, hotspot.mantlePosition));
        }

        f64 maxAngularSpan = 0.0;
        for (u32 k = 0; k < hotspotAgeSteps_; ++k)
        {
            const f64 alongAngular =
                static_cast<f64>(k) * desc.hotspotChainSpacingMeters /
                planetRadiusMeters_;

            const math::Double3 offsetPoint = math::Normalize(
                hotspot.mantlePosition + trailDirection * alongAngular);

            hotspot.chainPoint[k] = math::LengthSquared(offsetPoint) > 0.0
                ? offsetPoint
                : hotspot.mantlePosition;

            hotspot.chainAmplitude[k] =
                desc.hotspotBaseReliefMeters *
                std::pow(desc.hotspotAgeDecay, static_cast<f64>(k));

            const f64 radiusMeters =
                desc.hotspotCoreRadiusMeters *
                (1.0 + desc.hotspotRadiusGrowthPerAge * static_cast<f64>(k));
            const f64 radiusAngular = std::clamp(
                radiusMeters / planetRadiusMeters_, 0.0, std::numbers::pi);
            // Chord length between two unit vectors separated by
            // `radiusAngular` radians -- lets HotspotElevationMeters
            // compare against a simple squared-distance instead of acos.
            hotspot.chainChordRadius[k] = 2.0 * std::sin(radiusAngular * 0.5);

            maxAngularSpan = std::max(maxAngularSpan, alongAngular + radiusAngular);
        }

        hotspot.boundingCosine =
            std::cos(std::min(maxAngularSpan + 0.01, std::numbers::pi));
    }
}

TectonicSample TectonicField::Sample(
    const math::Double3& direction) const noexcept
{
    u32 nearest = 0;
    u32 second = 0;
    f64 d0 = -2.0;
    f64 d1 = -2.0;

    for (u32 i = 0; i < plateCount_; ++i)
    {
        const f64 d =
            math::Dot(direction, plates_[i].seedDirection) +
            plates_[i].sizeBiasDot;
        if (d > d0)
        {
            second = nearest;
            d1 = d0;
            nearest = i;
            d0 = d;
        }
        else if (d > d1)
        {
            second = i;
            d1 = d;
        }
    }

    if (plateCount_ <= 1)
    {
        return {
            .nearestPlate = nearest,
            .secondPlate = nearest,
            .convergenceMask = 0.0,
            .divergenceMask = 0.0,
            .transformMask = 0.0,
            .plateBiasMeters = plates_[nearest].continentalBiasMeters,
            .nearestIsContinental = plates_[nearest].isContinental,
            .secondIsContinental = plates_[nearest].isContinental
        };
    }

    const f64 boundaryMask =
        Smooth(1.0 - (d0 - d1) / std::max(desc_.boundaryWidthDot, 1.0e-9));

    f64 convergenceMask = 0.0;
    f64 divergenceMask = 0.0;
    f64 transformMask = 0.0;
    if (boundaryMask > 0.0)
    {
        const math::Double3 towardSecond =
            plates_[second].seedDirection - plates_[nearest].seedDirection;
        const math::Double3 tangentToward =
            towardSecond - direction * math::Dot(towardSecond, direction);
        const math::Double3 normal = math::Normalize(tangentToward);

        if (math::LengthSquared(normal) > 0.0)
        {
            const math::Double3 vNearest =
                math::Cross(plates_[nearest].eulerVector, direction);
            const math::Double3 vSecond =
                math::Cross(plates_[second].eulerVector, direction);
            const math::Double3 relative = vNearest - vSecond;

            // Motion perpendicular to the boundary line (along `normal`)
            // is convergence/divergence; motion along the boundary line
            // itself (perpendicular to `normal`, within the tangent
            // plane) is lateral shear -- a transform fault.
            const math::Double3 alongBoundary =
                math::Cross(direction, normal);

            const f64 normalSpeed = math::Dot(relative, normal);
            const f64 shearSpeed = math::Dot(relative, alongBoundary);

            const f64 convergence = std::max(0.0, -normalSpeed);
            const f64 divergence = std::max(0.0, normalSpeed);
            const f64 shear = std::abs(shearSpeed);

            const bool eitherContinental =
                plates_[nearest].isContinental || plates_[second].isContinental;
            const f64 collisionScale =
                eitherContinental ? 1.0 : desc_.oceanicConvergenceScale;

            const f64 referenceSpeed =
                std::max(desc_.convergenceReferenceSpeed, 1.0e-9);
            const f64 transformReferenceSpeed =
                std::max(desc_.transformReferenceSpeed, 1.0e-9);

            convergenceMask = boundaryMask *
                Smooth(convergence / referenceSpeed) *
                collisionScale;
            divergenceMask = boundaryMask *
                Smooth(divergence / referenceSpeed);
            transformMask = boundaryMask *
                Smooth(shear / transformReferenceSpeed);
        }
    }

    const f64 crossBlend = Smooth(
        0.5 + 0.5 * (d1 - d0) / std::max(desc_.boundaryWidthDot, 1.0e-9));
    const f64 plateBiasMeters = Lerp(
        plates_[nearest].continentalBiasMeters,
        plates_[second].continentalBiasMeters,
        crossBlend);

    return {
        .nearestPlate = nearest,
        .secondPlate = second,
        .convergenceMask = convergenceMask,
        .divergenceMask = divergenceMask,
        .transformMask = transformMask,
        .plateBiasMeters = plateBiasMeters,
        .nearestIsContinental = plates_[nearest].isContinental,
        .secondIsContinental = plates_[second].isContinental
    };
}

f64 TectonicField::HotspotElevationMeters(
    const math::Double3& direction) const noexcept
{
    f64 sum = 0.0;

    for (u32 h = 0; h < hotspotCount_; ++h)
    {
        const Hotspot& hotspot = hotspots_[h];

        if (math::Dot(direction, hotspot.mantlePosition) < hotspot.boundingCosine)
        {
            continue;
        }

        for (u32 k = 0; k < hotspotAgeSteps_; ++k)
        {
            const f64 radius = hotspot.chainChordRadius[k];
            if (radius <= 0.0)
            {
                continue;
            }

            const math::Double3 delta = direction - hotspot.chainPoint[k];
            const f64 chordSquared = math::LengthSquared(delta);
            const f64 t = 1.0 - chordSquared / (radius * radius);
            if (t <= 0.0)
            {
                continue;
            }

            const f64 falloff = Smooth(t);
            sum += hotspot.chainAmplitude[k] * falloff * falloff;
        }
    }

    return sum;
}

std::vector<GpuTectonicPlate> TectonicField::BuildGpuPlates() const
{
    std::vector<GpuTectonicPlate> result;
    result.reserve(plateCount_);

    for (u32 i = 0; i < plateCount_; ++i)
    {
        const Plate& plate = plates_[i];
        result.push_back({
            .seedDirection = plate.seedDirection,
            .isContinental = plate.isContinental,
            .continentalBiasMeters = plate.continentalBiasMeters,
            .eulerVector = plate.eulerVector,
            .sizeBiasDot = plate.sizeBiasDot
        });
    }

    return result;
}

std::vector<GpuTectonicHotspot> TectonicField::BuildGpuHotspots() const
{
    std::vector<GpuTectonicHotspot> result;
    result.reserve(hotspotCount_);

    for (u32 h = 0; h < hotspotCount_; ++h)
    {
        const Hotspot& hotspot = hotspots_[h];
        result.push_back({
            .mantlePosition = hotspot.mantlePosition,
            .boundingCosine = hotspot.boundingCosine,
            .ageSteps = hotspotAgeSteps_,
            .chainPoint = hotspot.chainPoint,
            .chainAmplitude = hotspot.chainAmplitude,
            .chainChordRadius = hotspot.chainChordRadius
        });
    }

    return result;
}
} // namespace orbit::terrain::detail
