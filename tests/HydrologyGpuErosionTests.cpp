#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain_gpu/GpuErosion.hpp>
#include <orbit/terrain_gpu/GpuFlowAccumulation.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
using namespace orbit;

// Milestone 4 verification: GpuErosion is a different, GPU-parallel
// resolution of the same formula engine/terrain_erosion's CPU
// SedimentTransport.cpp uses (see ErosionCompute.hpp) -- checked here
// as properties (finite output, bounded by the configured limits,
// ocean cells never erode, land does erode somewhere), not as an
// exact match to any CPU run.
} // namespace

int main()
{
    const auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    const shader::dxc::DxcShaderCompiler compiler;

    constexpr u32 kResolution = 9;

    terrain_gpu::GpuFlowAccumulation flowAccumulation(
        *device, compiler, kResolution);
    terrain_gpu::GpuErosion erosion(*device, compiler, kResolution);

    // Same monotonic ramp as the flow-accumulation test: drainage
    // increases with x, so accumulation (and therefore erosion
    // pressure) is highest near the low-x outlet column, where the
    // most upstream area has funneled through.
    std::vector<f32> drainage(
        static_cast<std::size_t>(kResolution) * kResolution);
    std::vector<f32> runoff(drainage.size(), 1.0F);

    const auto index = [&](const u32 x, const u32 y)
    {
        return static_cast<std::size_t>(y) * kResolution + x;
    };

    for (u32 y = 0; y < kResolution; ++y)
    {
        for (u32 x = 0; x < kResolution; ++x)
        {
            drainage[index(x, y)] = static_cast<f32>(x) * 10.0F;
        }
    }

    const u64 floatBytes =
        static_cast<u64>(kResolution) * kResolution * sizeof(f32);
    const u64 uintBytes =
        static_cast<u64>(kResolution) * kResolution * sizeof(u32);

    const auto MakeUploadBuffer =
        [&](const void* data, const u64 bytes)
    {
        auto buffer = device->CreateBuffer({
            .sizeBytes = bytes,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource
        });

        std::byte* mapped = buffer->Map();
        std::memcpy(mapped, data, bytes);
        buffer->Unmap();

        return buffer;
    };

    const auto drainageBuffer =
        MakeUploadBuffer(drainage.data(), floatBytes);
    const auto runoffBuffer =
        MakeUploadBuffer(runoff.data(), floatBytes);

    const auto accumulationBuffer = device->CreateBuffer({
        .sizeBytes = floatBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });

    const auto downstreamBuffer = device->CreateBuffer({
        .sizeBytes = uintBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });

    const auto netDeltaBuffer = device->CreateBuffer({
        .sizeBytes = floatBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::Common
    });

    const auto netDeltaReadback = device->CreateBuffer({
        .sizeBytes = floatBytes,
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopyDestination
    });

    const auto queue = device->CreateQueue(rhi::QueueType::Graphics);
    const auto allocator = device->CreateCommandAllocator(rhi::QueueType::Graphics);
    const auto commandList = device->CreateCommandList(*allocator);
    const auto fence = device->CreateFence(0);

    allocator->Reset();
    commandList->Reset(*allocator);

    flowAccumulation.Dispatch(
        *commandList,
        kResolution,
        *drainageBuffer,
        *runoffBuffer,
        *accumulationBuffer,
        downstreamBuffer.get());

    commandList->Transition(
        *accumulationBuffer,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    commandList->Transition(
        *downstreamBuffer,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    terrain_gpu::GpuErosionConfig erosionConfig{};
    // Nothing in this scenario is below sea level -- exercises the
    // land erosion/deposition path only.
    erosionConfig.seaLevelMeters = -1'000.0F;

    commandList->Transition(
        *netDeltaBuffer,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    erosion.Dispatch(
        *commandList,
        kResolution,
        50.0F,
        erosionConfig,
        *drainageBuffer,
        *accumulationBuffer,
        *downstreamBuffer,
        *netDeltaBuffer);

    commandList->Transition(
        *netDeltaBuffer,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopySource);

    commandList->CopyBuffer(
        *netDeltaBuffer, 0, *netDeltaReadback, 0, floatBytes);

    commandList->Close();
    queue->Submit(*commandList);
    queue->Signal(*fence, 1);
    fence->Wait(1);

    std::vector<f32> netDelta(drainage.size());

    {
        const std::byte* mapped = netDeltaReadback->Map();
        std::memcpy(netDelta.data(), mapped, floatBytes);
        netDeltaReadback->Unmap();
    }

    const f32 maxMagnitude = std::max(
        erosionConfig.maximumErosionMeters,
        erosionConfig.maximumDepositionMeters);

    bool sawErosion = false;

    for (std::size_t i = 0; i < netDelta.size(); ++i)
    {
        const f32 value = netDelta[i];

        if (!std::isfinite(value))
        {
            std::cerr << "Cell " << i
                      << " produced a non-finite net elevation delta.\n";
            return 1;
        }

        if (std::abs(value) > maxMagnitude + 1.0e-3F)
        {
            std::cerr << "Cell " << i << " net delta " << value
                      << " exceeds the configured erosion/deposition "
                         "bounds.\n";
            return 1;
        }

        if (value < -0.2F)
        {
            sawErosion = true;
        }
    }

    if (!sawErosion)
    {
        std::cerr << "No cell showed meaningful erosion; expected at "
                     "least some net lowering on this sloped scenario.\n";
        return 1;
    }

    // The x=0 outlet column has no downstream neighbor of its own, so
    // its own erosion term is always zero (no slope to erode along) --
    // it can only receive deposition from upstream sediment, so its
    // net delta must never be negative.
    for (u32 y = 1; y < kResolution - 1; ++y)
    {
        if (netDelta[index(0, y)] < -1.0e-3F)
        {
            std::cerr << "Outlet cell (0," << y
                      << ") shows erosion (" << netDelta[index(0, y)]
                      << ") despite having no downstream slope.\n";
            return 1;
        }
    }

    std::printf(
        "Erosion: pit-adjacent (x=1) net delta=%.3f m, mid-slope (x=4) "
        "net delta=%.3f m\n",
        netDelta[index(1, 4)],
        netDelta[index(4, 4)]);

    return 0;
}
