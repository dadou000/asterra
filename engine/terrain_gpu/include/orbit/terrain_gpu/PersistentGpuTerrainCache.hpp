#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Resource.hpp>
#include <orbit/terrain/TerrainContracts.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace orbit::terrain_gpu
{
enum class CachedTerrainProduct : u32
{
    None = 0,
    MaterialColumn = 1U << 0U,
    Drainage = 1U << 1U,
    Hydraulic = 1U << 2U,
    Thermal = 1U << 3U,
    Aeolian = 1U << 4U,
    SedimentExchange = 1U << 5U,
    Coastal = 1U << 6U,
    Scatter = 1U << 7U,
    RiverGeometry = 1U << 8U,
    PhysicalSurface = 1U << 9U
};

using CachedTerrainProductMask = u32;

[[nodiscard]] constexpr CachedTerrainProductMask ProductBit(
    const CachedTerrainProduct product) noexcept
{
    return static_cast<CachedTerrainProductMask>(product);
}

struct PersistentGpuTerrainCacheKey
{
    terrain::PhysicalTerrainPageAddress address{};
    u8 physicalLod{0};
    terrain::TerrainGenerationRevisions revisions{};

    [[nodiscard]] constexpr bool operator==(
        const PersistentGpuTerrainCacheKey&) const noexcept = default;
};

struct PersistentGpuTerrainCacheKeyHash
{
    [[nodiscard]] std::size_t operator()(
        const PersistentGpuTerrainCacheKey& key) const noexcept;
};

[[nodiscard]] u64 PersistentGpuTerrainCacheFingerprint(
    const PersistentGpuTerrainCacheKey& key) noexcept;

class CachedGpuTerrainPage
{
public:
    CachedGpuTerrainPage() = default;

    CachedTerrainProductMask products{0};

    std::vector<std::shared_ptr<rhi::Buffer>> buffers;
    std::vector<std::shared_ptr<rhi::Texture>> textures;

    [[nodiscard]] bool IsCacheable() const noexcept;
    [[nodiscard]] u64 ResidentBytes() const noexcept;
};

struct PersistentGpuTerrainCacheConfig
{
    u64 maximumResidentBytes{512ULL * 1024ULL * 1024ULL};
    u32 maximumPages{4096};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct PersistentGpuTerrainCacheStats
{
    u64 hits{0};
    u64 misses{0};
    u64 generations{0};
    u64 insertions{0};
    u64 evictions{0};
    u64 residentPages{0};
    u64 residentBytes{0};
};

class PersistentGpuTerrainCache
{
public:
    using Generator =
        std::function<std::shared_ptr<CachedGpuTerrainPage>()>;

    explicit PersistentGpuTerrainCache(
        PersistentGpuTerrainCacheConfig config = {});

    [[nodiscard]] std::shared_ptr<CachedGpuTerrainPage>
    Find(const PersistentGpuTerrainCacheKey& key);

    [[nodiscard]] bool IsResident(
        const PersistentGpuTerrainCacheKey& key) const noexcept;

    [[nodiscard]] std::shared_ptr<CachedGpuTerrainPage>
    GetOrCreate(
        const PersistentGpuTerrainCacheKey& key,
        const Generator& generator);

    void Insert(
        const PersistentGpuTerrainCacheKey& key,
        std::shared_ptr<CachedGpuTerrainPage> page);

    [[nodiscard]] bool Erase(
        const PersistentGpuTerrainCacheKey& key);

    [[nodiscard]] u64 InvalidateAddress(
        const terrain::PhysicalTerrainPageAddress& address);

    void Clear() noexcept;

    [[nodiscard]] const PersistentGpuTerrainCacheConfig&
    Config() const noexcept;

    [[nodiscard]] const PersistentGpuTerrainCacheStats&
    Stats() const noexcept;

private:
    struct Entry
    {
        std::shared_ptr<CachedGpuTerrainPage> page;
        u64 bytes{0};
        u64 accessSerial{0};
    };

    void Touch(Entry& entry) noexcept;
    void EvictToBudget();

    PersistentGpuTerrainCacheConfig config_{};
    PersistentGpuTerrainCacheStats stats_{};
    u64 accessSerial_{0};

    std::unordered_map<
        PersistentGpuTerrainCacheKey,
        Entry,
        PersistentGpuTerrainCacheKeyHash> entries_;
};
} // namespace orbit::terrain_gpu
