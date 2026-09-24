#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>
#include <orbit/terrain_gpu/GpuPhysicalPageComposite.hpp>
#include <orbit/terrain_gpu/GpuRegionDelta.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionCache.hpp>
#include <orbit/terrain_render/SurfaceEffects.hpp>
#include <orbit/terrain_view/ClipmapLayout.hpp>
#include <orbit/world/Planet.hpp>
#include <orbit/world/WorldPosition.hpp>

#include <memory>
#include <span>

namespace orbit::terrain_render
{
struct TerrainStreamingStats
{
    u64 generatedSamplesLastUpdate{0};
    u32 refreshedRegionsLastUpdate{0};
    u32 levelsTouchedLastUpdate{0};

    u64 uploadedBytesLastFrame{0};
    u32 drawCallsLastFrame{0};

    u64 cumulativeGeneratedSamples{0};
    u64 cumulativeUploadedBytes{0};

    u64 submittedBatches{0};
    u64 committedBatches{0};
    u64 supersededBatches{0};
    u64 revisionInvalidations{0};
    u64 staleRevisionBatches{0};
    u64 coverageTierChanges{0};
    // Committed full-grid rebuilds, excluding initial population.
    u64 rebaseCount{0};
    u32 lastRebaseLevels{0};
    const char* lastRebaseReason{"NONE"};
    f64 secondsSinceLastRebase{-1.0};

    u32 adaptiveCoverageTier{0};
    f64 activeBaseSpacingMeters{0.0};
    f64 activeOuterHalfExtentMeters{0.0};

    bool updatePending{false};
};

struct TerrainPreviewCamera
{
    math::Float3 forward{0.0F, -0.28F, 1.0F};
    math::Float3 up{0.0F, 1.0F, 0.0F};
    // Zero retains the standalone preview configuration.
    f32 verticalFovRadians{0.0F};
    f32 nearPlaneMeters{0.0F};
    f32 farPlaneMeters{0.0F};
};

struct TerrainPreviewConfig
{
    terrain_view::ClipmapConfig clipmap{
        .levelCount = 12,
        .gridResolution = 65,
        .baseSpacingMeters = 20.0,
        .levelScale = 2.0,
        .overlapCells = 6
    };

    terrain_view::AdaptiveClipmapCoverageConfig
        adaptiveCoverage{};

    f32 verticalFovRadians{1.22173048F};
    f32 nearPlaneMeters{10.0F};
    f32 farPlaneMeters{12'000'000.0F};
    bool wireframe{false};
    u32 framesInFlight{3};
};

class TerrainPreviewRenderer
{
public:
    // `regionDeltaComposite`/`hydrologyRegionCache` are optional --
    // when both are set, each dirty region's freshly GPU-generated
    // samples are composited against whichever hydrology region tile
    // (see engine/terrain_gpu's GpuHydrologyRegion and
    // DerivedTerrainRegionCache's GPU path) covers it and is ready,
    // in place, right after generation -- so the clipmap itself shows
    // GPU-computed erosion/lake-fill, not just the raw procedural
    // field. Leave both null to keep the raw-field-only behavior
    // (e.g. for tests with no region cache available).
    TerrainPreviewRenderer(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        const world::PlanetDefinition& planet,
        terrain_gpu::GpuFieldGenerator& gpuFieldGenerator,
        const world::WorldPosition& observer,
        TerrainPreviewConfig config = {},
        terrain_gpu::GpuRegionDelta* regionDeltaComposite = nullptr,
        terrain_region::DerivedTerrainRegionCache*
            hydrologyRegionCache = nullptr);

    ~TerrainPreviewRenderer();

    TerrainPreviewRenderer(
        const TerrainPreviewRenderer&) = delete;
    TerrainPreviewRenderer& operator=(
        const TerrainPreviewRenderer&) = delete;
    TerrainPreviewRenderer(
        TerrainPreviewRenderer&&) noexcept;
    TerrainPreviewRenderer& operator=(
        TerrainPreviewRenderer&&) noexcept;

    void UpdateObserver(
        const world::WorldPosition& observer);

    // F2 debug menu (see apps/sandbox/src/Main.cpp): LOD lattice
    // coloring and a side cutaway through the observer, and pausing
    // clipmap regeneration so LOD boundaries hold still for
    // inspection while the camera keeps moving freely.
    void SetDebugVisuals(
        bool lodColorEnabled,
        bool sideCutEnabled);
    void SetGenerationFrozen(bool frozen);

    // M12 live physical pages. A changed generation records one full derived
    // refresh of the current clipmap lattice; stable generations are free.
    void SetPhysicalPages(
        std::span<const terrain_gpu::GpuPhysicalSurfacePage> pages,
        u64 generation);

    // M38 transient physical-surface influence. The renderer keeps one
    // frame-in-flight-safe upload buffer per graphics frame and never mutates
    // authored terrain data.
    void SetSurfaceEffects(
        std::span<const SurfaceEffectGpuStamp> effects);

    void Draw(
        rhi::CommandList& commandList,
        u32 frameIndex,
        u32 targetWidth,
        u32 targetHeight,
        const TerrainPreviewCamera& camera);

    [[nodiscard]] u32 VertexCount() const noexcept;
    [[nodiscard]] u32 IndexCount() const noexcept;

    [[nodiscard]] const TerrainStreamingStats&
    StreamingStats() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::terrain_render
