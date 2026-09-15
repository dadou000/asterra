#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain_gpu/GpuLakeBasins.hpp>

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

    terrain_gpu::GpuLakeBasins lakeBasins(*device, compiler, kResolution);

    // Two separate 3x3 "lakes" (raw well below drainage, i.e. heavily
    // filled) at opposite corners of the interior, everything else
    // unfilled (raw == drainage).
    std::vector<f32> rawElevation(
        static_cast<std::size_t>(kResolution) * kResolution, 50.0F);
    std::vector<f32> drainage(rawElevation);

    const auto index = [&](const u32 x, const u32 y)
    {
        return static_cast<std::size_t>(y) * kResolution + x;
    };

    const auto MakeLake =
        [&](const u32 x0, const u32 y0)
    {
        for (u32 y = y0; y < y0 + 3; ++y)
        {
            for (u32 x = x0; x < x0 + 3; ++x)
            {
                rawElevation[index(x, y)] = 0.0F;
                drainage[index(x, y)] = 20.0F;
            }
        }
    };

    MakeLake(1, 1);
    MakeLake(5, 5);

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

    const auto rawBuffer =
        MakeUploadBuffer(rawElevation.data(), floatBytes);
    const auto drainageBuffer =
        MakeUploadBuffer(drainage.data(), floatBytes);

    const auto labelBuffer = device->CreateBuffer({
        .sizeBytes = uintBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });

    const auto labelReadback = device->CreateBuffer({
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

    lakeBasins.Dispatch(
        *commandList,
        kResolution,
        5.0F,
        *rawBuffer,
        *drainageBuffer,
        *labelBuffer);

    commandList->Transition(
        *labelBuffer,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::CopySource);

    commandList->CopyBuffer(*labelBuffer, 0, *labelReadback, 0, uintBytes);

    commandList->Close();
    queue->Submit(*commandList);
    queue->Signal(*fence, 1);
    fence->Wait(1);

    std::vector<u32> labels(
        static_cast<std::size_t>(kResolution) * kResolution);

    {
        const std::byte* mapped = labelReadback->Map();
        std::memcpy(labels.data(), mapped, uintBytes);
        labelReadback->Unmap();
    }

    constexpr u32 kNotLake = 0xFFFFFFFFU;

    // Every cell in lake 1 shares one label; every cell in lake 2
    // shares a (different) label; everything else is kNotLake.
    const u32 lake1Label = labels[index(1, 1)];
    const u32 lake2Label = labels[index(5, 5)];

    if (lake1Label == kNotLake || lake2Label == kNotLake)
    {
        std::cerr << "A lake seed cell was not labeled as a lake.\n";
        return 1;
    }

    if (lake1Label == lake2Label)
    {
        std::cerr << "The two separate lakes were merged into one "
                     "label (" << lake1Label << ").\n";
        return 1;
    }

    for (u32 y = 0; y < kResolution; ++y)
    {
        for (u32 x = 0; x < kResolution; ++x)
        {
            const bool inLake1 = x >= 1 && x < 4 && y >= 1 && y < 4;
            const bool inLake2 = x >= 5 && x < 8 && y >= 5 && y < 8;
            const u32 label = labels[index(x, y)];

            if (inLake1 && label != lake1Label)
            {
                std::cerr << "Cell (" << x << "," << y
                          << ") in lake 1 has label " << label
                          << ", expected " << lake1Label << "\n";
                return 1;
            }
            else if (inLake2 && label != lake2Label)
            {
                std::cerr << "Cell (" << x << "," << y
                          << ") in lake 2 has label " << label
                          << ", expected " << lake2Label << "\n";
                return 1;
            }
            else if (!inLake1 && !inLake2 && label != kNotLake)
            {
                std::cerr << "Cell (" << x << "," << y
                          << ") outside both lakes was labeled a lake ("
                          << label << ").\n";
                return 1;
            }
        }
    }

    std::printf(
        "Lake basins: two separate 3x3 lakes correctly labeled "
        "distinctly (%u vs %u), everything else unlabeled.\n",
        lake1Label,
        lake2Label);

    return 0;
}
