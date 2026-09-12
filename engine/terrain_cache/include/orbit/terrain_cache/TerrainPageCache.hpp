#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_cache/TerrainPage.hpp>
#include <orbit/world/Planet.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <utility>

namespace orbit::terrain_cache
{
struct TerrainPageCacheConfig
{
    static constexpr std::size_t DefaultBudgetBytes =
        256ULL * 1024ULL * 1024ULL;

    std::size_t budgetBytes{DefaultBudgetBytes};
    std::size_t softEntryLimit{4096};

};

struct TerrainPageCacheStats
{
    u64 acceptedRequests{0};
    u64 duplicateRequests{0};
    u64 readyHits{0};
    u64 misses{0};
    u64 evictions{0};
    u64 capacityRejects{0};
    std::size_t residentBytes{0};
    std::size_t budgetBytes{0};
    std::size_t peakResidentBytes{0};
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

    // Diagnostic only: resident entry count and byte total per tile
    // quadtree level (index = level, 0-30). Used to find which
    // levels are actually consuming the byte budget.
    [[nodiscard]] std::array<
        std::pair<u64, std::size_t>,
        31>
        EntriesByLevel() const;

    void WaitAll();

    // Proactively evicts resident pages whose tile center is farther
    // than `keepRadiusTileWidths` tile-widths from `observerDirection`
    // (great-circle distance, in units of the *evicted tile's own*
    // width so coarse tiles get to keep a large real-world margin and
    // fine tiles a small one). Without this, pages only ever get
    // evicted once the whole byte budget is already exhausted, so a
    // long one-directional flight accumulates every tile it has ever
    // passed near until that happens. Cheap to call periodically
    // (e.g. once a second) rather than every frame.
    void PruneFarPages(
        const math::Double3& observerDirection,
        f64 keepRadiusTileWidths = 8.0);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::terrain_cache
