#include <orbit/terrain_gpu/GpuFlowAccumulation.hpp>

#include "FlowAccumulationCompute.hpp"

#include <algorithm>
#include <array>
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

[[nodiscard]] std::unique_ptr<rhi::ComputePipeline> CreatePipeline(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const char* source,
    const u32 shaderResourceBuffers)
{
    const shader::Binary compute = shaderCompiler.Compile({
        .source = source,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    if (compute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile a flow-accumulation compute "
            "shader.");
    }

    return device.CreateComputePipeline({
        .computeShader = {
            .data = compute.bytecode.data(),
            .size = compute.bytecode.size()
        },
        .pushConstantDwords = 1,
        .shaderResourceBuffers = shaderResourceBuffers
    });
}
} // namespace

GpuFlowAccumulation::GpuFlowAccumulation(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const u32 maxResolution)
    : maxResolution_(maxResolution)
{
    downstreamPipeline_ = CreatePipeline(
        device,
        shaderCompiler,
        detail::kFlowDownstreamComputeShader,
        2);

    accumulationPipeline_ = CreatePipeline(
        device,
        shaderCompiler,
        detail::kFlowAccumulationComputeShader,
        4);

    downstream_ = CreateScratchBuffer(device, maxResolution);
    accumA_ = CreateScratchBuffer(device, maxResolution);
    accumB_ = CreateScratchBuffer(device, maxResolution);
}

GpuFlowAccumulation::~GpuFlowAccumulation() = default;

u64 GpuFlowAccumulation::TransientWorkingSetBytes() const noexcept
{
    return downstream_->SizeBytes() +
        accumA_->SizeBytes() +
        accumB_->SizeBytes();
}

void GpuFlowAccumulation::Dispatch(
    rhi::CommandList& commandList,
    const u32 resolution,
    rhi::Buffer& drainageElevation,
    rhi::Buffer& runoff,
    rhi::Buffer& accumulationOut,
    rhi::Buffer* downstreamOut) const
{
    if (resolution > maxResolution_ || resolution == 0)
    {
        throw std::out_of_range(
            "Orbit flow-accumulation resolution exceeds the pool this "
            "GpuFlowAccumulation was constructed for.");
    }

    const u64 floatBytes =
        static_cast<u64>(resolution) * resolution * sizeof(f32);
    const u64 uintBytes =
        static_cast<u64>(resolution) * resolution * sizeof(u32);

    const std::array<u32, 1> pushConstants{resolution};

    const u32 groupCount =
        (resolution + kThreadGroupSize - 1) / kThreadGroupSize;

    // --- Pass 1: steepest-descent downstream pointers (one pass). ---
    commandList.Transition(
        *downstream_,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    commandList.SetComputePipeline(*downstreamPipeline_);
    commandList.SetComputeBuffer(0, drainageElevation);
    commandList.SetComputeBuffer(1, *downstream_);
    commandList.SetComputeConstants(pushConstants);
    commandList.Dispatch(groupCount, groupCount, 1);
    commandList.UavBarrier(*downstream_);

    commandList.Transition(
        *downstream_,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::ShaderResource);

    // --- Pass 2: Jacobi accumulation, seeded with accum = runoff. ---
    commandList.Transition(
        *accumA_,
        rhi::ResourceState::Common,
        rhi::ResourceState::CopyDestination);

    commandList.CopyBuffer(runoff, 0, *accumA_, 0, floatBytes);

    commandList.Transition(
        *accumA_,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::UnorderedAccess);

    commandList.Transition(
        *accumB_,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    rhi::Buffer* current = accumA_.get();
    rhi::Buffer* other = accumB_.get();

    // Enough passes for the longest downstream chain in the grid to
    // fully propagate -- unlike GpuDepressionFill's relaxation, this
    // sum has no epsilon-creep failure mode (it's exact once every
    // contributing chain has had enough passes to reach its sink), so
    // a generous multiple of the grid diagonal is a safe bound even
    // for a meandering flow path.
    const u32 iterationCount = resolution * 4;

    for (u32 iteration = 0; iteration < iterationCount; ++iteration)
    {
        commandList.SetComputePipeline(*accumulationPipeline_);
        commandList.SetComputeBuffer(0, *downstream_);
        commandList.SetComputeBuffer(1, runoff);
        commandList.SetComputeBuffer(2, *current);
        commandList.SetComputeBuffer(3, *other);
        commandList.SetComputeConstants(pushConstants);

        commandList.Dispatch(groupCount, groupCount, 1);

        commandList.UavBarrier(*other);

        std::swap(current, other);
    }

    commandList.Transition(
        *current,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopySource);

    commandList.CopyBuffer(*current, 0, accumulationOut, 0, floatBytes);

    commandList.Transition(
        *current,
        rhi::ResourceState::CopySource,
        rhi::ResourceState::Common);

    commandList.Transition(
        *other,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::Common);

    if (downstreamOut != nullptr)
    {
        commandList.Transition(
            *downstream_,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::CopySource);

        commandList.CopyBuffer(*downstream_, 0, *downstreamOut, 0, uintBytes);

        commandList.Transition(
            *downstream_,
            rhi::ResourceState::CopySource,
            rhi::ResourceState::Common);
    }
    else
    {
        commandList.Transition(
            *downstream_,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::Common);
    }
}
} // namespace orbit::terrain_gpu
