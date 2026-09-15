#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>

#include <orbit/core/Log.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionGpu.hpp>

#include <atomic>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

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

struct GpuPendingRequest
{
    DerivedTerrainRegionId id;
    std::shared_ptr<CacheEntry> entry;
};

// One fixed-size working set the GPU path dispatches a single tile's
// worth of hydrology into, reads back once its fence retires, then
// reuses for the next queued tile -- see Flush(). Buffers are sized
// once, for config_.region.hydrology.resolution, at construction.
struct GpuBuildSlot
{
    bool active{false};
    u64 targetFenceValue{0};

    DerivedTerrainRegionId id{};
    std::shared_ptr<CacheEntry> entry;
    world::SurfaceFrame surfaceFrame{};
    f64 approximateTileWidthMeters{0.0};
    f64 halfExtentMeters{0.0};
    f64 spacingMeters{0.0};

    std::unique_ptr<rhi::Buffer> rawGpu;
    std::unique_ptr<rhi::Buffer> drainageGpu;
    std::unique_ptr<rhi::Buffer> accumulationGpu;
    std::unique_ptr<rhi::Buffer> downstreamGpu;
    std::unique_ptr<rhi::Buffer> netDeltaGpu;

    std::unique_ptr<rhi::Buffer> rawReadback;
    std::unique_ptr<rhi::Buffer> drainageReadback;
    std::unique_ptr<rhi::Buffer> accumulationReadback;
    std::unique_ptr<rhi::Buffer> downstreamReadback;
    std::unique_ptr<rhi::Buffer> netDeltaReadback;
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

        readySnapshot_.store(
            std::make_shared<
                const ReadyRegionList>(),
            std::memory_order_release);

        if ((config_.gpuHydrology != nullptr) !=
            (config_.gpuFence != nullptr))
        {
            throw std::invalid_argument(
                "Orbit derived terrain region cache requires both "
                "gpuDevice/gpuHydrology/gpuFence set together, or all "
                "left null for the CPU path.");
        }

