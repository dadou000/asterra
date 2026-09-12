#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>

#include <orbit/core/Log.hpp>

#include <atomic>
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
    EntryState state{EntryState::Pending};
    std::shared_ptr<DerivedTerrainRegion> region;
    u64 lastUseTicket{0};
};

struct CacheSharedState
{
    mutable std::mutex mutex;

    std::unordered_map<
        DerivedTerrainRegionId,
        std::shared_ptr<CacheEntry>,
        DerivedTerrainRegionIdHash>
        entries;

    u64 nextUseTicket{0};

    std::atomic<u64> contentRevision{0};

    u64 acceptedRequests{0};
    u64 duplicateRequests{0};
    u64 readyHits{0};
    u64 misses{0};
    u64 evictions{0};
    u64 capacityRejects{0};
    u64 failedBuilds{0};
    u64 staleRemovals{0};
    u64 staleRequestRejects{0};
};

[[nodiscard]] u64 NextUseTicket(
    CacheSharedState& state) noexcept
{
    return ++state.nextUseTicket;
}

void RemoveStaleEntriesLocked(
    CacheSharedState& state,
    const u64 currentSourceRevision)
{
    for (auto iterator =
             state.entries.begin();
         iterator != state.entries.end();)
    {
        if (iterator->first.sourceRevision ==
            currentSourceRevision)
        {
            ++iterator;
            continue;
        }

        const bool wasReady =
            iterator->second->state ==
            EntryState::Ready;

        iterator =
            state.entries.erase(iterator);

        ++state.staleRemovals;

        if (wasReady)
        {
            state.contentRevision.fetch_add(
                1,
                std::memory_order_release);
        }
    }
}

[[nodiscard]] bool EvictOneReusableLocked(
    CacheSharedState& state)
{
    for (auto iterator =
             state.entries.begin();
         iterator != state.entries.end();
         ++iterator)
    {
        if (iterator->second->state ==
            EntryState::Failed)
        {
            state.entries.erase(iterator);
            return true;
        }
    }

    auto oldest =
        state.entries.end();

    u64 oldestTicket =
        std::numeric_limits<u64>::max();

    for (auto iterator =
             state.entries.begin();
         iterator != state.entries.end();
         ++iterator)
    {
        const CacheEntry& entry =
            *iterator->second;

        if (entry.state !=
            EntryState::Ready)
        {
            continue;
        }

        if (entry.lastUseTicket <
            oldestTicket)
        {
            oldestTicket =
                entry.lastUseTicket;

            oldest = iterator;
        }
    }

    if (oldest ==
        state.entries.end())
    {
        return false;
    }

    state.entries.erase(oldest);
    ++state.evictions;

    state.contentRevision.fetch_add(
        1,
        std::memory_order_release);

    return true;
}
} // namespace

class DerivedTerrainRegionCache::Impl
{
public:
    Impl(
        const world::PlanetDefinition planet,
        std::shared_ptr<
            const terrain::TerrainSource> source,
        jobs::JobSystem& jobs,
        const DerivedTerrainRegionCacheConfig config)
        : planet_(planet),
          source_(std::move(source)),
          jobs_(jobs),
          config_(config),
          state_(
              std::make_shared<
                  CacheSharedState>())
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

