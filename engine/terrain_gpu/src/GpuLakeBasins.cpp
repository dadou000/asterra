#include <orbit/terrain_gpu/GpuLakeBasins.hpp>

#include "LakeBasinCompute.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u32 kThreadGroupSize = 8;

[[nodiscard]] std::unique_ptr<rhi::Buffer> CreateScratchBuffer(
    rhi::Device& device,
    const u32 maxResolution)
{
    const u64 bytes =
        static_cast<u64>(maxResolution) * maxResolution * sizeof(u32);

    return device.CreateBuffer({
        .sizeBytes = bytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::Common
    });
}
} // namespace

GpuLakeBasins::GpuLakeBasins(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const u32 maxResolution)
    : maxResolution_(maxResolution)
{
    const shader::Binary initCompute = shaderCompiler.Compile({
        .source = detail::kLakeBasinInitComputeShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    const shader::Binary propagateCompute = shaderCompiler.Compile({
        .source = detail::kLakeBasinPropagateComputeShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    if (initCompute.bytecode.empty() || propagateCompute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile a lake-basin compute shader.");
    }

    initPipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = initCompute.bytecode.data(),
            .size = initCompute.bytecode.size()
        },
        .pushConstantDwords = 2,
        .shaderResourceBuffers = 3
    });

    propagatePipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = propagateCompute.bytecode.data(),
            .size = propagateCompute.bytecode.size()
        },
        .pushConstantDwords = 1,
        .shaderResourceBuffers = 2
    });

    labelA_ = CreateScratchBuffer(device, maxResolution);
    labelB_ = CreateScratchBuffer(device, maxResolution);
}

GpuLakeBasins::~GpuLakeBasins() = default;

void GpuLakeBasins::Dispatch(
    rhi::CommandList& commandList,
    const u32 resolution,
    const f32 fillThresholdMeters,
    rhi::Buffer& rawElevation,
    rhi::Buffer& drainageElevation,
    rhi::Buffer& labelOut) const
{
    if (resolution > maxResolution_ || resolution == 0)
    {
        throw std::out_of_range(
            "Orbit lake-basin resolution exceeds the pool this "
            "GpuLakeBasins was constructed for.");
    }

    const u64 bytes =
        static_cast<u64>(resolution) * resolution * sizeof(u32);

    const u32 groupCount =
        (resolution + kThreadGroupSize - 1) / kThreadGroupSize;

    rhi::Buffer* current = labelA_.get();
    rhi::Buffer* other = labelB_.get();

    commandList.Transition(
        *current,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    {
        const std::array<u32, 2> initPushConstants{
            resolution, std::bit_cast<u32>(fillThresholdMeters)};

        commandList.SetComputePipeline(*initPipeline_);
        commandList.SetComputeBuffer(0, rawElevation);
        commandList.SetComputeBuffer(1, drainageElevation);
        commandList.SetComputeBuffer(2, *current);
        commandList.SetComputeConstants(initPushConstants);
        commandList.Dispatch(groupCount, groupCount, 1);
        commandList.UavBarrier(*current);
    }

    commandList.Transition(
        *other,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    const std::array<u32, 1> propagatePushConstants{resolution};

    // Min-label propagation converges once every cell in a connected
    // blob has had enough passes for the blob's minimum index to
    // reach it -- bounded by the blob's own diameter, never more than
    // the grid's.
    const u32 iterationCount = resolution * 4;

    for (u32 iteration = 0; iteration < iterationCount; ++iteration)
    {
        commandList.SetComputePipeline(*propagatePipeline_);
        commandList.SetComputeBuffer(0, *current);
        commandList.SetComputeBuffer(1, *other);
        commandList.SetComputeConstants(propagatePushConstants);

        commandList.Dispatch(groupCount, groupCount, 1);

        commandList.UavBarrier(*other);

        std::swap(current, other);
    }

    commandList.Transition(
        *current,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopySource);

    commandList.CopyBuffer(*current, 0, labelOut, 0, bytes);

    commandList.Transition(
        *current,
        rhi::ResourceState::CopySource,
        rhi::ResourceState::Common);

    commandList.Transition(
        *other,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::Common);
}
} // namespace orbit::terrain_gpu
