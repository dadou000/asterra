#include <orbit/terrain_cache/TerrainPageCache.hpp>

#include <orbit/core/Log.hpp>
#include <orbit/terrain_cache/TerrainPageBuilder.hpp>

#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace orbit::terrain_cache
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
    std::shared_ptr<TerrainPage> page;
    std::exception_ptr exception;
    std::atomic<u64> lastUseTicket{0};
};
} // namespace

class TerrainPageCache::Impl
{
public:
    Impl(
        const world::PlanetDefinition planet,
        std::shared_ptr<
            const terrain::TerrainSource> source,
        jobs::JobSystem& jobs,
        const TerrainPageCacheConfig config)
        : planet_(planet),
          source_(std::move(source)),
          jobs_(jobs),
          config_(config)
    {
        if (!source_)
        {
            throw std::invalid_argument(
                "Orbit terrain page cache requires a terrain source.");
        }

        if (planet_.radiusMeters <= 0.0)
        {
            throw std::invalid_argument(
                "Orbit terrain page cache requires a positive planet radius.");
        }

        if (config_.maxEntries == 0)
        {
            throw std::invalid_argument(
                "Orbit terrain page cache requires at least one entry.");
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
                "Terrain page generation failed while destroying the cache.");
        }
    }

    bool Request(
        const TerrainPageDesc& desc)
    {
        if (desc.resolution < 2)
        {
            throw std::invalid_argument(
                "Orbit terrain cache page resolution must be at least two.");
        }

        std::shared_ptr<CacheEntry> entry;

        {
            std::scoped_lock lock(
                entriesMutex_);

            const auto existing =
                entries_.find(desc);

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

                if (failed)
                {
                    entries_.erase(existing);

                    entryCount_.store(
                        entries_.size(),
                        std::memory_order_release);
                }
                else
                {
                    duplicateRequests_.
                        fetch_add(
                            1,
                            std::memory_order_relaxed);

                    return false;
                }
            }

            while (entries_.size() >=
                   config_.maxEntries)
            {
                if (!EvictOldestReusableLocked())
                {
                    capacityRejects_.
                        fetch_add(
                            1,
                            std::memory_order_relaxed);

                    return false;
                }
            }

            entry =
                std::make_shared<
                    CacheEntry>();

            entry->lastUseTicket.store(
                NextTicket(),
                std::memory_order_relaxed);

            entries_.emplace(
                desc,
                entry);

            entryCount_.store(
                entries_.size(),
                std::memory_order_release);
        }

        acceptedRequests_.
            fetch_add(
                1,
                std::memory_order_relaxed);

        const world::PlanetDefinition
            planet = planet_;

        const std::shared_ptr<
            const terrain::TerrainSource>
            source = source_;

        jobs_.Submit(
            group_,
            [
                planet,
                source,
                desc,
                entry
            ]
            {
                try
                {
                    auto page =
                        std::make_shared<
                            TerrainPage>(
                                BuildTerrainPage(
                                    planet,
                                    *source,
                                    desc));

                    std::scoped_lock lock(
                        entry->mutex);

                    entry->page =
                        std::move(page);

                    entry->state =
                        EntryState::Ready;
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

                    throw;
                }
            });

        return true;
    }

    std::shared_ptr<const TerrainPage>
    TryGet(
        const TerrainPageDesc& desc) const
    {
        const std::shared_ptr<CacheEntry>
            entry = Find(desc);

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

        return entry->page;
    }

    bool IsPending(
        const TerrainPageDesc& desc) const
    {
        const std::shared_ptr<CacheEntry>
            entry = Find(desc);

        if (!entry)
        {
            return false;
        }

        std::scoped_lock lock(
            entry->mutex);

        return entry->state ==
            EntryState::Pending;
    }

    std::size_t EntryCount() const noexcept
    {
        return entryCount_.load(
            std::memory_order_acquire);
    }

    TerrainPageCacheStats Stats() const noexcept
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
            .entries =
                entryCount_.load(
                    std::memory_order_acquire)
        };
    }

    void WaitAll()
    {
        jobs_.Wait(group_);
    }

private:
    [[nodiscard]] u64 NextTicket() const noexcept
    {
        return accessTicket_.
            fetch_add(
                1,
                std::memory_order_relaxed) +
            1;
    }

    bool EvictOldestReusableLocked()
    {
        auto oldest =
            entries_.end();

        u64 oldestTicket =
            std::numeric_limits<u64>::max();

        for (auto iterator =
                 entries_.begin();
             iterator !=
                 entries_.end();
             ++iterator)
        {
            const std::shared_ptr<
                CacheEntry>& entry =
                    iterator->second;

            std::scoped_lock entryLock(
                entry->mutex);

            if (entry->state ==
                EntryState::Pending)
            {
                continue;
            }

            const u64 ticket =
                entry->lastUseTicket.load(
                    std::memory_order_relaxed);

            if (ticket <
                oldestTicket)
            {
                oldestTicket =
                    ticket;

                oldest =
                    iterator;
            }
        }

        if (oldest ==
            entries_.end())
        {
            return false;
        }

        entries_.erase(oldest);

        evictions_.fetch_add(
            1,
            std::memory_order_relaxed);

        entryCount_.store(
            entries_.size(),
            std::memory_order_release);

        return true;
    }

    std::shared_ptr<CacheEntry> Find(
        const TerrainPageDesc& desc) const
    {
        std::scoped_lock lock(
            entriesMutex_);

        const auto iterator =
            entries_.find(desc);

        if (iterator ==
            entries_.end())
        {
            return {};
        }

        return iterator->second;
    }

    world::PlanetDefinition planet_;

    std::shared_ptr<
        const terrain::TerrainSource>
        source_;

    jobs::JobSystem& jobs_;
    TerrainPageCacheConfig config_;
    jobs::JobGroup group_;

    mutable std::mutex entriesMutex_;

    std::unordered_map<
        TerrainPageDesc,
        std::shared_ptr<CacheEntry>,
        TerrainPageDescHash>
        entries_;

    mutable std::atomic<u64>
        accessTicket_{0};

    std::atomic<std::size_t>
        entryCount_{0};

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
};

TerrainPageCache::TerrainPageCache(
    const world::PlanetDefinition planet,
    std::shared_ptr<
        const terrain::TerrainSource> source,
    jobs::JobSystem& jobs,
    const TerrainPageCacheConfig config)
    : impl_(
        std::make_unique<Impl>(
            planet,
            std::move(source),
            jobs,
            config))
{
}

TerrainPageCache::~TerrainPageCache() =
    default;

bool TerrainPageCache::Request(
    const TerrainPageDesc& desc)
{
    return impl_->Request(desc);
}

std::shared_ptr<const TerrainPage>
TerrainPageCache::TryGet(
    const TerrainPageDesc& desc) const
{
    return impl_->TryGet(desc);
}

bool TerrainPageCache::IsPending(
    const TerrainPageDesc& desc) const
{
    return impl_->IsPending(desc);
}

std::size_t TerrainPageCache::
EntryCount() const
{
    return impl_->EntryCount();
}

TerrainPageCacheStats
TerrainPageCache::Stats() const noexcept
{
    return impl_->Stats();
}

void TerrainPageCache::WaitAll()
{
    impl_->WaitAll();
}
} // namespace orbit::terrain_cache
