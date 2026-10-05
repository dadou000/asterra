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
#include <orbit/terrain_view/ClipmapPlanner.hpp>
#include <orbit/world/Planet.hpp>
#include <orbit/world/WorldPosition.hpp>

#include <array>
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

    // Dynamic clipmap plan (terrain_view::ClipmapPlanner): the active range of
    // the ladder, what drove it, and how often it has changed.
    bool plannerDynamic{false};
    u32 ladderLevels{0};
    u32 activeFirstLevel{0};
    u32 activeLastLevel{0};
    f64 nearestGroundMeters{0.0};
    f64 visibleArcMeters{0.0};
    f64 requiredSpacingMeters{0.0};
    u64 planChanges{0};

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

    // Which levels of the ladder the camera needs each frame.
    terrain_view::ClipmapPlannerConfig planner{};

    f32 verticalFovRadians{1.22173048F};
    f32 nearPlaneMeters{10.0F};
    f32 farPlaneMeters{12'000'000.0F};
    bool wireframe{false};
    bool drySurface{false};
    // CPU command/upload resources may still use this ring depth. Terrain
    // sample residency itself is persistent per LOD and is not duplicated
    // across these frame slots.
    u32 framesInFlight{3};
};

// Optics of the standing water drawn over the clipmap bed. The defaults match
// celestial_ocean::OceanOpticalParameters so an unconfigured view looks like
// the orbital ocean. Transient presentation state, uploaded once per frame.
struct TerrainWaterOptics
{
    // Absorption per metre for red, green, blue (1/m).
    math::Float3 absorptionPerMeter{0.18F, 0.055F, 0.025F};
    f32 refractiveIndex{1.333F};
    // Albedo the water column tends to when it is deep.
    math::Float3 deepColor{0.008F, 0.035F, 0.075F};
    f32 deepColorDepthMeters{40.0F};
    // Near-field surface roughness (a calm sea; the orbital value is a
    // far-glint parameter and is not reused here).
    f32 roughness{0.06F};
    // Direction to the sun in the body-fixed frame and its irradiance, in the
    // units the deferred lighting pass uses, so the water is lit like the scene.
    math::Float3 sunDirectionBody{0.55F, 0.72F, -0.48F};
    f32 sunIrradiance{1.0F};
    // Sky irradiance (linear) for the body colour and the sky reflection.
    math::Float3 skyIrradiance{0.05F, 0.07F, 0.10F};
    // Near-field representation weight, so the water fades with the terrain.
    f32 opacity{1.0F};
    // Elevation of the sea surface. Dry land is signed against it so the
    // waterline is resolved per pixel.
    f32 seaLevelMeters{0.0F};
};

// The elevation of the terrain the clipmap actually draws under the camera,
// read back from the GPU a few frames late (the highest of the four vertices
// around the point below the camera). Camera ground clearance uses it so the
// camera cannot end up under what is on screen when that differs from the CPU
// terrain source (physical pages, generator filtering).
struct TerrainRenderedGround
{
    bool valid{false};
    // Metres above the planet radius.
    f64 elevationMeters{0.0};
    // Unit direction (planet centred) the sample was taken under.
    math::Double3 direction{};
    f64 spacingMeters{0.0};
    // The four vertices behind elevationMeters: where they are (unit directions),
    // what the GPU generated there, and the footprint it generated them with. Lets
    // the CPU terrain source be evaluated at exactly the same points to compare.
    std::array<math::Double3, 4> cornerDirections{};
    std::array<f32, 4> cornerElevations{};
    f64 footprintMeters{0.0};
};

// One level of the clipmap as it is laid out now (read-only diagnostics).
struct TerrainClipmapLevelSummary
{
    u32 level{0U};
    bool active{false};
    f64 spacingMeters{0.0};
    f64 halfExtentMeters{0.0};
    // Camera-distance band of a banded level (0 for the ladder).
    f64 bandInnerMeters{0.0};
    f64 bandOuterMeters{0.0};
    // Grid resolution the level is laid out with now (what the vertex shader indexes), and the vertex
    // count the renderer actually submits for it. They agree when drawnVertices == 6 * (resolution - 1)^2;
    // fewer drawn vertices than that leaves rows of the level undrawn.
    u32 gridResolution{0U};
    u64 drawnVertices{0U};
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

