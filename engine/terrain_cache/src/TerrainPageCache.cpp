#include <orbit/terrain_cache/TerrainPageCache.hpp>

#include <orbit/core/Log.hpp>
#include <orbit/terrain_cache/TerrainPageBuilder.hpp>

#include <algorithm>
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

    // Protected by TerrainPageCache::Impl::entriesMutex_.
    std::size_t reservedBytes{0};
    std::size_t residentBytes{0};
};

[[nodiscard]] std::size_t EstimatePageBytes(
    const TerrainPageDesc& desc)
{
    const std::size_t resolution =
        static_cast<std::size_t>(
            desc.resolution);

    if (resolution >
        std::numeric_limits<
            std::size_t>::max() /
            resolution)
    {
        throw std::overflow_error(
            "Orbit terrain page byte estimate overflow.");
    }

    const std::size_t sampleCount =
        resolution *
        resolution;

    constexpr std::size_t sampleBytes =
        sizeof(terrain::TerrainSample);

    if (sampleCount >
        (std::numeric_limits<
             std::size_t>::max() -
         sizeof(TerrainPage)) /
            sampleBytes)
    {
        throw std::overflow_error(
            "Orbit terrain page byte estimate overflow.");
    }

    return sizeof(TerrainPage) +
           sampleCount *
               sampleBytes;
}

