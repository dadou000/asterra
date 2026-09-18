#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain_gpu/GpuDepressionFill.hpp>
#include <orbit/terrain_gpu/GpuMaterialColumnResources.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
struct GpuDrainagePageRequest
{
    u32 resolution{65};
    f32 spacingMeters{1.0F};
    f32 minimumDrainageDropMeters{0.01F};
    f32 seaLevelMeters{-1.0e9F};
    f32 authoredGuidanceWeight{0.20F};

    terrain_hydrology::DepressionRoutingPolicy depressionPolicy{
        terrain_hydrology::DepressionRoutingPolicy::FillToBoundary};

    [[nodiscard]] bool IsValid() const noexcept;
};

// M09 GPU drainage over one M08 physical page plus a one-cell neighboring
// boundary ring. Flow accumulation uses a FastFlow-style rake-compress
// reduction over the deterministic SFD forest.
//
// Buffer layout:
// - haloConditioned: (resolution + 2)^2 f32. Only the outer ring is read.
// - runoff/guidance/incomingArea/incomingDischarge: resolution^2 f32.
// - drainage/area/discharge/downstream outputs: (resolution + 2)^2 values.
//   The output ring is useful for exporting flux to adjacent pages.
//
// All input scalar buffers must be in ShaderResource state. Output buffers
// must enter in CopyDestination state and are restored to that state before
// return. M08 material textures must be in UnorderedAccess state, which is
// exactly the state left by GpuMaterialColumnResources::Upload.
class GpuDrainagePage
{
public:
    GpuDrainagePage(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        u32 maxCoreResolution);
    ~GpuDrainagePage();

    GpuDrainagePage(const GpuDrainagePage&) = delete;
    GpuDrainagePage& operator=(const GpuDrainagePage&) = delete;

    [[nodiscard]] static constexpr u32 PaddedResolution(
        const u32 coreResolution) noexcept
    {
        return coreResolution + 2U;
    }

    void Dispatch(
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
        rhi::Buffer& downstreamOut) const;

private:
    u32 maxCoreResolution_{0};
    u32 maxPaddedResolution_{0};

    std::unique_ptr<rhi::ComputePipeline> extractSurfacePipeline_;
    std::unique_ptr<rhi::ComputePipeline> downstreamPipeline_;
    std::unique_ptr<rhi::ComputePipeline> initializeFlowPipeline_;
    std::unique_ptr<rhi::ComputePipeline> buildDonorsPipeline_;
    std::unique_ptr<rhi::ComputePipeline> rakeCompressPipeline_;

    GpuDepressionFill depressionFill_;

    std::unique_ptr<rhi::Buffer> physicalSurface_;

    std::unique_ptr<rhi::Buffer> donorCountA_;
    std::unique_ptr<rhi::Buffer> donorCountB_;
    std::unique_ptr<rhi::Buffer> donorsA_;
    std::unique_ptr<rhi::Buffer> donorsB_;

    std::unique_ptr<rhi::Buffer> areaA_;
    std::unique_ptr<rhi::Buffer> areaB_;
    std::unique_ptr<rhi::Buffer> dischargeA_;
    std::unique_ptr<rhi::Buffer> dischargeB_;
};
} // namespace orbit::terrain_gpu