        if (config_.gpuHydrology != nullptr)
        {
            if (config_.gpuDevice == nullptr)
            {
                throw std::invalid_argument(
                    "Orbit derived terrain region cache's GPU path "
                    "requires a device.");
            }

            const u32 resolution = config_.region.hydrology.resolution;
            const u64 floatBytes =
                static_cast<u64>(resolution) * resolution * sizeof(f32);
            const u64 uintBytes =
                static_cast<u64>(resolution) * resolution * sizeof(u32);

            const auto makeGpu = [&](const u64 bytes)
            {
                return config_.gpuDevice->CreateBuffer({
                    .sizeBytes = bytes,
                    .usage = rhi::BufferUsage::Structured,
                    .memory = rhi::MemoryUsage::GpuOnly,
                    .initialState = rhi::ResourceState::CopyDestination
                });
            };

            const auto makeReadback = [&](const u64 bytes)
            {
                return config_.gpuDevice->CreateBuffer({
                    .sizeBytes = bytes,
                    .usage = rhi::BufferUsage::Generic,
                    .memory = rhi::MemoryUsage::HostVisible,
                    .initialState = rhi::ResourceState::CopyDestination
                });
            };

            gpuSlot_.rawGpu = makeGpu(floatBytes);
            gpuSlot_.drainageGpu = makeGpu(floatBytes);
            gpuSlot_.accumulationGpu = makeGpu(floatBytes);
            gpuSlot_.downstreamGpu = makeGpu(uintBytes);
            gpuSlot_.netDeltaGpu = makeGpu(floatBytes);

            gpuSlot_.rawReadback = makeReadback(floatBytes);
            gpuSlot_.drainageReadback = makeReadback(floatBytes);
            gpuSlot_.accumulationReadback = makeReadback(floatBytes);
            gpuSlot_.downstreamReadback = makeReadback(uintBytes);
            gpuSlot_.netDeltaReadback = makeReadback(floatBytes);
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

        if (config_.gpuHydrology != nullptr)
        {
            std::scoped_lock lock(entriesMutex_);
            pendingGpuRequests_.push_back({.id = id, .entry = entry});
            return true;
        }

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

                        {
                            std::scoped_lock entriesLock(
                                entriesMutex_);

                            PublishCountsLocked();
                            PublishReadySnapshotLocked();
                        }

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

                        {
                            std::scoped_lock entriesLock(
                                entriesMutex_);

                            PublishCountsLocked();
                        }

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

    [[nodiscard]] std::shared_ptr<
        const ReadyRegionList>
    ReadyRegionsSnapshot() const noexcept
    {
        return readySnapshot_.load(
            std::memory_order_acquire);
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

    void Flush(
        rhi::CommandList& commandList,
        const u64 submittedFenceValue)
    {
        if (config_.gpuHydrology == nullptr)
        {
            return;
        }

        // 1. Promote a retired in-flight dispatch: read it back and
        // hand the rest (CPU-only graph extraction) to a background
        // job, exactly like the CPU path's own job does today.
        if (gpuSlot_.active &&
            gpuSlot_.targetFenceValue <=
                config_.gpuFence->CompletedValue())
        {
            PromoteRetiredSlot();
        }

        // 2. Dispatch the next queued tile, if the slot is free.
        if (!gpuSlot_.active)
        {
            GpuPendingRequest request;

            {
                std::scoped_lock lock(entriesMutex_);

                if (pendingGpuRequests_.empty())
                {
                    return;
                }

                request = std::move(pendingGpuRequests_.front());
                pendingGpuRequests_.pop_front();
            }

            // The entry may have been evicted (capacity pressure)
            // since it was queued -- nothing to build for it anymore.
            {
                std::scoped_lock entryLock(request.entry->mutex);

                if (request.entry->state == EntryState::Failed)
                {
                    return;
                }
            }

            DispatchSlot(
                commandList, submittedFenceValue, std::move(request));
        }
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
            PublishReadySnapshotLocked();

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

    void PublishReadySnapshotLocked()
    {
        auto snapshot =
            std::make_shared<ReadyRegionList>();

        snapshot->reserve(
            entries_.size());

        for (const auto& [id, entry] :
             entries_)
        {
            static_cast<void>(id);

            std::scoped_lock entryLock(
                entry->mutex);

            if (entry->state ==
                    EntryState::Ready &&
                entry->region)
            {
                snapshot->push_back(
                    entry->region);
            }
        }

        readyEntries_.store(
            snapshot->size(),
            std::memory_order_release);

        readySnapshot_.store(
            std::move(snapshot),
            std::memory_order_release);
    }

    void DispatchSlot(
        rhi::CommandList& commandList,
        const u64 submittedFenceValue,
        GpuPendingRequest&& request)
    {
        const math::Double3 centerDirection =
            world::CubeToUnitDirection(
                world::TileCenter(request.id.tile));

        const world::SurfaceFrame surfaceFrame =
            world::MakeSurfaceFrame(centerDirection);

        const f64 approximateTileWidthMeters =
            world::ApproximateTileWidthMeters(planet_, request.id.tile);

        const f64 halfExtentMeters =
            approximateTileWidthMeters *
            config_.region.overlapScale * 0.5;

        const u32 resolution = config_.region.hydrology.resolution;

        const f64 spacingMeters =
            halfExtentMeters * 2.0 /
            static_cast<f64>(resolution - 1U);

        const f64 footprintMeters =
            config_.region.hydrology.footprintMeters > 0.0
                ? config_.region.hydrology.footprintMeters
                : spacingMeters;

        gpuSlot_.id = request.id;
        gpuSlot_.entry = std::move(request.entry);
        gpuSlot_.surfaceFrame = surfaceFrame;
        gpuSlot_.approximateTileWidthMeters = approximateTileWidthMeters;
        gpuSlot_.halfExtentMeters = halfExtentMeters;
        gpuSlot_.spacingMeters = spacingMeters;

        terrain_gpu::GpuHydrologyRegionRequest gpuRequest{};
        gpuRequest.resolution = resolution;
        gpuRequest.spacingMeters = spacingMeters;
        gpuRequest.footprintMeters = footprintMeters;
        gpuRequest.surfaceFrame = surfaceFrame;
        // Sea level isn't part of DerivedTerrainRegionConfig today --
        // 0.0 matches AnalyticTerrainDesc's own default.
        gpuRequest.seaLevelMeters = 0.0F;
        gpuRequest.minimumDropMeters = static_cast<f32>(
            config_.region.hydrology.minimumDrainageDropMeters);

        config_.gpuHydrology->Dispatch(
            commandList,
            gpuRequest,
            *gpuSlot_.rawGpu,
            *gpuSlot_.drainageGpu,
            *gpuSlot_.accumulationGpu,
            *gpuSlot_.downstreamGpu,
            *gpuSlot_.netDeltaGpu);

        const u64 floatBytes =
            static_cast<u64>(resolution) * resolution * sizeof(f32);
        const u64 uintBytes =
            static_cast<u64>(resolution) * resolution * sizeof(u32);

        const auto copyOut =
            [&](rhi::Buffer& source, rhi::Buffer& readback,
                const u64 bytes)
        {
            commandList.Transition(
                source,
                rhi::ResourceState::CopyDestination,
                rhi::ResourceState::CopySource);

            commandList.CopyBuffer(source, 0, readback, 0, bytes);
        };

        copyOut(*gpuSlot_.rawGpu, *gpuSlot_.rawReadback, floatBytes);
        copyOut(
            *gpuSlot_.drainageGpu, *gpuSlot_.drainageReadback,
            floatBytes);
        copyOut(
            *gpuSlot_.accumulationGpu, *gpuSlot_.accumulationReadback,
            floatBytes);
        copyOut(
            *gpuSlot_.downstreamGpu, *gpuSlot_.downstreamReadback,
            uintBytes);
        copyOut(
            *gpuSlot_.netDeltaGpu, *gpuSlot_.netDeltaReadback,
            floatBytes);

        // Restore every GPU-only buffer back to CopyDestination (its
        // resting state) so the next DispatchSlot call can reuse them
        // the same way GpuHydrologyRegion::Dispatch itself expects.
        const auto restore = [&](rhi::Buffer& buffer)
        {
            commandList.Transition(
                buffer,
                rhi::ResourceState::CopySource,
                rhi::ResourceState::CopyDestination);
        };

        restore(*gpuSlot_.rawGpu);
        restore(*gpuSlot_.drainageGpu);
        restore(*gpuSlot_.accumulationGpu);
        restore(*gpuSlot_.downstreamGpu);
        restore(*gpuSlot_.netDeltaGpu);

        gpuSlot_.active = true;
        gpuSlot_.targetFenceValue = submittedFenceValue;
    }

    void PromoteRetiredSlot()
    {
        const u32 resolution = config_.region.hydrology.resolution;
        const std::size_t cellCount =
            static_cast<std::size_t>(resolution) * resolution;

        auto raw = std::make_shared<std::vector<f32>>(cellCount);
        auto drainage = std::make_shared<std::vector<f32>>(cellCount);
        auto accumulation =
            std::make_shared<std::vector<f32>>(cellCount);
        auto downstream = std::make_shared<std::vector<u32>>(cellCount);
        auto netDelta = std::make_shared<std::vector<f32>>(cellCount);

        const auto readInto = [](rhi::Buffer& buffer, void* dest,
                                  const std::size_t bytes)
        {
            const std::byte* mapped = buffer.Map();
            std::memcpy(dest, mapped, bytes);
            buffer.Unmap();
        };

        readInto(
            *gpuSlot_.rawReadback, raw->data(), cellCount * sizeof(f32));
        readInto(
            *gpuSlot_.drainageReadback, drainage->data(),
            cellCount * sizeof(f32));
        readInto(
            *gpuSlot_.accumulationReadback, accumulation->data(),
            cellCount * sizeof(f32));
        readInto(
            *gpuSlot_.downstreamReadback, downstream->data(),
            cellCount * sizeof(u32));
        readInto(
            *gpuSlot_.netDeltaReadback, netDelta->data(),
            cellCount * sizeof(f32));

        const DerivedTerrainRegionId id = gpuSlot_.id;
        const auto entry = gpuSlot_.entry;
        const world::SurfaceFrame surfaceFrame = gpuSlot_.surfaceFrame;
        const f64 approximateTileWidthMeters =
            gpuSlot_.approximateTileWidthMeters;
        const f64 halfExtentMeters = gpuSlot_.halfExtentMeters;
        const f64 spacingMeters = gpuSlot_.spacingMeters;
        const DerivedTerrainRegionConfig regionConfig = config_.region;

        gpuSlot_.active = false;
        gpuSlot_.entry.reset();

        try
        {
            jobs_.Submit(
                group_,
                jobs::JobPriority::Low,
                [
                    this,
                    id,
                    entry,
                    surfaceFrame,
                    approximateTileWidthMeters,
                    halfExtentMeters,
                    spacingMeters,
                    regionConfig,
                    resolution,
                    raw,
                    drainage,
                    accumulation,
                    downstream,
                    netDelta
                ]
                {
                    try
                    {
                        GpuHydrologyReadback readback{};
                        readback.resolution = resolution;
                        readback.spacingMeters = spacingMeters;
                        readback.seaLevelMeters = 0.0F;
                        readback.surfaceFrame = surfaceFrame;
                        readback.rawElevationMeters = *raw;
                        readback.drainageElevationMeters = *drainage;
                        readback.accumulation = *accumulation;
                        readback.downstream = *downstream;
                        readback.netElevationDeltaMeters = *netDelta;

                        auto region =
                            std::make_shared<DerivedTerrainRegion>(
                                BuildDerivedTerrainRegionFromGpuReadback(
                                    id,
                                    approximateTileWidthMeters,
                                    halfExtentMeters,
                                    readback,
                                    regionConfig));

                        {
                            std::scoped_lock lock(entry->mutex);

                            entry->region = std::move(region);
                            entry->state = EntryState::Ready;
                        }

                        {
                            std::scoped_lock entriesLock(entriesMutex_);

                            PublishCountsLocked();
                            PublishReadySnapshotLocked();
                        }

                        contentRevision_.fetch_add(
                            1, std::memory_order_acq_rel);
                    }
                    catch (...)
                    {
                        {
                            std::scoped_lock lock(entry->mutex);

                            entry->exception =
                                std::current_exception();

                            entry->state = EntryState::Failed;
                        }

                        {
                            std::scoped_lock entriesLock(entriesMutex_);

                            PublishCountsLocked();
                        }

                        failedBuilds_.fetch_add(
                            1, std::memory_order_relaxed);

                        throw;
                    }
                });
        }
        catch (...)
        {
            {
                std::scoped_lock lock(entry->mutex);

                entry->exception = std::current_exception();
                entry->state = EntryState::Failed;
            }

            failedBuilds_.fetch_add(1, std::memory_order_relaxed);
        }
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
    std::deque<GpuPendingRequest> pendingGpuRequests_;
    GpuBuildSlot gpuSlot_;

    mutable std::atomic<u64>
        accessTicket_{0};

    std::atomic<std::size_t>
        entryCount_{0};

    std::atomic<std::size_t>
        readyEntries_{0};

    std::atomic<
        std::shared_ptr<
            const ReadyRegionList>>
        readySnapshot_{};

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

std::shared_ptr<
    const DerivedTerrainRegionCache::
        ReadyRegionList>
DerivedTerrainRegionCache::ReadyRegionsSnapshot()
    const noexcept
{
    return impl_->ReadyRegionsSnapshot();
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

void DerivedTerrainRegionCache::Flush(
    rhi::CommandList& commandList,
    const u64 submittedFenceValue)
{
    impl_->Flush(commandList, submittedFenceValue);
}
} // namespace orbit::terrain_region
