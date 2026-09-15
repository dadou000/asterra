#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
// One physical sub-rectangle of a level's logical sample grid to
// (re)generate -- same granularity as
// terrain_stream::TerrainSampleRequest::regions, so a caller already
// computing dirty regions for the CPU path (ToroidalResidency,
// TerrainMorphRefresh) needs no new bookkeeping to drive this instead.
struct GpuFieldRegion
{
    u32 x{0};
    u32 y{0};
    u32 width{0};
    u32 height{0};
};

// Everything GpuFieldGenerator::Dispatch needs to fill one clipmap level's
// sample buffer over one region -- mirrors
// terrain_stream::TerrainSampleRequest (see its comments for the meaning
// of each field) but for a single GPU dispatch instead of a batch of CPU
// jobs.
struct GpuFieldRequest
{
    u32 resolution{0};
    f64 spacingMeters{0.0};
    f64 footprintMeters{0.0};

    bool morphToCoarser{false};
    f64 morphStartHalfExtentMeters{0.0};
    f64 morphEndHalfExtentMeters{0.0};
    f64 coarseSpacingMeters{0.0};
    f64 coarseFootprintMeters{0.0};

    // See terrain_stream::TerrainSampleRequest::fineNormalFootprintMeters/
    // fineNormalEpsilonMeters -- ground-truth shading-normal detail,
    // independent of this level's own (possibly much coarser) spacing.
    f64 fineNormalFootprintMeters{0.0};
    f64 fineNormalEpsilonMeters{0.0};

    // Snapped window center inside the stable surfaceFrame lattice.
    math::Double2 centerOffsetMeters{};

    world::SurfaceFrame surfaceFrame{};
    world::SurfaceFrame coarseSurfaceFrame{};

    u32 originX{0};
    u32 originY{0};

    GpuFieldRegion region{};
};

// GPU compute replacement for terrain_stream::TerrainSampleStreamer's CPU
// worker path: the same AnalyticTerrainSource noise/tectonics/climate/
// biome math (see FieldGenerationCompute.hpp), evaluated on the GPU and
// written directly into a clipmap level's sample buffer -- no CPU
// Sample() calls, no staging-buffer upload.
//
// The noise lattice hash is a bit-exact HLSL uint64_t port of the CPU
// AnalyticTerrainSource's Mix64 (requires shaderInt64, enabled in
// VulkanDevice.cpp) -- CPU (map, hydrology) and GPU (clipmap) sample the
// same planet, so a different hash would produce different coastlines/
// mountains between them. See FieldGenerationCompute.hpp's MakeU64/Mix64
// and the exact-salt-constant call sites for the details. Tectonic plate
// and hotspot placement is exported verbatim from the CPU GlobalTerrainFields
// instance this generator is built from (see GpuTectonicPlate's comment),
// so GPU-rendered terrain shows the same mountain ranges/islands as
// anything still generated CPU-side (the 2D map, hydrology).
class GpuFieldGenerator
{
public:
    // Built from an already-constructed AnalyticTerrainSource (not a
    // bare AnalyticTerrainDesc) so the GPU generator's tectonic plates/
    // hotspots and scalar recipe come from exactly this instance's
    // already-resolved state (GlobalFields().TectonicPlatesForGpu()/
    // TectonicHotspotsForGpu(), Description()) -- constructing a second,
    // separate CPU source from the same desc would re-resolve any
    // zero-means-derive seed fields independently and could diverge.
    GpuFieldGenerator(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        world::PlanetDefinition planet,
        const terrain::AnalyticTerrainSource& source);
    ~GpuFieldGenerator();

    GpuFieldGenerator(const GpuFieldGenerator&) = delete;
    GpuFieldGenerator& operator=(const GpuFieldGenerator&) = delete;

    // Records a compute dispatch filling `request.region`'s samples
    // (32-byte terrain_stream::TerrainSampleValue records, see
    // FieldGenerationCompute.hpp) into `outputSamples`. The caller is
    // responsible for `outputSamples` already being in
    // ResourceState::UnorderedAccess (and transitioning it back to
    // ResourceState::ShaderResource afterward before the clipmap vertex
    // shader reads it) -- this mirrors how SetGraphicsBuffer callers
    // already own their own resource-state transitions elsewhere in this
    // codebase.
    void Dispatch(
        rhi::CommandList& commandList,
        const GpuFieldRequest& request,
        rhi::Buffer& outputSamples) const;

private:
    world::PlanetDefinition planet_;
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
    std::unique_ptr<rhi::Buffer> paramsBuffer_;
    std::unique_ptr<rhi::Buffer> platesBuffer_;
    std::unique_ptr<rhi::Buffer> hotspotsBuffer_;
};
} // namespace orbit::terrain_gpu
