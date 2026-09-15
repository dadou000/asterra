#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain_gpu/GpuFlowAccumulation.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
using namespace orbit;

// Milestone 4 verification (see the GPU terrain generation plan):
// GpuFlowAccumulation's downstream pointers form a forest (every
// non-outlet cell has exactly one strictly-lower neighbor it drains
// into, outlets have none), so total accumulation summed over every
// outlet cell must exactly equal total runoff -- every unit of runoff
// starts somewhere and, following its single downstream chain, ends at
// exactly one outlet. This is a mass-conservation property, not an
// exact per-cell value check (the plan explicitly calls for
// property-based tests here, not bit-parity with anything CPU-side --
// there is no CPU flow accumulation this even could match).
} // namespace

int main()
{
    const auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    const shader::dxc::DxcShaderCompiler compiler;

    constexpr u32 kResolution = 9;

    terrain_gpu::GpuFlowAccumulation flowAccumulation(
        *device, compiler, kResolution);

    // A monotonic slope in x: drainage strictly increases with x, so
    // every interior cell's steepest descent points toward lower x,
    // and everything not already on the boundary eventually drains
    // into the x=0 column.
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

    const auto accumulationReadback = device->CreateBuffer({
        .sizeBytes = floatBytes,
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopyDestination
    });

    const auto downstreamReadback = device->CreateBuffer({
        .sizeBytes = uintBytes,
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
        rhi::ResourceState::CopySource);

    commandList->CopyBuffer(
        *accumulationBuffer, 0, *accumulationReadback, 0, floatBytes);

    commandList->Transition(
        *downstreamBuffer,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::CopySource);

    commandList->CopyBuffer(
        *downstreamBuffer, 0, *downstreamReadback, 0, uintBytes);

    commandList->Close();
    queue->Submit(*commandList);
    queue->Signal(*fence, 1);
    fence->Wait(1);

    std::vector<f32> accumulation(drainage.size());
    std::vector<u32> downstream(drainage.size());

    {
        const std::byte* mapped = accumulationReadback->Map();
        std::memcpy(accumulation.data(), mapped, floatBytes);
        accumulationReadback->Unmap();
    }
    {
        const std::byte* mapped = downstreamReadback->Map();
        std::memcpy(downstream.data(), mapped, uintBytes);
        downstreamReadback->Unmap();
    }

    constexpr u32 kNoDownstream = 0xFFFFFFFFU;

    // 1. Every downstream pointer is either kNoDownstream (a boundary
    // outlet) or points to a strictly-lower-drainage neighbor.
    for (u32 y = 0; y < kResolution; ++y)
    {
        for (u32 x = 0; x < kResolution; ++x)
        {
            const std::size_t i = index(x, y);
            const bool isBoundary =
                x == 0 || y == 0 ||
                x == kResolution - 1 || y == kResolution - 1;

            if (isBoundary)
            {
                if (downstream[i] != kNoDownstream)
                {
                    std::cerr << "Boundary cell (" << x << "," << y
                              << ") has a downstream pointer.\n";
                    return 1;
                }

                continue;
            }

            if (downstream[i] == kNoDownstream)
            {
                std::cerr << "Interior cell (" << x << "," << y
                          << ") has no downstream pointer.\n";
                return 1;
            }

            if (drainage[downstream[i]] >= drainage[i])
            {
                std::cerr << "Cell (" << x << "," << y
                          << ") drains to a neighbor that isn't "
                             "lower.\n";
                return 1;
            }
        }
    }

    // 2. Mass conservation: every outlet cell's accumulation summed
    // equals total runoff (81.0) exactly, since the downstream graph
    // is a forest -- every cell's runoff reaches exactly one outlet.
    f64 outletTotal = 0.0;
    f64 totalRunoff = 0.0;

    for (std::size_t i = 0; i < accumulation.size(); ++i)
    {
        totalRunoff += static_cast<f64>(runoff[i]);

        if (downstream[i] == kNoDownstream)
        {
            outletTotal += static_cast<f64>(accumulation[i]);
        }
    }

    constexpr f64 kTolerance = 1.0e-2;

    if (std::abs(outletTotal - totalRunoff) > kTolerance)
    {
        std::cerr << "Mass not conserved: total runoff=" << totalRunoff
                   << " but outlet accumulation sums to " << outletTotal
                   << "\n";
        return 1;
    }

    // 3. The far corner (highest drainage, longest chain to an outlet)
    // should have accumulated at least its own runoff -- a basic
    // sanity check that accumulation isn't just reading back zeros.
    const f32 farCornerAccum = accumulation[index(kResolution - 2, 4)];

    if (farCornerAccum < 1.0F)
    {
        std::cerr << "Near-far-edge cell's accumulation looks wrong: "
                  << farCornerAccum << "\n";
        return 1;
    }

    std::printf(
        "Flow accumulation: total runoff=%.2f, outlet total=%.2f, "
        "x=7 row-center accum=%.2f\n",
        totalRunoff, outletTotal, farCornerAccum);

    return 0;
}
