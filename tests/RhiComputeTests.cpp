#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
using namespace orbit;

// Milestone 1 verification (see the GPU terrain generation plan): proves
// the RHI/Vulkan compute foundation actually works end to end -- a
// compute pipeline can be created, dispatched, bound to buffers via push
// descriptors, and its output read back on the CPU -- before any terrain
// code is built on top of it.

[[nodiscard]] std::unique_ptr<rhi::Buffer> CreateReadbackBuffer(
    rhi::Device& device,
    const u64 sizeBytes)
{
    return device.CreateBuffer({
        .sizeBytes = sizeBytes,
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopyDestination
    });
}

// Runs the given recorder against a fresh command list, submits it, and
// blocks until the GPU has completed it -- simplest possible synchronous
// submission shape for a one-shot test (no frames-in-flight pipelining
// needed here).
template <typename Recorder>
void SubmitAndWait(
    rhi::Queue& queue,
    rhi::CommandAllocator& allocator,
    rhi::CommandList& commandList,
    rhi::Fence& fence,
    u64& nextFenceValue,
    Recorder&& recorder)
{
    allocator.Reset();
    commandList.Reset(allocator);

    recorder(commandList);

    commandList.Close();
    queue.Submit(commandList);

    ++nextFenceValue;
    queue.Signal(fence, nextFenceValue);
    fence.Wait(nextFenceValue);
}

