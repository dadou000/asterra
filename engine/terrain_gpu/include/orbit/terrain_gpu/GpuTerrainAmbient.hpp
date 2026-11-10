#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain/TerrainAmbientLayout.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
// One clipmap level as the ambient pass sees it (the same lattice the vertex
// shader indexes: offset = centerOffset + (logical - (resolution - 1) / 2) *
// spacing, physical = (logical + origin) % resolution).
struct GpuTerrainAmbientLevel
{
    u32 resolution{0};
    f64 spacingMeters{0.0};
    math::Double2 centerOffsetMeters{};
    u32 originX{0};
    u32 originY{0};
    world::SurfaceFrame surfaceFrame{};
    // First sample of this level in the shared ambient buffer.
    u32 firstSample{0};
};

struct GpuTerrainAmbientRequest
{
    GpuTerrainAmbientLevel level{};
    // The next coarser level, whose finished ambient supplies the horizon
    // beyond this level's own reach. Zero resolution = none.
    GpuTerrainAmbientLevel coarser{};
    f64 planetRadiusMeters{0.0};
};

// Computes the terrain sky-visibility channel (see
// orbit/terrain/TerrainAmbientLayout.hpp) for a clipmap level from its
// finished toroidal elevation samples: sixteen horizon azimuths marched across
// the level, merged with the coarser level's result for far occluders.
class GpuTerrainAmbient
{
public:
    GpuTerrainAmbient(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler);
    ~GpuTerrainAmbient();

    GpuTerrainAmbient(const GpuTerrainAmbient&) = delete;
    GpuTerrainAmbient& operator=(const GpuTerrainAmbient&) = delete;

    // Marks every texel of `ambient` not computed. `ambient` must be in the
    // UnorderedAccess state.
    void Clear(
        rhi::CommandList& commandList,
        u32 sampleCount,
        rhi::Buffer& ambient) const;

    // Recomputes the whole level. `samples` is the level's terrain sample
    // buffer (32-byte records, elevation first) and must be readable;
    // `ambient` must be in the UnorderedAccess state, with a UAV barrier
    // between dispatches that read a coarser level another dispatch wrote.
    void Dispatch(
        rhi::CommandList& commandList,
        const GpuTerrainAmbientRequest& request,
        rhi::Buffer& samples,
        rhi::Buffer& ambient) const;

private:
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
};
} // namespace orbit::terrain_gpu
