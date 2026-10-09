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
// Plates within one boundary width of the top that take part in the boundary
// structure at a point; more than this only occurs at pathological junctions.
// Mirrored by the GPU field generator.
constexpr u32 kMaxBoundaryCandidates = 8;

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

        const f64 thicknessRoll =
            HashValue(i, 700, 0, seed ^ 0x5448494BULL);
        const f64 ageRoll =
            HashValue(i, 710, 0, seed ^ 0x41474521ULL) * 0.5 + 0.5;
        // Continental crust: thick, buoyant and old. Oceanic: thin, dense
        // and young.
        plate.crustThicknessKm = plate.isContinental
            ? 35.0 + 8.0 * thicknessRoll
            : 7.0 + 1.5 * thicknessRoll;
        plate.crustAge = plate.isContinental
            ? 0.55 + 0.45 * ageRoll
            : 0.05 + 0.55 * ageRoll;
        plate.continentalBase = plate.isContinental ? 0.78 : 0.22;
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
    ClaimArray claims{};
    for (u32 i = 0; i < plateCount_; ++i)
    {
        claims[i] =
            math::Dot(direction, plates_[i].seedDirection) +
            plates_[i].sizeBiasDot;
    }
    return SampleWithClaims(direction, claims, nullptr, desc_.boundaryWidthDot);
}

