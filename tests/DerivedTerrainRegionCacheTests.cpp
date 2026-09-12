#include <orbit/jobs/JobSystem.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>

#include <atomic>
#include <iostream>
#include <memory>

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
        const orbit::terrain::TerrainQuery& query)
        const noexcept override
    {
        const orbit::f64 elevation =
            750.0 +
            query.unitDirection.x * 80.0 +
            query.unitDirection.y * 45.0 +
            query.unitDirection.z * 20.0;

        return {
            .elevationMeters = elevation,
            .coarseElevationMeters = elevation,
            .climate = {
                .temperatureC = 16.0F,
                .humidity = 0.5F,
                .precipitation = 0.5F,
                .continentality = 0.5F
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
    std::atomic<orbit::u64>
        revision_{0};
};

[[nodiscard]]
orbit::terrain_region::
DerivedTerrainRegionConfig TestRegionConfig()
{
    orbit::terrain_region::
        DerivedTerrainRegionConfig config{};

    config.overlapScale = 1.10;
    config.hydrology.resolution = 5;
    config.hydrology.footprintMeters = 0.0;
    config.hydrology.useCoarseElevation = false;
    config.hydrology.conditionDepressions = true;
    config.hydrology.minimumDrainageDropMeters =
        0.01;
    config.refinement.iterations = 1;
    config.refinement.elevationDeltaScale =
        0.15;
    config.minimumRiverDrainageAreaSquareMeters =
        1.0;
    config.carving.spatialIndexResolution = 4;

    return config;
}
} // namespace

int main()
{
    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0
    };

    auto source =
        std::make_shared<
            MutableTerrainSource>(7);

    orbit::jobs::JobSystem jobs(2);

    orbit::terrain_region::
        DerivedTerrainRegionCache cache(
            planet,
            source,
            jobs,
            {
                .tileLevel = 5,
                .generatorVersion = 42,
                .maxEntries = 2,
                .region = TestRegionConfig()
            });

    const orbit::math::Double3 directionA{
        1.0,
        0.0,
        0.0
    };

    const orbit::math::Double3 directionB{
        0.0,
        1.0,
        0.0
    };

    const orbit::math::Double3 directionC{
        0.0,
        0.0,
        1.0
    };

    const auto idA =
        cache.IdForDirection(
            directionA);

    if (idA.sourceRevision != 7 ||
        idA.generatorVersion != 42 ||
        idA.tile.level != 5)
    {
        std::cerr
            << "Derived region cache generated an invalid region ID.\n";
        return 1;
    }

    if (!cache.Request(idA))
    {
        std::cerr
            << "Initial derived region request was rejected.\n";
        return 1;
    }

    if (cache.Request(idA))
    {
        std::cerr
            << "Duplicate derived region request was not deduplicated.\n";
        return 1;
    }

    cache.WaitAll();

    if (cache.IsPending(idA))
    {
        std::cerr
            << "Ready derived region remained pending.\n";
        return 1;
    }

    const auto regionA =
        cache.TryGet(idA);

    if (!regionA ||
        !(regionA->id == idA))
    {
        std::cerr
            << "Asynchronous derived region never became ready.\n";
        return 1;
    }

    if (cache.ContentRevision() == 0)
    {
        std::cerr
            << "Derived region cache did not publish a content revision.\n";
        return 1;
    }

    const auto idB =
        cache.IdForDirection(
            directionB);

    if (!cache.Request(idB))
    {
        std::cerr
            << "Second derived region request was rejected.\n";
        return 1;
    }

    cache.WaitAll();

    if (!cache.TryGet(idA))
    {
        std::cerr
            << "Could not refresh derived region A LRU state.\n";
        return 1;
    }

    const auto idC =
        cache.IdForDirection(
            directionC);

    if (!cache.Request(idC))
    {
        std::cerr
            << "Third derived region request was rejected after LRU eviction.\n";
        return 1;
    }

    cache.WaitAll();

    if (!cache.TryGet(idA) ||
        cache.TryGet(idB) ||
        !cache.TryGet(idC))
    {
        std::cerr
            << "Derived region cache evicted the wrong LRU entry.\n";
        return 1;
    }

    const auto beforeStaleRemoval =
        cache.ContentRevision();

    source->SetRevision(8);

    if (cache.TryGet(idA))
    {
        std::cerr
            << "Old source-revision region remained usable.\n";
        return 1;
    }

    if (cache.ContentRevision() <=
        beforeStaleRemoval)
    {
        std::cerr
            << "Removing stale ready regions did not change content revision.\n";
        return 1;
    }

    const auto revisedId =
        cache.IdForDirection(
            directionA);

    if (revisedId.sourceRevision != 8 ||
        revisedId == idA)
    {
        std::cerr
            << "Source revision did not produce a new derived-region identity.\n";
        return 1;
    }

    if (cache.Request(idA))
    {
        std::cerr
            << "Cache accepted a stale derived-region request.\n";
        return 1;
    }

    if (!cache.Request(revisedId))
    {
        std::cerr
            << "Cache rejected a current-revision derived region.\n";
        return 1;
    }

    cache.WaitAll();

    if (!cache.TryGet(revisedId))
    {
        std::cerr
            << "Current-revision region did not become ready.\n";
        return 1;
    }

    const auto stats =
        cache.Stats();

    if (stats.duplicateRequests == 0 ||
        stats.evictions == 0 ||
        stats.staleRemovals == 0 ||
        stats.staleRequestRejects == 0 ||
        stats.failedBuilds != 0)
    {
        std::cerr
            << "Derived region cache telemetry is inconsistent.\n";
        return 1;
    }

    return 0;
}
