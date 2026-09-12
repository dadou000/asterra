#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>
#include <orbit/world/Planet.hpp>

#include <cmath>
#include <iostream>
#include <vector>

namespace
{
class ConstantTerrainSource final :
    public orbit::terrain::TerrainSource
{
public:
    [[nodiscard]] orbit::terrain::TerrainSample
    Sample(
        const orbit::terrain::TerrainQuery& query)
        const noexcept override
    {
        return {
            .elevationMeters =
                query.footprintMeters,
            .coarseElevationMeters =
                query.footprintMeters,
            .biomes = {
                .ocean = 0.0F,
                .desert = 1.0F,
                .grassland = 0.0F,
                .temperateForest = 0.0F,
                .borealForest = 0.0F,
                .tundra = 0.0F,
                .alpine = 0.0F,
                .wetland = 0.0F
            }
        };
    }
};

bool NearlyEqual(
    const orbit::f32 a,
    const orbit::f32 b)
{
    return std::abs(a - b) <= 1.0e-6F;
}
} // namespace

int main()
{
    orbit::jobs::JobSystem jobs(2);

    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0
    };

    const ConstantTerrainSource source;

    orbit::terrain_stream::TerrainSampleStreamer
        streamer(
            jobs,
            planet,
            source);

    const orbit::world::SurfaceFrame frame =
        orbit::world::MakeSurfaceFrame(
            orbit::math::Double3{
                1.0,
                0.0,
                0.0
            });

    std::vector<
        orbit::terrain_stream::TerrainSampleRequest>
        requests;

    requests.push_back({
        .levelIndex = 0,
        .resolution = 9,
        .spacingMeters = 10.0,
        .footprintMeters = 5.0,
        .surfaceFrame = frame,
        .originX = 1,
        .originY = 0,
        .regions = {
            {
                .x = 0,
                .y = 0,
                .width = 2,
                .height = 9
            },
            {
                .x = 7,
                .y = 0,
                .width = 2,
                .height = 9
            }
        }
    });

    requests.push_back({
        .levelIndex = 1,
        .resolution = 9,
        .spacingMeters = 20.0,
        .footprintMeters = 20.0,
        .surfaceFrame = frame,
        .originX = 0,
        .originY = 2,
        .regions = {
            {
                .x = 0,
                .y = 8,
                .width = 9,
                .height = 1
            }
        }
    });

    const auto results =
        streamer.GenerateBlocking(
            requests);

    if (results.size() != 2)
    {
        std::cerr
            << "Terrain sample streamer returned the wrong result count.\n";
        return 1;
    }

    if (results[0].levelIndex != 0 ||
        results[0].sampleCount != 36 ||
        results[0].patches.size() != 2)
    {
        std::cerr
            << "Terrain sample streamer level zero metadata is wrong.\n";
        return 1;
    }

    if (results[1].levelIndex != 1 ||
        results[1].sampleCount != 9 ||
        results[1].patches.size() != 1)
    {
        std::cerr
            << "Terrain sample streamer level one metadata is wrong.\n";
        return 1;
    }

    for (const auto& patch :
         results[0].patches)
    {
        if (patch.samples.size() !=
            static_cast<std::size_t>(
                patch.region.width) *
            patch.region.height)
        {
            std::cerr
                << "Terrain sample patch size does not match its region.\n";
            return 1;
        }

        for (const auto& sample :
             patch.samples)
        {
            if (!NearlyEqual(
                    sample.elevationMeters,
                    5.0F))
            {
                std::cerr
                    << "Terrain sample streamer returned the wrong fine elevation.\n";
                return 1;
            }

            if (sample.biomeWeights0 !=
                    0x0000FF00U ||
                sample.biomeWeights1 != 0U)
            {
                std::cerr
                    << "Terrain sample streamer did not pack biome weights correctly.\n";
                return 1;
            }
        }
    }

    for (const auto& sample :
         results[1].patches[0].samples)
    {
        if (!NearlyEqual(
                sample.elevationMeters,
                20.0F))
        {
            std::cerr
                << "Terrain sample streamer returned the wrong coarse elevation.\n";
            return 1;
        }
    }

    auto submitted =
        streamer.Submit(
            requests);

    if (!submitted.IsValid())
    {
        std::cerr
            << "Terrain sample async batch is unexpectedly invalid.\n";
        return 1;
    }

    auto movedBatch =
        std::move(submitted);

    if (submitted.IsValid())
    {
        std::cerr
            << "Moved-from terrain sample batch still owns state.\n";
        return 1;
    }

    jobs.WaitIdle();

    if (!movedBatch.IsComplete())
    {
        std::cerr
            << "Terrain sample async batch did not report completion.\n";
        return 1;
    }

    std::vector<
        orbit::terrain_stream::TerrainSampleResult>
        asyncResults;

    if (!streamer.TryCollect(
            movedBatch,
            asyncResults))
    {
        std::cerr
            << "Terrain sample async results could not be collected.\n";
        return 1;
    }

    if (movedBatch.IsValid())
    {
        std::cerr
            << "Collected terrain sample batch still owns result state.\n";
        return 1;
    }

    if (asyncResults.size() != 2 ||
        asyncResults[0].sampleCount != 36 ||
        asyncResults[1].sampleCount != 9)
    {
        std::cerr
            << "Terrain sample async results do not match blocking results.\n";
        return 1;
    }

    const orbit::world::SurfaceFrame
        fineFrame =
            orbit::world::SurfaceFrameAtOffset(
                planet,
                frame,
                {20.0, 0.0});

    const std::vector<
        orbit::terrain_stream::TerrainSampleRequest>
        morphRequests{
            {
                .levelIndex = 0,
                .resolution = 9,
                .spacingMeters = 10.0,
                .footprintMeters = 5.0,
                .morphToCoarser = true,
                .morphStartHalfExtentMeters = 20.0,
                .morphEndHalfExtentMeters = 40.0,
                .coarseSpacingMeters = 40.0,
                .coarseFootprintMeters = 20.0,
                .surfaceFrame = fineFrame,
                .coarseSurfaceFrame = frame,
                .originX = 0,
                .originY = 0,
                .regions = {
                    {
                        .x = 0,
                        .y = 0,
                        .width = 9,
                        .height = 9
                    }
                }
            }
        };

    const auto morphResults =
        streamer.GenerateBlocking(
            morphRequests);

    const auto& morphSamples =
        morphResults[0].
            patches[0].
            samples;

    if (!NearlyEqual(
            morphSamples[4U * 9U + 4U].
                elevationMeters,
            5.0F))
    {
        std::cerr
            << "Terrain LOD morph changed the unmorphed center height.\n";
        return 1;
    }

    const auto& transitionSample =
        morphSamples[
            4U * 9U + 7U];

    if (!NearlyEqual(
            transitionSample.
                elevationMeters,
            12.5F))
    {
        std::cerr
            << "Terrain LOD morph did not blend transition height.\n";
        return 1;
    }

    const orbit::math::Double3
        transitionTargetDirection =
            orbit::world::
                DirectionAtSurfaceOffset(
                    planet,
                    fineFrame,
                    {
                        transitionSample.
                            morphTargetXMeters,
                        transitionSample.
                            morphTargetYMeters
                    });

    const orbit::math::Double2
        transitionInCoarseFrame =
            orbit::world::
                SurfaceOffsetBetweenDirections(
                    planet,
                    frame,
                    transitionTargetDirection);

    const orbit::f64 coarseXCells =
        transitionInCoarseFrame.x /
        40.0;

    const orbit::f64 coarseYCells =
        transitionInCoarseFrame.y /
        40.0;

    if (std::abs(
            coarseXCells -
            std::round(coarseXCells)) >
            1.0e-3 ||
        std::abs(
            coarseYCells -
            std::round(coarseYCells)) >
            1.0e-3)
    {
        std::cerr
            << "Terrain LOD morph target is not aligned to the coarser ring lattice.\n";
        return 1;
    }

    const auto& edgeSample =
        morphSamples[0];

    if (!NearlyEqual(
            edgeSample.elevationMeters,
            20.0F))
    {
        std::cerr
            << "Terrain LOD morph did not reach the coarse edge height.\n";
        return 1;
    }

    return 0;
}