[[nodiscard]] std::size_t ResidentPageBytes(
    const TerrainPage& page)
{
    constexpr std::size_t sampleBytes =
        sizeof(terrain::TerrainSample);

    if (page.samples.capacity() >
        (std::numeric_limits<
             std::size_t>::max() -
         sizeof(TerrainPage)) /
            sampleBytes)
    {
        throw std::overflow_error(
            "Orbit terrain page resident byte count overflow.");
    }

    return sizeof(TerrainPage) +
           page.samples.capacity() *
               sampleBytes;
}
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

        if (config_.budgetBytes == 0)
        {
            throw std::invalid_argument(
                "Orbit terrain page cache requires a positive byte budget.");
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

        const std::size_t reservationBytes =
            EstimatePageBytes(desc);

        if (reservationBytes >
            config_.budgetBytes)
        {
            capacityRejects_.
                fetch_add(
                    1,
                    std::memory_order_relaxed);

            return false;
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
                    RemoveEntryLocked(
                        existing);
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

            ReclaimFailedLocked();

            while (WouldExceedBudgetLocked(
                       reservationBytes) ||
                   WouldExceedEntryGuardLocked())
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

            entry->reservedBytes =
                reservationBytes;

            entries_.emplace(
                desc,
                entry);

            reservedBytesLocked_ +=
                reservationBytes;

            PublishEntryCountLocked();
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

        try
        {
            jobs_.Submit(
                group_,
                jobs::JobPriority::Low,
                [
                    this,
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

                        FinishReady(
                            entry,
                            std::move(page));
                    }
                    catch (...)
                    {
                        FinishFailed(
                            entry,
                            std::current_exception());

                        throw;
                    }
                });
        }
        catch (...)
        {
            FinishFailed(
                entry,
                std::current_exception());

            throw;
        }

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
            .residentBytes =
                residentBytes_.load(
                    std::memory_order_acquire),
            .budgetBytes =
                config_.budgetBytes,
            .peakResidentBytes =
                peakResidentBytes_.load(
                    std::memory_order_acquire),
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

    using EntryMap =
        std::unordered_map<
            TerrainPageDesc,
            std::shared_ptr<CacheEntry>,
            TerrainPageDescHash>;

    [[nodiscard]] bool WouldExceedBudgetLocked(
        const std::size_t incomingBytes) const noexcept
    {
        const std::size_t usedBytes =
            residentBytesLocked_ +
            reservedBytesLocked_;

        return incomingBytes >
               config_.budgetBytes -
                   (std::min)(
                       usedBytes,
                       config_.budgetBytes);
    }

    [[nodiscard]] bool WouldExceedEntryGuardLocked()
        const noexcept
    {
        return config_.softEntryLimit != 0 &&
               entries_.size() >=
                   config_.softEntryLimit;
    }

    void FinishReady(
        const std::shared_ptr<CacheEntry>& entry,
        std::shared_ptr<TerrainPage> page)
    {
        const std::size_t pageBytes =
            ResidentPageBytes(
                *page);

        std::scoped_lock lock(
            entriesMutex_);

        std::scoped_lock entryLock(
            entry->mutex);

        if (entry->state !=
            EntryState::Pending)
        {
            return;
        }

        ReleaseReservationLocked(
            *entry);

        while (pageBytes >
               config_.budgetBytes -
                   (std::min)(
                       residentBytesLocked_ +
                           reservedBytesLocked_,
                       config_.budgetBytes))
        {
            if (!EvictOldestReusableLocked(
                    entry.get()))
            {
                entry->exception =
                    std::make_exception_ptr(
                        std::runtime_error(
                            "Orbit terrain page could not fit the configured byte budget."));

                entry->state =
                    EntryState::Failed;

                capacityRejects_.
                    fetch_add(
                        1,
                        std::memory_order_relaxed);

                PublishResidentBytesLocked();
                return;
            }
        }

        entry->page =
            std::move(page);

        entry->residentBytes =
            pageBytes;

        entry->state =
            EntryState::Ready;

        residentBytesLocked_ +=
            pageBytes;

        peakResidentBytesLocked_ =
            (std::max)(
                peakResidentBytesLocked_,
                residentBytesLocked_);

        PublishResidentBytesLocked();
    }

    void FinishFailed(
        const std::shared_ptr<CacheEntry>& entry,
        const std::exception_ptr exception)
    {
        std::scoped_lock lock(
            entriesMutex_);

        std::scoped_lock entryLock(
            entry->mutex);

        if (entry->state !=
            EntryState::Pending)
        {
            return;
        }

        ReleaseReservationLocked(
            *entry);

        entry->page.reset();
        entry->exception =
            exception;
        entry->state =
            EntryState::Failed;

        PublishResidentBytesLocked();
    }

    void ReleaseReservationLocked(
        CacheEntry& entry) noexcept
    {
        if (entry.reservedBytes == 0)
        {
            return;
        }

        reservedBytesLocked_ -=
            (std::min)(
                reservedBytesLocked_,
                entry.reservedBytes);

        entry.reservedBytes = 0;
    }

    bool EvictOldestReusableLocked(
        const CacheEntry* protectedEntry = nullptr)
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
                CacheEntry>& candidate =
                    iterator->second;

            if (candidate.get() ==
                protectedEntry)
            {
                continue;
            }

            EntryState state =
                EntryState::Pending;

            u64 ticket = 0;

            {
                std::scoped_lock entryLock(
                    candidate->mutex);

                state =
                    candidate->state;

                ticket =
                    candidate->lastUseTicket.load(
                        std::memory_order_relaxed);
            }

            if (state ==
                EntryState::Pending)
            {
                continue;
            }

            if (state ==
                EntryState::Failed)
            {
                RemoveEntryLocked(
                    iterator);

                return true;
            }

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

        {
            std::scoped_lock entryLock(
                oldest->second->mutex);

            residentBytesLocked_ -=
                (std::min)(
                    residentBytesLocked_,
                    oldest->second->
                        residentBytes);
        }

        entries_.erase(oldest);

        evictions_.fetch_add(
            1,
            std::memory_order_relaxed);

        PublishEntryCountLocked();
        PublishResidentBytesLocked();

        return true;
    }

    void ReclaimFailedLocked()
    {
        for (auto iterator =
                 entries_.begin();
             iterator !=
                 entries_.end();)
        {
            bool failed = false;

            {
                std::scoped_lock entryLock(
                    iterator->second->mutex);

                failed =
                    iterator->second->state ==
                    EntryState::Failed;
            }

            if (!failed)
            {
                ++iterator;
                continue;
            }

            iterator =
                RemoveEntryLocked(
                    iterator);
        }
    }

    EntryMap::iterator RemoveEntryLocked(
        const EntryMap::iterator iterator)
    {
        {
            std::scoped_lock entryLock(
                iterator->second->mutex);

            ReleaseReservationLocked(
                *iterator->second);

            residentBytesLocked_ -=
                (std::min)(
                    residentBytesLocked_,
                    iterator->second->
                        residentBytes);
        }

        const auto next =
            entries_.erase(
                iterator);

        PublishEntryCountLocked();
        PublishResidentBytesLocked();

        return next;
    }

    void PublishEntryCountLocked() noexcept
    {
        entryCount_.store(
            entries_.size(),
            std::memory_order_release);
    }

    void PublishResidentBytesLocked() noexcept
    {
        residentBytes_.store(
            residentBytesLocked_,
            std::memory_order_release);

        peakResidentBytes_.store(
            peakResidentBytesLocked_,
            std::memory_order_release);
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

    EntryMap entries_;

    std::size_t residentBytesLocked_{0};
    std::size_t reservedBytesLocked_{0};
    std::size_t peakResidentBytesLocked_{0};

    mutable std::atomic<u64>
        accessTicket_{0};

    std::atomic<std::size_t>
        entryCount_{0};

    std::atomic<std::size_t>
        residentBytes_{0};

    std::atomic<std::size_t>
        peakResidentBytes_{0};

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