TectonicSample TectonicField::SampleWithClaims(
    const math::Double3& direction,
    const ClaimArray& claims,
    const BoundaryNormalFn* const normalFn,
    const f64 boundaryWidth,
    const ClaimArray* const widthLimit,
    const ClaimArray* const widthScale) const noexcept
{
    const ClaimArray& d = claims;
    u32 nearest = 0;
    u32 second = 0;
    f64 d0 = -2.0;
    f64 d1 = -2.0;

    for (u32 i = 0; i < plateCount_; ++i)
    {
        if (d[i] > d0)
        {
            second = nearest;
            d1 = d0;
            nearest = i;
            d0 = d[i];
        }
        else if (d[i] > d1)
        {
            second = i;
            d1 = d[i];
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

    const f64 width = std::max(boundaryWidth, 1.0e-9);
    // Continental/oceanic bias keeps blending over the recipe width.
    const f64 biasWidth = std::max(desc_.boundaryWidthDot, 1.0e-9);

    // Every plate whose claim is within one boundary width of the top is part
    // of the local boundary structure. Evaluating all pairs among them, rather
    // than only (nearest, runner-up), keeps every mask continuous: with a
    // single runner-up pair the boundary normal and relative velocity jump
    // along the line where the runner-up plate changes identity, cutting
    // mountain belts with straight edges that start at triple junctions.
    std::array<u32, kMaxBoundaryCandidates> candidates{};
    u32 candidateCount = 0;
    for (u32 i = 0; i < plateCount_ && candidateCount < kMaxBoundaryCandidates; ++i)
    {
        if (d0 - d[i] < width)
        {
            candidates[candidateCount++] = i;
        }
    }

    const f64 referenceSpeed =
        std::max(desc_.convergenceReferenceSpeed, 1.0e-9);
    const f64 transformReferenceSpeed =
        std::max(desc_.transformReferenceSpeed, 1.0e-9);

    f64 convergenceMask = 0.0;
    f64 divergenceMask = 0.0;
    f64 transformMask = 0.0;
    f64 convergenceContinental = 0.0;
    f64 convergenceMixed = 0.0;
    f64 convergenceOceanic = 0.0;
    f64 subductionTrench = 0.0;
    f64 subductionArc = 0.0;
    f64 relativeSpeed = 0.0;

    for (u32 a = 0; a < candidateCount; ++a)
    {
        for (u32 b = a + 1; b < candidateCount; ++b)
        {
            const u32 i = candidates[a];
            const u32 j = candidates[b];

            // Closeness of each plate to the top, and of the pair to each
            // other. For the top two plates this reduces exactly to the
            // original (d0 - d1) boundary mask.
            // Small plates narrow the boundary around them (see widthLimit).
            f64 pairWidth = width;
            if (widthScale != nullptr)
            {
                pairWidth *= 0.5 * ((*widthScale)[i] + (*widthScale)[j]);
            }
            if (widthLimit != nullptr)
            {
                pairWidth = std::min({pairWidth, (*widthLimit)[i], (*widthLimit)[j]});
            }
            pairWidth = std::max(pairWidth, 1.0e-9);
            const f64 weight = std::min(
                {Smooth(1.0 - (d0 - d[i]) / pairWidth),
                 Smooth(1.0 - (d0 - d[j]) / pairWidth),
                 Smooth(1.0 - std::abs(d[i] - d[j]) / pairWidth)});
            if (weight <= 0.0)
            {
                continue;
            }

            math::Double3 normal{};
            if (normalFn != nullptr)
            {
                const math::Double3 raw = (*normalFn)(i, j);
                const math::Double3 tangent = raw - direction * math::Dot(raw, direction);
                normal = math::Normalize(tangent);
            }
            else
            {
                const math::Double3 towardJ =
                    plates_[j].seedDirection - plates_[i].seedDirection;
                const math::Double3 tangentToward =
                    towardJ - direction * math::Dot(towardJ, direction);
                normal = math::Normalize(tangentToward);
            }
            if (!(math::LengthSquared(normal) > 0.0))
            {
                continue;
            }

            const math::Double3 relative =
                math::Cross(plates_[i].eulerVector, direction) -
                math::Cross(plates_[j].eulerVector, direction);

            // Motion perpendicular to the boundary line is convergence or
            // divergence; motion along it is lateral shear. Swapping i and j
            // flips both the normal and the relative velocity, so every term
            // below is symmetric in the pair.
            const math::Double3 alongBoundary =
                math::Cross(direction, normal);
            const f64 normalSpeed = math::Dot(relative, normal);
            const f64 shearSpeed = math::Dot(relative, alongBoundary);

            relativeSpeed = std::max(relativeSpeed, weight * std::sqrt(math::LengthSquared(relative)));
            const f64 convergence = std::max(0.0, -normalSpeed);
            const f64 divergence = std::max(0.0, normalSpeed);
            const f64 shear = std::abs(shearSpeed);

            const bool continentalI = plates_[i].isContinental;
            const bool continentalJ = plates_[j].isContinental;
            const f64 collisionScale =
                (continentalI || continentalJ)
                    ? 1.0
                    : desc_.oceanicConvergenceScale;

            const f64 convergenceTerm =
                weight * Smooth(convergence / referenceSpeed);
            convergenceMask = std::max(
                convergenceMask, convergenceTerm * collisionScale);
            divergenceMask = std::max(
                divergenceMask, weight * Smooth(divergence / referenceSpeed));
            transformMask = std::max(
                transformMask, weight * Smooth(shear / transformReferenceSpeed));

            // Subduction polarity. The more oceanic plate descends; between
            // equally oceanic plates the older (denser) one does. Continent-
            // continent pairs collide without a trench or arc.
            const f64 baseI = plates_[i].continentalBase;
            const f64 baseJ = plates_[j].continentalBase;
            if (std::min(baseI, baseJ) < 0.5)
            {
                bool iDescends = baseI < baseJ;
                if (std::abs(baseI - baseJ) < 1.0e-6)
                {
                    iDescends = plates_[i].crustAge > plates_[j].crustAge;
                }
                const u32 descending = iDescends ? i : j;
                const u32 overriding = iDescends ? j : i;
                // -1..1 across the boundary, positive on the overriding side.
                const f64 side = (d[overriding] - d[descending]) / width;
                const f64 pairTerm = convergenceTerm * collisionScale;
                const f64 trenchOffset = (side + 0.35) / 0.25;
                const f64 arcOffset = (side - 0.45) / 0.50;
                subductionTrench = std::max(
                    subductionTrench, pairTerm * std::exp(-trenchOffset * trenchOffset));
                subductionArc = std::max(
                    subductionArc, pairTerm * std::exp(-arcOffset * arcOffset));
            }

            if (continentalI && continentalJ)
            {
                convergenceContinental =
                    std::max(convergenceContinental, convergenceTerm);
            }
            else if (continentalI || continentalJ)
            {
                convergenceMixed =
                    std::max(convergenceMixed, convergenceTerm);
            }
            else
            {
                convergenceOceanic =
                    std::max(convergenceOceanic, convergenceTerm);
            }
        }
    }

    // Continental/oceanic bias: a blend over every plate near the top. Each
    // plate's weight depends only on its own claim relative to the top, so the
    // blend is symmetric and continuous where the nearest or the runner-up
    // plate changes. With g the original cross-blend factor (0.5 for the top
    // plate, falling to 0 a boundary width away), weights g / (1 - g) make the
    // two-plate case reproduce the original lerp(nearest, second, g) exactly.
    f64 biasWeightSum = 0.0;
    f64 biasSum = 0.0;
    for (u32 i = 0; i < plateCount_; ++i)
    {
        const f64 g = Smooth(0.5 + 0.5 * (d[i] - d0) / biasWidth);
        const f64 u = g / (1.0 - g);
        biasWeightSum += u;
        biasSum += u * plates_[i].continentalBiasMeters;
    }
    const f64 plateBiasMeters = biasWeightSum > 0.0
        ? biasSum / biasWeightSum
        : plates_[nearest].continentalBiasMeters;

    return {
        .nearestPlate = nearest,
        .secondPlate = second,
        .convergenceMask = convergenceMask,
        .orogenEnvelope = convergenceMask,
        .relativeSpeed = relativeSpeed,
        .divergenceMask = divergenceMask,
        .transformMask = transformMask,
        .plateBiasMeters = plateBiasMeters,
        .convergenceContinental = convergenceContinental,
        .convergenceMixed = convergenceMixed,
        .convergenceOceanic = convergenceOceanic,
        .subductionTrench = subductionTrench,
        .subductionArc = subductionArc,
        .nearestIsContinental = plates_[nearest].isContinental,
        .secondIsContinental = plates_[second].isContinental
    };
}

TectonicStructureSample TectonicField::SampleStructure(
    const math::Double3& direction,
    const bool includeHotspot) const noexcept
{
    ClaimArray claims{};
    for (u32 i = 0; i < plateCount_; ++i)
    {
        claims[i] =
            math::Dot(direction, plates_[i].seedDirection) +
            plates_[i].sizeBiasDot;
    }
    return SampleStructureWithClaims(
        direction, includeHotspot, claims, nullptr, desc_.boundaryWidthDot);
}

TectonicStructureSample TectonicField::SampleStructureWithClaims(
    const math::Double3& direction,
    const bool includeHotspot,
    const ClaimArray& claims,
    const BoundaryNormalFn* const normalFn,
    const f64 boundaryWidth,
    const ClaimArray* const widthLimit,
    const ClaimArray* const widthScale) const noexcept
{
    const TectonicSample base = SampleWithClaims(
        direction, claims, normalFn, boundaryWidth, widthLimit, widthScale);
    const Plate& plate = plates_[base.nearestPlate];

    TectonicStructureSample out{};
    out.plateId = base.nearestPlate;
    out.neighbourPlateId = base.secondPlate;
    out.continental = base.nearestIsContinental;
    out.neighbourContinental = base.secondIsContinental;
    out.convergence = base.convergenceMask;
    out.divergence = base.divergenceMask;
    out.transform = base.transformMask;
    out.plateSpeedMetersPerUnit = planetRadiusMeters_ *
        std::sqrt(math::LengthSquared(math::Cross(plate.eulerVector, direction)));

    const f64 strongest = std::max(
        {base.convergenceMask, base.divergenceMask, base.transformMask});
    out.boundaryStrength = strongest;
    if (strongest > 0.05)
    {
        out.boundaryType = base.convergenceMask >= base.divergenceMask &&
                base.convergenceMask >= base.transformMask
            ? TectonicBoundaryType::Convergent
            : (base.divergenceMask >= base.transformMask
                ? TectonicBoundaryType::Divergent
                : TectonicBoundaryType::Transform);
    }

    // Interior crust blends across the boundary band with the same symmetric
    // weights as the plate bias (see Sample), so thickness and age stay
    // continuous where the nearest or the runner-up plate changes.
    f64 dTop = -2.0;
    const ClaimArray& claim = claims;
    for (u32 i = 0; i < plateCount_; ++i)
    {
        dTop = std::max(dTop, claim[i]);
    }
    const f64 blendWidth = std::max(desc_.boundaryWidthDot, 1.0e-9);
    f64 weightSum = 0.0;
    f64 thicknessSum = 0.0;
    f64 ageSum = 0.0;
    f64 baseFractionSum = 0.0;
    for (u32 i = 0; i < plateCount_; ++i)
    {
        const f64 g = Smooth(0.5 + 0.5 * (claim[i] - dTop) / blendWidth);
        const f64 u = g / (1.0 - g);
        weightSum += u;
        baseFractionSum += u * plates_[i].continentalBase;
        thicknessSum += u * plates_[i].crustThicknessKm;
        ageSum += u * plates_[i].crustAge;
    }
    f64 thickness = weightSum > 0.0
        ? thicknessSum / weightSum : plate.crustThicknessKm;
    f64 age = weightSum > 0.0
        ? ageSum / weightSum : plate.crustAge;

    // Continental crust is its own field: the plate-wide tendency plus warped
    // low-frequency noise, so continents have their own outlines inside (and
    // across) plates. Thickness and age follow it rather than the plate flag.
    const f64 baseFraction = weightSum > 0.0
        ? baseFractionSum / weightSum : plate.continentalBase;
    const u64 crustSeed = desc_.seed ^ 0x43525553544655ULL;
    const math::Double3 warped = direction * 1.4 +
        VectorNoise3D(direction * 1.7, crustSeed ^ 0x57415250ULL) * 0.55;
    const f64 outline =
        0.65 * ValueNoise3D(warped * 1.6, crustSeed) +
        0.35 * ValueNoise3D(warped * 4.1, crustSeed ^ 0x32ULL);
    const f64 fraction = Smooth(std::clamp(
        0.5 + (baseFraction - 0.5) * 1.1 + outline * 0.5, 0.0, 1.0));
    out.continentalCrustFraction = fraction;
    thickness = Lerp(7.5, 39.0, fraction) + (thickness - Lerp(7.5, 39.0, baseFraction));
    age = Lerp(std::min(age, 0.5), std::max(age, 0.55), fraction);

    // Collision type comes from the continuous per-class masks, not from the
    // runner-up plate's flags (which switch abruptly).
    const f64 arcMask =
        std::max(base.convergenceMixed, base.convergenceOceanic);

    // Collision thickens crust (roots of an orogen); spreading thins it and
    // resets its age to newly formed.
    thickness += std::max(
        {base.convergenceContinental * 28.0, arcMask * 12.0});
    thickness -= base.divergenceMask * 0.45 * thickness;
    age = Lerp(age, 0.02, base.divergenceMask);
    out.crustThicknessKm = std::max(thickness, 3.0);
    out.crustAge = std::clamp(age, 0.0, 1.0);

    // Geological age drives morphology: active orogens read young even when
    // the underlying plate is old.
    out.geologicalAge = std::clamp(
        Lerp(out.crustAge, 0.12, std::max(base.convergenceMask, base.divergenceMask) * 0.8),
        0.0, 1.0);

    const f64 hotspot =
        includeHotspot ? HotspotElevationMeters(direction) : 0.0;
    const f64 hotspotRelief = std::max(desc_.hotspotBaseReliefMeters, 1.0);

    // Uplift and subsidence mirror what the terrain stack already applies so
    // consumers see the same structure, but are exposed as separate fields.
    out.upliftMeters = base.convergenceMask * desc_.convergenceUpliftMeters +
        std::max(hotspot, 0.0);
    // Trench on the oceanic side of a subduction zone, rift floor on
    // divergence, and passive-margin sag where oceanic crust is old.
    // The trench is one-sided now (descending plate), and the arc lifts the
    // overriding plate inland of it.
    const f64 trench = base.subductionTrench * 0.55;
    out.upliftMeters += base.subductionArc * 0.3 * desc_.convergenceUpliftMeters;
    out.subductionTrench = base.subductionTrench;
    out.volcanicArc = base.subductionArc;
    out.subsidenceMeters = trench * desc_.convergenceUpliftMeters +
        base.divergenceMask * 0.25 * desc_.convergenceUpliftMeters;

    {
        // Structural elevation: what the structure implies before noise.
        const f64 unitMeters = desc_.convergenceUpliftMeters;
        const f64 divergence = std::clamp(base.divergenceMask, 0.0, 1.0);
        const f64 oceanic = 1.0 - fraction;
        // Spreading ridge swell, and ocean floor deepening with age away from it.
        const f64 ridge = divergence * oceanic * 0.65;
        const f64 ageDepth = -oceanic * out.crustAge * 0.4;
        // Continental rift: floor drops, flanks (partial divergence) rise.
        const f64 riftFloor = -divergence * fraction * 0.35;
        const f64 riftShoulder = 4.0 * divergence * (1.0 - divergence) * fraction * 0.25;
        const f64 trenchDepth = -base.subductionTrench * 1.2;
        // Volcanic arcs are chains of volcanoes, not a continuous ridge: along
        // strike they come and go. Where the overriding plate is oceanic the
        // chain must reach above sea level (island arcs) from a -2.6 km floor;
        // on continental crust the arc rides the range and needs far less.
        const f64 chainNoise = 0.5 + 0.5 * ValueNoise3D(direction * 38.0, desc_.seed ^ 0x41524358ULL);
        const f64 chain = Smooth(std::clamp((chainNoise - 0.35) / 0.25, 0.0, 1.0));
        const f64 arcRise = base.subductionArc *
            ((1.0 - fraction) * (0.45 + 2.2 * chain) + fraction * 0.55);
        out.structuralElevationMeters = unitMeters *
            (ridge + ageDepth + riftFloor + riftShoulder + trenchDepth + arcRise);
    }

    out.stress = std::clamp(
        std::max(base.convergenceMask, base.transformMask * 0.8), 0.0, 1.0);
    const f64 arc = base.subductionArc * 0.9;
    out.volcanism = std::clamp(
        std::max({arc, base.divergenceMask * 0.6,
                  std::clamp(hotspot / hotspotRelief, 0.0, 1.0)}),
        0.0, 1.0);
    return out;
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
