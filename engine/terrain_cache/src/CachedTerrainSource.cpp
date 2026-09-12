#include <orbit/terrain_cache/CachedTerrainSource.hpp>

#include <orbit/math/Vector.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace orbit::terrain_cache
{
class CachedTerrainSource::Impl
{
public:
    Impl(
        const world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        jobs::JobSystem& jobs,
        const CachedTerrainSourceConfig config)
        : planet_(planet),
          source_(std::move(source)),
          config_(config),
          cache_(
              planet_,
              source_,
              jobs,
              config_.cache)
    {
        if (!source_)
        {
            throw std::invalid_argument(
                "Orbit cached terrain source requires an underlying source.");
        }

        if (planet_.radiusMeters <= 0.0)
        {
            throw std::invalid_argument(
                "Orbit cached terrain source requires a positive planet radius.");
        }

        if (config_.pageResolution < 2)
        {
            throw std::invalid_argument(
                "Orbit cached terrain pages require at least two samples per axis.");
        }

        if (config_.requestMissThreshold == 0)
        {
            throw std::invalid_argument(
                "Orbit cached terrain miss threshold must be at least one.");
        }

        if (config_.maximumTileLevel >
                30 ||
            config_.minimumTileLevel >
                config_.maximumTileLevel)
        {
            throw std::invalid_argument(
                "Orbit cached terrain tile-level range is invalid.");
        }
    }

    terrain::TerrainSample Sample(
        const terrain::TerrainQuery& query) const noexcept
    {
        if (math::LengthSquared(
                query.unitDirection) <= 0.0 ||
            !std::isfinite(
                query.footprintMeters) ||
            query.footprintMeters <= 0.0)
        {
            directFallbackSamples_.
                fetch_add(
                    1,
                    std::memory_order_relaxed);

            return source_->Sample(query);
        }

        try
        {
            const TerrainPageDesc desc =
                SelectPage(query);

            if (const auto page =
                    cache_.TryGet(desc))
            {
                pageHits_.fetch_add(
                    1,
                    std::memory_order_relaxed);

                return page->SampleDirection(
                    query.unitDirection);
            }

            bool shouldRequest = false;

            {
                std::scoped_lock lock(
                    missMutex_);

                u32& count =
                    missCounts_[desc];

                if (count <
                    config_.
                        requestMissThreshold)
                {
                    ++count;
                }

                if (count >=
                    config_.
                        requestMissThreshold)
                {
                    shouldRequest = true;
                    missCounts_.erase(desc);
                }
            }

            if (shouldRequest &&
                cache_.Request(desc))
            {
                pageRequests_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        }
        catch (...)
        {
        }

        directFallbackSamples_.
            fetch_add(
                1,
                std::memory_order_relaxed);

        return source_->Sample(query);
    }

    u64 Revision() const noexcept
    {
        return source_->Revision();
    }

    CachedTerrainSourceStats Stats() const noexcept
    {
        return {
            .pageHits =
                pageHits_.load(
                    std::memory_order_relaxed),
            .directFallbackSamples =
                directFallbackSamples_.load(
                    std::memory_order_relaxed),
            .pageRequests =
                pageRequests_.load(
                    std::memory_order_relaxed)
        };
    }

    TerrainPageCacheStats PageCacheStats() const noexcept
    {
        return cache_.Stats();
    }

    void WaitAll()
    {
        cache_.WaitAll();
    }

private:
    TerrainPageDesc SelectPage(
        const terrain::TerrainQuery& query) const
    {
        const f64 footprint =
            std::max(
                query.footprintMeters,
                1.0e-3);

        world::PlanetTileId selected =
            world::TileForDirection(
                query.unitDirection,
                config_.maximumTileLevel);

        for (u32 level =
                 config_.minimumTileLevel;
             level <=
                 config_.maximumTileLevel;
             ++level)
        {
            const auto tile =
                world::TileForDirection(
                    query.unitDirection,
                    static_cast<u8>(level));

            selected = tile;

            const f64 spacing =
                world::
                    ApproximateTileWidthMeters(
                        planet_,
                        tile) /
                static_cast<f64>(
                    config_.
                        pageResolution -
                    1U);

            if (spacing <=
                footprint)
            {
                break;
            }
        }

        return {
            .tile = selected,
            .resolution =
                config_.pageResolution,
            .sourceRevision =
                source_->Revision()
        };
    }

    world::PlanetDefinition planet_;

    std::shared_ptr<const terrain::TerrainSource>
        source_;

    CachedTerrainSourceConfig config_;
    mutable TerrainPageCache cache_;

    mutable std::mutex missMutex_;

    mutable std::unordered_map<
        TerrainPageDesc,
        u32,
        TerrainPageDescHash>
        missCounts_;

    mutable std::atomic<u64>
        pageHits_{0};

    mutable std::atomic<u64>
        directFallbackSamples_{0};

    mutable std::atomic<u64>
        pageRequests_{0};
};

CachedTerrainSource::CachedTerrainSource(
    const world::PlanetDefinition planet,
    std::shared_ptr<const terrain::TerrainSource> source,
    jobs::JobSystem& jobs,
    const CachedTerrainSourceConfig config)
    : impl_(
        std::make_unique<Impl>(
            planet,
            std::move(source),
            jobs,
            config))
{
}

CachedTerrainSource::~CachedTerrainSource() =
    default;

terrain::TerrainSample
CachedTerrainSource::Sample(
    const terrain::TerrainQuery& query) const noexcept
{
    return impl_->Sample(query);
}

u64 CachedTerrainSource::Revision() const noexcept
{
    return impl_->Revision();
}

CachedTerrainSourceStats
CachedTerrainSource::Stats() const noexcept
{
    return impl_->Stats();
}

TerrainPageCacheStats
CachedTerrainSource::PageCacheStats() const noexcept
{
    return impl_->PageCacheStats();
}

void CachedTerrainSource::WaitAll()
{
    impl_->WaitAll();
}
} // namespace orbit::terrain_cache