// Test 1: SV_DispatchThreadID fill -- proves pipeline creation, Dispatch,
// SetComputeBuffer, and GPU->CPU readback all work.
[[nodiscard]] bool RunFillTest(
    rhi::Device& device,
    const shader::dxc::DxcShaderCompiler& compiler,
    rhi::Queue& queue,
    rhi::CommandAllocator& allocator,
    rhi::CommandList& commandList,
    rhi::Fence& fence,
    u64& nextFenceValue)
{
    constexpr const char* source = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    g_output.Store(id.x * 4u, id.x);
}
)";

    const shader::Binary compute = compiler.Compile({
        .source = source,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    if (compute.bytecode.empty())
    {
        std::cerr << "Orbit failed to compile the fill compute shader.\n";
        return false;
    }

    const auto pipeline = device.CreateComputePipeline({
        .computeShader = {
            .data = reinterpret_cast<const u8*>(compute.bytecode.data()),
            .size = compute.bytecode.size()
        },
        .shaderResourceBuffers = 1
    });

    constexpr u32 kElementCount = 256;
    constexpr u64 kSizeBytes = kElementCount * sizeof(u32);

    const auto gpuBuffer = device.CreateBuffer({
        .sizeBytes = kSizeBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::Common
    });

    const auto readback = CreateReadbackBuffer(device, kSizeBytes);

    SubmitAndWait(
        queue, allocator, commandList, fence, nextFenceValue,
        [&](rhi::CommandList& list)
        {
            list.Transition(
                *gpuBuffer,
                rhi::ResourceState::Common,
                rhi::ResourceState::UnorderedAccess);

            list.SetComputePipeline(*pipeline);
            list.SetComputeBuffer(0, *gpuBuffer);
            list.Dispatch(kElementCount / 64, 1, 1);

            list.Transition(
                *gpuBuffer,
                rhi::ResourceState::UnorderedAccess,
                rhi::ResourceState::CopySource);

            list.CopyBuffer(
                *gpuBuffer, 0, *readback, 0, kSizeBytes);
        });

    const std::byte* mapped = readback->Map();
    std::vector<u32> values(kElementCount);
    std::memcpy(values.data(), mapped, kSizeBytes);
    readback->Unmap();

    for (u32 i = 0; i < kElementCount; ++i)
    {
        if (values[i] != i)
        {
            std::cerr
                << "Orbit compute fill produced the wrong value at "
                << i << ": expected " << i << ", got " << values[i]
                << ".\n";
            return false;
        }
    }

    return true;
}

// Test 2: a 3-pass ping-pong relaxation (out[i] = in[i] + 1 each pass,
// alternating which buffer is source/destination) with a UavBarrier
// between every pass -- proves UavBarrier actually orders GPU work
// correctly between dispatches, which every bounded-iteration relaxation
// pass planned for hydrology/erosion depends on.
[[nodiscard]] bool RunPingPongTest(
    rhi::Device& device,
    const shader::dxc::DxcShaderCompiler& compiler,
    rhi::Queue& queue,
    rhi::CommandAllocator& allocator,
    rhi::CommandList& commandList,
    rhi::Fence& fence,
    u64& nextFenceValue)
{
    constexpr const char* source = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_input : register(u0);
[[vk::binding(1, 0)]]
RWByteAddressBuffer g_output : register(u1);

[numthreads(8, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint value = g_input.Load(id.x * 4u);
    g_output.Store(id.x * 4u, value + 1u);
}
)";

    const shader::Binary compute = compiler.Compile({
        .source = source,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    if (compute.bytecode.empty())
    {
        std::cerr
            << "Orbit failed to compile the ping-pong compute shader.\n";
        return false;
    }

    const auto pipeline = device.CreateComputePipeline({
        .computeShader = {
            .data = reinterpret_cast<const u8*>(compute.bytecode.data()),
            .size = compute.bytecode.size()
        },
        .shaderResourceBuffers = 2
    });

    constexpr u32 kElementCount = 8;
    constexpr u64 kSizeBytes = kElementCount * sizeof(u32);

    std::array<std::unique_ptr<rhi::Buffer>, 2> buffers{
        device.CreateBuffer({
            .sizeBytes = kSizeBytes,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::GpuOnly,
            .initialState = rhi::ResourceState::Common
        }),
        device.CreateBuffer({
            .sizeBytes = kSizeBytes,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::GpuOnly,
            .initialState = rhi::ResourceState::Common
        })
    };

    const auto readback = CreateReadbackBuffer(device, kSizeBytes);

    // A freshly created GpuOnly buffer's content is undefined, not zero
    // -- buffers[0] (the first pass's source) needs an explicit zero-fill
    // before the relaxation loop, or the test would be asserting against
    // an unknown starting value.
    const auto zeroUpload = device.CreateBuffer({
        .sizeBytes = kSizeBytes,
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopySource
    });

    std::byte* zeroMapped = zeroUpload->Map();
    std::memset(zeroMapped, 0, kSizeBytes);
    zeroUpload->Unmap();

    constexpr u32 kIterations = 3;

    SubmitAndWait(
        queue, allocator, commandList, fence, nextFenceValue,
        [&](rhi::CommandList& list)
        {
            list.Transition(
                *buffers[0],
                rhi::ResourceState::Common,
                rhi::ResourceState::CopyDestination);

            list.CopyBuffer(
                *zeroUpload, 0, *buffers[0], 0, kSizeBytes);

            list.Transition(
                *buffers[0],
                rhi::ResourceState::CopyDestination,
                rhi::ResourceState::UnorderedAccess);

            list.Transition(
                *buffers[1],
                rhi::ResourceState::Common,
                rhi::ResourceState::UnorderedAccess);

            list.SetComputePipeline(*pipeline);

            for (u32 iteration = 0; iteration < kIterations; ++iteration)
            {
                rhi::Buffer& source_ = *buffers[iteration % 2];
                rhi::Buffer& destination = *buffers[(iteration + 1) % 2];

                list.SetComputeBuffer(0, source_);
                list.SetComputeBuffer(1, destination);
                list.Dispatch(kElementCount / 8, 1, 1);

                // Without this, the next pass's read of `destination`
                // (as the following iteration's source) is a genuine
                // race against this pass's write -- this is exactly the
                // hazard UavBarrier exists to fix (see Command.hpp).
                list.UavBarrier(destination);
            }

            rhi::Buffer& finalBuffer = *buffers[kIterations % 2];

            list.Transition(
                finalBuffer,
                rhi::ResourceState::UnorderedAccess,
                rhi::ResourceState::CopySource);

            list.CopyBuffer(finalBuffer, 0, *readback, 0, kSizeBytes);
        });

    const std::byte* mapped = readback->Map();
    std::vector<u32> values(kElementCount);
    std::memcpy(values.data(), mapped, kSizeBytes);
    readback->Unmap();

    for (const u32 value : values)
    {
        if (value != kIterations)
        {
            std::cerr
                << "Orbit compute ping-pong relaxation produced " << value
                << " after " << kIterations
                << " passes; UavBarrier did not correctly order the "
                   "dispatches.\n";
            return false;
        }
    }

    return true;
}
} // namespace

int main()
{
    const auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    const shader::dxc::DxcShaderCompiler compiler;

    const auto queue = device->CreateQueue(rhi::QueueType::Graphics);
    const auto allocator =
        device->CreateCommandAllocator(rhi::QueueType::Graphics);
    const auto commandList = device->CreateCommandList(*allocator);
    const auto fence = device->CreateFence(0);
    u64 nextFenceValue = 0;

    if (!RunFillTest(
            *device, compiler, *queue, *allocator, *commandList, *fence,
            nextFenceValue))
    {
        return 1;
    }

    if (!RunPingPongTest(
            *device, compiler, *queue, *allocator, *commandList, *fence,
            nextFenceValue))
    {
        return 1;
    }

    return 0;
}
