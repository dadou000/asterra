#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
// GPU flow accumulation over a square hydrology grid -- see
// FlowAccumulationCompute.hpp for the two-pass algorithm (steepest-
// descent downstream pointers, then a Jacobi relaxation that sums
// upstream contributions along the resulting forest). Owns its own
// fixed pool of scratch buffers, sized for `maxResolution` at
// construction.
class GpuFlowAccumulation
{
public:
    GpuFlowAccumulation(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        u32 maxResolution);
    ~GpuFlowAccumulation();

    GpuFlowAccumulation(const GpuFlowAccumulation&) = delete;
    GpuFlowAccumulation& operator=(const GpuFlowAccumulation&) = delete;

    // `drainageElevation` should be GpuDepressionFill's output (not raw
    // elevation) so every non-outlet cell has a valid strictly-lower
    // neighbor to route through. `runoff` is per-cell input flow (pass
    // a buffer of 1.0s for uniform rainfall). Both must already be in
    // ResourceState::ShaderResource. `accumulationOut` and
    // `downstreamOut` (optional, may be null if the caller doesn't
    // need the pointers themselves) must already be in
    // ResourceState::CopyDestination; the caller transitions them to
    // ShaderResource afterward.
    void Dispatch(
        rhi::CommandList& commandList,
        u32 resolution,
        rhi::Buffer& drainageElevation,
        rhi::Buffer& runoff,
        rhi::Buffer& accumulationOut,
        rhi::Buffer* downstreamOut = nullptr) const;

private:
    u32 maxResolution_;
    std::unique_ptr<rhi::ComputePipeline> downstreamPipeline_;
    std::unique_ptr<rhi::ComputePipeline> accumulationPipeline_;
    std::unique_ptr<rhi::Buffer> downstream_;
    std::unique_ptr<rhi::Buffer> accumA_;
    std::unique_ptr<rhi::Buffer> accumB_;
};
} // namespace orbit::terrain_gpu
