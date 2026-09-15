#include <orbit/terrain_gpu/GpuElevationQuery.hpp>

#include <orbit/terrain_gpu/GpuDepressionFill.hpp>
#include <orbit/terrain_gpu/GpuErosion.hpp>
#include <orbit/terrain_gpu/GpuFlowAccumulation.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

namespace orbit::terrain_gpu
{
namespace
{
// A small local neighborhood run through the full GPU hydrology stack
// (field generation -> depression fill -> flow accumulation -> erosion)
// per query -- enough for the center cell's slope/accumulation to be
// meaningful, cheap enough to run per in-flight slot every time it's
// reused (17x17 = 289 cells, a handful of ping-pong passes each).
constexpr u32 kNeighborhoodResolution = 17;
constexpr u32 kCenterX = kNeighborhoodResolution / 2;
constexpr u32 kCenterY = kNeighborhoodResolution / 2;
constexpr u32 kCenterIndex = kCenterY * kNeighborhoodResolution + kCenterX;
constexpr f64 kNeighborhoodSpacingMeters = 10.0;

constexpr u64 kSampleStrideBytes =
    sizeof(terrain_stream::TerrainSampleValue);

// A cached sample is only trusted as a stand-in for a *different*
// nearby direction within this central angle -- beyond it, the coarse
// analytic estimate is a safer guess than a stale, unrelated location.
// ~50 km on a planet of a few thousand km radius.
constexpr f64 kMaxUsefulAngleRadians = 0.01;

// A cell counts as "lake" (filled enough that collision should rest on
// the water surface, not the raw lake-bed elevation) once conditioning
// raised it more than this above its raw elevation.
constexpr f32 kLakeFillThresholdMeters = 2.0F;

// Extracts just the elevation float (offset 0 of each 32-byte
// TerrainSampleValue record) into a tightly-packed f32 grid --
// GpuDepressionFill/GpuFlowAccumulation/GpuErosion all expect a plain
// stride-4 buffer, not GpuFieldGenerator's richer per-sample record.
constexpr const char* kExtractElevationComputeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_samples : register(t0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_elevationOut : register(u1);

struct PushConstants
{
    uint resolution;
};

[[vk::push_constant]] PushConstants g_pc;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.resolution || dispatchId.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = dispatchId.y * g_pc.resolution + dispatchId.x;
    const float elevation = asfloat(g_samples.Load(index * 32u));
    g_elevationOut.Store(index * 4u, asuint(elevation));
}
)";
} // namespace

