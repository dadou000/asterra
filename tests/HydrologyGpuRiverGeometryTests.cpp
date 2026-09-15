#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain_gpu/GpuFlowAccumulation.hpp>
#include <orbit/terrain_gpu/GpuRiverGeometry.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
using namespace orbit;
} // namespace

int main()
{
    const auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    const shader::dxc::DxcShaderCompiler compiler;

    constexpr u32 kResolution = 9;

    terrain_gpu::GpuFlowAccumulation flowAccumulation(
        *device, compiler, kResolution);
    terrain_gpu::GpuRiverGeometry riverGeometry(
        *device, compiler, kResolution);

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
    const u64 outputBytes =
        static_cast<u64>(kResolution) * kResolution * 16;

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

    const auto outputBuffer = device->CreateBuffer({
        .sizeBytes = outputBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });

    const auto outputReadback = device->CreateBuffer({
        .sizeBytes = outputBytes,
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
        *accumulationBuffer);

    commandList->Transition(
        *accumulationBuffer,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    terrain_gpu::RiverGeometryConfig riverConfig{};
    riverConfig.seaLevelMeters = -1'000.0F;
    // Small enough that some interior cell (accumulation up to ~9,
    // cellArea 2500) clears the gate: drainageArea up to ~22500 m^2.
    riverConfig.minimumDrainageAreaSquareMeters = 5'000.0F;
    riverConfig.referenceDrainageAreaSquareMeters = 10'000.0F;

    riverGeometry.Dispatch(
        *commandList,
        kResolution,
        50.0F,
        riverConfig,
        *drainageBuffer,
        *accumulationBuffer,
        *outputBuffer);

    commandList->Transition(
        *outputBuffer,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::CopySource);

    commandList->CopyBuffer(*outputBuffer, 0, *outputReadback, 0, outputBytes);

    commandList->Close();
    queue->Submit(*commandList);
    queue->Signal(*fence, 1);
    fence->Wait(1);

    std::vector<f32> output(
        static_cast<std::size_t>(kResolution) * kResolution * 4);

    {
        const std::byte* mapped = outputReadback->Map();
        std::memcpy(output.data(), mapped, outputBytes);
        outputReadback->Unmap();
    }

    bool sawRiver = false;
    bool sawNonRiver = false;

    for (u32 y = 0; y < kResolution; ++y)
    {
        for (u32 x = 0; x < kResolution; ++x)
        {
            const std::size_t base = index(x, y) * 4;
            const f32 halfWidth = output[base + 0];
            const f32 depth = output[base + 1];
            const f32 bedElevation = output[base + 2];
            const bool isRiver = output[base + 3] != 0.0F;

            if (!std::isfinite(halfWidth) || !std::isfinite(depth) ||
                !std::isfinite(bedElevation))
            {
                std::cerr << "Cell (" << x << "," << y
                          << ") produced non-finite river geometry.\n";
                return 1;
            }

            if (halfWidth < riverConfig.minimumChannelHalfWidthMeters -
                                1.0e-3F ||
                halfWidth > riverConfig.maximumChannelHalfWidthMeters +
                                1.0e-3F)
            {
                std::cerr << "Cell (" << x << "," << y
                          << ") half-width " << halfWidth
                          << " is out of the configured bounds.\n";
                return 1;
            }

            if (depth < riverConfig.minimumDepthMeters - 1.0e-3F ||
                depth > riverConfig.maximumDepthMeters + 1.0e-3F)
            {
                std::cerr << "Cell (" << x << "," << y << ") depth "
                          << depth
                          << " is out of the configured bounds.\n";
                return 1;
            }

            if (bedElevation > drainage[index(x, y)] + 1.0e-3F)
            {
                std::cerr << "Cell (" << x << "," << y
                          << ") bed elevation is above the surface.\n";
                return 1;
            }

            if (isRiver)
            {
                sawRiver = true;
            }
            else
            {
                sawNonRiver = true;
            }
        }
    }

    if (!sawRiver || !sawNonRiver)
    {
        std::cerr << "Expected a mix of river and non-river cells "
                     "(sawRiver="
                  << sawRiver << " sawNonRiver=" << sawNonRiver << ").\n";
        return 1;
    }

    std::printf(
        "River geometry: found both river and non-river cells; all "
        "widths/depths within bounds, all beds at or below the "
        "surface.\n");

    return 0;
}
