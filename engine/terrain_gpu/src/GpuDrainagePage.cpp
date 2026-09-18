#include <orbit/terrain_gpu/GpuDrainagePage.hpp>

#include "M09DrainageCompute.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u32 kGridThreadGroupSize = 8U;
constexpr u32 kRakeThreadGroupSize = 256U;
constexpr u32 kMaxDonors = 8U;

[[nodiscard]] u64 ScalarBytes(
    const u32 resolution)
{
    return
        static_cast<u64>(
            resolution) *
        resolution *
        sizeof(f32);
}

[[nodiscard]] u64 DonorBytes(
    const u32 resolution)
{
    return
        static_cast<u64>(
            resolution) *
        resolution *
        kMaxDonors *
        sizeof(u32);
}

[[nodiscard]] std::unique_ptr<rhi::Buffer>
CreateScratch(
    rhi::Device& device,
    const u64 bytes)
{
    return device.CreateBuffer({
        .sizeBytes = bytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::Common
    });
}

[[nodiscard]] std::unique_ptr<rhi::ComputePipeline>
CompilePipeline(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const char* source,
    const u32 pushConstantDwords,
    const u32 bufferCount,
    const u32 storageTextureCount = 0U)
{
    const shader::Binary compute =
        shaderCompiler.Compile({
            .source = source,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    if (compute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile an M09 drainage compute shader.");
    }

    return device.CreateComputePipeline({
        .computeShader = {
            .data = compute.bytecode.data(),
            .size = compute.bytecode.size()
        },
        .pushConstantDwords =
            pushConstantDwords,
        .shaderResourceBuffers =
            bufferCount,
        .storageTextures =
            storageTextureCount
    });
}

void RequireBufferSize(
    const rhi::Buffer& buffer,
    const u64 requiredBytes,
    const char* name)
{
    if (buffer.SizeBytes() <
        requiredBytes)
    {
        throw std::invalid_argument(
            std::string(
                "Orbit M09 buffer is too small: ") +
            name);
    }
}

void TransitionRakeScratchToUav(
    rhi::CommandList& commandList,
    rhi::Buffer& donorCountA,
    rhi::Buffer& donorCountB,
    rhi::Buffer& donorsA,
    rhi::Buffer& donorsB,
    rhi::Buffer& areaA,
    rhi::Buffer& areaB,
    rhi::Buffer& dischargeA,
    rhi::Buffer& dischargeB)
{
    for (rhi::Buffer* buffer :
         std::array<rhi::Buffer*, 8>{
             &donorCountA,
             &donorCountB,
             &donorsA,
             &donorsB,
             &areaA,
             &areaB,
             &dischargeA,
             &dischargeB})
    {
        commandList.Transition(
            *buffer,
            rhi::ResourceState::Common,
            rhi::ResourceState::UnorderedAccess);
    }
}

void BarrierRakeState(
    rhi::CommandList& commandList,
    rhi::Buffer& donorCountIn,
    rhi::Buffer& donorsIn,
    rhi::Buffer& areaIn,
    rhi::Buffer& dischargeIn,
    rhi::Buffer& donorCountOut,
    rhi::Buffer& donorsOut,
    rhi::Buffer& areaOut,
    rhi::Buffer& dischargeOut)
{
    // Both ping-pong halves need an execution/memory dependency. The output
    // half becomes input next round; the previous input half becomes the next
    // output and must not be overwritten while the prior dispatch can read it.
    for (rhi::Buffer* buffer :
         std::array<rhi::Buffer*, 8>{
             &donorCountIn,
             &donorsIn,
             &areaIn,
             &dischargeIn,
             &donorCountOut,
             &donorsOut,
             &areaOut,
             &dischargeOut})
    {
        commandList.UavBarrier(
            *buffer);
    }
}
} // namespace

bool GpuDrainagePageRequest::IsValid() const noexcept
{
    return
        resolution > 0U &&
        std::isfinite(spacingMeters) &&
        spacingMeters > 0.0F &&
        std::isfinite(
            minimumDrainageDropMeters) &&
        minimumDrainageDropMeters >=
            0.0F &&
        std::isfinite(
            seaLevelMeters) &&
        std::isfinite(
            authoredGuidanceWeight) &&
        authoredGuidanceWeight >=
            0.0F &&
        authoredGuidanceWeight <
            1.0F;
}

GpuDrainagePage::GpuDrainagePage(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const u32 maxCoreResolution)
    : maxCoreResolution_(
          maxCoreResolution),
      maxPaddedResolution_(
          PaddedResolution(
              maxCoreResolution)),
      depressionFill_(
          device,
          shaderCompiler,
          PaddedResolution(
              maxCoreResolution))
{
    if (maxCoreResolution == 0U ||
        maxCoreResolution >
            std::numeric_limits<u16>::max())
    {
        throw std::invalid_argument(
            "Orbit M09 GPU drainage requires a non-zero practical "
            "maximum page resolution.");
    }

    extractSurfacePipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM09ExtractSurfaceShader,
            2U,
            2U,
            2U);

    downstreamPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM09DownstreamShader,
            4U,
            3U);

    initializeFlowPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM09InitializeFlowShader,
            3U,
            6U);

    buildDonorsPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM09BuildDonorsShader,
            1U,
            3U);

    rakeCompressPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM09RakeCompressShader,
            1U,
            8U);

    const u64 scalarBytes =
        ScalarBytes(
            maxPaddedResolution_);

    const u64 donorBytes =
        DonorBytes(
            maxPaddedResolution_);

    physicalSurface_ =
        CreateScratch(
            device,
            scalarBytes);

    donorCountA_ =
        CreateScratch(
            device,
            scalarBytes);

    donorCountB_ =
        CreateScratch(
            device,
            scalarBytes);

    donorsA_ =
        CreateScratch(
            device,
            donorBytes);

    donorsB_ =
        CreateScratch(
            device,
            donorBytes);

    areaA_ =
        CreateScratch(
            device,
            scalarBytes);

    areaB_ =
        CreateScratch(
            device,
            scalarBytes);

    dischargeA_ =
        CreateScratch(
            device,
            scalarBytes);

    dischargeB_ =
        CreateScratch(
            device,
            scalarBytes);
}