class GpuElevationQuery::Impl
{
public:
    Impl(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        const GpuFieldGenerator& generator,
        const terrain::GlobalTerrainFields& globalFields,
        rhi::Fence& fence,
        const u32 maxInFlight)
        : device_(device),
          generator_(generator),
          globalFields_(globalFields),
          fence_(fence),
          depressionFill_(device, shaderCompiler, kNeighborhoodResolution),
          flowAccumulation_(device, shaderCompiler, kNeighborhoodResolution),
          erosion_(device, shaderCompiler, kNeighborhoodResolution)
    {
        const shader::Binary extractCompute = shaderCompiler.Compile({
            .source = kExtractElevationComputeShader,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

        if (extractCompute.bytecode.empty())
        {
            throw std::runtime_error(
                "Orbit failed to compile the elevation-extraction "
                "compute shader.");
        }

        extractPipeline_ = device_.CreateComputePipeline({
            .computeShader = {
                .data = extractCompute.bytecode.data(),
                .size = extractCompute.bytecode.size()
            },
            .pushConstantDwords = 1,
            .shaderResourceBuffers = 2
        });

        constexpr u64 sampleBufferBytes =
            static_cast<u64>(kNeighborhoodResolution) *
            kNeighborhoodResolution * kSampleStrideBytes;

        constexpr u64 floatGridBytes =
            static_cast<u64>(kNeighborhoodResolution) *
            kNeighborhoodResolution * sizeof(f32);

        constexpr u64 uintGridBytes =
            static_cast<u64>(kNeighborhoodResolution) *
            kNeighborhoodResolution * sizeof(u32);

        // Runoff is always uniform (1.0) for this local, single-purpose
        // query -- one shared, permanently-ShaderResource buffer for
        // every slot's flow accumulation, uploaded once here.
        runoff_ = device_.CreateBuffer({
            .sizeBytes = floatGridBytes,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource
        });

        {
            std::vector<f32> ones(
                static_cast<std::size_t>(kNeighborhoodResolution) *
                    kNeighborhoodResolution,
                1.0F);

            std::byte* mapped = runoff_->Map();
            std::memcpy(
                mapped, ones.data(), ones.size() * sizeof(f32));
            runoff_->Unmap();
        }

        slots_.reserve(maxInFlight);

        for (u32 i = 0; i < maxInFlight; ++i)
        {
            Slot slot;

            slot.sampleGrid = device_.CreateBuffer({
                .sizeBytes = sampleBufferBytes,
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::GpuOnly,
                .initialState = rhi::ResourceState::Common
            });

            slot.rawGrid = device_.CreateBuffer({
                .sizeBytes = floatGridBytes,
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::GpuOnly,
                .initialState = rhi::ResourceState::Common
            });

            slot.drainageGrid = device_.CreateBuffer({
                .sizeBytes = floatGridBytes,
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::GpuOnly,
                .initialState = rhi::ResourceState::Common
            });

            slot.accumulationGrid = device_.CreateBuffer({
                .sizeBytes = floatGridBytes,
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::GpuOnly,
                .initialState = rhi::ResourceState::Common
            });

            slot.downstreamGrid = device_.CreateBuffer({
                .sizeBytes = uintGridBytes,
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::GpuOnly,
                .initialState = rhi::ResourceState::Common
            });

            slot.netDeltaGrid = device_.CreateBuffer({
                .sizeBytes = floatGridBytes,
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::GpuOnly,
                .initialState = rhi::ResourceState::Common
            });

            // raw[center], drainage[center], netDelta[center].
            slot.readbackBuffer = device_.CreateBuffer({
                .sizeBytes = 12,
                .usage = rhi::BufferUsage::Generic,
                .memory = rhi::MemoryUsage::HostVisible,
                .initialState = rhi::ResourceState::CopyDestination
            });

            slots_.push_back(std::move(slot));
        }
    }

    [[nodiscard]] ElevationQueryHandle Request(
        const math::Double3& unitDirection)
    {
        const u64 handleId = ++nextHandleId_;
        const u64 completed = fence_.CompletedValue();

        // Rank 0 = Free (always safe to reuse), 1 = Pending (queued
        // but no GPU work recorded yet, also always safe), 2 =
        // InFlight with its dispatch already retired (safe -- its
        // buffers are no longer referenced by any in-flight command
        // list), 3 = InFlight and still in flight (last resort only;
        // reusing it races the still-pending dispatch/copy against the
        // new one). Ties broken by least-recently-touched.
        const auto rank = [&](const Slot& s) -> int
        {
            switch (s.state)
            {
            case SlotState::Free:
                return 0;
            case SlotState::Pending:
                return 1;
            case SlotState::InFlight:
                return s.targetFenceValue <= completed ? 2 : 3;
            }
            return 3;
        };

        std::size_t bestSlot = 0;
        int bestRank = rank(slots_[0]);

        for (std::size_t i = 1; i < slots_.size(); ++i)
        {
            const int r = rank(slots_[i]);

            if (r < bestRank ||
                (r == bestRank &&
                 slots_[i].lastTouchedOrder <
                     slots_[bestSlot].lastTouchedOrder))
            {
                bestSlot = i;
                bestRank = r;
            }
        }

        Slot& slot = slots_[bestSlot];
        slot.state = SlotState::Pending;
        slot.direction = math::Normalize(unitDirection);
        slot.handleId = handleId;
        slot.targetFenceValue = 0;
        slot.lastTouchedOrder = ++touchCounter_;
        slot.hasResult = false;

        return {handleId};
    }

    void Flush(
        rhi::CommandList& commandList,
        const u64 submittedFenceValue)
    {
        const u64 completed = fence_.CompletedValue();

        for (Slot& slot : slots_)
        {
            if (slot.state == SlotState::InFlight &&
                slot.targetFenceValue <= completed)
            {
                std::byte* mapped = slot.readbackBuffer->Map();

                f32 raw = 0.0F;
                f32 drainage = 0.0F;
                f32 netDelta = 0.0F;
                std::memcpy(&raw, mapped, sizeof(f32));
                std::memcpy(
                    &drainage,
                    mapped + sizeof(f32),
                    sizeof(f32));
                std::memcpy(
                    &netDelta,
                    mapped + 2 * sizeof(f32),
                    sizeof(f32));

                slot.readbackBuffer->Unmap();

                const bool isLake =
                    (drainage - raw) > kLakeFillThresholdMeters;

                const f64 elevationMeters = isLake
                    ? static_cast<f64>(drainage)
                    : static_cast<f64>(raw) + static_cast<f64>(netDelta);

                UpdateCache(slot.direction, elevationMeters);

                slot.lastResult = elevationMeters;
                slot.hasResult = true;

                slot.state = SlotState::Free;
            }
        }

        for (Slot& slot : slots_)
        {
            if (slot.state != SlotState::Pending)
            {
                continue;
            }

            DispatchNeighborhood(commandList, slot);

            slot.state = SlotState::InFlight;
            slot.targetFenceValue = submittedFenceValue;
        }
    }

    [[nodiscard]] bool TryGetResult(
        const ElevationQueryHandle handle,
        f64& outElevationMeters) const
    {
        if (!handle.IsValid())
        {
            return false;
        }

        // No separate result history -- once a slot's handle is
        // superseded by a later Request() reusing that slot, its
        // previous handle's result simply becomes unavailable. A
        // handle should be polled soon after Request(), not held
        // indefinitely.
        for (const Slot& slot : slots_)
        {
            if (slot.handleId == handle.id && slot.hasResult)
            {
                outElevationMeters = slot.lastResult;
                return true;
            }
        }

        return false;
    }

    [[nodiscard]] f64 LastKnownGood(
        const math::Double3& unitDirection) const
    {
        const math::Double3 direction =
            math::Normalize(unitDirection);

        std::optional<f64> bestElevation;
        f64 bestCosine = -1.0;

        for (const CacheSlot& entry : cache_)
        {
            if (!entry.assigned)
            {
                continue;
            }

            const f64 cosine = math::Dot(direction, entry.direction);

            if (cosine > bestCosine)
            {
                bestCosine = cosine;
                bestElevation = entry.elevationMeters;
            }
        }

        if (bestElevation.has_value() &&
            bestCosine >= std::cos(kMaxUsefulAngleRadians))
        {
            return *bestElevation;
        }

        return globalFields_.PlateElevationEstimateMeters(direction);
    }

private:
    enum class SlotState : u8
    {
        Free,
        Pending,
        InFlight
    };

    struct Slot
    {
        std::unique_ptr<rhi::Buffer> sampleGrid;
        std::unique_ptr<rhi::Buffer> rawGrid;
        std::unique_ptr<rhi::Buffer> drainageGrid;
        std::unique_ptr<rhi::Buffer> accumulationGrid;
        std::unique_ptr<rhi::Buffer> downstreamGrid;
        std::unique_ptr<rhi::Buffer> netDeltaGrid;
        std::unique_ptr<rhi::Buffer> readbackBuffer;

        SlotState state{SlotState::Free};
        math::Double3 direction{};
        u64 handleId{0};
        u64 targetFenceValue{0};
        u64 lastTouchedOrder{0};
        f64 lastResult{0.0};
        bool hasResult{false};
    };

    struct CacheSlot
    {
        math::Double3 direction{};
        f64 elevationMeters{0.0};
        bool assigned{false};
    };

    void DispatchNeighborhood(
        rhi::CommandList& commandList,
        Slot& slot) const
    {
        const world::SurfaceFrame frame =
            world::MakeSurfaceFrame(slot.direction);

        GpuFieldRequest fieldRequest{};
        fieldRequest.resolution = kNeighborhoodResolution;
        fieldRequest.spacingMeters = kNeighborhoodSpacingMeters;
        fieldRequest.footprintMeters = kNeighborhoodSpacingMeters;
        fieldRequest.morphToCoarser = false;
        fieldRequest.surfaceFrame = frame;
        fieldRequest.coarseSurfaceFrame = frame;
        fieldRequest.originX = 0;
        fieldRequest.originY = 0;
        fieldRequest.region = {
            .x = 0,
            .y = 0,
            .width = kNeighborhoodResolution,
            .height = kNeighborhoodResolution
        };

        commandList.Transition(
            *slot.sampleGrid,
            rhi::ResourceState::Common,
            rhi::ResourceState::UnorderedAccess);

        generator_.Dispatch(commandList, fieldRequest, *slot.sampleGrid);

        commandList.UavBarrier(*slot.sampleGrid);

        commandList.Transition(
            *slot.sampleGrid,
            rhi::ResourceState::UnorderedAccess,
            rhi::ResourceState::ShaderResource);

        // Extract the plain elevation grid the relaxation passes need.
        commandList.Transition(
            *slot.rawGrid,
            rhi::ResourceState::Common,
            rhi::ResourceState::UnorderedAccess);

        {
            const std::array<u32, 1> extractPushConstants{
                kNeighborhoodResolution};

            constexpr u32 kThreadGroupSize = 8;
            constexpr u32 groupCount =
                (kNeighborhoodResolution + kThreadGroupSize - 1) /
                kThreadGroupSize;

            commandList.SetComputePipeline(*extractPipeline_);
            commandList.SetComputeBuffer(0, *slot.sampleGrid);
            commandList.SetComputeBuffer(1, *slot.rawGrid);
            commandList.SetComputeConstants(extractPushConstants);
            commandList.Dispatch(groupCount, groupCount, 1);
            commandList.UavBarrier(*slot.rawGrid);
        }

        commandList.Transition(
            *slot.sampleGrid,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::Common);

        commandList.Transition(
            *slot.rawGrid,
            rhi::ResourceState::UnorderedAccess,
            rhi::ResourceState::ShaderResource);

        commandList.Transition(
            *slot.drainageGrid,
            rhi::ResourceState::Common,
            rhi::ResourceState::CopyDestination);

        depressionFill_.Dispatch(
            commandList,
            kNeighborhoodResolution,
            // A shallow drop is enough at this footprint -- this
            // feeds a collision estimate, not clipmap geometry.
            0.05F,
            // Nothing here is meant to represent an ocean; only real
            // local depressions (lakes) should seed as outlets besides
            // the neighborhood's own border.
            -1.0e6F,
            *slot.rawGrid,
            *slot.drainageGrid);

        commandList.Transition(
            *slot.drainageGrid,
            rhi::ResourceState::CopyDestination,
            rhi::ResourceState::ShaderResource);

        commandList.Transition(
            *slot.accumulationGrid,
            rhi::ResourceState::Common,
            rhi::ResourceState::CopyDestination);

        commandList.Transition(
            *slot.downstreamGrid,
            rhi::ResourceState::Common,
            rhi::ResourceState::CopyDestination);

        flowAccumulation_.Dispatch(
            commandList,
            kNeighborhoodResolution,
            *slot.drainageGrid,
            *runoff_,
            *slot.accumulationGrid,
            slot.downstreamGrid.get());

        commandList.Transition(
            *slot.accumulationGrid,
            rhi::ResourceState::CopyDestination,
            rhi::ResourceState::ShaderResource);

        commandList.Transition(
            *slot.downstreamGrid,
            rhi::ResourceState::CopyDestination,
            rhi::ResourceState::ShaderResource);

        GpuErosionConfig erosionConfig{};
        erosionConfig.seaLevelMeters = -1.0e6F;

        commandList.Transition(
            *slot.netDeltaGrid,
            rhi::ResourceState::Common,
            rhi::ResourceState::UnorderedAccess);

        erosion_.Dispatch(
            commandList,
            kNeighborhoodResolution,
            static_cast<f32>(kNeighborhoodSpacingMeters),
            erosionConfig,
            *slot.drainageGrid,
            *slot.accumulationGrid,
            *slot.downstreamGrid,
            *slot.netDeltaGrid);

        commandList.Transition(
            *slot.netDeltaGrid,
            rhi::ResourceState::UnorderedAccess,
            rhi::ResourceState::CopySource);

        commandList.Transition(
            *slot.rawGrid,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::CopySource);

        commandList.Transition(
            *slot.drainageGrid,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::CopySource);

        constexpr u64 centerByteOffset =
            static_cast<u64>(kCenterIndex) * sizeof(f32);

        commandList.CopyBuffer(
            *slot.rawGrid, centerByteOffset,
            *slot.readbackBuffer, 0,
            sizeof(f32));

        commandList.CopyBuffer(
            *slot.drainageGrid, centerByteOffset,
            *slot.readbackBuffer, sizeof(f32),
            sizeof(f32));

        commandList.CopyBuffer(
            *slot.netDeltaGrid, centerByteOffset,
            *slot.readbackBuffer, 2 * sizeof(f32),
            sizeof(f32));

        commandList.Transition(
            *slot.rawGrid,
            rhi::ResourceState::CopySource,
            rhi::ResourceState::Common);

        commandList.Transition(
            *slot.drainageGrid,
            rhi::ResourceState::CopySource,
            rhi::ResourceState::Common);

        commandList.Transition(
            *slot.accumulationGrid,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::Common);

        commandList.Transition(
            *slot.downstreamGrid,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::Common);

        commandList.Transition(
            *slot.netDeltaGrid,
            rhi::ResourceState::CopySource,
            rhi::ResourceState::Common);
    }

    void UpdateCache(
        const math::Double3& direction,
        const f64 elevationMeters)
    {
        // One cache entry per in-flight slot is already enough to
        // keep LastKnownGood useful during continuous movement (each
        // new Request naturally supersedes the slot's previous entry
        // for roughly the same nearby direction) -- a fixed
        // round-robin ring, same shape as the request pool itself.
        cache_[cacheCursor_].direction = direction;
        cache_[cacheCursor_].elevationMeters = elevationMeters;
        cache_[cacheCursor_].assigned = true;
        cacheCursor_ = (cacheCursor_ + 1) % cache_.size();
    }

    rhi::Device& device_;
    const GpuFieldGenerator& generator_;
    const terrain::GlobalTerrainFields& globalFields_;
    rhi::Fence& fence_;

    GpuDepressionFill depressionFill_;
    GpuFlowAccumulation flowAccumulation_;
    GpuErosion erosion_;
    std::unique_ptr<rhi::ComputePipeline> extractPipeline_;
    std::unique_ptr<rhi::Buffer> runoff_;

    std::vector<Slot> slots_;
    u64 nextHandleId_{0};
    u64 touchCounter_{0};

    std::array<CacheSlot, 8> cache_{};
    std::size_t cacheCursor_{0};
};

GpuElevationQuery::GpuElevationQuery(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const GpuFieldGenerator& generator,
    const terrain::GlobalTerrainFields& globalFields,
    rhi::Fence& fence,
    const u32 maxInFlight)
    : impl_(std::make_unique<Impl>(
        device,
        shaderCompiler,
        generator,
        globalFields,
        fence,
        maxInFlight))
{
}

GpuElevationQuery::~GpuElevationQuery() = default;

ElevationQueryHandle GpuElevationQuery::Request(
    const math::Double3& unitDirection)
{
    return impl_->Request(unitDirection);
}

void GpuElevationQuery::Flush(
    rhi::CommandList& commandList,
    const u64 submittedFenceValue)
{
    impl_->Flush(commandList, submittedFenceValue);
}

bool GpuElevationQuery::TryGetResult(
    const ElevationQueryHandle handle,
    f64& outElevationMeters) const
{
    return impl_->TryGetResult(handle, outElevationMeters);
}

f64 GpuElevationQuery::LastKnownGood(
    const math::Double3& unitDirection) const
{
    return impl_->LastKnownGood(unitDirection);
}
} // namespace orbit::terrain_gpu
