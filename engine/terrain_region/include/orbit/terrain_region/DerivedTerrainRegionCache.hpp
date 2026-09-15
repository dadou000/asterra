#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Fence.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_gpu/GpuHydrologyRegion.hpp>
#include <orbit/terrain_region/DerivedTerrainRegion.hpp>
#include <orbit/world/Planet.hpp>

#include <cstddef>
#include <memory>
#include <vector>

namespace orbit::terrain_region
{
struct DerivedTerrainRegionCacheConfig
{
    u8 tileLevel{5};
    std::size_t maxEntries{12};
    DerivedTerrainRegionConfig region{};

    // Optional -- when both are set, region tiles are built on the
    // GPU (see DerivedTerrainRegionGpu.hpp and the GPU terrain
    // generation plan's Milestone 4) instead of entirely on a
    // background CPU job: Request() only queues the tile, and the
    // caller's own Flush() (called once per frame, same pattern as
    // terrain_gpu::GpuElevationQuery) records the actual GPU dispatch
    // and, once its fence value retires, hands the readback to a
    // lightweight background job that just runs the CPU-side graph
    // extraction (BuildRiverGraph and friends) the GPU passes don't
    // replace. Leave both null to keep the fully-CPU path (e.g. for
    // tests that have no GPU device available).
    rhi::Device* gpuDevice{nullptr};
    terrain_gpu::GpuHydrologyRegion* gpuHydrology{nullptr};
    rhi::Fence* gpuFence{nullptr};
};

struct DerivedTerrainRegionCacheStats
{
    u64 acceptedRequests{0};
    u64 duplicateRequests{0};
    u64 readyHits{0};
    u64 misses{0};
    u64 evictions{0};
    u64 capacityRejects{0};
    u64 failedBuilds{0};
    std::size_t entries{0};
    std::size_t readyEntries{0};
    std::size_t pendingEntries{0};
};

class DerivedTerrainRegionCache
{
public:
    DerivedTerrainRegionCache(
        world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        jobs::JobSystem& jobs,
        DerivedTerrainRegionCacheConfig config = {});

    ~DerivedTerrainRegionCache();

    DerivedTerrainRegionCache(
        const DerivedTerrainRegionCache&) = delete;

    DerivedTerrainRegionCache& operator=(
        const DerivedTerrainRegionCache&) = delete;

    [[nodiscard]] DerivedTerrainRegionId IdForDirection(
        const math::Double3& direction) const noexcept;

    [[nodiscard]] DerivedTerrainRegionId IdForTile(
        world::PlanetTileId tile) const noexcept;

    [[nodiscard]] bool Request(
        const DerivedTerrainRegionId& id);

    [[nodiscard]] bool RequestDirection(
        const math::Double3& direction);

    [[nodiscard]] std::shared_ptr<const DerivedTerrainRegion>
    TryGet(
        const DerivedTerrainRegionId& id) const;

    [[nodiscard]] bool IsPending(
        const DerivedTerrainRegionId& id) const;

    using ReadyRegionList =
        std::vector<
            std::shared_ptr<
                const DerivedTerrainRegion>>;

    [[nodiscard]] std::shared_ptr<
        const ReadyRegionList>
    ReadyRegionsSnapshot() const noexcept;

    [[nodiscard]] u8 TileLevel() const noexcept;
    [[nodiscard]] std::size_t EntryCount() const noexcept;

    // Changes when the set of ready derived regions changes.
    [[nodiscard]] u64 ContentRevision() const noexcept;

    [[nodiscard]] DerivedTerrainRegionCacheStats Stats() const noexcept;

    void WaitAll();

    // No-op unless constructed with gpuHydrology/gpuFence set. Call
    // once per frame, with the same command list about to be
    // submitted this frame and the fence value that submission will
    // signal -- see GpuElevationQuery::Flush's identical contract.
    void Flush(
        rhi::CommandList& commandList,
        u64 submittedFenceValue);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::terrain_region
