#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>

#include <orbit/core/Log.hpp>

#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace orbit::terrain_region
{
namespace
{
enum class EntryState
{
    Pending,
    Ready,
    Failed
};

struct CacheEntry
{
    mutable std::mutex mutex;
    EntryState state{EntryState::Pending};

    std::shared_ptr<DerivedTerrainRegion> region;
    std::exception_ptr exception;

    std::atomic<u64> lastUseTicket{0};
};
} // namespace

class DerivedTerrainRegionCache::Impl
{
public:
    Impl(
        const world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        jobs::JobSystem& jobs,
        const DerivedTerrainRegionCacheConfig config)
        : planet_(planet),
          source_(std::move(source)),
          jobs_(jobs),
          config_(config)
    {
        if (!source_)
        {
            throw std::invalid_argument(
                "Orbit derived terrain region cache requires a terrain source.");
        }

        if (planet_.radiusMeters <= 0.0)
        {
            throw std::invalid_argument(
                "Orbit derived terrain region cache requires a positive planet radius.");
        }

        if (config_.tileLevel > 30)
        {
            throw std::invalid_argument(
                "Orbit derived terrain region cache tile level is too large.");
        }

        if (config_.maxEntries == 0)
        {
            throw std::invalid_argument(
                "Orbit derived terrain region cache requires at least one entry.");
        }

        if (config_.region.generatorVersion == 0)
        {
            throw std::invalid_argument(
                "Orbit derived terrain region cache generator version must be non-zero.");
        }
    }

    ~Impl()
    {
        try
        {
            WaitAll();
        }
        catch (...)
        {
            log::Error(
                "Derived terrain region generation failed while destroying its cache.");
        }
    }

    [[nodiscard]] DerivedTerrainRegionId IdForDirection(
        const math::Double3& direction) const noexcept
    {
        return IdForTile(
            world::TileForDirection(
                direction,
                config_.tileLevel));
    }

    [[nodiscard]] DerivedTerrainRegionId IdForTile(
        const world::PlanetTileId tile) const noexcept
    {
        return {
            .tile = tile,
            .sourceRevision =
                source_->Revision(),
            .generatorVersion =
                config_.region.generatorVersion
        };
    }

    bool Request(
        const DerivedTerrainRegionId& id)
    {
        if (id.tile.level != config_.tileLevel ||
            id.generatorVersion !=
                config_.region.generatorVersion)
        {
            throw std::invalid_argument(
                "Orbit derived terrain region request does not match the cache configuration.");
        }

        if (id.sourceRevision != source_->Revision())
        {
            return false;
        }

        std::shared_ptr<CacheEntry> entry;

        {
            std::scoped_lock lock(entriesMutex_);

            const auto existing =
                entries_.find(id);

            if (existing != entries_.end())
            {
                bool failed = false;

                {
                    std::scoped_lock entryLock(
                        existing->second->mutex);

                    failed =
                        existing->second->state ==
                        EntryState::Failed;
                }

                if (!failed)
                {
                    duplicateRequests_.fetch_add(
                        1,
                        std::memory_order_relaxed);

                    return false;
                }

                entries_.erase(existing);
                PublishCountsLocked();
            }

            while (entries_.size() >=
                   config_.maxEntries)
            {
                if (!EvictOldestReusableLocked())
                {
                    capacityRejects_.fetch_add(
                        1,
                        std::memory_order_relaxed);

                    return false;
                }
            }

            entry =
                std::make_shared<CacheEntry>();

            entry->lastUseTicket.store(
                NextTicket(),
                std::memory_order_relaxed);

            entries_.emplace(
                id,
                entry);

            PublishCountsLocked();
        }

        acceptedRequests_.fetch_add(
            1,
            std::memory_order_relaxed);

        const world::PlanetDefinition planet =
            planet_;

        const auto source =
            source_;

        const DerivedTerrainRegionConfig
            regionConfig =
                config_.region;

        try
        {
            jobs_.Submit(
                group_,
                jobs::JobPriority::Low,
                [
                    this,
                    planet,
                    source,
                    id,
                    regionConfig,
                    entry
                ]
                {
                    try
                    {
                        auto region =
                            std::make_shared<DerivedTerrainRegion>(
                                BuildDerivedTerrainRegion(
                                    planet,
                                    *source,
                                    id,
                                    regionConfig));

                        {
                            std::scoped_lock lock(
                                entry->mutex);

                            entry->region =
                                std::move(region);

                            entry->state =
                                EntryState::Ready;
                        }

                        readyEntries_.fetch_add(
                            1,
                            std::memory_order_acq_rel);

                        pendingEntries_.fetch_sub(
                            1,
                            std::memory_order_acq_rel);

                        contentRevision_.fetch_add(
                            1,
                            std::memory_order_acq_rel);
                    }
                    catch (...)
                    {
                        {
                            std::scoped_lock lock(
                                entry->mutex);

                            entry->exception =
                                std::current_exception();

                            entry->state =
                                EntryState::Failed;
                        }

                        pendingEntries_.fetch_sub(
                            1,
                            std::memory_order_acq_rel);

                        failedBuilds_.fetch_add(
                            1,
                            std::memory_order_relaxed);

                        throw;
                    }
                });
        }
        catch (...)
        {
            {
                std::scoped_lock lock(
                    entry->mutex);

                entry->exception =
                    std::current_exception();

                entry->state =
                    EntryState::Failed;
            }

            pendingEntries_.fetch_sub(
                1,
                std::memory_order_acq_rel);

            failedBuilds_.fetch_add(
                1,
                std::memory_order_relaxed);

            throw;
        }

        return true;
    }

    bool RequestDirection(
        const math::Double3& direction)
    {
        return Request(
            IdForDirection(direction));
    }

    [[nodiscard]] std::shared_ptr<const DerivedTerrainRegion>
    TryGet(
        const DerivedTerrainRegionId& id) const
    {
        const auto entry =
            Find(id);

        if (!entry)
        {
            misses_.fetch_add(
                1,
                std::memory_order_relaxed);

            return {};
        }

        std::scoped_lock lock(
            entry->mutex);

        if (entry->state !=
            EntryState::Ready)
        {
            misses_.fetch_add(
                1,
                std::memory_order_relaxed);

            return {};
        }

        entry->lastUseTicket.store(
            NextTicket(),
            std::memory_order_relaxed);

        readyHits_.fetch_add(
            1,
            std::memory_order_relaxed);

        return entry->region;
    }

    [[nodiscard]] bool IsPending(
        const DerivedTerrainRegionId& id) const
    {
        const auto entry =
            Find(id);

        if (!entry)
        {
            return false;
        }

        std::scoped_lock lock(
            entry->mutex);

        return
            entry->state ==
            EntryState::Pending;
    }

    [[nodiscard]] u8 TileLevel() const noexcept
    {
        return config_.tileLevel;
    }

    [[nodiscard]] std::size_t EntryCount() const noexcept
    {
        return entryCount_.load(
            std::memory_order_acquire);
    }

    [[nodiscard]] u64 ContentRevision() const noexcept
    {
        return contentRevision_.load(
            std::memory_order_acquire);
    }

    [[nodiscard]] DerivedTerrainRegionCacheStats
    Stats() const noexcept
    {
        return {
            .acceptedRequests =
                acceptedRequests_.load(
                    std::memory_order_relaxed),
            .duplicateRequests =
                duplicateRequests_.load(
                    std::memory_order_relaxed),
            .readyHits =
                readyHits_.load(
                    std::memory_order_relaxed),
            .misses =
                misses_.load(
                    std::memory_order_relaxed),
            .evictions =
                evictions_.load(
                    std::memory_order_relaxed),
            .capacityRejects =
                capacityRejects_.load(
                    std::memory_order_relaxed),
            .failedBuilds =
                failedBuilds_.load(
                    std::memory_order_relaxed),
            .entries =
                entryCount_.load(
                    std::memory_order_acquire),
            .readyEntries =
                readyEntries_.load(
                    std::memory_order_acquire),
            .pendingEntries =
                pendingEntries_.load(
                    std::memory_order_acquire)
        };
    }

    void WaitAll()
    {
        jobs_.Wait(group_);
    }

private:
    using EntryMap =
        std::unordered_map<
            DerivedTerrainRegionId,
            std::shared_ptr<CacheEntry>,
            DerivedTerrainRegionIdHash>;

    [[nodiscard]] u64 NextTicket() const noexcept
    {
        return
            accessTicket_.fetch_add(
                1,
                std::memory_order_relaxed) +
            1;
    }

    [[nodiscard]] std::shared_ptr<CacheEntry> Find(
        const DerivedTerrainRegionId& id) const
    {
        std::scoped_lock lock(
            entriesMutex_);

        const auto iterator =
            entries_.find(id);

        if (iterator ==
            entries_.end())
        {
            return {};
        }

        return iterator->second;
    }

    bool EvictOldestReusableLocked()
    {
        auto oldest =
            entries_.end();

        u64 oldestTicket =
            (std::numeric_limits<u64>::max)();

        bool oldestReady = false;

        for (auto iterator =
                 entries_.begin();
             iterator !=
                 entries_.end();
             ++iterator)
        {
            EntryState state =
                EntryState::Pending;

            {
                std::scoped_lock entryLock(
                    iterator->second->mutex);

                state =
                    iterator->second->state;
            }

            if (state ==
                EntryState::Pending)
            {
                continue;
            }

            if (state ==
                EntryState::Failed)
            {
                entries_.erase(iterator);
                PublishCountsLocked();
                return true;
            }

            const u64 ticket =
                iterator->second->
                    lastUseTicket.load(
                        std::memory_order_relaxed);

            if (ticket < oldestTicket)
            {
                oldestTicket = ticket;
                oldest = iterator;
                oldestReady = true;
            }
        }

        if (oldest ==
            entries_.end())
        {
            return false;
        }

        entries_.erase(oldest);

        if (oldestReady)
        {
            readyEntries_.fetch_sub(
                1,
                std::memory_order_acq_rel);

            contentRevision_.fetch_add(
                1,
                std::memory_order_acq_rel);
        }

        evictions_.fetch_add(
            1,
            std::memory_order_relaxed);

        PublishCountsLocked();
        return true;
    }

    void PublishCountsLocked() noexcept
    {
        entryCount_.store(
            entries_.size(),
            std::memory_order_release);

        std::size_t pending = 0;

        for (const auto& [id, entry] :
             entries_)
        {
            static_cast<void>(id);

            std::scoped_lock entryLock(
                entry->mutex);

            if (entry->state ==
                EntryState::Pending)
            {
                ++pending;
            }
        }

        pendingEntries_.store(
            pending,
            std::memory_order_release);
    }

    world::PlanetDefinition planet_{};

    std::shared_ptr<const terrain::TerrainSource>
        source_;

    jobs::JobSystem& jobs_;
    DerivedTerrainRegionCacheConfig config_{};
    jobs::JobGroup group_;

    mutable std::mutex entriesMutex_;
    EntryMap entries_;

    mutable std::atomic<u64>
        accessTicket_{0};

    std::atomic<std::size_t>
        entryCount_{0};

    std::atomic<std::size_t>
        readyEntries_{0};

    std::atomic<std::size_t>
        pendingEntries_{0};

    std::atomic<u64>
        contentRevision_{1};

    std::atomic<u64>
        acceptedRequests_{0};

    std::atomic<u64>
        duplicateRequests_{0};

    mutable std::atomic<u64>
        readyHits_{0};

    mutable std::atomic<u64>
        misses_{0};

    std::atomic<u64>
        evictions_{0};

    std::atomic<u64>
        capacityRejects_{0};

    std::atomic<u64>
        failedBuilds_{0};
};

DerivedTerrainRegionCache::
DerivedTerrainRegionCache(
    const world::PlanetDefinition planet,
    std::shared_ptr<const terrain::TerrainSource> source,
    jobs::JobSystem& jobs,
    const DerivedTerrainRegionCacheConfig config)
    : impl_(
        std::make_unique<Impl>(
            planet,
            std::move(source),
            jobs,
            config))
{
}

DerivedTerrainRegionCache::
~DerivedTerrainRegionCache() = default;

DerivedTerrainRegionId
DerivedTerrainRegionCache::IdForDirection(
    const math::Double3& direction)
    const noexcept
{
    return impl_->IdForDirection(direction);
}

DerivedTerrainRegionId
DerivedTerrainRegionCache::IdForTile(
    const world::PlanetTileId tile)
    const noexcept
{
    return impl_->IdForTile(tile);
}

bool DerivedTerrainRegionCache::Request(
    const DerivedTerrainRegionId& id)
{
    return impl_->Request(id);
}

bool DerivedTerrainRegionCache::RequestDirection(
    const math::Double3& direction)
{
    return impl_->RequestDirection(direction);
}

std::shared_ptr<const DerivedTerrainRegion>
DerivedTerrainRegionCache::TryGet(
    const DerivedTerrainRegionId& id)
    const
{
    return impl_->TryGet(id);
}

bool DerivedTerrainRegionCache::IsPending(
    const DerivedTerrainRegionId& id)
    const
{
    return impl_->IsPending(id);
}

u8 DerivedTerrainRegionCache::TileLevel()
    const noexcept
{
    return impl_->TileLevel();
}

std::size_t DerivedTerrainRegionCache::EntryCount()
    const noexcept
{
    return impl_->EntryCount();
}

u64 DerivedTerrainRegionCache::ContentRevision()
    const noexcept
{
    return impl_->ContentRevision();
}

DerivedTerrainRegionCacheStats
DerivedTerrainRegionCache::Stats()
    const noexcept
{
    return impl_->Stats();
}

void DerivedTerrainRegionCache::WaitAll()
{
    impl_->WaitAll();
}
} // namespace orbit::terrain_region
