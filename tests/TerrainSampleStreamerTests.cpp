#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
class MutableTerrainSource final :
    public orbit::terrain::TerrainSource
{
public:
    explicit MutableTerrainSource(
        const orbit::u64 revision) noexcept
        : revision_(revision)
    {
    }

    [[nodiscard]] orbit::terrain::TerrainSample
    Sample(
        const orbit::terrain::TerrainQuery&)
        const noexcept override
    {
        return {};
    }

    [[nodiscard]] orbit::u64 Revision()
        const noexcept override
    {
        return revision_.load(
            std::memory_order_acquire);
    }

    void SetRevision(
        const orbit::u64 revision) noexcept
    {
        revision_.store(
            revision,
            std::memory_order_release);
    }

private:
    std::atomic<orbit::u64> revision_;
};

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

// Both filtering footprint and finite-difference spacing change this source's
// slopes. A constant-height source cannot detect a parent-normal mismatch.
class SlopingTerrainSource final : public orbit::terrain::TerrainSource
{
public:
    SlopingTerrainSource(const orbit::world::SurfaceFrame& frame,
                         const orbit::f64 radius) noexcept
        : frame_(frame), radius_(radius) {}

    [[nodiscard]] orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery& query) const noexcept override
    {
        const auto east = orbit::math::Dot(query.unitDirection, frame_.east) * radius_;
        const auto north = orbit::math::Dot(query.unitDirection, frame_.north) * radius_;
        return {
            .elevationMeters = query.footprintMeters * east +
                0.05 * east * east + 0.02 * north * north
        };
    }

private:
    orbit::world::SurfaceFrame frame_;
    orbit::f64 radius_;
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

    MutableTerrainSource mutableSource(7);

    orbit::terrain_stream::TerrainSampleStreamer
        revisionStreamer(
            jobs,
            planet,
            mutableSource);

    if (revisionStreamer.SourceRevision() != 7)
    {
        std::cerr
            << "Terrain sample streamer did not expose the initial source revision.\n";
        return 1;
    }

    const std::vector<
        orbit::terrain_stream::TerrainSampleRequest>
        noRequests;

    auto revisionBatch =
        revisionStreamer.Submit(
            noRequests);

    if (revisionBatch.SourceRevision() != 7)
    {
        std::cerr
            << "Terrain sample batch did not snapshot its source revision.\n";
        return 1;
    }

    mutableSource.SetRevision(8);

    if (revisionStreamer.SourceRevision() != 8)
    {
        std::cerr
            << "Terrain sample streamer did not reflect the changed source revision.\n";
        return 1;
    }

    if (revisionBatch.SourceRevision() != 7)
    {
        std::cerr
            << "Terrain sample batch source revision changed after submission.\n";
        return 1;
    }

    std::vector<
        orbit::terrain_stream::TerrainSampleResult>
        revisionResults;

    if (!revisionStreamer.TryCollect(
            revisionBatch,
            revisionResults) ||
        !revisionResults.empty())
    {
        std::cerr
            << "Terrain sample empty revision batch did not collect cleanly.\n";
        return 1;
    }

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
                .centerOffsetMeters = {20.0, 0.0},
                .surfaceFrame = frame,
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
                    frame,
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

    // Compare the morphed child with independently generated parent samples,
    // including the perimeter, an intermediate blend and the untouched centre.
    const SlopingTerrainSource slopingSource(frame, planet.radiusMeters);
    orbit::terrain_stream::TerrainSampleStreamer slopeStreamer(jobs, planet, slopingSource);
    auto childRequest = morphRequests[0];
    auto fineRequest = childRequest;
    fineRequest.morphToCoarser = false;
    auto parentRequest = fineRequest;
    parentRequest.spacingMeters = childRequest.coarseSpacingMeters;
    parentRequest.footprintMeters = childRequest.coarseFootprintMeters;
    parentRequest.centerOffsetMeters = {0.0, 0.0};
    const orbit::terrain_stream::TerrainSampleRequest slopeRequests[]{
        childRequest, fineRequest, parentRequest
    };
    const auto slopeResults = slopeStreamer.GenerateBlocking(slopeRequests);
    const auto& child = slopeResults[0].patches[0].samples;
    const auto& fine = slopeResults[1].patches[0].samples;
    const auto& parent = slopeResults[2].patches[0].samples;
    for (orbit::u32 y = 0; y < 9; ++y)
    {
        for (orbit::u32 x = 0; x < 9; ++x)
        {
            const auto index = y * 9 + x;
            const auto& actual = child[index];
            const auto parentX = 4 + static_cast<int>(std::lround(
                actual.morphTargetXMeters / parentRequest.spacingMeters));
            const auto parentY = 4 + static_cast<int>(std::lround(
                actual.morphTargetYMeters / parentRequest.spacingMeters));
            const auto& coarse = parent[parentY * 9 + parentX];
            const auto edgeDistance = std::max(
                std::abs(static_cast<int>(x) - 4),
                std::abs(static_cast<int>(y) - 4)) * childRequest.spacingMeters;
            const auto t = std::clamp(
                (edgeDistance - childRequest.morphStartHalfExtentMeters) /
                    (childRequest.morphEndHalfExtentMeters - childRequest.morphStartHalfExtentMeters),
                0.0, 1.0);
            const auto morph = t * t * (3.0 - 2.0 * t);
            const auto expectedEast = fine[index].fineSlopeEast +
                (coarse.fineSlopeEast - fine[index].fineSlopeEast) * morph;
            const auto expectedNorth = fine[index].fineSlopeNorth +
                (coarse.fineSlopeNorth - fine[index].fineSlopeNorth) * morph;
            if (!std::isfinite(actual.fineSlopeEast) ||
                !std::isfinite(actual.fineSlopeNorth) ||
                std::abs(actual.fineSlopeEast - expectedEast) > 1.0e-5 ||
                std::abs(actual.fineSlopeNorth - expectedNorth) > 1.0e-5)
            {
                std::cerr << "Terrain morph normals disagree with independent parent slopes at "
                          << x << ", " << y << ".\n";
                return 1;
            }
        }
    }

    return 0;
}
