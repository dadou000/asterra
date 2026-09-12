#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_cache/TerrainPageCache.hpp>
#include <orbit/world/Planet.hpp>

#include <array>
#include <memory>
#include <utility>

namespace orbit::terrain_cache
{
struct CachedTerrainSourceConfig
{
    u32 pageResolution{33};
    u32 requestMissThreshold{16};
    u8 minimumTileLevel{0};
    u8 maximumTileLevel{24};
    TerrainPageCacheConfig cache{};
};

struct CachedTerrainSourceStats
{
    u64 pageHits{0};
    u64 directFallbackSamples{0};
    u64 pageRequests{0};
};

class CachedTerrainSource final :
    public terrain::TerrainSource
{
public:
    CachedTerrainSource(
        world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        jobs::JobSystem& jobs,
        CachedTerrainSourceConfig config = {});

    ~CachedTerrainSource() override;

    CachedTerrainSource(const CachedTerrainSource&) = delete;
    CachedTerrainSource& operator=(const CachedTerrainSource&) = delete;

    [[nodiscard]] terrain::TerrainSample Sample(
        const terrain::TerrainQuery& query) const noexcept override;

    [[nodiscard]] u64 Revision() const noexcept override;

    [[nodiscard]] CachedTerrainSourceStats Stats() const noexcept;
    [[nodiscard]] TerrainPageCacheStats PageCacheStats() const noexcept;

    // Diagnostic only: forwards to TerrainPageCache::EntriesByLevel.
    [[nodiscard]] std::array<
        std::pair<u64, std::size_t>,
        31>
        PageCacheEntriesByLevel() const;

    void WaitAll();

    // Forwards to TerrainPageCache::PruneFarPages -- proactively
    // drops cached pages that have fallen far behind the observer
    // instead of only evicting once the byte budget is full. Cheap
    // enough to call every second or so, not every frame.
    void PruneFarPages(
        const math::Double3& observerDirection,
        f64 keepRadiusTileWidths = 8.0);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::terrain_cache
