#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>

#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace orbit::terrain_gpu
{
namespace
{
[[nodiscard]] u64 ResourceTextureBytes(
    const rhi::Texture& texture) noexcept
{
    const u64 width = texture.Width();
    const u64 height = texture.Height();
    const u64 bytesPerTexel =
        rhi::TextureFormatBytesPerTexel(texture.Format());

    if (width == 0 ||
        height == 0 ||
        bytesPerTexel == 0 ||
        width > std::numeric_limits<u64>::max() / height)
    {
        return 0;
    }

    const u64 texels = width * height;
    if (texels >
        std::numeric_limits<u64>::max() / bytesPerTexel)
    {
        return 0;
    }

    return texels * bytesPerTexel;
}
} // namespace

std::size_t PersistentGpuTerrainCacheKeyHash::operator()(
    const PersistentGpuTerrainCacheKey& key) const noexcept
{
    return static_cast<std::size_t>(
        PersistentGpuTerrainCacheFingerprint(key));
}

u64 PersistentGpuTerrainCacheFingerprint(
    const PersistentGpuTerrainCacheKey& key) noexcept
{
    u64 value = 0x4D32364750554348ULL;
    value = terrain::StableCombine64(value, key.address.planet.high);
    value = terrain::StableCombine64(value, key.address.planet.low);
    value = terrain::StableCombine64(
        value,
        static_cast<u64>(key.address.tile.face));
    value = terrain::StableCombine64(value, key.address.tile.level);
    value = terrain::StableCombine64(value, key.address.tile.x);
    value = terrain::StableCombine64(value, key.address.tile.y);
    value = terrain::StableCombine64(value, key.physicalLod);
    value = terrain::StableCombine64(
        value,
        terrain::RevisionFingerprint(key.revisions));
    return value;
}

bool CachedGpuTerrainPage::IsCacheable() const noexcept
{
    return
        products != 0 &&
        (physicalSurface != nullptr ||
         !buffers.empty() ||
         !textures.empty()) &&
        ResidentBytes() > 0;
}

u64 CachedGpuTerrainPage::ResidentBytes() const noexcept
{
    u64 total = 0;
    std::unordered_set<const void*> seen;

    if (physicalSurface &&
        seen.insert(
            physicalSurface.get()).second)
    {
        total =
            physicalSurface->SizeBytes();
    }

    for (const auto& buffer : buffers)
    {
        if (!buffer ||
            !seen.insert(buffer.get()).second)
        {
            continue;
        }

        const u64 bytes = buffer->SizeBytes();
        if (bytes > std::numeric_limits<u64>::max() - total)
        {
            return std::numeric_limits<u64>::max();
        }
        total += bytes;
    }

    for (const auto& texture : textures)
    {
        if (!texture ||
            !seen.insert(texture.get()).second)
        {
            continue;
        }

        const u64 bytes = ResourceTextureBytes(*texture);
        if (bytes > std::numeric_limits<u64>::max() - total)
        {
            return std::numeric_limits<u64>::max();
        }
        total += bytes;
    }

    return total;
}

bool PersistentGpuTerrainCacheConfig::IsValid() const noexcept
{
    return maximumResidentBytes > 0 && maximumPages > 0;
}

PersistentGpuTerrainCache::PersistentGpuTerrainCache(
    const PersistentGpuTerrainCacheConfig config)
    : config_(config)
{
    if (!config_.IsValid())
    {
        throw std::invalid_argument(
            "M26 persistent GPU terrain cache config is invalid.");
    }
}

void PersistentGpuTerrainCache::Touch(Entry& entry) noexcept
{
    ++accessSerial_;
    if (accessSerial_ == 0)
    {
        accessSerial_ = 1;
    }
    entry.accessSerial = accessSerial_;
}

std::shared_ptr<CachedGpuTerrainPage>
PersistentGpuTerrainCache::Find(
    const PersistentGpuTerrainCacheKey& key)
{
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        ++stats_.misses;
        return {};
    }

    ++stats_.hits;
    Touch(it->second);
    return it->second.page;
}

bool PersistentGpuTerrainCache::IsResident(
    const PersistentGpuTerrainCacheKey& key) const noexcept
{
    return entries_.contains(key);
}


std::shared_ptr<CachedGpuTerrainPage>
PersistentGpuTerrainCache::GetOrCreate(
    const PersistentGpuTerrainCacheKey& key,
    const Generator& generator)
{
    if (!generator)
    {
        throw std::invalid_argument(
            "M26 cache generator is empty.");
    }

    if (auto existing = Find(key))
    {
        return existing;
    }

    ++stats_.generations;

    auto generated = generator();
    if (!generated || !generated->IsCacheable())
    {
        throw std::runtime_error(
            "M26 generator returned a non-cacheable GPU terrain page.");
    }

    Insert(key, generated);
    return generated;
}

void PersistentGpuTerrainCache::Insert(
    const PersistentGpuTerrainCacheKey& key,
    std::shared_ptr<CachedGpuTerrainPage> page)
{
    if (!key.address.planet.IsValid() ||
        !page ||
        !page->IsCacheable())
    {
        throw std::invalid_argument(
            "M26 persistent GPU terrain cache insertion is invalid.");
    }

    const u64 bytes = page->ResidentBytes();

    const auto found = entries_.find(key);
    if (found != entries_.end())
    {
        stats_.residentBytes -= found->second.bytes;
        found->second.page = std::move(page);
        found->second.bytes = bytes;
        stats_.residentBytes += bytes;
        Touch(found->second);
    }
    else
    {
        Entry entry{
            .page = std::move(page),
            .bytes = bytes,
            .accessSerial = 0
        };
        Touch(entry);
        entries_.emplace(key, std::move(entry));
        ++stats_.insertions;
        ++stats_.residentPages;
        stats_.residentBytes += bytes;
    }

    EvictToBudget();
}

bool PersistentGpuTerrainCache::Erase(
    const PersistentGpuTerrainCacheKey& key)
{
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return false;
    }

    stats_.residentBytes -= it->second.bytes;
    --stats_.residentPages;
    entries_.erase(it);
    return true;
}

u64 PersistentGpuTerrainCache::InvalidateAddress(
    const terrain::PhysicalTerrainPageAddress& address)
{
    u64 removed = 0;

    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (!(it->first.address == address))
        {
            ++it;
            continue;
        }

        stats_.residentBytes -= it->second.bytes;
        --stats_.residentPages;
        ++removed;
        it = entries_.erase(it);
    }

    return removed;
}

void PersistentGpuTerrainCache::Clear() noexcept
{
    entries_.clear();
    stats_.residentPages = 0;
    stats_.residentBytes = 0;
}

const PersistentGpuTerrainCacheConfig&
PersistentGpuTerrainCache::Config() const noexcept
{
    return config_;
}

const PersistentGpuTerrainCacheStats&
PersistentGpuTerrainCache::Stats() const noexcept
{
    return stats_;
}

void PersistentGpuTerrainCache::EvictToBudget()
{
    while (!entries_.empty() &&
           (stats_.residentBytes > config_.maximumResidentBytes ||
            stats_.residentPages > config_.maximumPages))
    {
        auto victim = entries_.begin();

        for (auto it = entries_.begin(); it != entries_.end(); ++it)
        {
            if (it->second.accessSerial <
                victim->second.accessSerial)
            {
                victim = it;
            }
        }

        stats_.residentBytes -= victim->second.bytes;
        --stats_.residentPages;
        ++stats_.evictions;
        entries_.erase(victim);
    }
}
} // namespace orbit::terrain_gpu
