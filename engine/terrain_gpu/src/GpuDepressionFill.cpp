#include <orbit/terrain_gpu/GpuDepressionFill.hpp>

#include "HydrologyRelaxCompute.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u32 kPushConstantDwords = 3;
constexpr u32 kThreadGroupSize = 8;

[[nodiscard]] std::unique_ptr<rhi::Buffer> CreateScratchBuffer(
    rhi::Device& device,
    const u32 maxResolution)
{
    const u64 bytes =
        static_cast<u64>(maxResolution) * maxResolution * sizeof(f32);

    return device.CreateBuffer({
        .sizeBytes = bytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::Common
    });
}
} // namespace

GpuDepressionFill::GpuDepressionFill(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const u32 maxResolution)
    : maxResolution_(maxResolution)
{
    const shader::Binary compute = shaderCompiler.Compile({
        .source = detail::kHydrologyRelaxComputeShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    if (compute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile the hydrology depression-fill "
            "compute shader.");
    }

    pipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = compute.bytecode.data(),
            .size = compute.bytecode.size()
        },
        .pushConstantDwords = kPushConstantDwords,
        .shaderResourceBuffers = 3
    });

    scratchA_ = CreateScratchBuffer(device, maxResolution);
    scratchB_ = CreateScratchBuffer(device, maxResolution);
}

GpuDepressionFill::~GpuDepressionFill() = default;

u64 GpuDepressionFill::TransientWorkingSetBytes() const noexcept
{
    return scratchA_->SizeBytes() +
        scratchB_->SizeBytes();
}

void GpuDepressionFill::Dispatch(
    rhi::CommandList& commandList,
    const u32 resolution,
    const f32 minimumDropMeters,
    const f32 seaLevelMeters,
    rhi::Buffer& rawElevation,
    rhi::Buffer& drainageOut) const
{
    if (resolution > maxResolution_ || resolution == 0)
    {
        throw std::out_of_range(
            "Orbit depression-fill resolution exceeds the pool this "
            "GpuDepressionFill was constructed for.");
    }

    const u64 bytes =
        static_cast<u64>(resolution) * resolution * sizeof(f32);

    // Both scratch buffers rest in ResourceState::Common between calls
    // (matching their creation state) -- restored explicitly at the
    // end of this function, so each call is self-contained regardless
    // of how many ping-pong iterations ran.
    rhi::Buffer* current = scratchA_.get();
    rhi::Buffer* other = scratchB_.get();

    commandList.Transition(
        *current,
        rhi::ResourceState::Common,
        rhi::ResourceState::CopyDestination);

    commandList.CopyBuffer(rawElevation, 0, *current, 0, bytes);

    commandList.Transition(
        *current,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::UnorderedAccess);

    commandList.Transition(
        *other,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    std::array<u32, kPushConstantDwords> pushConstants{};
    pushConstants[0] = resolution;
    pushConstants[1] = std::bit_cast<u32>(minimumDropMeters);
    pushConstants[2] = std::bit_cast<u32>(seaLevelMeters);

    const u32 groupCount =
        (resolution + kThreadGroupSize - 1) / kThreadGroupSize;

    // Naive Jacobi relaxation doesn't just need enough passes to cross
    // the grid's diagonal -- a cluster of cells that only see each
    // other (not yet a cheaper real route) inflate together by
    // ~minimumDropMeters per pass until that mutual cost finally
    // exceeds the real route's cost, which can take passes proportional
    // to (elevation range) / minimumDropMeters, not just geometric
    // diameter. A generous fixed multiplier covers realistic terrain
    // relief without needing a data-dependent bound.
    const u32 iterationCount = resolution * 8;

    for (u32 iteration = 0; iteration < iterationCount; ++iteration)
    {
        commandList.SetComputePipeline(*pipeline_);
        commandList.SetComputeBuffer(0, rawElevation);
        commandList.SetComputeBuffer(1, *current);
        commandList.SetComputeBuffer(2, *other);
        commandList.SetComputeConstants(pushConstants);

        commandList.Dispatch(groupCount, groupCount, 1);

        commandList.UavBarrier(*other);

        std::swap(current, other);
    }

    commandList.Transition(
        *current,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopySource);

    commandList.CopyBuffer(*current, 0, drainageOut, 0, bytes);

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
