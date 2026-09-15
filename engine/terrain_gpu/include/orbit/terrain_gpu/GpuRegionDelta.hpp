#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
// One clipmap-level dispatch's worth of region-tile info to composite
// into an already-generated sample buffer -- see RegionDeltaCompute.hpp.
struct GpuRegionDeltaRequest
{
    // Matches the clipmap dispatch this composites into exactly --
    // same shape as terrain_gpu::GpuFieldRequest's own fields.
    u32 resolution{0};
    f64 spacingMeters{0.0};
    world::SurfaceFrame surfaceFrame{};
    u32 originX{0};
    u32 originY{0};
    struct
    {
        u32 x{0};
        u32 y{0};
        u32 width{0};
        u32 height{0};
    } region{};

    f64 planetRadiusMeters{0.0};

    // The hydrology region tile being composited in.
    world::SurfaceFrame regionSurfaceFrame{};
    f64 regionHalfExtentMeters{0.0};
    f64 regionSpacingMeters{0.0};
    u32 regionResolution{0};

    // 0..1 fraction of regionHalfExtentMeters where the fade to zero
    // begins (1.0 = no fade, apply at full strength everywhere inside
    // the tile).
    f32 edgeFadeStartDot{0.75F};
};

// Composites a GPU hydrology region tile's elevation delta into an
// already-generated clipmap sample buffer, in place -- see
// RegionDeltaCompute.hpp for the reprojection/blend math and why this
// is a separate pass rather than folded into GpuFieldGenerator's own
// shader.
class GpuRegionDelta
{
public:
    GpuRegionDelta(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler);
    ~GpuRegionDelta();

    GpuRegionDelta(const GpuRegionDelta&) = delete;
    GpuRegionDelta& operator=(const GpuRegionDelta&) = delete;

    // `regionDelta` (the hydrology tile's dense elevation-delta grid,
    // `regionResolution^2` tightly-packed f32 values) must already be
    // ResourceState::ShaderResource. `samples` (the clipmap's own
    // TerrainSampleValue buffer, already populated by a prior
    // GpuFieldGenerator::Dispatch over the same region) must already
    // be ResourceState::UnorderedAccess; the caller transitions it
    // back afterward, mirroring GpuFieldGenerator::Dispatch's own
    // convention for the same buffer.
    void Dispatch(
        rhi::CommandList& commandList,
        const GpuRegionDeltaRequest& request,
        rhi::Buffer& regionDelta,
        rhi::Buffer& samples) const;

private:
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
};
} // namespace orbit::terrain_gpu