GpuDrainagePage::~GpuDrainagePage() = default;

void GpuDrainagePage::Dispatch(
    rhi::CommandList& commandList,
    const GpuDrainagePageRequest& request,
    GpuMaterialColumnResources& materialColumn,
    rhi::Buffer& haloConditioned,
    rhi::Buffer& runoffRate,
    rhi::Buffer& authoredGuidance,
    rhi::Buffer& incomingArea,
    rhi::Buffer& incomingDischarge,
    rhi::Buffer& drainageOut,
    rhi::Buffer& drainageAreaOut,
    rhi::Buffer& dischargeOut,
    rhi::Buffer& downstreamOut) const
{
    if (!request.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M09 GPU drainage request is invalid.");
    }

    if (request.resolution >
            maxCoreResolution_ ||
        materialColumn.Resolution() !=
            request.resolution)
    {
        throw std::out_of_range(
            "Orbit M09 GPU drainage resolution exceeds its fixed pool "
            "or does not match the M08 material page.");
    }

    const u32 paddedResolution =
        PaddedResolution(
            request.resolution);

    const u64 coreScalarBytes =
        ScalarBytes(
            request.resolution);

    const u64 paddedScalarBytes =
        ScalarBytes(
            paddedResolution);

    RequireBufferSize(
        haloConditioned,
        paddedScalarBytes,
        "haloConditioned");

    RequireBufferSize(
        runoffRate,
        coreScalarBytes,
        "runoffRate");

    RequireBufferSize(
        authoredGuidance,
        coreScalarBytes,
        "authoredGuidance");

    RequireBufferSize(
        incomingArea,
        coreScalarBytes,
        "incomingArea");

    RequireBufferSize(
        incomingDischarge,
        coreScalarBytes,
        "incomingDischarge");

    RequireBufferSize(
        drainageOut,
        paddedScalarBytes,
        "drainageOut");

    RequireBufferSize(
        drainageAreaOut,
        paddedScalarBytes,
        "drainageAreaOut");

    RequireBufferSize(
        dischargeOut,
        paddedScalarBytes,
        "dischargeOut");

    RequireBufferSize(
        downstreamOut,
        paddedScalarBytes,
        "downstreamOut");

    const u32 gridGroupCount =
        (paddedResolution +
         kGridThreadGroupSize -
         1U) /
        kGridThreadGroupSize;

    // M08 -> M09 physical surface. Material textures stay in UAV state so a
    // process can update them and this pass can consume them without a format
    // conversion or CPU round-trip.
    commandList.UavBarrier(
        materialColumn.
            BedrockHeight());

    commandList.UavBarrier(
        materialColumn.
            LooseMaterials());

    commandList.Transition(
        *physicalSurface_,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    commandList.SetComputePipeline(
        *extractSurfacePipeline_);

    commandList.SetComputeBuffer(
        0U,
        haloConditioned);

    commandList.SetComputeBuffer(
        1U,
        *physicalSurface_);

    commandList.SetComputeStorageTexture(
        0U,
        materialColumn.
            BedrockHeight());

    commandList.SetComputeStorageTexture(
        1U,
        materialColumn.
            LooseMaterials());

    const std::array<u32, 2>
        extractConstants{
            request.resolution,
            paddedResolution
        };

    commandList.SetComputeConstants(
        extractConstants);

    commandList.Dispatch(
        gridGroupCount,
        gridGroupCount,
        1U);

    commandList.UavBarrier(
        *physicalSurface_);

    commandList.Transition(
        *physicalSurface_,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::ShaderResource);

    if (request.depressionPolicy ==
        terrain_hydrology::
            DepressionRoutingPolicy::
                FillToBoundary)
    {
        depressionFill_.Dispatch(
            commandList,
            paddedResolution,
            request.
                minimumDrainageDropMeters,
            request.seaLevelMeters,
            *physicalSurface_,
            drainageOut);

        commandList.Transition(
            *physicalSurface_,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::Common);
    }
    else
    {
        commandList.Transition(
            *physicalSurface_,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::CopySource);

        commandList.CopyBuffer(
            *physicalSurface_,
            0U,
            drainageOut,
            0U,
            paddedScalarBytes);

        commandList.Transition(
            *physicalSurface_,
            rhi::ResourceState::CopySource,
            rhi::ResourceState::Common);
    }

    commandList.Transition(
        drainageOut,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    // Deterministic guided SFD over the conditioned field.
    commandList.Transition(
        downstreamOut,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::UnorderedAccess);

    commandList.SetComputePipeline(
        *downstreamPipeline_);

    commandList.SetComputeBuffer(
        0U,
        drainageOut);

    commandList.SetComputeBuffer(
        1U,
        authoredGuidance);

    commandList.SetComputeBuffer(
        2U,
        downstreamOut);

    std::array<u32, 4>
        downstreamConstants{};

    downstreamConstants[0] =
        request.resolution;

    downstreamConstants[1] =
        paddedResolution;

    downstreamConstants[2] =
        std::bit_cast<u32>(
            request.spacingMeters);

    downstreamConstants[3] =
        std::bit_cast<u32>(
            request.
                authoredGuidanceWeight);

    commandList.SetComputeConstants(
        downstreamConstants);

    commandList.Dispatch(
        gridGroupCount,
        gridGroupCount,
        1U);

    commandList.UavBarrier(
        downstreamOut);

    commandList.Transition(
        downstreamOut,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::ShaderResource);

    TransitionRakeScratchToUav(
        commandList,
        *donorCountA_,
        *donorCountB_,
        *donorsA_,
        *donorsB_,
        *areaA_,
        *areaB_,
        *dischargeA_,
        *dischargeB_);

    // Seed physical cell area/discharge and clear donor counts.
    commandList.SetComputePipeline(
        *initializeFlowPipeline_);

    commandList.SetComputeBuffer(
        0U,
        runoffRate);

    commandList.SetComputeBuffer(
        1U,
        incomingArea);

    commandList.SetComputeBuffer(
        2U,
        incomingDischarge);

    commandList.SetComputeBuffer(
        3U,
        *donorCountA_);

    commandList.SetComputeBuffer(
        4U,
        *areaA_);

    commandList.SetComputeBuffer(
        5U,
        *dischargeA_);

    const f32 cellAreaSquareMeters =
        request.spacingMeters *
        request.spacingMeters;

    std::array<u32, 3>
        initializeConstants{};

    initializeConstants[0] =
        request.resolution;

    initializeConstants[1] =
        paddedResolution;

    initializeConstants[2] =
        std::bit_cast<u32>(
            cellAreaSquareMeters);

    commandList.SetComputeConstants(
        initializeConstants);

    commandList.Dispatch(
        gridGroupCount,
        gridGroupCount,
        1U);

    commandList.UavBarrier(
        *donorCountA_);

    commandList.UavBarrier(
        *areaA_);

    commandList.UavBarrier(
        *dischargeA_);

    // Invert downstream pointers into the D8 donor matrix.
    commandList.SetComputePipeline(
        *buildDonorsPipeline_);

    commandList.SetComputeBuffer(
        0U,
        downstreamOut);

    commandList.SetComputeBuffer(
        1U,
        *donorCountA_);

    commandList.SetComputeBuffer(
        2U,
        *donorsA_);

    const std::array<u32, 1>
        donorConstants{
            paddedResolution
        };

    commandList.SetComputeConstants(
        donorConstants);

    commandList.Dispatch(
        gridGroupCount,
        gridGroupCount,
        1U);

    commandList.UavBarrier(
        *donorCountA_);

    commandList.UavBarrier(
        *donorsA_);

    const u32 nodeCount =
        paddedResolution *
        paddedResolution;

    const u32 rakeGroupCount =
        (nodeCount +
         kRakeThreadGroupSize -
         1U) /
        kRakeThreadGroupSize;

    // ceil(log2(nodeCount)) rounds are sufficient for rake-compress:
    // leaves are pruned while single-donor chains pointer-jump each round.
    const u32 iterationCount =
        std::bit_width(
            nodeCount - 1U);

    rhi::Buffer* donorCountIn =
        donorCountA_.get();

    rhi::Buffer* donorCountOut =
        donorCountB_.get();

    rhi::Buffer* donorsIn =
        donorsA_.get();

    rhi::Buffer* donorsOut =
        donorsB_.get();

    rhi::Buffer* areaIn =
        areaA_.get();

    rhi::Buffer* areaOut =
        areaB_.get();

    rhi::Buffer* dischargeIn =
        dischargeA_.get();

    rhi::Buffer* dischargeScratchOut =
        dischargeB_.get();

    for (u32 iteration = 0U;
         iteration < iterationCount;
         ++iteration)
    {
        commandList.SetComputePipeline(
            *rakeCompressPipeline_);

        commandList.SetComputeBuffer(
            0U,
            *donorCountIn);

        commandList.SetComputeBuffer(
            1U,
            *donorsIn);

        commandList.SetComputeBuffer(
            2U,
            *areaIn);

        commandList.SetComputeBuffer(
            3U,
            *dischargeIn);

        commandList.SetComputeBuffer(
            4U,
            *donorCountOut);

        commandList.SetComputeBuffer(
            5U,
            *donorsOut);

        commandList.SetComputeBuffer(
            6U,
            *areaOut);

        commandList.SetComputeBuffer(
            7U,
            *dischargeScratchOut);

        const std::array<u32, 1>
            rakeConstants{
                nodeCount
            };

        commandList.SetComputeConstants(
            rakeConstants);

        commandList.Dispatch(
            rakeGroupCount,
            1U,
            1U);

        BarrierRakeState(
            commandList,
            *donorCountIn,
            *donorsIn,
            *areaIn,
            *dischargeIn,
            *donorCountOut,
            *donorsOut,
            *areaOut,
            *dischargeScratchOut);

        std::swap(
            donorCountIn,
            donorCountOut);

        std::swap(
            donorsIn,
            donorsOut);

        std::swap(
            areaIn,
            areaOut);

        std::swap(
            dischargeIn,
            dischargeScratchOut);
    }

    // Latest ping-pong half contains the complete per-cell upstream sums.
    commandList.Transition(
        *areaIn,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopySource);

    commandList.Transition(
        *dischargeIn,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopySource);

    commandList.CopyBuffer(
        *areaIn,
        0U,
        drainageAreaOut,
        0U,
        paddedScalarBytes);

    commandList.CopyBuffer(
        *dischargeIn,
        0U,
        dischargeOut,
        0U,
        paddedScalarBytes);

    commandList.Transition(
        *areaIn,
        rhi::ResourceState::CopySource,
        rhi::ResourceState::Common);

    commandList.Transition(
        *dischargeIn,
        rhi::ResourceState::CopySource,
        rhi::ResourceState::Common);

    // Restore the other ping-pong half and graph scratch to their pool rest
    // state so Dispatch is allocation-free and repeatable.
    commandList.Transition(
        *areaOut,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::Common);

    commandList.Transition(
        *dischargeScratchOut,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::Common);

    for (rhi::Buffer* buffer :
         std::array<rhi::Buffer*, 4>{
             donorCountA_.get(),
             donorCountB_.get(),
             donorsA_.get(),
             donorsB_.get()})
    {
        commandList.Transition(
            *buffer,
            rhi::ResourceState::UnorderedAccess,
            rhi::ResourceState::Common);
    }

    commandList.Transition(
        downstreamOut,
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::CopyDestination);

    commandList.Transition(
        drainageOut,
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::CopyDestination);
}
} // namespace orbit::terrain_gpu
