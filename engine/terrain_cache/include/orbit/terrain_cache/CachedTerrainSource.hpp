#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_cache/TerrainPageCache.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>

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

    [[nodiscard]] CachedTerrainSourceStats Stats() const noexcept;
    [[nodiscard]] TerrainPageCacheStats PageCacheStats() const noexcept;

    void WaitAll();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::terrain_cache
