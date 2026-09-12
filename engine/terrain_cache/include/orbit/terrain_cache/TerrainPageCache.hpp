#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_cache/TerrainPage.hpp>
#include <orbit/world/Planet.hpp>

#include <cstddef>
#include <memory>

namespace orbit::terrain_cache
{
struct TerrainPageCacheConfig
{
    std::size_t maxEntries{256};
};

struct TerrainPageCacheStats
{
    u64 acceptedRequests{0};
    u64 duplicateRequests{0};
    u64 readyHits{0};
    u64 misses{0};
    u64 evictions{0};
    u64 capacityRejects{0};
    std::size_t entries{0};
};

class TerrainPageCache
{
public:
    TerrainPageCache(
        world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        jobs::JobSystem& jobs,
        TerrainPageCacheConfig config = {});

    ~TerrainPageCache();

    TerrainPageCache(const TerrainPageCache&) = delete;
    TerrainPageCache& operator=(const TerrainPageCache&) = delete;
    TerrainPageCache(TerrainPageCache&&) = delete;
    TerrainPageCache& operator=(TerrainPageCache&&) = delete;

    [[nodiscard]] bool Request(const TerrainPageDesc& desc);

    [[nodiscard]] std::shared_ptr<const TerrainPage> TryGet(
        const TerrainPageDesc& desc) const;

    [[nodiscard]] bool IsPending(
        const TerrainPageDesc& desc) const;

    [[nodiscard]] std::size_t EntryCount() const;

    [[nodiscard]] TerrainPageCacheStats Stats() const noexcept;

    void WaitAll();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::terrain_cache
