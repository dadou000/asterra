#include <orbit/terrain/GlobalTerrainFields.hpp>

#include "ProceduralNoise.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::terrain
{
GlobalTerrainFields::GlobalTerrainFields(
    const world::PlanetDefinition planet,
    const GlobalTerrainFieldDesc desc)
    : planet_(planet),
      desc_(desc)
{
}

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

    const f64 continentalPrimary =
        detail::SampleBand(
            direction,
            planet_.radiusMeters,
            desc_.
                continentalWavelengthMeters,
            desc_.seed);

    const f64 continentalSecondary =
        detail::SampleBand(
            direction,
            planet_.radiusMeters,
            desc_.
                continentalWavelengthMeters *
                0.53,
            desc_.seed ^
                0x58F38DED8C5A935FULL);

    const f64 continentSignal =
        continentalPrimary *
            0.76 +
        continentalSecondary *
            0.24;

    const f64 continentalElevation =
        (continentSignal *
             desc_.
                 continentalAmplitudeMeters +
         desc_.continentalBiasMeters) *
        continentalWeight;

    const f64 landMask =
        detail::Smooth(
            (continentSignal +
             0.15) /
            0.55);

    const f64 mountainRidges =
        detail::RidgedBand(
            direction,
            planet_.radiusMeters,
            desc_.
                mountainWavelengthMeters,
            desc_.seed ^
                0xD1B54A32D192ED03ULL);

    const f64 mountainModulation =
        std::clamp(
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
            1.0);

    const f64 mountainElevation =
        mountainRidges *
        mountainModulation *
        landMask *
        desc_.
            mountainAmplitudeMeters *
        mountainWeight;

    const f64 coarseElevation =
        continentalElevation +
        mountainElevation;

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
        .coarseElevationMeters =
            coarseElevation,
        .climate =
            climate,
        .biomes =
            ClassifyBiomeWeights(
                climate,
                coarseElevation,
                desc_.
                    seaLevelMeters)
    };
}

const GlobalTerrainFieldDesc&
GlobalTerrainFields::Description()
    const noexcept
{
    return desc_;
}
} // namespace orbit::terrain
