#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
// GPU lake-basin connected-component labeling -- see
// LakeBasinCompute.hpp. Owns a fixed pool of ping-pong scratch buffers
// sized for `maxResolution`.
class GpuLakeBasins
{
public:
    GpuLakeBasins(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        u32 maxResolution);
    ~GpuLakeBasins();

    GpuLakeBasins(const GpuLakeBasins&) = delete;
    GpuLakeBasins& operator=(const GpuLakeBasins&) = delete;

    // `rawElevation` and `drainageElevation` (GpuDepressionFill's
    // output) must already be ResourceState::ShaderResource.
    // `fillThresholdMeters` is how much a cell must have been raised
    // above its raw elevation to count as a lake (matches
    // engine/terrain_water's LakeWater.cpp's depth gate in spirit, not
    // value -- tune per use). `labelOut` (one u32 per cell: the
    // component's shared label, or 0xFFFFFFFF for "not a lake cell")
    // must already be ResourceState::CopyDestination; the caller
    // transitions it to ShaderResource afterward.
    void Dispatch(
        rhi::CommandList& commandList,
        u32 resolution,
        f32 fillThresholdMeters,
        rhi::Buffer& rawElevation,
        rhi::Buffer& drainageElevation,
        rhi::Buffer& labelOut) const;

private:
    u32 maxResolution_;
    std::unique_ptr<rhi::ComputePipeline> initPipeline_;
    std::unique_ptr<rhi::ComputePipeline> propagatePipeline_;
    std::unique_ptr<rhi::Buffer> labelA_;
    std::unique_ptr<rhi::Buffer> labelB_;
};
} // namespace orbit::terrain_gpu
