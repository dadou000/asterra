#include <orbit/terrain_gpu/GpuRiverGeometry.hpp>

#include "RiverGeometryCompute.hpp"

#include <array>
#include <bit>
#include <stdexcept>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u32 kThreadGroupSize = 8;
constexpr u32 kPushConstantDwords = 14;
constexpr u32 kOutputStrideBytes = 16;

[[nodiscard]] std::unique_ptr<rhi::ComputePipeline> CreatePipeline(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler)
{
    const shader::Binary compute = shaderCompiler.Compile({
        .source = detail::kRiverGeometryComputeShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    if (compute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile the river-geometry compute "
            "shader.");
    }

    return device.CreateComputePipeline({
        .computeShader = {
            .data = compute.bytecode.data(),
            .size = compute.bytecode.size()
        },
        .pushConstantDwords = kPushConstantDwords,
        .shaderResourceBuffers = 3
    });
}
} // namespace

GpuRiverGeometry::GpuRiverGeometry(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const u32 maxResolution)
    : maxResolution_(maxResolution)
{
    pipeline_ = CreatePipeline(device, shaderCompiler);

    const u64 bytes =
        static_cast<u64>(maxResolution) * maxResolution *
        kOutputStrideBytes;

    scratch_ = device.CreateBuffer({
        .sizeBytes = bytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::Common
    });
}

GpuRiverGeometry::~GpuRiverGeometry() = default;

void GpuRiverGeometry::Dispatch(
    rhi::CommandList& commandList,
    const u32 resolution,
    const f32 spacingMeters,
    const RiverGeometryConfig& config,
    rhi::Buffer& drainageElevation,
    rhi::Buffer& accumulation,
    rhi::Buffer& output) const
{
    if (resolution > maxResolution_ || resolution == 0)
    {
        throw std::out_of_range(
            "Orbit river-geometry resolution exceeds the pool this "
            "GpuRiverGeometry was constructed for.");
    }

    const u64 bytes =
        static_cast<u64>(resolution) * resolution * kOutputStrideBytes;

    std::array<u32, kPushConstantDwords> pushConstants{};
    pushConstants[0] = resolution;
    pushConstants[1] = std::bit_cast<u32>(spacingMeters);
    pushConstants[2] = std::bit_cast<u32>(config.seaLevelMeters);
    pushConstants[3] =
        std::bit_cast<u32>(config.referenceDrainageAreaSquareMeters);
    pushConstants[4] =
        std::bit_cast<u32>(config.minimumDrainageAreaSquareMeters);
    pushConstants[5] =
        std::bit_cast<u32>(config.baseChannelHalfWidthMeters);
    pushConstants[6] =
        std::bit_cast<u32>(config.minimumChannelHalfWidthMeters);
    pushConstants[7] =
        std::bit_cast<u32>(config.maximumChannelHalfWidthMeters);
    pushConstants[8] = std::bit_cast<u32>(config.widthExponent);
    pushConstants[9] = std::bit_cast<u32>(config.baseDepthMeters);
    pushConstants[10] = std::bit_cast<u32>(config.minimumDepthMeters);
    pushConstants[11] = std::bit_cast<u32>(config.maximumDepthMeters);
    pushConstants[12] = std::bit_cast<u32>(config.depthExponent);
    pushConstants[13] = std::bit_cast<u32>(config.maximumIncisionMeters);

    const u32 groupCount =
        (resolution + kThreadGroupSize - 1) / kThreadGroupSize;

    commandList.Transition(
        *scratch_,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    commandList.SetComputePipeline(*pipeline_);
    commandList.SetComputeBuffer(0, drainageElevation);
    commandList.SetComputeBuffer(1, accumulation);
    commandList.SetComputeBuffer(2, *scratch_);
    commandList.SetComputeConstants(pushConstants);
    commandList.Dispatch(groupCount, groupCount, 1);
    commandList.UavBarrier(*scratch_);

    commandList.Transition(
        *scratch_,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopySource);

    commandList.CopyBuffer(*scratch_, 0, output, 0, bytes);

    commandList.Transition(
        *scratch_,
        rhi::ResourceState::CopySource,
        rhi::ResourceState::Common);
}
} // namespace orbit::terrain_gpu
