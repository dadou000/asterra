#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
struct GpuPhysicalSurfaceTexel
{
    f32 elevationMeters{0.0F};
    f32 standingWaterDepthMeters{0.0F};
};

static_assert(
    sizeof(GpuPhysicalSurfaceTexel) ==
    2U * sizeof(f32));

struct GpuPhysicalSurfacePage
{
    terrain::PhysicalTerrainPageAddress address{};
    u32 resolution{0U};
    std::shared_ptr<rhi::Buffer> samples;

    [[nodiscard]] bool IsValid() const noexcept;
};

class GpuPhysicalPageComposite
{
public:
    GpuPhysicalPageComposite(
        rhi::Device& device,
        const shader::Compiler& compiler);
    ~GpuPhysicalPageComposite();

    GpuPhysicalPageComposite(
        const GpuPhysicalPageComposite&) = delete;
    GpuPhysicalPageComposite& operator=(
        const GpuPhysicalPageComposite&) = delete;

    void Dispatch(
        rhi::CommandList& commandList,
        const terrain_stream::TerrainSampleRequest& request,
        const terrain_stream::PhysicalRegion& region,
        const GpuPhysicalSurfacePage& page,
        f64 planetRadiusMeters,
        rhi::Buffer& outputSamples) const;

private:
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
};
} // namespace orbit::terrain_gpu
