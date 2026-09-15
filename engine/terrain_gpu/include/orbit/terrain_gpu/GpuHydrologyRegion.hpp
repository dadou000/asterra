#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain_gpu/GpuDepressionFill.hpp>
#include <orbit/terrain_gpu/GpuErosion.hpp>
#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>
#include <orbit/terrain_gpu/GpuFlowAccumulation.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
struct GpuHydrologyRegionRequest
{
    u32 resolution{129};
    f64 spacingMeters{0.0};
    f64 footprintMeters{0.0};
    world::SurfaceFrame surfaceFrame{};
    f32 seaLevelMeters{0.0F};
    f32 minimumDropMeters{0.25F};
    GpuErosionConfig erosion{};
};

// Runs the full GPU terrain-generation + hydrology stack for one
// square region tile -- field generation, depression filling, flow
// accumulation, and erosion, chained the same way
// GpuElevationQuery's per-query neighborhood dispatch does, but at a
// caller-chosen resolution (region tiles, not a handful of collision
// cells) and exposing every intermediate buffer so a caller can
// populate a full terrain_hydrology::HydrologyGrid (plus a cumulative
// elevation-delta array) from the result -- see
// engine/terrain_region's GPU-backed region builder, which does
// exactly that and then hands the populated grid to the *unchanged*
// CPU BuildRiverGraph/BuildRegionalElevationDeltaField/
// BuildRiverCarvingField/BuildRiverWaterNetwork/BuildLakeWaterField
// functions.
//
// Like the other Milestone 4 passes, must be driven from the main
// thread's own command list/fence -- the Vulkan RHI has no
// thread-safety today, so unlike the CPU pipeline this replaces
// (built entirely on background worker threads), the GPU dispatch
// itself cannot happen there. A caller wiring this into an
// async/background pipeline needs to split "record + submit this
// frame" (main thread) from "read the result back and do the
// remaining CPU-side graph work" (safe to background once the fence
// retires).
class GpuHydrologyRegion
{
public:
    GpuHydrologyRegion(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        const GpuFieldGenerator& generator,
        u32 maxResolution);
    ~GpuHydrologyRegion();

    GpuHydrologyRegion(const GpuHydrologyRegion&) = delete;
    GpuHydrologyRegion& operator=(const GpuHydrologyRegion&) = delete;

    // Every output buffer must be `resolution * resolution` elements
    // (f32 for all but `downstreamOut`, which is u32), already in
    // ResourceState::CopyDestination; the caller transitions each to
    // ResourceState::ShaderResource (or maps it directly, if
    // HostVisible) afterward. `resolution` must not exceed the
    // `maxResolution` this was constructed with.
    void Dispatch(
        rhi::CommandList& commandList,
        const GpuHydrologyRegionRequest& request,
        rhi::Buffer& rawElevationOut,
        rhi::Buffer& drainageOut,
        rhi::Buffer& accumulationOut,
        rhi::Buffer& downstreamOut,
        rhi::Buffer& netElevationDeltaOut) const;

private:
    u32 maxResolution_;
    const GpuFieldGenerator& generator_;
    std::unique_ptr<rhi::ComputePipeline> extractPipeline_;
    GpuDepressionFill depressionFill_;
    GpuFlowAccumulation flowAccumulation_;
    GpuErosion erosion_;

    std::unique_ptr<rhi::Buffer> sampleScratch_;
    std::unique_ptr<rhi::Buffer> runoff_;
};
} // namespace orbit::terrain_gpu
