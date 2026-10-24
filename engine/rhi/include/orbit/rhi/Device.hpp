#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/AccelerationStructure.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Fence.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Query.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/rhi/Swapchain.hpp>

#include <future>
#include <memory>
#include <optional>
#include <string_view>

namespace orbit::rhi
{
enum class Backend : u8
{
    Vulkan
};

struct DeviceCapabilities
{
    // Kept for source compatibility with early Orbit callers. This mirrors
    // rayTracingPipeline, not generic "RT capability".
    bool rayTracing{false};

    bool accelerationStructures{false};
    bool rayQuery{false};
    bool rayTracingPipeline{false};

    bool meshShaders{false};
    bool variableRateShading{false};
    bool presentTearing{false};
    // The GPU timestamp clock can be read together with the CPU's performance
    // counter (see Device::CalibrateGpuClock).
    bool calibratedTimestamps{false};
    u32 shaderModelMajor{0};
    u32 shaderModelMinor{0};
};

struct PipelineCompilationStats
{
    u64 queued{0U};
    u64 compiling{0U};
    u64 completed{0U};
    u64 rejected{0U};
};

class Device
{
public:
    virtual ~Device() = default;

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] virtual Backend GetBackend() const noexcept = 0;
    [[nodiscard]] virtual std::string_view AdapterName() const noexcept = 0;
    [[nodiscard]] virtual const DeviceCapabilities& Capabilities() const noexcept = 0;

    [[nodiscard]] virtual std::unique_ptr<Queue> CreateQueue(QueueType type) = 0;
    [[nodiscard]] virtual std::unique_ptr<Fence> CreateFence(u64 initialValue) = 0;
    [[nodiscard]] virtual std::unique_ptr<CommandAllocator> CreateCommandAllocator(
        QueueType type) = 0;
    [[nodiscard]] virtual std::unique_ptr<CommandList> CreateCommandList(
        CommandAllocator& allocator) = 0;

    [[nodiscard]] virtual std::unique_ptr<Buffer> CreateBuffer(
        const BufferDesc& desc) = 0;

    [[nodiscard]] virtual std::unique_ptr<Texture> CreateTexture(
        const TextureDesc& desc) = 0;

    [[nodiscard]] virtual std::unique_ptr<AccelerationStructure>
    CreateAabbAccelerationStructure(
        std::span<const AccelerationAabb> aabbs) = 0;

    [[nodiscard]] virtual std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        const GraphicsPipelineDesc& desc) = 0;

    [[nodiscard]] virtual std::unique_ptr<ComputePipeline> CreateComputePipeline(
        const ComputePipelineDesc& desc) = 0;

    // Optional driver-side pipeline compilation that never waits on the
    // graphics-owning thread. The backend takes its own copies of bytecode
    // and attribute arrays before returning. Nullopt means its bounded
    // compile queue is full/unsupported; retry on a subsequent frame.
    // The future is packaged-task-owned, never std::async joined at teardown.
    [[nodiscard]] virtual std::optional<
        std::future<std::unique_ptr<GraphicsPipeline>>>
    TryCreateGraphicsPipelineAsync(const GraphicsPipelineDesc&)
    {
        return std::nullopt;
    }

    [[nodiscard]] virtual std::optional<
        std::future<std::unique_ptr<ComputePipeline>>>
    TryCreateComputePipelineAsync(const ComputePipelineDesc&)
    {
        return std::nullopt;
    }

    [[nodiscard]] virtual PipelineCompilationStats PipelineCompilation() noexcept
    {
        return {};
    }

    // GPU-completed Vulkan/VMA objects awaiting a bounded retirement pass.
    // Supported backends override this for profiler/validation integration.
    [[nodiscard]] virtual u64 PendingGpuRetirements() const noexcept
    {
        return 0U;
    }

    [[nodiscard]] virtual std::unique_ptr<Swapchain> CreateSwapchain(
        Queue& queue,
        const SwapchainDesc& desc) = 0;

    [[nodiscard]] virtual std::unique_ptr<TimestampQueryPool>
    CreateTimestampQueryPool(u32 count) = 0;

    // Nanoseconds per GPU timestamp tick, for converting
    // TimestampQueryPool::TryGetResults' raw ticks into real time.
    [[nodiscard]] virtual f64 TimestampPeriodNanoseconds()
        const noexcept = 0;

    // Reads the GPU timestamp clock and the CPU's QueryPerformanceCounter at the
    // same instant, so GPU timestamps can be placed on the CPU profiler's
    // timeline. Returns false when the backend cannot calibrate; GPU timings are
    // then only durations.
    [[nodiscard]] virtual bool CalibrateGpuClock(
        u64* gpuTicks,
        u64* cpuTicks) const noexcept
    {
        static_cast<void>(gpuTicks);
        static_cast<void>(cpuTicks);
        return false;
    }

protected:
    Device() = default;
};
} // namespace orbit::rhi
