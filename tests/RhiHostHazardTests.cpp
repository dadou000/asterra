#include <orbit/rhi/vulkan/VulkanBackend.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
using namespace orbit;

// The Vulkan queue is no longer idled after every submit, so the CPU runs
// ahead of the GPU and lifetime is enforced per object instead. These tests
// never wait on a fence between iterations: if Buffer::Map, CommandAllocator::
// Reset or deferred destruction failed to protect a submit still in flight,
// the readback would mix iterations, validation would report an object in use,
// or the device would be lost.

constexpr u64 kBufferBytes = 2ULL * 1024ULL * 1024ULL;
constexpr u32 kIterations = 24U;

[[nodiscard]] std::unique_ptr<rhi::Buffer> CreateHostBuffer(
    rhi::Device& device, const rhi::ResourceState state)
{
    return device.CreateBuffer({
        .sizeBytes = kBufferBytes,
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = state
    });
}

void FillPattern(rhi::Buffer& buffer, const u32 seed)
{
    std::byte* data = buffer.Map();
    auto* words = reinterpret_cast<u32*>(data);
    const std::size_t count = kBufferBytes / sizeof(u32);
    for (std::size_t i = 0; i < count; ++i)
    {
        words[i] = seed * 2654435761U + static_cast<u32>(i);
    }
    buffer.Unmap();
}

[[nodiscard]] bool MatchesPattern(rhi::Buffer& buffer, const u32 seed)
{
    const std::byte* data = buffer.Map();
    const auto* words = reinterpret_cast<const u32*>(data);
    const std::size_t count = kBufferBytes / sizeof(u32);
    bool ok = true;
    for (std::size_t i = 0; i < count && ok; ++i)
    {
        ok = words[i] == seed * 2654435761U + static_cast<u32>(i);
    }
    buffer.Unmap();
    return ok;
}

// Writes a new pattern into the SAME source buffer every iteration, copies it
// to the SAME destination, submits without waiting, and reads the destination
// back immediately. The source rewrite races the previous iteration's copy and
// the readback races this iteration's, so both Map calls must wait precisely.
[[nodiscard]] bool RunBackToBackCopyTest(
    rhi::Device& device, rhi::Queue& queue)
{
    const auto source = CreateHostBuffer(device, rhi::ResourceState::CopySource);
    const auto destination =
        CreateHostBuffer(device, rhi::ResourceState::CopyDestination);
    const auto allocator =
        device.CreateCommandAllocator(rhi::QueueType::Graphics);
    const auto commands = device.CreateCommandList(*allocator);

    for (u32 iteration = 1; iteration <= kIterations; ++iteration)
    {
        FillPattern(*source, iteration);

        // Reusing the allocator straight after a submit that is still in
        // flight is only legal because Reset waits for that submit.
        allocator->Reset();
        commands->Reset(*allocator);
        commands->CopyBuffer(*source, 0, *destination, 0, kBufferBytes);
        commands->Close();
        queue.Submit(*commands);

        if (!MatchesPattern(*destination, iteration))
        {
            std::cerr << "back-to-back copy: readback mismatch at iteration "
                      << iteration << "\n";
            return false;
        }
    }

    return true;
}

// Destroys buffers a just-submitted copy still reads and writes, and replaces
// them, without waiting. Deferred destruction must keep them alive until the
// submit completes (validation reports vkDestroyBuffer on a buffer in use).
[[nodiscard]] bool RunDestroyWhileInFlightTest(
    rhi::Device& device, rhi::Queue& queue)
{
    const auto allocator =
        device.CreateCommandAllocator(rhi::QueueType::Graphics);
    const auto commands = device.CreateCommandList(*allocator);
    auto fence = device.CreateFence(0);
    u64 fenceValue = 0;

    for (u32 iteration = 1; iteration <= kIterations; ++iteration)
    {
        auto source =
            CreateHostBuffer(device, rhi::ResourceState::CopySource);
        auto destination =
            CreateHostBuffer(device, rhi::ResourceState::CopyDestination);
        FillPattern(*source, iteration);

        allocator->Reset();
        commands->Reset(*allocator);
        commands->CopyBuffer(*source, 0, *destination, 0, kBufferBytes);
        commands->Close();
        queue.Submit(*commands);

        source.reset();
        destination.reset();
    }

    ++fenceValue;
    queue.Signal(*fence, fenceValue);
    fence->Wait(fenceValue);
    return true;
}

// The CPU must be able to run ahead: queuing a batch of heavy copies must
// return long before the GPU finishes them. With an idle after every submit
// the loop takes the sum of all GPU times.
[[nodiscard]] bool RunSubmitDoesNotBlockTest(
    rhi::Device& device, rhi::Queue& queue)
{
    const auto source = CreateHostBuffer(device, rhi::ResourceState::CopySource);
    const auto destination =
        CreateHostBuffer(device, rhi::ResourceState::CopyDestination);
    FillPattern(*source, 1U);

    constexpr u32 kCommandLists = 12U;
    constexpr u32 kCopiesPerList = 160U;
    std::vector<std::unique_ptr<rhi::CommandAllocator>> allocators;
    std::vector<std::unique_ptr<rhi::CommandList>> lists;
    for (u32 i = 0; i < kCommandLists; ++i)
    {
        allocators.push_back(
            device.CreateCommandAllocator(rhi::QueueType::Graphics));
        lists.push_back(device.CreateCommandList(*allocators.back()));
        allocators.back()->Reset();
        lists.back()->Reset(*allocators.back());
        for (u32 c = 0; c < kCopiesPerList; ++c)
        {
            lists.back()->CopyBuffer(
                *source, 0, *destination, 0, kBufferBytes);
        }
        lists.back()->Close();
    }

    auto fence = device.CreateFence(0);
    const auto started = std::chrono::steady_clock::now();
    for (const auto& list : lists)
    {
        queue.Submit(*list);
    }
    const auto submitted = std::chrono::steady_clock::now();

    queue.Signal(*fence, 1U);
    fence->Wait(1U);
    const auto finished = std::chrono::steady_clock::now();

    const double submitMs =
        std::chrono::duration<double, std::milli>(submitted - started).count();
    const double totalMs =
        std::chrono::duration<double, std::milli>(finished - started).count();
    std::cout << "submit loop " << submitMs << " ms, GPU drained after "
              << totalMs << " ms\n";

    // Only meaningful when the GPU work is long enough to measure.
    if (totalMs > 5.0 && submitMs > totalMs * 0.5)
    {
        std::cerr << "queue submit appears to block on GPU completion\n";
        return false;
    }

    return true;
}
} // namespace

int main()
{
    const auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    const auto queue = device->CreateQueue(rhi::QueueType::Graphics);

    if (!RunBackToBackCopyTest(*device, *queue))
    {
        return 1;
    }

    if (!RunDestroyWhileInFlightTest(*device, *queue))
    {
        return 2;
    }

    if (!RunSubmitDoesNotBlockTest(*device, *queue))
    {
        return 3;
    }

    std::cout << "RhiHostHazardTests passed\n";
    return 0;
}
