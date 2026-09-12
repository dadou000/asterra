#include <orbit/math/Vector.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain/TerrainFields.hpp>
#include <orbit/world/Planet.hpp>

#include <cmath>
#include <iostream>

namespace
{
bool NearlyEqual(
    const orbit::f64 a,
    const orbit::f64 b,
    const orbit::f64 epsilon)
{
    return std::abs(a - b) <= epsilon;
}

orbit::f64 BiomeSum(
    const orbit::terrain::BiomeWeights& weights)
{
    return
        weights.ocean +
        weights.desert +
        weights.grassland +
        weights.temperateForest +
        weights.borealForest +
        weights.tundra +
        weights.alpine +
        weights.wetland;
}
} // namespace

int main()
{
    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0
    };

    const orbit::terrain::GlobalTerrainFields
        fields(
            planet,
            {
                .seed = 1234567,
                .continentalAmplitudeMeters = 3'000.0,
                .continentalWavelengthMeters = 4'000'000.0,
                .continentalBiasMeters = -500.0,
                .mountainAmplitudeMeters = 2'000.0,
                .mountainWavelengthMeters = 1'200'000.0,
                .climateWavelengthMeters = 3'000'000.0
            });

    const orbit::math::Double3 direction =
        orbit::math::Normalize(
            orbit::math::Double3{
                0.71,
                0.18,
                -0.59
            });

    const auto first =
        fields.Sample({
            .unitDirection = direction,
            .footprintMeters = 1'000.0
        });

    const auto repeated =
        fields.Sample({
            .unitDirection = direction,
            .footprintMeters = 1'000.0
        });

    if (!NearlyEqual(
            first.coarseElevationMeters,
            repeated.coarseElevationMeters,
            0.0) ||
        first.climate.temperatureC !=
            repeated.climate.temperatureC ||
        first.climate.humidity !=
            repeated.climate.humidity)
    {
        std::cerr
            << "Global terrain fields are not deterministic.\n";
        return 1;
    }

    const auto equator =
        fields.Sample({
            .unitDirection = {
                1.0,
                0.0,
                0.0
            },
            .footprintMeters = 10'000.0
        });

    const auto pole =
        fields.Sample({
            .unitDirection = {
                0.0,
                1.0,
                0.0
            },
            .footprintMeters = 10'000.0
        });

    if (!(equator.climate.temperatureC >
          pole.climate.temperatureC +
              20.0F))
    {
        std::cerr
            << "Global climate latitude gradient is too weak or inverted.\n";
        return 1;
    }

    if (!NearlyEqual(
            BiomeSum(first.biomes),
            1.0,
            1.0e-5))
    {
        std::cerr
            << "Global biome weights are not normalized.\n";
        return 1;
    }

    const orbit::terrain::BiomeWeights ocean =
        orbit::terrain::ClassifyBiomeWeights(
            {
                .temperatureC = 18.0F,
                .humidity = 0.9F,
                .precipitation = 0.8F,
                .continentality = 0.1F
            },
            -1'000.0,
            0.0);

    if (ocean.ocean < 0.8F)
    {
        std::cerr
            << "Sub-sea terrain did not classify as ocean.\n";
        return 1;
    }

    const orbit::terrain::BiomeWeights desert =
        orbit::terrain::ClassifyBiomeWeights(
            {
                .temperatureC = 34.0F,
                .humidity = 0.12F,
                .precipitation = 0.10F,
                .continentality = 0.8F
            },
            300.0,
            0.0);

    if (desert.desert <=
        desert.temperateForest)
    {
        std::cerr
            << "Hot dry terrain did not prefer desert biome.\n";
        return 1;
    }

    const orbit::terrain::BiomeWeights alpine =
        orbit::terrain::ClassifyBiomeWeights(
            {
                .temperatureC = 4.0F,
                .humidity = 0.5F,
                .precipitation = 0.5F,
                .continentality = 0.6F
            },
            4'500.0,
            0.0);

    if (alpine.alpine < 0.5F)
    {
        std::cerr
            << "High terrain did not classify as alpine.\n";
        return 1;
    }

    const orbit::terrain::AnalyticTerrainSource
        terrain(
            planet,
            {
                .seed = 9988,
                .macroAmplitudeMeters = 1'000.0,
                .macroWavelengthMeters = 600'000.0,
                .detailAmplitudeMeters = 500.0,
                .detailWavelengthMeters = 50'000.0,
                .detailOctaves = 5
            });

    const auto fine =
        terrain.Sample({
            .unitDirection = direction,
            .footprintMeters = 100.0
        });

    const auto coarse =
        terrain.Sample({
            .unitDirection = direction,
            .footprintMeters = 200'000.0
        });

    if (!std::isfinite(
            fine.elevationMeters) ||
        !std::isfinite(
            fine.coarseElevationMeters) ||
        !std::isfinite(
            coarse.elevationMeters))
    {
        std::cerr
            << "Layered terrain produced a non-finite sample.\n";
        return 1;
    }

    if (!NearlyEqual(
            BiomeSum(fine.biomes),
            1.0,
            1.0e-5))
    {
        std::cerr
            << "Analytic terrain biome weights are not normalized.\n";
        return 1;
    }

    if (std::abs(
            fine.elevationMeters -
            fine.coarseElevationMeters) <
        1.0e-6)
    {
        std::cerr
            << "Fine terrain did not add regional/local detail over coarse elevation.\n";
        return 1;
    }

    return 0;
}
