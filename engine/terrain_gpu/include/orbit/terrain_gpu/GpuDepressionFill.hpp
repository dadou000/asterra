#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
// GPU depression filling for a square hydrology grid -- see
// HydrologyRelaxCompute.hpp for the algorithm and its explicit,
// accepted non-parity with HydrologyGrid.cpp's CPU ConditionDepressions
// (same kind of result -- every interior cell raised just enough for a
// strictly-downhill path to an outlet -- reached by a different,
// GPU-parallel route).
//
// Owns a fixed pair of ping-pong scratch buffers sized for
// `maxResolution`, allocated once at construction (this codebase's usual
// fixed-pool convention), so `Dispatch` never allocates.
class GpuDepressionFill
{
public:
    GpuDepressionFill(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        u32 maxResolution);
    ~GpuDepressionFill();

    GpuDepressionFill(const GpuDepressionFill&) = delete;
    GpuDepressionFill& operator=(const GpuDepressionFill&) = delete;

    // `rawElevation` must hold `resolution * resolution` tightly-packed
    // f32 values (row-major, index = y * resolution + x), already in
    // ResourceState::ShaderResource. `drainageOut` receives the same
    // layout and must already be in ResourceState::CopyDestination;
    // the caller transitions it to ShaderResource afterward. `resolution`
    // must not exceed the `maxResolution` this was constructed with.
    void Dispatch(
        rhi::CommandList& commandList,
        u32 resolution,
        f32 minimumDropMeters,
        f32 seaLevelMeters,
        rhi::Buffer& rawElevation,
        rhi::Buffer& drainageOut) const;

    // Exact bytes owned by this fixed scratch pool. Dispatch does not
    // allocate, so this is also its peak page-generation transient use.
    [[nodiscard]] u64 TransientWorkingSetBytes() const noexcept;

private:
    u32 maxResolution_;
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
    std::unique_ptr<rhi::Buffer> scratchA_;
    std::unique_ptr<rhi::Buffer> scratchB_;
};
} // namespace orbit::terrain_gpu
