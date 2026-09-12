#include <orbit/jobs/JobSystem.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_cache/CachedTerrainSource.hpp>
#include <orbit/terrain_cache/TerrainPageBuilder.hpp>
#include <orbit/terrain_cache/TerrainPageCache.hpp>

#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>

namespace
{
class CountingTerrainSource final :
    public orbit::terrain::TerrainSource
{
public:
    [[nodiscard]] orbit::terrain::TerrainSample
    Sample(
        const orbit::terrain::TerrainQuery&)
        const noexcept override
    {
        calls.fetch_add(
            1,
            std::memory_order_relaxed);

        return {
            .elevationMeters = 123.0,
            .coarseElevationMeters = 100.0,
            .climate = {
                .temperatureC = 27.0F,
                .humidity = 0.18F,
                .precipitation = 0.12F,
                .continentality = 0.82F
            },
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

    mutable std::atomic<orbit::u64> calls{0};
};

class BlockingTerrainSource final :
    public orbit::terrain::TerrainSource
{
public:
    [[nodiscard]] orbit::terrain::TerrainSample
    Sample(
        const orbit::terrain::TerrainQuery&)
        const noexcept override
    {
        entered.store(
            true,
            std::memory_order_release);

        while (!released.load(
            std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        return {
            .elevationMeters = 321.0,
            .coarseElevationMeters = 300.0
        };
    }

    void WaitUntilEntered() const noexcept
    {
        while (!entered.load(
            std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
    }

    void Release() const noexcept
    {
        released.store(
            true,
            std::memory_order_release);
    }

private:
    mutable std::atomic<bool> entered{false};
    mutable std::atomic<bool> released{false};
};
} // namespace

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

    if (positiveX.samples.size() != expectedCount ||
        negativeZ.samples.size() != expectedCount)
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
        readyPage->samples.size() !=
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

    constexpr std::size_t
        lruResolution = 17;

    const std::size_t pageBytesEstimate =
        sizeof(orbit::terrain_cache::TerrainPage) +
        lruResolution *
            lruResolution *
            sizeof(orbit::terrain::TerrainSample);

    orbit::terrain_cache::TerrainPageCache
        limitedCache(
            planet,
            terrain,
            jobs,
            {
                .budgetBytes =
                    pageBytesEstimate *
                    5U /
                    2U,
                .softEntryLimit = 0
            });

    const orbit::terrain_cache::TerrainPageDesc
        pageA{
            .tile = {
                .face =
                    orbit::world::CubeFace::PositiveX,
                .level = 4,
                .x = 5,
                .y = 5
            },
            .resolution = 17
        };

    const orbit::terrain_cache::TerrainPageDesc
        pageB{
            .tile = {
                .face =
                    orbit::world::CubeFace::PositiveX,
                .level = 4,
                .x = 6,
                .y = 5
            },
            .resolution = 17
        };

    const orbit::terrain_cache::TerrainPageDesc
        pageC{
            .tile = {
                .face =
                    orbit::world::CubeFace::PositiveX,
                .level = 4,
                .x = 7,
                .y = 5
            },
            .resolution = 17
        };

    if (!limitedCache.Request(pageA))
    {
        std::cerr
            << "Limited cache rejected page A.\n";
        return 1;
    }

    limitedCache.WaitAll();

    if (!limitedCache.Request(pageB))
    {
        std::cerr
            << "Limited cache rejected page B.\n";
        return 1;
    }

    limitedCache.WaitAll();

    if (!limitedCache.TryGet(pageA))
    {
        std::cerr
            << "Limited cache could not refresh page A LRU state.\n";
        return 1;
    }

    if (!limitedCache.Request(pageC))
    {
        std::cerr
            << "Limited cache rejected page C after ready-page eviction.\n";
        return 1;
    }

    limitedCache.WaitAll();

    if (limitedCache.EntryCount() != 2)
    {
        std::cerr
            << "Limited terrain cache did not converge to the byte-budgeted residency set.\n";
        return 1;
    }

    if (!limitedCache.TryGet(pageA) ||
        !limitedCache.TryGet(pageC) ||
        limitedCache.TryGet(pageB))
    {
        std::cerr
            << "Terrain cache LRU eviction selected the wrong page.\n";
        return 1;
    }

    const auto limitedStats =
        limitedCache.Stats();

    if (limitedStats.evictions == 0)
    {
        std::cerr
            << "Terrain cache did not report its ready-page eviction.\n";
        return 1;
    }

    if (limitedStats.residentBytes >
            limitedStats.budgetBytes ||
        limitedStats.peakResidentBytes >
            limitedStats.budgetBytes ||
        limitedStats.budgetBytes !=
            pageBytesEstimate *
                5U /
                2U)
    {
        std::cerr
            << "Terrain cache byte accounting exceeded its configured budget.\n";
        return 1;
    }

    const auto blockingSource =
        std::make_shared<
            BlockingTerrainSource>();

    orbit::terrain_cache::TerrainPageCache
        pendingCache(
            planet,
            blockingSource,
            jobs,
            {
                .budgetBytes =
                    pageBytesEstimate *
                    3U /
                    2U,
                .softEntryLimit = 0
            });

    if (!pendingCache.Request(pageA))
    {
        std::cerr
            << "Pending-protection cache rejected its first page.\n";
        return 1;
    }

    blockingSource->WaitUntilEntered();

    if (pendingCache.Request(pageB))
    {
        blockingSource->Release();
        pendingCache.WaitAll();

        std::cerr
            << "Terrain cache evicted or overcommitted a pending page.\n";
        return 1;
    }

    if (!pendingCache.IsPending(pageA) ||
        pendingCache.EntryCount() != 1)
    {
        blockingSource->Release();
        pendingCache.WaitAll();

        std::cerr
            << "Pending terrain page did not remain resident under pressure.\n";
        return 1;
    }

    if (pendingCache.Stats().capacityRejects == 0)
    {
        blockingSource->Release();
        pendingCache.WaitAll();

        std::cerr
            << "Terrain cache did not report pending-page budget pressure.\n";
        return 1;
    }

    blockingSource->Release();
    pendingCache.WaitAll();

    if (!pendingCache.TryGet(pageA))
    {
        std::cerr
            << "Pending terrain page failed to become ready after release.\n";
        return 1;
    }

    const auto pendingStats =
        pendingCache.Stats();

    if (pendingStats.residentBytes >
            pendingStats.budgetBytes ||
        pendingStats.peakResidentBytes >
            pendingStats.budgetBytes)
    {
        std::cerr
            << "Pending cache exceeded its hard byte budget.\n";
        return 1;
    }

    const auto countingSource =
        std::make_shared<
            CountingTerrainSource>();

    orbit::terrain_cache::CachedTerrainSource
        cachedSource(
            planet,
            countingSource,
            jobs,
            {
                .pageResolution = 9,
                .requestMissThreshold = 2,
                .minimumTileLevel = 6,
                .maximumTileLevel = 6,
                .cache = {
                    .softEntryLimit = 0
                }
            });

    const orbit::terrain::TerrainQuery
        cachedQuery{
            .unitDirection =
                orbit::math::Normalize(
                    orbit::math::Double3{
                        1.0,
                        0.1,
                        0.2
                    }),
            .footprintMeters = 1'000.0
        };

    const auto coldA =
        cachedSource.Sample(
            cachedQuery);

    if (countingSource->calls.load(
            std::memory_order_relaxed) != 1)
    {
        std::cerr
            << "Cached terrain source did not use direct fallback on first miss.\n";
        return 1;
    }

    const auto coldB =
        cachedSource.Sample(
            cachedQuery);

    if (coldA.elevationMeters != 123.0 ||
        coldB.elevationMeters != 123.0)
    {
        std::cerr
            << "Cached terrain direct fallback changed source data.\n";
        return 1;
    }

    cachedSource.WaitAll();

    const orbit::u64 callsAfterWarmup =
        countingSource->calls.load(
            std::memory_order_relaxed);

    if (callsAfterWarmup <= 2)
    {
        std::cerr
            << "Cached terrain page was not generated after the miss threshold.\n";
        return 1;
    }

    const auto warm =
        cachedSource.Sample(
            cachedQuery);

    if (warm.elevationMeters != 123.0 ||
        std::abs(
            warm.coarseElevationMeters -
            100.0) >
            1.0e-4 ||
        std::abs(
            warm.climate.temperatureC -
            27.0F) >
            1.0e-4F ||
        warm.biomes.desert < 0.99F ||
        countingSource->calls.load(
            std::memory_order_relaxed) !=
            callsAfterWarmup)
    {
        std::cerr
            << "Warm terrain page did not preserve the full semantic sample or eliminate authoritative sampling.\n";
        return 1;
    }

    const auto cachedStats =
        cachedSource.Stats();

    if (cachedStats.pageHits == 0 ||
        cachedStats.pageRequests == 0 ||
        cachedStats.directFallbackSamples < 2)
    {
        std::cerr
            << "Cached terrain source telemetry is inconsistent.\n";
        return 1;
    }

    return 0;
}
