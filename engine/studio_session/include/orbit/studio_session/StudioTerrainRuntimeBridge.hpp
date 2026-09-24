#pragma once

#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/studio_session/ViewportTargetRegistry.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_debug/TerrainDebugLivePages.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>
#include <orbit/terrain_stream/ToroidalResidency.hpp>
#include <orbit/terrain_view/ClipmapLayout.hpp>
#include <orbit/terrain_view/ClipmapTracker.hpp>
#include <orbit/world/WorldPosition.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_session
{
struct StudioTerrainRuntimeConfig
{
    terrain_view::ClipmapConfig clipmap{
        .levelCount = 12U,
        .gridResolution = 129U,
        .baseSpacingMeters = 1.0,
        .levelScale = 2.0,
        .overlapCells = 6U
    };

    terrain_view::AdaptiveClipmapCoverageConfig
        adaptiveCoverage{};

    // Physical page selection is diagnostic/process identity, not render LOD.
    u8 physicalPageLevel{8U};

    f64 defaultObserverAltitudeMeters{10'000.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Value-only snapshot of one viewport's current production-terrain binding.
// No pointer into SurfaceComposition/TerrainBodyServices is retained here.
struct StudioTerrainViewportRuntimeSnapshot
{
    std::string viewportId;

    u64 worldGeneration{0U};
    u64 universeGeneration{0U};
    u64 runtimeGeneration{0U};

    scene::ObjectId semanticBody{};
    universe::BodyId body{};
    scene::ObjectId terrainObject{};

    world::PlanetDefinition planet{};
    world::WorldPosition observer{};

    terrain::PhysicalTerrainPageAddress
        observerPhysicalPage{};
    u8 physicalPageLevel{0U};

    u64 terrainSourceRevision{0U};
    u64 surfaceSourceRevision{0U};

    u32 adaptiveCoverageTier{0U};
    terrain_view::ClipmapConfig clipmap{};
    terrain_view::ClipmapLayout layout{};
    terrain_view::ClipmapMotionUpdate motion{};
    terrain_stream::ResidencyUpdate residency{};

    // Dirty production clipmap regions produced by the same
    // ClipmapTracker/ToroidalResidency contracts used by terrain rendering.
    std::vector<terrain_stream::TerrainSampleRequest>
        sampleRequests;

    terrain_gpu::PersistentGpuTerrainCacheStats
        cacheStats{};
};

// Session-side bridge between logical Studio viewport targets and the
// production terrain streaming contracts. It owns only derived view interest;
// semantic terrain authority stays in EditorWorldSession/SurfaceComposition.
class StudioTerrainRuntimeBridge
{
public:
    StudioTerrainRuntimeBridge(
        editor_session::EditorWorldSession& world,
        ViewportTargetRegistry& viewports,
        terrain_debug::TerrainDebugLivePages& debugPages,
        StudioTerrainRuntimeConfig config = {});

    ~StudioTerrainRuntimeBridge();

    StudioTerrainRuntimeBridge(
        const StudioTerrainRuntimeBridge&) = delete;
    StudioTerrainRuntimeBridge& operator=(
        const StudioTerrainRuntimeBridge&) = delete;

    // Rebinds after world/universe/viewport target changes. Returns true only
    // when a viewport runtime was created, replaced or removed.
    [[nodiscard]] bool Refresh();

    void Clear() noexcept;

    // Presentation-only observer motion. This changes clipmap/residency plans
    // and the selected physical page, but never semantic/terrain revisions.
    [[nodiscard]] bool SetObserver(
        std::string_view viewportId,
        const world::WorldPosition& observer);

    [[nodiscard]] std::optional<
        StudioTerrainViewportRuntimeSnapshot>
    Capture(std::string_view viewportId) const;

    [[nodiscard]] std::vector<
        StudioTerrainViewportRuntimeSnapshot>
    Catalog() const;

    [[nodiscard]] bool IsCurrent(
        const StudioTerrainViewportRuntimeSnapshot& snapshot) const noexcept;

    // Generation-safe live access. Callers must not retain these references
    // beyond the snapshot's lifetime/currentness.
    [[nodiscard]] const terrain::TerrainSource& TerrainSource(
        const StudioTerrainViewportRuntimeSnapshot& snapshot) const;

    [[nodiscard]] terrain_gpu::PersistentGpuTerrainCache& Cache(
        const StudioTerrainViewportRuntimeSnapshot& snapshot);

    [[nodiscard]] const terrain_gpu::PersistentGpuTerrainCache& Cache(
        const StudioTerrainViewportRuntimeSnapshot& snapshot) const;

    // M03 establishes the publication seam. M12 will feed complete live
    // physical solver products through this method.
    void PublishDebugPage(
        const StudioTerrainViewportRuntimeSnapshot& snapshot,
        std::shared_ptr<const terrain_debug::TerrainDebugPageData> page);

private:
    struct RuntimeState;

    struct ObserverHistory
    {
        scene::ObjectId semanticBody{};
        world::PlanetId planet{};
        world::WorldPosition observer{};
    };

    [[nodiscard]] std::unique_ptr<RuntimeState>
    BuildRuntime(
        std::string viewportId,
        const ViewportTargetState& target,
        std::optional<world::WorldPosition> previousObserver);

    void UpdateObserverPlan(
        RuntimeState& runtime,
        const world::WorldPosition& observer);

    [[nodiscard]] StudioTerrainViewportRuntimeSnapshot
    Snapshot(const RuntimeState& runtime) const;

    [[nodiscard]] RuntimeState&
    RequireCurrent(
        const StudioTerrainViewportRuntimeSnapshot& snapshot);

    [[nodiscard]] const RuntimeState&
    RequireCurrent(
        const StudioTerrainViewportRuntimeSnapshot& snapshot) const;

    editor_session::EditorWorldSession* world_{nullptr};
    ViewportTargetRegistry* viewports_{nullptr};
    terrain_debug::TerrainDebugLivePages* debugPages_{nullptr};

    StudioTerrainRuntimeConfig config_{};

    std::map<
        std::string,
        std::unique_ptr<RuntimeState>,
        std::less<>>
        runtimes_;

    std::map<
        std::string,
        ObserverHistory,
        std::less<>>
        observerHistory_;

    u64 observedWorldGeneration_{~u64{0}};
    u64 observedUniverseGeneration_{~u64{0}};
    u64 nextRuntimeGeneration_{1U};
};
} // namespace orbit::studio_session
