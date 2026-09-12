#include <orbit/jobs/JobSystem.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_cache/TerrainPageBuilder.hpp>
#include <orbit/terrain_cache/TerrainPageCache.hpp>

#include <cmath>
#include <iostream>
#include <memory>

int main()
{
    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0
    };

    const auto terrain =
        std::make_shared<orbit::terrain::AnalyticTerrainSource>(
            planet,
            orbit::terrain::AnalyticTerrainDesc{
                .seed = 0xA57E22AULL,
                .macroAmplitudeMeters = 3'000.0,
                .macroWavelengthMeters = 800'000.0,
                .detailAmplitudeMeters = 500.0,
                .detailWavelengthMeters = 80'000.0,
                .detailOctaves = 5
            });

    constexpr orbit::u32 resolution = 33;

    const auto positiveX = orbit::terrain_cache::BuildTerrainPage(
        planet,
        *terrain,
        {
            .tile = {
                .face = orbit::world::CubeFace::PositiveX,
                .level = 0,
                .x = 0,
                .y = 0
            },
            .resolution = resolution
        });

    const auto negativeZ = orbit::terrain_cache::BuildTerrainPage(
        planet,
        *terrain,
        {
            .tile = {
                .face = orbit::world::CubeFace::NegativeZ,
                .level = 0,
                .x = 0,
                .y = 0
            },
            .resolution = resolution
        });

    const std::size_t expectedCount =
        static_cast<std::size_t>(resolution) *
        static_cast<std::size_t>(resolution);

    if (positiveX.elevationMeters.size() != expectedCount ||
        negativeZ.elevationMeters.size() != expectedCount)
    {
        std::cerr << "Terrain page sample count is wrong.\n";
        return 1;
    }

    if (!(positiveX.approximateSampleSpacingMeters > 0.0))
    {
        std::cerr << "Terrain page spacing is invalid.\n";
        return 1;
    }

    for (orbit::u32 y = 0; y < resolution; ++y)
    {
        const float xFaceEdge =
            positiveX.At(resolution - 1U, y);

        const float zFaceEdge =
            negativeZ.At(0, y);

        if (std::abs(xFaceEdge - zFaceEdge) > 1.0e-5F)
        {
            std::cerr
                << "Terrain cache seam mismatch between +X and -Z.\n";
            return 1;
        }
    }

    orbit::jobs::JobSystem jobs(4);
    orbit::terrain_cache::TerrainPageCache cache(
        planet,
        terrain,
        jobs);

    const orbit::terrain_cache::TerrainPageDesc asyncDesc{
        .tile = {
            .face = orbit::world::CubeFace::PositiveY,
            .level = 5,
            .x = 11,
            .y = 19
        },
        .resolution = 65
    };

    if (!cache.Request(asyncDesc))
    {
        std::cerr << "Initial terrain cache request was rejected.\n";
        return 1;
    }

    if (cache.Request(asyncDesc))
    {
        std::cerr << "Duplicate terrain cache request was not deduplicated.\n";
        return 1;
    }

    if (cache.EntryCount() != 1)
    {
        std::cerr << "Terrain cache entry count is wrong.\n";
        return 1;
    }

    cache.WaitAll();

    const auto readyPage = cache.TryGet(asyncDesc);

    if (!readyPage)
    {
        std::cerr << "Asynchronous terrain page never became ready.\n";
        return 1;
    }

    if (readyPage->desc != asyncDesc ||
        readyPage->elevationMeters.size() !=
            static_cast<std::size_t>(asyncDesc.resolution) *
            static_cast<std::size_t>(asyncDesc.resolution))
    {
        std::cerr << "Asynchronous terrain page contents are invalid.\n";
        return 1;
    }

    if (cache.IsPending(asyncDesc))
    {
        std::cerr << "Ready terrain page is still marked pending.\n";
        return 1;
    }

    return 0;
}
