#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_region/DerivedRegionTerrainSource.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionStreamer.hpp>
#include <orbit/world/Planet.hpp>

#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>

namespace
{
class TestTerrainSource final :
    public orbit::terrain::TerrainSource
{
public:
    [[nodiscard]] orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery& query)
        const noexcept override
    {
        const orbit::f64 elevation =
            1'000.0 +
            query.unitDirection.x * 120.0 +
            query.unitDirection.y * 80.0 +
            query.unitDirection.z * 40.0;

        return {
            .elevationMeters = elevation,
            .coarseElevationMeters = elevation,
            .climate = {
                .temperatureC = 18.0F,
                .humidity = 0.6F,
                .precipitation = 0.7F,
                .continentality = 0.4F
            },
            .biomes = {
                .grassland = 1.0F
            }
        };
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
    std::atomic<orbit::u64> revision_{7};
};

[[nodiscard]] orbit::terrain_region::
DerivedTerrainRegionCacheConfig CacheConfig(
    const std::size_t maxEntries)
{
    orbit::terrain_region::
        DerivedTerrainRegionCacheConfig
            config{};

    config.tileLevel = 8;
    config.maxEntries = maxEntries;

    config.region.generatorVersion = 3;
    config.region.overlapScale = 1.10;
    config.region.hydrology.resolution = 5;
    config.region.hydrology.footprintMeters = 0.0;
    config.region.hydrology.useCoarseElevation = false;
    config.region.hydrology.conditionDepressions = true;
    config.region.hydrology.minimumDrainageDropMeters =
        0.01;

    config.region.refinement.iterations = 1;
    config.region.refinement.elevationDeltaScale =
        0.10;

    config.region.minimumRiverDrainageAreaSquareMeters =
        1.0;

    config.region.carving.spatialIndexResolution =
        8;

    return config;
}
} // namespace

int main()
{
    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0
    };

    auto source =
        std::make_shared<TestTerrainSource>();

    orbit::jobs::JobSystem jobs(2);

    {
        auto cache =
            std::make_shared<
                orbit::terrain_region::
                    DerivedTerrainRegionCache>(
                        planet,
                        source,
                        jobs,
                        CacheConfig(2));

        const orbit::math::Double3 directionA =
            orbit::math::Normalize(
                orbit::math::Double3{
                    1.0,
                    0.2,
                    0.1
                });

        const auto idA =
            cache->IdForDirection(
                directionA);

        if (!cache->Request(idA))
        {
            std::cerr
                << "First derived region request was rejected.\n";
            return 1;
        }

        if (cache->Request(idA))
        {
            std::cerr
                << "Duplicate derived region request was accepted.\n";
            return 1;
        }

        cache->WaitAll();

        if (!cache->TryGet(idA))
        {
            std::cerr
                << "Derived region did not become ready.\n";
            return 1;
        }

        const orbit::u64 firstContentRevision =
            cache->ContentRevision();

        if (firstContentRevision <= 1)
        {
            std::cerr
                << "Derived region readiness did not advance content revision.\n";
            return 1;
        }

        const orbit::math::Double3 directionB =
            orbit::math::Normalize(
                orbit::math::Double3{
                    1.0,
                    -0.25,
                    0.1
                });

        const auto idB =
            cache->IdForDirection(
                directionB);

        if (idB == idA)
        {
            std::cerr
                << "Cache test directions resolved to the same region.\n";
            return 1;
        }

        if (!cache->Request(idB))
        {
            std::cerr
                << "Second derived region request was rejected.\n";
            return 1;
        }

        cache->WaitAll();

        // Touch A so B is the least recently used ready entry.
        if (!cache->TryGet(idA))
        {
            std::cerr
                << "First region unexpectedly disappeared before LRU test.\n";
            return 1;
        }

        const orbit::math::Double3 directionC =
            orbit::math::Normalize(
                orbit::math::Double3{
                    1.0,
                    0.1,
                    -0.3
                });

        const auto idC =
            cache->IdForDirection(
                directionC);

        if (idC == idA ||
            idC == idB)
        {
            std::cerr
                << "Cache test third direction did not resolve to a unique region.\n";
            return 1;
        }

        if (!cache->Request(idC))
        {
            std::cerr
                << "Third derived region request was rejected instead of evicting LRU.\n";
            return 1;
        }

        cache->WaitAll();

        if (!cache->TryGet(idA) ||
            cache->TryGet(idB) ||
            !cache->TryGet(idC))
        {
            std::cerr
                << "Derived region cache did not preserve LRU ordering.\n";
            return 1;
        }

        const auto cacheStats =
            cache->Stats();

        if (cacheStats.evictions == 0 ||
            cacheStats.entries > 2)
        {
            std::cerr
                << "Derived region cache did not enforce its entry budget.\n";
            return 1;
        }

        orbit::terrain_region::
            DerivedRegionTerrainSource
                composed(
                    planet,
                    source,
                    cache);

        const orbit::u64 composedRevision =
            composed.Revision();

        const auto sample =
            composed.Sample({
                .unitDirection = directionA,
                .footprintMeters = 100.0
            });

        if (!std::isfinite(
                sample.elevationMeters))
        {
            std::cerr
                << "Derived region terrain source returned a non-finite sample.\n";
            return 1;
        }

        source->SetRevision(8);

        if (composed.Revision() ==
            composedRevision)
        {
            std::cerr
                << "Derived region terrain source revision ignored base source changes.\n";
            return 1;
        }

        const auto newId =
            cache->IdForDirection(
                directionA);

        if (newId.sourceRevision != 8 ||
            cache->Request(idA))
        {
            std::cerr
                << "Derived region cache accepted an obsolete source revision.\n";
            return 1;
        }

        // Restore the source revision before cache destruction so no
        // background work can be made stale by this test.
        source->SetRevision(7);
    }

    // Streamer request policy: first update populates, repeated identical
    // updates must not submit duplicate work.
    {
        auto streamingSource =
            std::make_shared<TestTerrainSource>();

        auto cache =
            std::make_shared<
                orbit::terrain_region::
                    DerivedTerrainRegionCache>(
                        planet,
                        streamingSource,
                        jobs,
                        CacheConfig(12));

        orbit::terrain_region::
            DerivedTerrainRegionStreamer
                streamer(
                    planet,
                    *cache,
                    {
                        .neighborhoodRadius = 1,
                        .forwardPrefetchDistanceTiles =
                            1.0
                    });

        const orbit::math::Double3 observer =
            orbit::math::Normalize(
                orbit::math::Double3{
                    1.0,
                    0.1,
                    0.1
                });

        const orbit::math::Double3 travel{
            0.0,
            0.0,
            1.0
        };

        streamer.Update(
            observer,
            travel);

        const auto firstStats =
            streamer.Stats();

        if (firstStats.desiredRegions < 9 ||
            firstStats.requestsLastUpdate == 0)
        {
            std::cerr
                << "Derived region streamer did not request the observer neighborhood.\n";
            return 1;
        }

        streamer.Update(
            observer,
            travel);

        if (streamer.Stats().
                requestsLastUpdate != 0)
        {
            std::cerr
                << "Repeated derived region streamer update caused a request storm.\n";
            return 1;
        }

        cache->WaitAll();

        streamer.Update(
            observer,
            travel);

        if (streamer.Stats().readyRegions == 0 ||
            streamer.Stats().
                requestsLastUpdate != 0)
        {
            std::cerr
                << "Derived region streamer did not reuse ready regions.\n";
            return 1;
        }
    }

    return 0;
}
