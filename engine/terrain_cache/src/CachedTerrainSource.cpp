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
        const f64 directionLengthSquared =
            math::LengthSquared(
                query.unitDirection);

        const bool directionValid =
            std::isfinite(query.unitDirection.x) &&
            std::isfinite(query.unitDirection.y) &&
            std::isfinite(query.unitDirection.z) &&
            std::isfinite(directionLengthSquared) &&
            directionLengthSquared > 0.0;

        const terrain::TerrainSampleFootprint footprint =
            query.Footprint();

        if (!directionValid ||
            !footprint.IsValid())
        {
            directFallbackSamples_.
                fetch_add(
                    1,
                    std::memory_order_relaxed);

            return source_->Sample(query);
        }

        // CachedTerrainSource is bound to one physical planet. Legacy callers
        // that omit the planet ID are upgraded at this boundary; an explicit
        // request for another planet bypasses this cache instead of aliasing
        // physical pages across bodies.
        if (query.planet.IsValid() &&
            planet_.id.IsValid() &&
            query.planet != planet_.id)
        {
            directFallbackSamples_.
                fetch_add(
                    1,
                    std::memory_order_relaxed);

            return source_->Sample(query);
        }

        const world::PlanetId effectivePlanet =
            query.planet.IsValid()
                ? query.planet
                : planet_.id;

        const terrain::PlanetSurfacePosition position =
            terrain::CanonicalizeSurfacePosition({
                .planet = effectivePlanet,
                .unitDirection = query.unitDirection,
                .radialOffsetMeters =
                    query.radialOffsetMeters
            });

        const terrain::TerrainQuery canonicalQuery =
            terrain::MakeTerrainQuery(
                position,
                footprint);

        try
        {
            const TerrainPageDesc desc =
                SelectPage(
                    position,
                    footprint);

            if (const auto page =
                    cache_.TryGet(desc))
            {
                pageHits_.fetch_add(
                    1,
                    std::memory_order_relaxed);

                return page->SampleDirection(
                    position.unitDirection);
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

        return source_->Sample(
            canonicalQuery);
    }

    u64 Revision() const noexcept
    {
        return source_->Revision();
    }

    terrain::TerrainGenerationRevisions
    GenerationRevisions() const noexcept
    {
        return source_->GenerationRevisions();
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

    std::array<
        std::pair<u64, std::size_t>,
        31>
        PageCacheEntriesByLevel() const
    {
        return cache_.EntriesByLevel();
    }

    void WaitAll()
    {
        cache_.WaitAll();
    }

    void PruneFarPages(
        const math::Double3&
            observerDirection,
        const f64
            keepRadiusTileWidths)
    {
        cache_.PruneFarPages(
            observerDirection,
            keepRadiusTileWidths);
    }

private:
    TerrainPageDesc SelectPage(
        const terrain::PlanetSurfacePosition& position,
        const terrain::TerrainSampleFootprint& footprint) const
    {
        const f64 footprintMeters =
            std::max(
                footprint.diameterMeters,
                1.0e-3);

        world::PlanetTileId selected =
            world::TileForDirection(
                position.unitDirection,
                config_.maximumTileLevel);

        for (u32 level =
                 config_.minimumTileLevel;
             level <=
                 config_.maximumTileLevel;
             ++level)
        {
            const auto tile =
                world::TileForDirection(
                    position.unitDirection,
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
                footprintMeters)
            {
                break;
            }
        }

        return {
            .planet = position.planet,
            .tile = selected,
            .resolution =
                config_.pageResolution,
            .revisions =
                source_->GenerationRevisions()
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

terrain::TerrainGenerationRevisions
CachedTerrainSource::GenerationRevisions() const noexcept
{
    return impl_->GenerationRevisions();
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

void CachedTerrainSource::PruneFarPages(
    const math::Double3& observerDirection,
    const f64 keepRadiusTileWidths)
{
    impl_->PruneFarPages(
        observerDirection,
        keepRadiusTileWidths);
}

std::array<
    std::pair<u64, std::size_t>,
    31>
CachedTerrainSource::
    PageCacheEntriesByLevel() const
{
    return impl_->
        PageCacheEntriesByLevel();
}
} // namespace orbit::terrain_cache
