#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
// GPU erosion/sediment transport -- see ErosionCompute.hpp. Mirrors
// engine/terrain_erosion's CPU SedimentTransport.cpp's formula and
// constants, but resolves the up-/downstream sediment budget via a
// Jacobi relaxation over GpuFlowAccumulation's downstream forest
// instead of a single elevation-sorted CPU sweep -- explicit, accepted
// non-parity (see the GPU terrain generation plan's Milestone 4).
struct GpuErosionConfig
{
    f32 referenceDrainageAreaSquareMeters{100'000'000.0F};
    f32 erosionScaleMeters{45.0F};
    f32 maximumErosionMeters{40.0F};
    f32 drainageAreaExponent{0.35F};
    f32 slopeExponent{0.70F};
    f32 depositionSlopeThreshold{0.002F};
    f32 maximumLandDepositionFraction{0.35F};
    f32 oceanDepositionFraction{0.85F};
    f32 maximumDepositionMeters{35.0F};
    f32 seaLevelMeters{0.0F};
};

class GpuErosion
{
public:
    GpuErosion(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        u32 maxResolution);
    ~GpuErosion();

    GpuErosion(const GpuErosion&) = delete;
    GpuErosion& operator=(const GpuErosion&) = delete;

    // `drainageElevation`, `accumulation`, and `downstream` are
    // GpuDepressionFill's and GpuFlowAccumulation's outputs (already
    // ResourceState::ShaderResource). `netElevationDeltaOut` (add this
    // to raw elevation: negative where eroded, positive where
    // deposited) is written directly every pass (not just once at the
    // end), so -- mirroring GpuFieldGenerator::Dispatch's convention --
    // the caller is responsible for it already being in
    // ResourceState::UnorderedAccess, and for transitioning it back to
    // ResourceState::ShaderResource afterward.
    void Dispatch(
        rhi::CommandList& commandList,
        u32 resolution,
        f32 spacingMeters,
        const GpuErosionConfig& config,
        rhi::Buffer& drainageElevation,
        rhi::Buffer& accumulation,
        rhi::Buffer& downstream,
        rhi::Buffer& netElevationDeltaOut) const;

private:
    u32 maxResolution_;
    std::unique_ptr<rhi::ComputePipeline> zeroPipeline_;
    std::unique_ptr<rhi::ComputePipeline> routingPipeline_;
    std::unique_ptr<rhi::Buffer> outgoingA_;
    std::unique_ptr<rhi::Buffer> outgoingB_;
};
} // namespace orbit::terrain_gpu
