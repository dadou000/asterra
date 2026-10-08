#include <orbit/terrain/GlobalTerrainFields.hpp>

#include "ProceduralNoise.hpp"
#include "TectonicField.hpp"
#include "TectonicGrowth.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::terrain
{
namespace
{
[[nodiscard]] TectonicFieldDesc ResolveTectonicDesc(
    const GlobalTerrainFieldDesc& desc) noexcept
{
    TectonicFieldDesc tectonic = desc.tectonic;
    if (tectonic.seed == 0)
    {
        tectonic.seed = desc.seed ^ 0x544543544F4E4943ULL;
    }
    return tectonic;
}
} // namespace

GlobalTerrainFields::GlobalTerrainFields(
    const world::PlanetDefinition planet,
    const GlobalTerrainFieldDesc desc)
    : planet_(planet),
      desc_(desc),
      tectonicField_(std::make_unique<detail::TectonicField>(
          planet.radiusMeters, ResolveTectonicDesc(desc)))
{
}

GlobalTerrainFields::~GlobalTerrainFields() = default;
GlobalTerrainFields::GlobalTerrainFields(GlobalTerrainFields&&) noexcept = default;
GlobalTerrainFields& GlobalTerrainFields::operator=(GlobalTerrainFields&&) noexcept = default;

GlobalTerrainFieldSample
GlobalTerrainFields::Sample(
    const TerrainQuery& query) const noexcept
{
    const math::Double3 direction =
        math::Normalize(
            query.unitDirection);

    if (math::LengthSquared(
            direction) <= 0.0 ||
        planet_.radiusMeters <= 0.0)
    {
        return {};
    }

    return SampleNormalized({direction, query.footprintMeters}, true);
}

f64 GlobalTerrainFields::ContinentalSignal(
    const math::Double3& direction) const noexcept
{
    const f64 continentalPrimary =
        detail::SampleBand(
            direction,
            planet_.radiusMeters,
            desc_.continentalWavelengthMeters,
            desc_.seed);

    const f64 continentalSecondary =
        detail::SampleBand(
            direction,
            planet_.radiusMeters,
            desc_.continentalWavelengthMeters * 0.53,
            desc_.seed ^ 0x58F38DED8C5A935FULL);

    return continentalPrimary * 0.76 + continentalSecondary * 0.24;
}

GlobalTerrainFieldSample GlobalTerrainFields::SampleNormalized(
    const TerrainQuery& query,
    const bool includeBiomes) const noexcept
{
    const auto& direction = query.unitDirection;

    const f64 continentalWeight =
        detail::DetailWeight(
            desc_.
                continentalWavelengthMeters,
            query.footprintMeters);

    const f64 mountainWeight =
        detail::DetailWeight(
            desc_.
                mountainWavelengthMeters,
            query.footprintMeters);

    const detail::TectonicSample tectonic =
        TectonicAt(direction);

    const f64 continentSignal =
        ContinentalSignal(direction);

    const f64 continentalAmplitudeSafe =
        std::max(
            desc_.continentalAmplitudeMeters,
            1.0e-9);

    // Plate identity nudges the same noise signal that already shapes
    // coastlines, rather than replacing it -- continents stay naturally
    // irregular while their overall placement coheres with plate shape.
    // tectonicContinentInfluence == 0 reproduces the old noise-only field.
    const f64 blendedSignal =
        continentSignal +
        (tectonic.plateBiasMeters / continentalAmplitudeSafe) *
            desc_.tectonic.tectonicContinentInfluence;

    const f64 continentalElevation =
        (blendedSignal *
             desc_.
                 continentalAmplitudeMeters +
         desc_.continentalBiasMeters) *
        continentalWeight;

    const f64 landMask =
        detail::Smooth(
            (blendedSignal +
             0.15) /
            0.55);

    const f64 mountainRidges =
        landMask > 0.0 && mountainWeight > 0.0 && desc_.mountainAmplitudeMeters > 0.0
        ? detail::RidgedBand(
            direction,
            planet_.radiusMeters,
            desc_.
                mountainWavelengthMeters,
            desc_.seed ^
                0xD1B54A32D192ED03ULL) : 0.0;

    const f64 mountainModulation =
        mountainRidges > 0.0 ? std::clamp(
            detail::SampleBand(
                direction,
                planet_.radiusMeters,
                desc_.
                    mountainWavelengthMeters *
                    2.4,
                desc_.seed ^
                    0x94D049BB133111EBULL) *
                0.5 +
            0.5,
            0.0,
            1.0) : 0.0;

    // Coarse ranges now cohere with plate boundaries instead of "wherever
    // this modulation noise happens to be high" -- convergenceMask carries
    // that structure (see TectonicField::Sample).
    // The belt is the plate boundary's own orogenic envelope and exists at
    // zero noise; ridged noise only sculpts it. The envelope comes from a
    // smooth raster, so it does not alias and must not fade with the sample
    // footprint -- only the noise sculpting does. Multiplying the whole belt by
    // mountainWeight made it vanish on coarse pages and reappear on fine ones,
    // a hard step of several kilometres wherever two levels meet.
    const f64 mountainElevation =
        (0.65 + 0.35 * mountainRidges * mountainModulation * mountainWeight) *
        landMask *
        tectonic.orogenEnvelope *
        desc_.
            mountainAmplitudeMeters;

    const f64 coarseElevation =
        continentalElevation +
        mountainElevation +
        tectonic.structuralElevationMeters;

    const f64 latitude =
        std::clamp(
            std::abs(
                direction.y),
            0.0,
            1.0);

    const f64 climateNoise =
        detail::SampleBand(
            direction,
            planet_.radiusMeters,
            desc_.
                climateWavelengthMeters,
            desc_.seed ^
                0xA24BAED4963EE407ULL);

    const f64 moistureNoise =
        detail::SampleBand(
            direction,
            planet_.radiusMeters,
            desc_.
                climateWavelengthMeters *
                0.72,
            desc_.seed ^
                0x9FB21C651E98DF25ULL);

    const f64 continentalityNoise =
        detail::SampleBand(
            direction,
            planet_.radiusMeters,
            desc_.
                climateWavelengthMeters *
                1.35,
            desc_.seed ^
                0xC13FA9A902A6328FULL);

    const f64 polarFactor =
        std::pow(
            latitude,
            1.18);

    f64 temperature =
        desc_.equatorTemperatureC +
        (desc_.poleTemperatureC -
         desc_.equatorTemperatureC) *
            polarFactor;

    temperature +=
        climateNoise *
        desc_.
            temperatureVariationC;

    temperature -=
        std::max(
            coarseElevation -
                desc_.seaLevelMeters,
            0.0) /
        1'000.0 *
        desc_.
            lapseRateCPerKilometer;

    f64 continentality =
        std::clamp(
            0.5 +
                continentalityNoise *
                    0.28 +
                std::max(
                    coarseElevation -
                        desc_.seaLevelMeters,
                    0.0) /
                    8'000.0,
            0.0,
            1.0);

    if (coarseElevation <
        desc_.seaLevelMeters)
    {
        continentality *= 0.2;
    }

    const f64 equatorialMoisture =
        1.0 -
        std::abs(
            latitude -
            0.16);

    f64 humidity =
        std::clamp(
            0.57 +
                moistureNoise *
                    0.30 -
                continentality *
                    0.18 +
                equatorialMoisture *
                    0.08,
            0.0,
            1.0);

    if (coarseElevation <
        desc_.seaLevelMeters)
    {
        humidity =
            std::max(
                humidity,
                0.88);
    }

    const f64 precipitation =
        std::clamp(
            humidity *
                (0.86 -
                 continentality *
                     0.25) *
                (0.88 +
                 climateNoise *
                     0.12),
            0.0,
            1.0);

    const TerrainClimate climate{
        .temperatureC =
            static_cast<f32>(
                temperature),
        .humidity =
            static_cast<f32>(
                humidity),
        .precipitation =
            static_cast<f32>(
                precipitation),
        .continentality =
            static_cast<f32>(
                continentality)
    };

    return {
        .nearestPlate = tectonic.nearestPlate,
        .secondPlate = tectonic.secondPlate,
        .coarseElevationMeters =
            coarseElevation,
        .landMask = landMask,
        .climate =
            climate,
        .biomes =
            includeBiomes ? ClassifyBiomeWeights(
                climate,
                coarseElevation,
                desc_.
                    seaLevelMeters) : BiomeWeights{},
        .convergenceMask = tectonic.convergenceMask,
        .orogenEnvelope = tectonic.orogenEnvelope,
        .divergenceMask = tectonic.divergenceMask,
        .transformMask = tectonic.transformMask,
        .convergenceContinental = tectonic.convergenceContinental,
        .convergenceMixed = tectonic.convergenceMixed,
        .convergenceOceanic = tectonic.convergenceOceanic,
        .nearestPlateContinental = tectonic.nearestIsContinental,
        .secondPlateContinental = tectonic.secondIsContinental,
        .hotspotElevationMeters =
            tectonicField_->HotspotElevationMeters(direction)
    };
}

f64 GlobalTerrainFields::PlateElevationEstimateMeters(
    const math::Double3& direction) const noexcept
{
    f64 plateBiasMeters = 0.0;
    f64 convergenceMask = 0.0;
    f64 structuralMeters = 0.0;
    if (desc_.bakedTectonics != nullptr)
    {
        const auto baked =
            desc_.bakedTectonics->SampleConvergenceAndBias(direction);
        plateBiasMeters = static_cast<f64>(baked.plateBiasMeters);
        structuralMeters = static_cast<f64>(baked.structuralElevationMeters);
        convergenceMask = std::clamp(static_cast<f64>(baked.convergence), 0.0, 1.0);
    }
    else
    {
        const detail::TectonicSample tectonic =
            tectonicField_->Sample(direction);
        plateBiasMeters = tectonic.plateBiasMeters;
        convergenceMask = tectonic.convergenceMask;
    }

    const f64 continentalAmplitudeSafe =
        std::max(
            desc_.continentalAmplitudeMeters,
            1.0e-9);

    const f64 blendedSignal =
        ContinentalSignal(direction) +
        (plateBiasMeters / continentalAmplitudeSafe) *
            desc_.tectonic.tectonicContinentInfluence;

    const f64 continentalElevation =
        blendedSignal *
            desc_.continentalAmplitudeMeters +
        desc_.continentalBiasMeters;

    const f64 convergenceBump =
        convergenceMask *
        desc_.tectonic.convergenceUpliftMeters;

    const f64 hotspotBump =
        tectonicField_->HotspotElevationMeters(direction);

    return continentalElevation + structuralMeters + convergenceBump + hotspotBump;
}

const GlobalTerrainFieldDesc&
GlobalTerrainFields::Description()
    const noexcept
{
    return desc_;
}

TectonicStructureSample GlobalTerrainFields::SampleTectonicStructure(
    const math::Double3& direction) const noexcept
{
    const math::Double3 unit = math::Normalize(direction);
    const math::Double3 safe =
        math::LengthSquared(unit) > 0.0 ? unit : math::Double3{0.0, 1.0, 0.0};

    if (desc_.bakedTectonics == nullptr)
    {
        return tectonicField_->SampleStructure(safe);
    }

    // Assemble from the baked layers; only the closed-form hotspot chains are
    // evaluated here.
    const BakedTectonicRasters& bake = *desc_.bakedTectonics;
    const BakedTectonicTexel texel = bake.Sample(safe);
    const auto layer = [&texel](const BakedTectonicLayer l)
    {
        return static_cast<f64>(texel.Get(l));
    };
    const auto unit01 = [](const f64 value)
    {
        return std::clamp(value, 0.0, 1.0);
    };

    TectonicStructureSample out{};
    out.plateId = texel.plate;
    out.neighbourPlateId = texel.neighbour;
    out.continental = bake.PlateIsContinental(texel.plate);
    out.neighbourContinental = bake.PlateIsContinental(texel.neighbour);
    out.convergence = unit01(layer(BakedTectonicLayer::Convergence));
    out.divergence = unit01(layer(BakedTectonicLayer::Divergence));
    out.transform = unit01(layer(BakedTectonicLayer::Transform));
    out.plateSpeedMetersPerUnit =
        std::max(0.0, layer(BakedTectonicLayer::PlateSpeedMeters));

    const f64 strongest =
        std::max({out.convergence, out.divergence, out.transform});
    out.boundaryStrength = strongest;
    if (strongest > 0.05)
    {
        out.boundaryType =
            out.convergence >= out.divergence &&
                    out.convergence >= out.transform
                ? TectonicBoundaryType::Convergent
                : (out.divergence >= out.transform
                       ? TectonicBoundaryType::Divergent
                       : TectonicBoundaryType::Transform);
    }

    out.continentalCrustFraction =
        unit01(layer(BakedTectonicLayer::ContinentalCrustFraction));
    out.subductionTrench = unit01(layer(BakedTectonicLayer::SubductionTrench));
    out.volcanicArc = unit01(layer(BakedTectonicLayer::VolcanicArc));
    out.structuralElevationMeters =
        layer(BakedTectonicLayer::StructuralElevationMeters);
    out.fractureDensity = unit01(layer(BakedTectonicLayer::FractureDensity));
    out.crustThicknessKm = std::max(3.0, layer(BakedTectonicLayer::CrustThicknessKm));
    out.crustAge = unit01(layer(BakedTectonicLayer::CrustAge));
    out.geologicalAge = unit01(layer(BakedTectonicLayer::GeologicalAge));
    out.stress = unit01(layer(BakedTectonicLayer::Stress));

    const f64 hotspot =
        std::max(tectonicField_->HotspotElevationMeters(safe), 0.0);
    const f64 hotspotRelief =
        std::max(desc_.tectonic.hotspotBaseReliefMeters, 1.0);
    out.upliftMeters =
        std::max(0.0, layer(BakedTectonicLayer::UpliftMeters)) + hotspot;
    out.subsidenceMeters =
        std::max(0.0, layer(BakedTectonicLayer::SubsidenceMeters));
    out.volcanism = unit01(std::max(
        layer(BakedTectonicLayer::VolcanismArc),
        std::clamp(hotspot / hotspotRelief, 0.0, 1.0)));
    return out;
}

std::shared_ptr<const TectonicGrowth> GlobalTerrainFields::BuildTectonicGrowth(
    const u32 resolution,
    const std::atomic<bool>* const cancel,
    const u32 workers) const
{
    return TectonicGrowth::Build(
        *tectonicField_, resolution, desc_.tectonic.seed != 0U ? desc_.tectonic.seed : desc_.seed,
        cancel, workers);
}

f64 GlobalTerrainFields::FaultIntensity(
    const math::Double3& direction,
    const f64 across,
    const f64 activity) const noexcept
{
    // Thin ridges of stripe sets parallel to the boundary (constant claim
    // difference follows every bend of it). The phase meanders only gently --
    // its gradient stays well below the stripe frequency, so the sets cannot
    // close into whorls -- and each set exists only in patches along strike,
    // so faults are segmented and offset instead of continuous.
    const u64 faultSeed = desc_.seed ^ 0x4641554C54ULL;
    const auto patch = [&](const f64 frequency, const u64 salt)
    {
        const f64 n = 0.5 + 0.5 * detail::ValueNoise3D(direction * frequency, faultSeed ^ salt);
        return detail::Smooth(std::clamp((n - 0.30) / 0.30, 0.0, 1.0));
    };
    constexpr f64 kPi = 3.14159265358979323846;
    // Spacing varies along strike and across (denser towards the boundary), so
    // the stripes are not an evenly spaced comb.
    const f64 u = std::pow(std::max(across, 0.0), 1.3);
    const f64 rateA = 2.2 + 0.9 * detail::ValueNoise3D(direction * 2.2, faultSeed ^ 0xCULL);
    const f64 rateB = 4.8 + 1.6 * detail::ValueNoise3D(direction * 2.7, faultSeed ^ 0xDULL);
    const f64 phaseA = rateA * u + 0.7 * detail::ValueNoise3D(direction * 3.0, faultSeed ^ 0xAULL);
    const f64 phaseB = rateB * u + 1.1 * detail::ValueNoise3D(direction * 3.7, faultSeed ^ 0xBULL);
    const f64 ridgeA = std::pow(1.0 - std::abs(std::sin(kPi * phaseA)), 3.0);
    const f64 ridgeB = std::pow(1.0 - std::abs(std::sin(kPi * phaseB)), 4.0);
    const f64 faults = std::max(
        patch(7.0, 0x11ULL) * ridgeA, 0.6 * patch(9.0, 0x12ULL) * ridgeB);
    return std::clamp(std::pow(activity, 0.7) * (0.08 + 0.92 * faults), 0.0, 1.0);
}

BakedTectonicTexel GlobalTerrainFields::EvaluateTectonicTexel(
    const TectonicGrowth& growth,
    const u32 face,
    const i32 x,
    const i32 y) const noexcept
{
    const u32 resolution = growth.Resolution();
    const math::Double3 safe = BakedTectonicTexelDirection(face, x, y, resolution);

    detail::ClaimArray claims{};
    growth.Claims(face, x, y, claims);
    const detail::BoundaryNormalFn normalFn =
        [&](const u32 i, const u32 j) { return growth.BoundaryNormal(face, x, y, i, j); };
    // Structure (boundary masks, trench, arc, rift, stress) is evaluated at the
    // narrow structure width; the wide envelope terrain relief is built from is
    // stored separately.
    const f64 structureWidth = tectonicField_->StructureWidth();
    const detail::ClaimArray& limits = growth.WidthLimits();
    const detail::ClaimArray& scales = growth.WidthScales();
    const detail::TectonicSample sample = tectonicField_->SampleWithClaims(
        safe, claims, &normalFn, structureWidth, &limits, &scales);
    const detail::TectonicSample wide = tectonicField_->SampleWithClaims(
        safe, claims, &normalFn, tectonicField_->BoundaryWidth(), nullptr, &scales);
    const detail::TectonicSample envelope = tectonicField_->SampleWithClaims(
        safe, claims, &normalFn, tectonicField_->EnvelopeWidth(), nullptr, &scales);
    TectonicStructureSample structure = tectonicField_->SampleStructureWithClaims(
        safe, false, claims, &normalFn, structureWidth, &limits, &scales);

    // Broad flanks. The narrow structure band alone makes trenches and ridge
    // crests hair-thin; a real trench has a wide flexural flank on the
    // descending plate and a mid-ocean ridge a broad thermal swell. Part of
    // each amplitude moves from the narrow band to the wide one.
    {
        const f64 unit = desc_.tectonic.convergenceUpliftMeters;
        const f64 oceanic = 1.0 - structure.continentalCrustFraction;
        structure.structuralElevationMeters +=
            0.35 * unit * structure.subductionTrench - 0.35 * unit * wide.subductionTrench;
        structure.structuralElevationMeters +=
            0.45 * unit * oceanic * (wide.divergenceMask - std::clamp(sample.divergenceMask, 0.0, 1.0));
    }

    // Distributed deformation: stress depends on how fast the plates move
    // past each other (the masks saturate, so on their own they paint every
    // boundary the same) and is the narrow core plus a broad halo that decays
    // away from the boundary.
    {
        const auto activity = [](const detail::TectonicSample& s)
        {
            return std::max(s.convergenceMask, 0.8 * s.transformMask);
        };
        const f64 speed = std::clamp(sample.relativeSpeed / 1.4, 0.0, 1.0);
        const f64 rate = 0.35 + 0.65 * speed;
        structure.stress = std::clamp(
            rate * (0.6 * activity(sample) + 0.4 * std::pow(activity(wide), 1.6)), 0.0, 1.0);
    }

    {
        const f64 activity = std::clamp(
            std::max({sample.convergenceMask, sample.divergenceMask, sample.transformMask}),
            0.0, 1.0);
        const f64 across = std::abs(claims[sample.nearestPlate] - claims[sample.secondPlate]) /
            std::max(structureWidth, 1.0e-9);
        structure.fractureDensity = FaultIntensity(safe, across, activity);
    }
    // Strike-slip zones carry fault valleys.
    structure.structuralElevationMeters -= 0.1 *
        desc_.tectonic.convergenceUpliftMeters * sample.transformMask *
        structure.fractureDensity;

    BakedTectonicTexel texel;
    texel.Set(BakedTectonicLayer::Convergence, static_cast<f32>(sample.convergenceMask));
    texel.Set(BakedTectonicLayer::Divergence, static_cast<f32>(sample.divergenceMask));
    texel.Set(BakedTectonicLayer::Transform, static_cast<f32>(sample.transformMask));
    texel.Set(BakedTectonicLayer::ConvergenceContinental,
        static_cast<f32>(sample.convergenceContinental));
    texel.Set(BakedTectonicLayer::ConvergenceMixed,
        static_cast<f32>(sample.convergenceMixed));
    texel.Set(BakedTectonicLayer::ConvergenceOceanic,
        static_cast<f32>(sample.convergenceOceanic));
    texel.Set(BakedTectonicLayer::PlateBiasMeters, static_cast<f32>(sample.plateBiasMeters));
    texel.Set(BakedTectonicLayer::CrustThicknessKm, static_cast<f32>(structure.crustThicknessKm));
    texel.Set(BakedTectonicLayer::CrustAge, static_cast<f32>(structure.crustAge));
    texel.Set(BakedTectonicLayer::GeologicalAge, static_cast<f32>(structure.geologicalAge));
    texel.Set(BakedTectonicLayer::UpliftMeters, static_cast<f32>(structure.upliftMeters));
    texel.Set(BakedTectonicLayer::SubsidenceMeters, static_cast<f32>(structure.subsidenceMeters));
    texel.Set(BakedTectonicLayer::Stress, static_cast<f32>(structure.stress));
    texel.Set(BakedTectonicLayer::VolcanismArc, static_cast<f32>(structure.volcanism));
    texel.Set(BakedTectonicLayer::PlateSpeedMeters,
        static_cast<f32>(structure.plateSpeedMetersPerUnit));
    texel.Set(BakedTectonicLayer::ContinentalCrustFraction,
        static_cast<f32>(structure.continentalCrustFraction));
    texel.Set(BakedTectonicLayer::SubductionTrench,
        static_cast<f32>(structure.subductionTrench));
    texel.Set(BakedTectonicLayer::VolcanicArc,
        static_cast<f32>(structure.volcanicArc));
    texel.Set(BakedTectonicLayer::StructuralElevationMeters,
        static_cast<f32>(structure.structuralElevationMeters));
    // Geography follows the continuous crust type, not the plate flag.
    texel.Set(BakedTectonicLayer::PlateBiasMeters,
        static_cast<f32>(detail::Lerp(
            desc_.tectonic.oceanicPlateBiasMeters,
            desc_.tectonic.continentalPlateBiasMeters,
            structure.continentalCrustFraction)));
    texel.Set(BakedTectonicLayer::FractureDensity,
        static_cast<f32>(structure.fractureDensity));
    texel.Set(BakedTectonicLayer::OrogenEnvelope, static_cast<f32>(envelope.convergenceMask));
    texel.plate = static_cast<u8>(sample.nearestPlate);
    texel.neighbour = static_cast<u8>(sample.secondPlate);
    return texel;
}

u32 GlobalTerrainFields::TectonicPlateCount() const noexcept
{
    return desc_.tectonic.plateCount;
}

bool GlobalTerrainFields::TectonicPlateIsContinental(const u32 plate) const noexcept
{
    const auto plates = tectonicField_->BuildGpuPlates();
    return plate < plates.size() && plates[plate].isContinental;
}

detail::TectonicSample GlobalTerrainFields::TectonicAt(
    const math::Double3& direction) const noexcept
{
    if (desc_.bakedTectonics == nullptr)
    {
        return tectonicField_->Sample(direction);
    }

    const BakedTectonicRasters& bake = *desc_.bakedTectonics;
    const BakedTectonicTexel texel = bake.Sample(direction);
    const auto mask = [&texel](const BakedTectonicLayer l)
    {
        return std::clamp(static_cast<f64>(texel.Get(l)), 0.0, 1.0);
    };

    detail::TectonicSample sample{};
    sample.nearestPlate = texel.plate;
    sample.secondPlate = texel.neighbour;
    sample.structuralElevationMeters =
        static_cast<f64>(texel.Get(BakedTectonicLayer::StructuralElevationMeters));
    sample.convergenceMask = mask(BakedTectonicLayer::Convergence);
    sample.orogenEnvelope = mask(BakedTectonicLayer::OrogenEnvelope);
    sample.divergenceMask = mask(BakedTectonicLayer::Divergence);
    sample.transformMask = mask(BakedTectonicLayer::Transform);
    sample.plateBiasMeters =
        static_cast<f64>(texel.Get(BakedTectonicLayer::PlateBiasMeters));
    sample.convergenceContinental = mask(BakedTectonicLayer::ConvergenceContinental);
    sample.convergenceMixed = mask(BakedTectonicLayer::ConvergenceMixed);
    sample.convergenceOceanic = mask(BakedTectonicLayer::ConvergenceOceanic);
    sample.nearestIsContinental = bake.PlateIsContinental(texel.plate);
    sample.secondIsContinental = bake.PlateIsContinental(texel.neighbour);
    return sample;
}

std::vector<GpuTectonicPlate>
GlobalTerrainFields::TectonicPlatesForGpu() const
{
    return tectonicField_->BuildGpuPlates();
}

std::vector<GpuTectonicHotspot>
GlobalTerrainFields::TectonicHotspotsForGpu() const
{
    return tectonicField_->BuildGpuHotspots();
}
} // namespace orbit::terrain
