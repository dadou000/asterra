#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
// One 16-byte record per cell: channelHalfWidthMeters (f32),
// depthMeters (f32), bedElevationMeters (f32), isRiver (u32, 0 or 1).
// See RiverGeometryCompute.hpp.
struct RiverGeometryConfig
{
    f32 seaLevelMeters{0.0F};
    f32 referenceDrainageAreaSquareMeters{1'000'000.0F};
    f32 minimumDrainageAreaSquareMeters{1'000'000.0F};
    f32 baseChannelHalfWidthMeters{4.0F};
    f32 minimumChannelHalfWidthMeters{1.5F};
    f32 maximumChannelHalfWidthMeters{120.0F};
    f32 widthExponent{0.32F};
    f32 baseDepthMeters{3.0F};
    f32 minimumDepthMeters{0.5F};
    f32 maximumDepthMeters{40.0F};
    f32 depthExponent{0.18F};
    f32 maximumIncisionMeters{180.0F};
};

// GPU river channel geometry -- a single parallel pass (no iteration),
// unlike the relaxation-based passes elsewhere in Milestone 4, since
// each cell's channel shape is a pure function of its own drainage
// area. Port of engine/terrain_erosion's RiverCarving.cpp /
// engine/terrain_hydrology's RiverGraph.cpp gate, same formula and
// constants -- no CPU/GPU non-parity here.
class GpuRiverGeometry
{
public:
    GpuRiverGeometry(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        u32 maxResolution);
    ~GpuRiverGeometry();

    GpuRiverGeometry(const GpuRiverGeometry&) = delete;
    GpuRiverGeometry& operator=(const GpuRiverGeometry&) = delete;

    // `drainageElevation` and `accumulation` must already be
    // ResourceState::ShaderResource. `output` (16 bytes/cell, see
    // above) must already be ResourceState::CopyDestination; the
    // caller transitions it to ShaderResource afterward.
    void Dispatch(
        rhi::CommandList& commandList,
        u32 resolution,
        f32 spacingMeters,
        const RiverGeometryConfig& config,
        rhi::Buffer& drainageElevation,
        rhi::Buffer& accumulation,
        rhi::Buffer& output) const;

private:
    u32 maxResolution_;
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
    std::unique_ptr<rhi::Buffer> scratch_;
};
} // namespace orbit::terrain_gpu
