#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M26 failure: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        Fail(message);
    }
}

class FakeBuffer final : public rhi::Buffer
{
public:
    explicit FakeBuffer(const u64 sizeBytes)
        : sizeBytes_(sizeBytes)
    {
    }

    [[nodiscard]] u64 SizeBytes() const noexcept override
    {
        return sizeBytes_;
    }

    [[nodiscard]] rhi::BufferUsage Usage() const noexcept override
    {
        return rhi::BufferUsage::Structured;
    }

    [[nodiscard]] rhi::MemoryUsage Memory() const noexcept override
    {
        return rhi::MemoryUsage::GpuOnly;
    }

    [[nodiscard]] std::byte* Map() override
    {
        return nullptr;
    }

    void Unmap() override
    {
    }

private:
    u64 sizeBytes_{0};
};

class FakeTexture final : public rhi::Texture
{
public:
    FakeTexture(
        const u32 width,
        const u32 height,
        const rhi::TextureFormat format)
        : width_(width),
          height_(height),
          format_(format)
    {
    }

    [[nodiscard]] u32 Width() const noexcept override { return width_; }
    [[nodiscard]] u32 Height() const noexcept override { return height_; }
    [[nodiscard]] rhi::TextureFormat Format() const noexcept override
    {
        return format_;
    }

private:
    u32 width_{0};
    u32 height_{0};
    rhi::TextureFormat format_{};
};

terrain_gpu::PersistentGpuTerrainCacheKey MakeKey(
    const u32 x,
    const u8 physicalLod = 2,
    const u64 processRevision = 10)
{
    return {
        .address = {
            .planet = {
                .high = 0x4F524249544D3236ULL,
                .low = 0x0000000000000001ULL
            },
            .tile = {
                .face = world::CubeFace::PositiveX,
                .level = 7,
                .x = x,
                .y = 4
            }
        },
        .physicalLod = physicalLod,
        .revisions = {
            .geology = 1,
            .climate = 2,
            .authoring = 3,
            .biome = 4,
            .water = 5,
            .processes = processRevision
        }
    };
}

std::shared_ptr<terrain_gpu::CachedGpuTerrainPage>
MakePage(const u64 bufferBytes)
{
    auto page =
        std::make_shared<terrain_gpu::CachedGpuTerrainPage>();

    page->products =
        terrain_gpu::ProductBit(
            terrain_gpu::CachedTerrainProduct::MaterialColumn) |
        terrain_gpu::ProductBit(
            terrain_gpu::CachedTerrainProduct::Drainage);

    page->buffers.push_back(
        std::make_shared<FakeBuffer>(bufferBytes));

    page->textures.push_back(
        std::make_shared<FakeTexture>(
            8,
            8,
            rhi::TextureFormat::R32_Float));

    return page;
}

void TestResourceAccountingCountsGpuResources()
{
    const auto page = MakePage(128);

    Require(
        page->ResidentBytes() ==
            128ULL + 8ULL * 8ULL * 4ULL,
        "M26 resident bytes must include GPU buffers and textures.");

    Require(
        page->IsCacheable(),
        "Solved resource page must be cacheable.");
}

void TestStationaryWarmupGeneratesOnce()
{
    terrain_gpu::PersistentGpuTerrainCache cache({
        .maximumResidentBytes = 16ULL * 1024ULL,
        .maximumPages = 16
    });

    const auto key = MakeKey(1);
    u64 generatorCalls = 0;

    const auto generate =
        [&]()
        {
            ++generatorCalls;
            return MakePage(256);
        };

    const auto first = cache.GetOrCreate(key, generate);
    const auto second = cache.GetOrCreate(key, generate);

    Require(
        first == second,
        "Warm cache lookup must return the same resident solved page.");

    Require(
        generatorCalls == 1 &&
        cache.Stats().generations == 1 &&
        cache.Stats().hits == 1 &&
        cache.Stats().misses == 1,
        "Stationary warmup must produce zero regeneration after first miss.");
}

void TestRevisitResidentTerrainDoesNotRegenerate()
{
    terrain_gpu::PersistentGpuTerrainCache cache({
        .maximumResidentBytes = 64ULL * 1024ULL,
        .maximumPages = 16
    });

    u64 aCalls = 0;
    u64 bCalls = 0;

    const auto aKey = MakeKey(2);
    const auto bKey = MakeKey(3);

    static_cast<void>(
        cache.GetOrCreate(
            aKey,
            [&]()
            {
                ++aCalls;
                return MakePage(512);
            }));

    static_cast<void>(
        cache.GetOrCreate(
            bKey,
        [&]()
            {
                ++bCalls;
                return MakePage(512);
            }));

    static_cast<void>(
        cache.GetOrCreate(
            aKey,
        [&]()
            {
                ++aCalls;
                return MakePage(512);
            }));

    Require(
        aCalls == 1 && bCalls == 1,
        "Returning to resident physical terrain must not regenerate it.");
}