    // The tangent frame TerrainPreviewCamera vectors must be expressed in. It is
    // parallel-transported with the observer, exactly like the clipmap geometry,
    // so it must be used instead of world::MakeSurfaceFrame (which differs by a
    // rotation about up and makes the terrain appear to slide with the camera).
    [[nodiscard]] const world::SurfaceFrame& CameraFrame() const noexcept;

    // F2 debug menu (see apps/sandbox/src/Main.cpp): LOD lattice
    // coloring and a side cutaway through the observer, and pausing
    // clipmap regeneration so LOD boundaries hold still for
    // inspection while the camera keeps moving freely.
    // sampleHealthEnabled colours each vertex by what is wrong with its sample (see the clipmap
    // vertex shader): bad elevation, bad morph target, bad slope, or healthy.
    void SetDebugVisuals(
        bool lodColorEnabled,
        bool sideCutEnabled,
        bool sampleHealthEnabled = false,
        // holeViewEnabled draws nothing culled and colours each vertex by why it would be.
        bool holeViewEnabled = false,
        // projectionViewEnabled draws nothing culled and colours each vertex by where its clip
        // position lands (non-finite, behind the camera, outside the depth range, off screen, ok).
        bool projectionViewEnabled = false);
    void SetGenerationFrozen(bool frozen);

    // Draws the clipmap as a wireframe (and hides the water surface over it).
    // Takes effect on the next Draw.
    void SetWireframe(bool wireframe);
    // Freezes the clipmap: the active-level plan, the window position, residency
    // and generated content all stop following the camera, which keeps moving
    // and can leave the clipmap to look at it from outside. Unfreezing snaps the
    // window back to the camera.
    void SetClipmapFrozen(bool frozen);
    [[nodiscard]] bool ClipmapFrozen() const noexcept;

    // Dynamic clipmap planning. Takes effect on the next Draw; turning it off
    // activates the whole ladder again.
    void SetClipmapPlanner(const terrain_view::ClipmapPlannerConfig& config);
    // Seconds a clipmap level takes to dissolve in or out when the plan adds or
    // drops it (clamped to [0, 5]; 0 swaps instantly).
    void SetLevelFadeSeconds(f64 seconds) noexcept;
    // Terrain elevation under the camera (metres above the planet radius), so the
    // planner measures distance to the actual ground.
    void SetGroundElevationHint(f64 elevationMeters) noexcept;
    [[nodiscard]] const terrain_view::ClipmapPlan& ClipmapPlan() const noexcept;
    [[nodiscard]] TerrainRenderedGround RenderedGround() const noexcept;
    // Every level of the current layout, finest first.
    [[nodiscard]] std::vector<TerrainClipmapLevelSummary> ClipmapLevels() const;
    [[nodiscard]] bool ClipmapBanded() const noexcept;

    // M12 live physical pages. A changed generation records one full derived
    // refresh of the current clipmap lattice; stable generations are free.
    void SetPhysicalPages(
        std::span<const terrain_gpu::GpuPhysicalSurfacePage> pages,
        u64 generation);

    // Hides the standing-water rendering (an "ocean off" view). Takes effect on
    // the next draw; no resources are rebuilt.
    void SetDrySurface(bool dry) noexcept;

    // Optics and sea level for the water pass. Takes effect on the next draw.
    void SetWaterOptics(const TerrainWaterOptics& optics) noexcept;

    // Draws standing water as its own object: a flat surface at sea level over
    // the clipmap, alpha-blended over the already-lit scene in `colorTarget`'s
    // pass. `terrainDepth` is the depth the terrain pass wrote; the pass tests
    // against it (that is the shoreline) and measures the water column with it.
    // Call after the terrain pass's Draw in the same frame. The render target
    // must be bound with SetRenderTargetsReadOnlyDepth(color, terrainDepth).
    void DrawWater(
        rhi::CommandList& commandList,
        u32 frameIndex,
        u32 targetWidth,
        u32 targetHeight,
        const TerrainPreviewCamera& camera,
        rhi::Texture& terrainDepth);

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
