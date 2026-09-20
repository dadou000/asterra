#pragma once

#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/studio_session/StudioTerrainRebuildScheduler.hpp>
#include <orbit/studio_session/StudioTerrainRuntimeBridge.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_debug/TerrainDebugPageData.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <memory>
#include <optional>
#include <span>
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
        std::span<const StudioTerrainViewportRuntimeSnapshot> runtimes);

    void QueueChange(
        const terrain_dependency::TerrainInvalidationRequest& request);

    void QueueChanges(
        std::span<const terrain_dependency::TerrainInvalidationRequest> requests);

    void Tick();
    void Tick(f64 deltaSeconds);
    void RebuildDirty();

    void SetPaused(bool paused) noexcept;
    [[nodiscard]] bool Paused() const noexcept;

    [[nodiscard]] std::shared_ptr<
        const StudioTerrainPhysicalPageSnapshot>
    Find(
        const terrain::PhysicalTerrainPageAddress& address) const;

    [[nodiscard]] std::optional<StudioTerrainPageRebuildStatus>
    PageStatus(
        const terrain::PhysicalTerrainPageAddress& address) const;

    [[nodiscard]] std::optional<StudioTerrainBodyRebuildStatus>
    BodyStatus(
        world::PlanetId planet) const;

    void Clear();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::studio_session
