#include <orbit/terrain_gpu/GpuErosion.hpp>

#include "ErosionCompute.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u32 kThreadGroupSize = 8;
constexpr u32 kRoutingPushConstantDwords = 12;

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

GpuErosion::GpuErosion(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const u32 maxResolution)
    : maxResolution_(maxResolution)
{
    const shader::Binary zeroCompute = shaderCompiler.Compile({
        .source = detail::kZeroFloatBufferComputeShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    const shader::Binary routingCompute = shaderCompiler.Compile({
        .source = detail::kErosionRoutingComputeShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    if (zeroCompute.bytecode.empty() || routingCompute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile an erosion compute shader.");
    }

    zeroPipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = zeroCompute.bytecode.data(),
            .size = zeroCompute.bytecode.size()
        },
        .pushConstantDwords = 1,
        .shaderResourceBuffers = 1
    });

    routingPipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = routingCompute.bytecode.data(),
            .size = routingCompute.bytecode.size()
        },
        .pushConstantDwords = kRoutingPushConstantDwords,
        .shaderResourceBuffers = 6
    });

    outgoingA_ = CreateScratchBuffer(device, maxResolution);
    outgoingB_ = CreateScratchBuffer(device, maxResolution);
}

GpuErosion::~GpuErosion() = default;

void GpuErosion::Dispatch(
    rhi::CommandList& commandList,
    const u32 resolution,
    const f32 spacingMeters,
    const GpuErosionConfig& config,
    rhi::Buffer& drainageElevation,
    rhi::Buffer& accumulation,
    rhi::Buffer& downstream,
    rhi::Buffer& netElevationDeltaOut) const
{
    if (resolution > maxResolution_ || resolution == 0)
    {
        throw std::out_of_range(
            "Orbit erosion resolution exceeds the pool this GpuErosion "
            "was constructed for.");
    }

    const u32 groupCount =
        (resolution + kThreadGroupSize - 1) / kThreadGroupSize;

    // --- Seed outgoingA_ with "no sediment yet" for this call. ---
    commandList.Transition(
        *outgoingA_,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    {
        const std::array<u32, 1> zeroPushConstants{resolution};

        commandList.SetComputePipeline(*zeroPipeline_);
        commandList.SetComputeBuffer(0, *outgoingA_);
        commandList.SetComputeConstants(zeroPushConstants);
        commandList.Dispatch(groupCount, groupCount, 1);
        commandList.UavBarrier(*outgoingA_);
    }

    commandList.Transition(
        *outgoingB_,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    std::array<u32, kRoutingPushConstantDwords> routingPushConstants{};
    routingPushConstants[0] = resolution;
    routingPushConstants[1] = std::bit_cast<u32>(spacingMeters);
    routingPushConstants[2] = std::bit_cast<u32>(config.seaLevelMeters);
    routingPushConstants[3] = std::bit_cast<u32>(
        config.referenceDrainageAreaSquareMeters);
    routingPushConstants[4] =
        std::bit_cast<u32>(config.erosionScaleMeters);
    routingPushConstants[5] =
        std::bit_cast<u32>(config.maximumErosionMeters);
    routingPushConstants[6] =
        std::bit_cast<u32>(config.drainageAreaExponent);
    routingPushConstants[7] = std::bit_cast<u32>(config.slopeExponent);
    routingPushConstants[8] = std::bit_cast<u32>(
        config.depositionSlopeThreshold);
    routingPushConstants[9] = std::bit_cast<u32>(
        config.maximumLandDepositionFraction);
    routingPushConstants[10] = std::bit_cast<u32>(
        config.oceanDepositionFraction);
    routingPushConstants[11] = std::bit_cast<u32>(
        config.maximumDepositionMeters);

    rhi::Buffer* current = outgoingA_.get();
    rhi::Buffer* other = outgoingB_.get();

    // Same non-epsilon convergence shape as GpuFlowAccumulation --
    // exact once every downstream chain has had enough passes to
    // drain, so a generous multiple of the grid diagonal is a safe
    // bound.
    const u32 iterationCount = resolution * 4;

    for (u32 iteration = 0; iteration < iterationCount; ++iteration)
    {
        commandList.SetComputePipeline(*routingPipeline_);
        commandList.SetComputeBuffer(0, drainageElevation);
        commandList.SetComputeBuffer(1, accumulation);
        commandList.SetComputeBuffer(2, downstream);
        commandList.SetComputeBuffer(3, *current);
        commandList.SetComputeBuffer(4, *other);
        commandList.SetComputeBuffer(5, netElevationDeltaOut);
        commandList.SetComputeConstants(routingPushConstants);

        commandList.Dispatch(groupCount, groupCount, 1);

        commandList.UavBarrier(*other);
        commandList.UavBarrier(netElevationDeltaOut);

        std::swap(current, other);
    }

    commandList.Transition(
        *current,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::Common);

    commandList.Transition(
        *other,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::Common);
}
} // namespace orbit::terrain_gpu
