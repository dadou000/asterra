#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_region/DerivedTerrainRegion.hpp>
#include <orbit/world/Planet.hpp>

#include <cstddef>
#include <memory>

namespace orbit::terrain_region
{
struct DerivedTerrainRegionCacheConfig
{
    u8 tileLevel{8};
    u32 generatorVersion{1};
    std::size_t maxEntries{12};
    DerivedTerrainRegionConfig region{};
};

struct DerivedTerrainRegionCacheStats
{
    u64 acceptedRequests{0};
    u64 duplicateRequests{0};
    u64 readyHits{0};
    u64 misses{0};
    u64 evictions{0};
    u64 capacityRejects{0};
    u64 failedBuilds{0};
    u64 staleRemovals{0};
    u64 staleRequestRejects{0};
    std::size_t entries{0};
    std::size_t readyEntries{0};
    std::size_t pendingEntries{0};
    std::size_t failedEntries{0};
};

class DerivedTerrainRegionCache
{
public:
    DerivedTerrainRegionCache(
        world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        jobs::JobSystem& jobs,
        DerivedTerrainRegionCacheConfig config = {});

    ~DerivedTerrainRegionCache();

    DerivedTerrainRegionCache(
        const DerivedTerrainRegionCache&) = delete;
    DerivedTerrainRegionCache& operator=(
        const DerivedTerrainRegionCache&) = delete;
    DerivedTerrainRegionCache(
        DerivedTerrainRegionCache&&) = delete;
    DerivedTerrainRegionCache& operator=(
        DerivedTerrainRegionCache&&) = delete;

    [[nodiscard]] DerivedTerrainRegionId IdForDirection(
        const math::Double3& direction) const;

    [[nodiscard]] bool Request(
        const DerivedTerrainRegionId& id);

    [[nodiscard]] std::shared_ptr<
        const DerivedTerrainRegion>
    TryGet(
        const DerivedTerrainRegionId& id) const;

    [[nodiscard]] bool IsPending(
        const DerivedTerrainRegionId& id) const;

    [[nodiscard]] u64 ContentRevision() const noexcept;

    [[nodiscard]] DerivedTerrainRegionCacheStats
    Stats() const;

    void WaitAll();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::terrain_region
