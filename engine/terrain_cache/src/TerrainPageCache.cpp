#include <orbit/terrain_cache/TerrainPageCache.hpp>

#include <orbit/core/Log.hpp>
#include <orbit/terrain_cache/TerrainPageBuilder.hpp>

#include <exception>
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
};

struct TerrainPageDescHash
{
    [[nodiscard]] std::size_t operator()(
        const TerrainPageDesc& desc) const noexcept
    {
        std::size_t hash =
            static_cast<std::size_t>(desc.tile.face);

        const auto combine = [&hash](const u64 value)
        {
            hash ^=
                static_cast<std::size_t>(value) +
                0x9E3779B97F4A7C15ULL +
                (hash << 6U) +
                (hash >> 2U);
        };

        combine(desc.tile.level);
        combine(desc.tile.x);
        combine(desc.tile.y);
        combine(desc.resolution);
        return hash;
    }
};
} // namespace

class TerrainPageCache::Impl
{
public:
    Impl(
        const world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        jobs::JobSystem& jobs)
        : planet_(planet),
          source_(std::move(source)),
          jobs_(jobs)
    {
        if (!source_)
        {
            throw std::invalid_argument(
                "Orbit terrain page cache requires a terrain source.");
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

    bool Request(const TerrainPageDesc& desc)
    {
        std::shared_ptr<CacheEntry> entry;

        {
            std::scoped_lock lock(entriesMutex_);

            if (entries_.contains(desc))
            {
                return false;
            }

            entry = std::make_shared<CacheEntry>();
            entries_.emplace(desc, entry);
        }

        const world::PlanetDefinition planet = planet_;
        const std::shared_ptr<const terrain::TerrainSource> source =
            source_;

        jobs_.Submit(
            group_,
            [planet, source, desc, entry]
            {
                try
                {
                    auto page = std::make_shared<TerrainPage>(
                        BuildTerrainPage(
                            planet,
                            *source,
                            desc));

                    std::scoped_lock lock(entry->mutex);
                    entry->page = std::move(page);
                    entry->state = EntryState::Ready;
                }
                catch (...)
                {
                    {
                        std::scoped_lock lock(entry->mutex);
                        entry->exception = std::current_exception();
                        entry->state = EntryState::Failed;
                    }

                    throw;
                }
            });

        return true;
    }

    std::shared_ptr<const TerrainPage> TryGet(
        const TerrainPageDesc& desc) const
    {
        const std::shared_ptr<CacheEntry> entry = Find(desc);

        if (!entry)
        {
            return {};
        }

        std::scoped_lock lock(entry->mutex);

        if (entry->state != EntryState::Ready)
        {
            return {};
        }

        return entry->page;
    }

    bool IsPending(const TerrainPageDesc& desc) const
    {
        const std::shared_ptr<CacheEntry> entry = Find(desc);

        if (!entry)
        {
            return false;
        }

        std::scoped_lock lock(entry->mutex);
        return entry->state == EntryState::Pending;
    }

    std::size_t EntryCount() const
    {
        std::scoped_lock lock(entriesMutex_);
        return entries_.size();
    }

    void WaitAll()
    {
        jobs_.Wait(group_);
    }

private:
    std::shared_ptr<CacheEntry> Find(
        const TerrainPageDesc& desc) const
    {
        std::scoped_lock lock(entriesMutex_);

        const auto iterator = entries_.find(desc);
        if (iterator == entries_.end())
        {
            return {};
        }

        return iterator->second;
    }

    world::PlanetDefinition planet_;
    std::shared_ptr<const terrain::TerrainSource> source_;
    jobs::JobSystem& jobs_;
    jobs::JobGroup group_;

    mutable std::mutex entriesMutex_;
    std::unordered_map<
        TerrainPageDesc,
        std::shared_ptr<CacheEntry>,
        TerrainPageDescHash> entries_;
};

TerrainPageCache::TerrainPageCache(
    const world::PlanetDefinition planet,
    std::shared_ptr<const terrain::TerrainSource> source,
    jobs::JobSystem& jobs)
    : impl_(std::make_unique<Impl>(
        planet,
        std::move(source),
        jobs))
{
}

TerrainPageCache::~TerrainPageCache() = default;

bool TerrainPageCache::Request(const TerrainPageDesc& desc)
{
    return impl_->Request(desc);
}

std::shared_ptr<const TerrainPage> TerrainPageCache::TryGet(
    const TerrainPageDesc& desc) const
{
    return impl_->TryGet(desc);
}

bool TerrainPageCache::IsPending(
    const TerrainPageDesc& desc) const
{
    return impl_->IsPending(desc);
}

std::size_t TerrainPageCache::EntryCount() const
{
    return impl_->EntryCount();
}

void TerrainPageCache::WaitAll()
{
    impl_->WaitAll();
}
} // namespace orbit::terrain_cache
