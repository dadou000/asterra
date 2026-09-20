#include <orbit/terrain_gpu/GpuHydrologyRegion.hpp>

#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u64 kSampleStrideBytes =
    sizeof(terrain_stream::TerrainSampleValue);
constexpr u32 kThreadGroupSize = 8;

// Same elevation-extraction shader GpuElevationQuery.cpp uses --
// pulls just the elevation float (offset 0 of each 32-byte
// TerrainSampleValue record) into a tightly-packed f32 grid.
constexpr const char* kExtractElevationComputeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_samples : register(t0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_elevationOut : register(u1);

struct PushConstants
{
    uint resolution;
};

[[vk::push_constant]] PushConstants g_pc;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.resolution || dispatchId.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = dispatchId.y * g_pc.resolution + dispatchId.x;
    const float elevation = asfloat(g_samples.Load(index * 32u));
    g_elevationOut.Store(index * 4u, asuint(elevation));
}
)";
} // namespace

GpuHydrologyRegion::GpuHydrologyRegion(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const GpuFieldGenerator& generator,
    const u32 maxResolution)
    : maxResolution_(maxResolution),
      generator_(generator),
      depressionFill_(device, shaderCompiler, maxResolution),
      flowAccumulation_(device, shaderCompiler, maxResolution),
      erosion_(device, shaderCompiler, maxResolution)
{
    const shader::Binary extractCompute = shaderCompiler.Compile({
        .source = kExtractElevationComputeShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    if (extractCompute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile the elevation-extraction compute "
            "shader.");
    }

    extractPipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = extractCompute.bytecode.data(),
            .size = extractCompute.bytecode.size()
        },
        .pushConstantDwords = 1,
        .shaderResourceBuffers = 2
    });

    const u64 sampleBufferBytes =
        static_cast<u64>(maxResolution) * maxResolution *
        kSampleStrideBytes;

    sampleScratch_ = device.CreateBuffer({
        .sizeBytes = sampleBufferBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::Common
    });

    // Runoff is always uniform (1.0) for this pass -- one shared,
    // permanently-ShaderResource buffer, uploaded once here.
    const u64 floatGridBytes =
        static_cast<u64>(maxResolution) * maxResolution * sizeof(f32);

    runoff_ = device.CreateBuffer({
        .sizeBytes = floatGridBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource
    });

    {
        const std::vector<f32> ones(
            static_cast<std::size_t>(maxResolution) * maxResolution,
            1.0F);

        std::byte* mapped = runoff_->Map();
        std::memcpy(mapped, ones.data(), ones.size() * sizeof(f32));
        runoff_->Unmap();
    }
}

GpuHydrologyRegion::~GpuHydrologyRegion() = default;

u64 GpuHydrologyRegion::TransientWorkingSetBytes() const noexcept
{
    return sampleScratch_->SizeBytes() +
        runoff_->SizeBytes() +
        depressionFill_.TransientWorkingSetBytes() +
        flowAccumulation_.TransientWorkingSetBytes() +
        erosion_.TransientWorkingSetBytes();
}

void GpuHydrologyRegion::Dispatch(
    rhi::CommandList& commandList,
    const GpuHydrologyRegionRequest& request,
    rhi::Buffer& rawElevationOut,
    rhi::Buffer& drainageOut,
    rhi::Buffer& accumulationOut,
    rhi::Buffer& downstreamOut,
    rhi::Buffer& netElevationDeltaOut) const
{
    if (request.resolution > maxResolution_ || request.resolution == 0)
    {
        throw std::out_of_range(
            "Orbit hydrology region resolution exceeds the pool this "
            "GpuHydrologyRegion was constructed for.");
    }

    // --- Field generation into the shared 32-byte-per-sample scratch. ---
    GpuFieldRequest fieldRequest{};
    fieldRequest.resolution = request.resolution;
    fieldRequest.spacingMeters = request.spacingMeters;
    fieldRequest.footprintMeters = request.footprintMeters;
    fieldRequest.morphToCoarser = false;
    fieldRequest.surfaceFrame = request.surfaceFrame;
    fieldRequest.coarseSurfaceFrame = request.surfaceFrame;
    fieldRequest.originX = 0;
    fieldRequest.originY = 0;
    fieldRequest.region = {
        .x = 0,
        .y = 0,
        .width = request.resolution,
        .height = request.resolution
    };

    commandList.Transition(
        *sampleScratch_,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    generator_.Dispatch(commandList, fieldRequest, *sampleScratch_);

    commandList.UavBarrier(*sampleScratch_);

    commandList.Transition(
        *sampleScratch_,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::ShaderResource);

    // --- Extract into rawElevationOut (also this pass's raw elevation
    // -- reused directly as GpuDepressionFill's input, no extra
    // scratch buffer needed). ---
    commandList.Transition(
        rawElevationOut,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::UnorderedAccess);

    {
        const std::array<u32, 1> extractPushConstants{request.resolution};

        const u32 groupCount =
            (request.resolution + kThreadGroupSize - 1) /
            kThreadGroupSize;

        commandList.SetComputePipeline(*extractPipeline_);
        commandList.SetComputeBuffer(0, *sampleScratch_);
        commandList.SetComputeBuffer(1, rawElevationOut);
        commandList.SetComputeConstants(extractPushConstants);
        commandList.Dispatch(groupCount, groupCount, 1);
        commandList.UavBarrier(rawElevationOut);
    }

    commandList.Transition(
        *sampleScratch_,
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::Common);

    commandList.Transition(
        rawElevationOut,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::ShaderResource);

    // --- Depression fill. ---
    depressionFill_.Dispatch(
        commandList,
        request.resolution,
        request.minimumDropMeters,
        request.seaLevelMeters,
        rawElevationOut,
        drainageOut);

    commandList.Transition(
        rawElevationOut,
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::CopyDestination);

    // --- Flow accumulation. ---
    commandList.Transition(
        drainageOut,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    flowAccumulation_.Dispatch(
        commandList,
        request.resolution,
        drainageOut,
        *runoff_,
        accumulationOut,
        &downstreamOut);

    commandList.Transition(
        drainageOut,
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::CopyDestination);

    // --- Erosion (writes netElevationDeltaOut directly, needs
    // drainage/accumulation/downstream as ShaderResource). ---
    commandList.Transition(
        drainageOut,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    commandList.Transition(
        accumulationOut,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    commandList.Transition(
        downstreamOut,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    commandList.Transition(
        netElevationDeltaOut,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::UnorderedAccess);

    erosion_.Dispatch(
        commandList,
        request.resolution,
        static_cast<f32>(request.spacingMeters),
        request.erosion,
        drainageOut,
        accumulationOut,
        downstreamOut,
        netElevationDeltaOut);

    commandList.Transition(
        drainageOut,
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::CopyDestination);

    commandList.Transition(
        accumulationOut,
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::CopyDestination);

    commandList.Transition(
        downstreamOut,
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::CopyDestination);

    commandList.Transition(
        netElevationDeltaOut,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopyDestination);

    // rawElevationOut is already ResourceState::CopyDestination here
    // (set right after depression fill, above) -- every output buffer
    // ends this call in that state, matching the documented contract.
}
} // namespace orbit::terrain_gpu
