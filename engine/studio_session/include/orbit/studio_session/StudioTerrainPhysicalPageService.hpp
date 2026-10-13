#pragma once

#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/path_geometry/CorridorTerrainSource.hpp>
#include <orbit/studio_session/StudioTerrainRebuildScheduler.hpp>
#include <orbit/studio_session/StudioTerrainRuntimeBridge.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_erosion/RiverNetwork.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>
#include <orbit/terrain_debug/TerrainDebugPageData.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/terrain_water/LakeWater.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <memory>
#include <map>
#include <tuple>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace orbit::studio_session
{
struct StudioTerrainPhysicalPageSnapshot
{
    terrain::PhysicalTerrainPageAddress address{};
    u8 physicalLod{0U};
    terrain::TerrainGenerationRevisions revisions{};
    u64 invalidationRevision{0U};
    u64 revisionFingerprint{0U};
    bool cacheResident{false};

    std::shared_ptr<
        const terrain_material_column::MaterialColumnPage>
        material;

    // Canonical resolved physical-surface elevation for renderer, physics,
    // and tooling consumers. Derived from the final material column AFTER
    // construction, erosion and deposits; never from the analytic base
    // source or the corridor mesh. Coordinates are page-local [0,1].
    [[nodiscard]] std::optional<f64> SampleSurfaceHeight(
        f64 u, f64 v) const noexcept;



    std::shared_ptr<const terrain_erosion::RiverNetwork> rivers;
    std::shared_ptr<const terrain_hydrology::DrainagePage> drainage;
    std::shared_ptr<const terrain_water::LakeWaterField> lakes;

    std::shared_ptr<
        const terrain_debug::TerrainDebugPageData>
        debugPage;
};

struct StudioTerrainPhysicalPageConfig
{
    u32 resolution{33U};
    u32 rebuildRequestsPerTick{2U};
    f64 editDebounceSeconds{0.15};

    [[nodiscard]] bool IsValid() const noexcept;
};

// M12 production physical-page owner for Studio. It is derived runtime state:
// ObjectStore/SurfaceComposition remain semantic authority; M27 owns revision
// invalidation; M06 owns bounded async scheduling; M29 receives immutable
// snapshots only after the newest physical revision converges.
class StudioTerrainPhysicalPageService
{
public:
    StudioTerrainPhysicalPageService(
        editor_session::EditorWorldSession& world,
        terrain_debug::TerrainDebugLivePages& debugPages,
        StudioTerrainPhysicalPageConfig config = {});

    ~StudioTerrainPhysicalPageService();

    StudioTerrainPhysicalPageService(
        const StudioTerrainPhysicalPageService&) = delete;
    StudioTerrainPhysicalPageService& operator=(
        const StudioTerrainPhysicalPageService&) = delete;

    // Reconciles body/page interest against the current M03 viewport runtime.
    // The center physical page plus its four seam neighbors are kept live.
    void Sync(
        std::span<const StudioTerrainViewportRuntimeSnapshot> runtimes,
        time::SimulationTime atTime = {});

    // Stage an immutable construction snapshot, then invalidate exactly the
    // supplied old+new footprint pages. All addresses must belong to the
    // owning planetary body. An empty set is intentionally rejected.
    void StageCorridorConstruction(
        universe::BodyId body,
        std::shared_ptr<const path_geometry::CorridorTerrainSource> overlay,
        std::span<const terrain::PhysicalTerrainPageAddress> affectedPages);

    void QueueChange(
        const terrain_dependency::TerrainInvalidationRequest& request);

    void QueueChanges(
        std::span<const terrain_dependency::TerrainInvalidationRequest> requests);

    void Tick();
    void Tick(f64 deltaSeconds);
    void RebuildDirty();

    void SetPaused(bool paused) noexcept;
    [[nodiscard]] bool Paused() const noexcept;

    // Query the final material-column height of a published, ready page.
    // Returns no value for unloaded/unfinished pages; rendering can fall
    // back to its analytic source without blocking physical workers.
    struct HeightPageBatch
    {
        world::PlanetId planet{};
        u8 level{0};
        u64 revisionFingerprint{0};
        struct TileLess
        {
            [[nodiscard]] bool operator()(
                const world::PlanetTileId& a,
                const world::PlanetTileId& b) const noexcept
            {
                return std::tuple{static_cast<u8>(a.face),a.level,a.x,a.y} <
                       std::tuple{static_cast<u8>(b.face),b.level,b.x,b.y};
            }
        };
        std::map<world::PlanetTileId,
            std::shared_ptr<const StudioTerrainPhysicalPageSnapshot>,
            TileLess> pages;
        [[nodiscard]] std::optional<f64> Sample(
            const math::Double3& direction) const noexcept;
    };

    // Capture a coherent set of immutable published snapshots once per patch,
    // not once per vertex. Include missing pages in the fingerprint so their
    // later arrival triggers a patch rebuild.
    [[nodiscard]] HeightPageBatch CaptureHeightTiles(
        world::PlanetId planet, u8 level,
        std::span<const world::PlanetTileId> tiles) const;

    [[nodiscard]] HeightPageBatch CaptureHeightPages(
        world::PlanetId planet, u8 level,
        std::span<const math::Double3> directions) const;

    [[nodiscard]] std::optional<f64> SamplePublishedSurfaceHeight(
        world::PlanetId planet,
        const math::Double3& unitDirection,
        u8 pageLevel) const noexcept;

    [[nodiscard]] std::shared_ptr<
        const StudioTerrainPhysicalPageSnapshot>
    Find(
        const terrain::PhysicalTerrainPageAddress& address) const;

    [[nodiscard]] std::optional<u64> BeginUpload(
        const terrain::PhysicalTerrainPageAddress& address);

    [[nodiscard]] bool CompleteUpload(
        const terrain::PhysicalTerrainPageAddress& address,
        u64 revisionFingerprint,
        bool success,
        std::string_view error = {});

    [[nodiscard]] std::optional<StudioTerrainPageRebuildStatus>
    PageStatus(
        const terrain::PhysicalTerrainPageAddress& address) const;

    [[nodiscard]] std::optional<StudioTerrainBodyRebuildStatus>
    BodyStatus(
        world::PlanetId planet) const;

    // The per-body GPU terrain cache this service's runtime for `planet` was
    // bound to when it was created, or null without a runtime. A diagnostic:
    // it must always equal the cache the surface layer currently owns for the
    // body, or invalidations and publications would touch a freed cache.
    [[nodiscard]] const terrain_gpu::PersistentGpuTerrainCache*
    BoundCache(
        world::PlanetId planet) const noexcept;

    // Workers, running and queued jobs of the pool that builds physical pages.
    [[nodiscard]] jobs::JobSystemTelemetry JobTelemetry() const noexcept;

    // M13 presentation diagnostics. Value-only scheduler snapshots; callers
    // gain no build/product authority through this seam.
    [[nodiscard]] std::vector<StudioTerrainPageRebuildStatus>
    Catalog(
        world::PlanetId planet) const;

    void Clear();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::studio_session