void TestRevisionAndPhysicalLodArePartOfIdentity()
{
    terrain_gpu::PersistentGpuTerrainCache cache({
        .maximumResidentBytes = 64ULL * 1024ULL,
        .maximumPages = 16
    });

    u64 calls = 0;
    const auto generate =
        [&]()
        {
            ++calls;
            return MakePage(128);
        };

    static_cast<void>(
        cache.GetOrCreate(MakeKey(4, 2, 10), generate));
    static_cast<void>(
        cache.GetOrCreate(MakeKey(4, 2, 11), generate));
    static_cast<void>(
        cache.GetOrCreate(MakeKey(4, 3, 11), generate));

    Require(
        calls == 3,
        "Physical LOD or authority revision changes must create distinct cache entries.");
}

void TestLruBudgetEvictsLeastRecentlyUsedPage()
{
    terrain_gpu::PersistentGpuTerrainCache cache({
        .maximumResidentBytes = 1300,
        .maximumPages = 8
    });

    const auto a = MakeKey(5);
    const auto b = MakeKey(6);
    const auto c = MakeKey(7);

    cache.Insert(a, MakePage(256));
    cache.Insert(b, MakePage(256));

    Require(cache.Find(a) != nullptr, "Touching A must make it newer than B.");

    cache.Insert(c, MakePage(256));

    Require(
        cache.Find(a) != nullptr,
        "Recently used A must survive LRU pressure.");

    Require(
        cache.Find(b) == nullptr,
        "Least-recently-used B must be evicted.");

    Require(
        cache.Stats().evictions >= 1,
        "Budget pressure must report an eviction.");
}

void TestExternalReferenceSurvivesCacheEviction()
{
    terrain_gpu::PersistentGpuTerrainCache cache({
        .maximumResidentBytes = 700,
        .maximumPages = 1
    });

    const auto aKey = MakeKey(8);
    const auto bKey = MakeKey(9);

    auto external = MakePage(128);
    cache.Insert(aKey, external);
    cache.Insert(bKey, MakePage(128));

    Require(
        cache.Find(aKey) == nullptr,
        "A must be evicted from the cache index.");

    Require(
        external->ResidentBytes() > 0,
        "External/in-flight ownership must keep evicted GPU resources alive.");
}

void TestAddressInvalidationRemovesAllPhysicalLods()
{
    terrain_gpu::PersistentGpuTerrainCache cache({
        .maximumResidentBytes = 64ULL * 1024ULL,
        .maximumPages = 16
    });

    cache.Insert(MakeKey(10, 1), MakePage(128));
    cache.Insert(MakeKey(10, 2), MakePage(128));
    cache.Insert(MakeKey(11, 2), MakePage(128));

    const auto address = MakeKey(10, 1).address;

    Require(
        cache.InvalidateAddress(address) == 2,
        "Dependency invalidation must remove every cached physical LOD for one page address.");

    Require(
        cache.Find(MakeKey(11, 2)) != nullptr,
        "Invalidating one address must not evict unrelated terrain.");
}

void TestStableFingerprintDependsOnlyOnPhysicalIdentity()
{
    const auto a = MakeKey(12, 2, 10);
    const auto b = MakeKey(12, 2, 10);

    const u64 first =
        terrain_gpu::PersistentGpuTerrainCacheFingerprint(a);
    const u64 second =
        terrain_gpu::PersistentGpuTerrainCacheFingerprint(b);

    Require(
        first != 0 && first == second,
        "Same physical page/LOD/revisions must produce the same cache identity.");
}
} // namespace

int main()
{
    TestResourceAccountingCountsGpuResources();
    TestStationaryWarmupGeneratesOnce();
    TestRevisitResidentTerrainDoesNotRegenerate();
    TestRevisionAndPhysicalLodArePartOfIdentity();
    TestLruBudgetEvictsLeastRecentlyUsedPage();
    TestExternalReferenceSurvivesCacheEviction();
    TestAddressInvalidationRemovesAllPhysicalLods();
    TestStableFingerprintDependsOnlyOnPhysicalIdentity();

    std::cout << "Orbit M26 persistent GPU terrain cache tests passed.\n";
    return EXIT_SUCCESS;
}