        if (config_.maxEntries == 0)
        {
            throw std::invalid_argument(
                "Orbit derived terrain region cache requires at least one entry.");
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
                "Derived terrain region generation failed while destroying the cache.");
        }
    }

    DerivedTerrainRegionId IdForDirection(
        const math::Double3& direction) const
    {
        return {
            .tile =
                world::TileForDirection(
                    direction,
                    config_.tileLevel),
            .sourceRevision =
                source_->Revision(),
            .generatorVersion =
                config_.generatorVersion
        };
    }

    bool Request(
        const DerivedTerrainRegionId& id)
    {
        const u64 currentSourceRevision =
            source_->Revision();

        std::shared_ptr<CacheEntry> entry;

        {
            std::scoped_lock lock(
                state_->mutex);

            RemoveStaleEntriesLocked(
                *state_,
                currentSourceRevision);

            if (id.sourceRevision !=
                currentSourceRevision)
            {
                ++state_->
                    staleRequestRejects;

                return false;
            }

            const auto existing =
                state_->entries.find(id);

            if (existing !=
                state_->entries.end())
            {
                if (existing->second->state ==
                    EntryState::Failed)
                {
                    state_->entries.erase(
                        existing);
                }
                else
                {
                    ++state_->
                        duplicateRequests;

                    return false;
                }
            }

            while (state_->entries.size() >=
                   config_.maxEntries)
            {
                if (!EvictOneReusableLocked(
                        *state_))
                {
                    ++state_->
                        capacityRejects;

                    return false;
                }
            }

            entry =
                std::make_shared<
                    CacheEntry>();

            entry->lastUseTicket =
                NextUseTicket(*state_);

            state_->entries.emplace(
                id,
                entry);

            ++state_->acceptedRequests;
        }

        const world::PlanetDefinition
            planet = planet_;

        const std::shared_ptr<
            const terrain::TerrainSource>
            source = source_;

        const DerivedTerrainRegionConfig
            regionConfig =
                config_.region;

        const std::shared_ptr<
            CacheSharedState>
            state = state_;

        try
        {
            jobs_.Submit(
                group_,
                jobs::JobPriority::Low,
                [
                    planet,
                    source,
                    regionConfig,
                    id,
                    entry,
                    state
                ]
                {
                    std::shared_ptr<
                        DerivedTerrainRegion>
                        region;

                    try
                    {
                        region =
                            std::make_shared<
                                DerivedTerrainRegion>(
                                    BuildDerivedTerrainRegion(
                                        planet,
                                        *source,
                                        id,
                                        regionConfig));
                    }
                    catch (...)
                    {
                        std::scoped_lock lock(
                            state->mutex);

                        const auto iterator =
                            state->entries.find(
                                id);

                        if (iterator !=
                                state->entries.end() &&
                            iterator->second ==
                                entry)
                        {
                            entry->state =
                                EntryState::Failed;

                            entry->region.reset();

                            ++state->failedBuilds;
                        }

                        return;
                    }

                    const u64
                        currentRevision =
                            source->Revision();

                    std::scoped_lock lock(
                        state->mutex);

                    const auto iterator =
                        state->entries.find(id);

                    if (iterator ==
                            state->entries.end() ||
                        iterator->second !=
                            entry)
                    {
                        return;
                    }

                    if (currentRevision !=
                        id.sourceRevision)
                    {
                        state->entries.erase(
                            iterator);

                        ++state->staleRemovals;
                        return;
                    }

                    entry->region =
                        std::move(region);

                    entry->state =
                        EntryState::Ready;

                    entry->lastUseTicket =
                        NextUseTicket(*state);

                    state->contentRevision.
                        fetch_add(
                            1,
                            std::memory_order_release);
                });
        }
        catch (...)
        {
            std::scoped_lock lock(
                state_->mutex);

            const auto iterator =
                state_->entries.find(id);

            if (iterator !=
                    state_->entries.end() &&
                iterator->second ==
                    entry)
            {
                state_->entries.erase(
                    iterator);
            }

            throw;
        }

        return true;
    }

    std::shared_ptr<
        const DerivedTerrainRegion>
    TryGet(
        const DerivedTerrainRegionId& id) const
    {
        const u64 currentSourceRevision =
            source_->Revision();

        std::scoped_lock lock(
            state_->mutex);

        RemoveStaleEntriesLocked(
            *state_,
            currentSourceRevision);

        const auto iterator =
            state_->entries.find(id);

        if (iterator ==
            state_->entries.end())
        {
            ++state_->misses;
            return {};
        }

        CacheEntry& entry =
            *iterator->second;

        if (entry.state !=
                EntryState::Ready ||
            !entry.region)
        {
            ++state_->misses;
            return {};
        }

        entry.lastUseTicket =
            NextUseTicket(*state_);

        ++state_->readyHits;

        return entry.region;
    }

    bool IsPending(
        const DerivedTerrainRegionId& id) const
    {
        const u64 currentSourceRevision =
            source_->Revision();

        std::scoped_lock lock(
            state_->mutex);

        RemoveStaleEntriesLocked(
            *state_,
            currentSourceRevision);

        const auto iterator =
            state_->entries.find(id);

        return
            iterator !=
                state_->entries.end() &&
            iterator->second->state ==
                EntryState::Pending;
    }

    u64 ContentRevision() const noexcept
    {
        return state_->contentRevision.load(
            std::memory_order_acquire);
    }

    DerivedTerrainRegionCacheStats
    Stats() const
    {
        const u64 currentSourceRevision =
            source_->Revision();

        std::scoped_lock lock(
            state_->mutex);

        RemoveStaleEntriesLocked(
            *state_,
            currentSourceRevision);

        DerivedTerrainRegionCacheStats
            result{
                .acceptedRequests =
                    state_->acceptedRequests,
                .duplicateRequests =
                    state_->duplicateRequests,
                .readyHits =
                    state_->readyHits,
                .misses =
                    state_->misses,
                .evictions =
                    state_->evictions,
                .capacityRejects =
                    state_->capacityRejects,
                .failedBuilds =
                    state_->failedBuilds,
                .staleRemovals =
                    state_->staleRemovals,
                .staleRequestRejects =
                    state_->
                        staleRequestRejects,
                .entries =
                    state_->entries.size()
            };

        for (const auto& [id, entry] :
             state_->entries)
        {
            static_cast<void>(id);

            switch (entry->state)
            {
            case EntryState::Pending:
                ++result.pendingEntries;
                break;
            case EntryState::Ready:
                ++result.readyEntries;
                break;
            case EntryState::Failed:
                ++result.failedEntries;
                break;
            }
        }

        return result;
    }

    void WaitAll()
    {
        jobs_.Wait(group_);
    }

private:
    world::PlanetDefinition planet_;

    std::shared_ptr<
        const terrain::TerrainSource>
        source_;

    jobs::JobSystem& jobs_;
    DerivedTerrainRegionCacheConfig config_;
    jobs::JobGroup group_;

    std::shared_ptr<
        CacheSharedState>
        state_;
};

DerivedTerrainRegionCache::
DerivedTerrainRegionCache(
    const world::PlanetDefinition planet,
    std::shared_ptr<
        const terrain::TerrainSource> source,
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
    const math::Double3& direction) const
{
    return impl_->IdForDirection(
        direction);
}

bool DerivedTerrainRegionCache::Request(
    const DerivedTerrainRegionId& id)
{
    return impl_->Request(id);
}

std::shared_ptr<const DerivedTerrainRegion>
DerivedTerrainRegionCache::TryGet(
    const DerivedTerrainRegionId& id) const
{
    return impl_->TryGet(id);
}

bool DerivedTerrainRegionCache::IsPending(
    const DerivedTerrainRegionId& id) const
{
    return impl_->IsPending(id);
}

u64 DerivedTerrainRegionCache::
ContentRevision() const noexcept
{
    return impl_->ContentRevision();
}

DerivedTerrainRegionCacheStats
DerivedTerrainRegionCache::Stats() const
{
    return impl_->Stats();
}

void DerivedTerrainRegionCache::WaitAll()
{
    impl_->WaitAll();
}
} // namespace orbit::terrain_region
