#pragma once

#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_rpc/EditorSessionRpcHost.hpp>
#include <orbit/editor_session/ActiveBodyModel.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/editor_session/WorldDocumentsModel.hpp>
#include <orbit/studio_session/SimulationClock.hpp>
#include <orbit/studio_session/CorridorConstructionRegistry.hpp>
#include <orbit/studio_session/StudioTerrainPerformanceDiagnostics.hpp>
#include <orbit/studio_session/StudioTerrainBakeController.hpp>
#include <orbit/studio_session/StudioTerrainPhysicalPageService.hpp>
#include <orbit/studio_session/StudioTerrainRuntimeBridge.hpp>
#include <orbit/terrain_dependency/TerrainDependencyGraph.hpp>
#include <orbit/studio_session/UniverseBoundPathCache.hpp>
#include <orbit/studio_session/UniverseBoundRoutePlanner.hpp>
#include <orbit/studio_session/ViewportTargetRegistry.hpp>
#include <orbit/studio_session/WorldBoundPathNetwork.hpp>
#include <orbit/terrain_debug/TerrainDebugLivePages.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_session
{
struct StudioTickResult
{
    // True only when semantic ObjectStore authority changed and the
    // UniverseComposition + SurfaceComposition pair was rebuilt this tick.
    bool compositionChanged{false};
    bool activeBodyChanged{false};
    bool viewportTargetsChanged{false};
    bool terrainRuntimeChanged{false};
    bool pathNetworkRebound{false};
    bool pathRoutingRebound{false};
    bool pathProductsInvalidated{false};
    u32 pluginsReloaded{0};
    u64 worldGeneration{0};
    u64 universeGeneration{0};
};

// Application-facing composition root for Orbit Studio's authoritative
// authoring session. World lifecycle always passes through the session-aware
// RPC boundary so its world-bound handlers are rebound together with the
// EditorWorldSession. UI, MCP and plugins therefore observe one service graph.
class StudioSession
{
public:
    explicit StudioSession(
        documents::ProjectDocument& project);

    [[nodiscard]] editor_session::EditorWorldSession&
    World() noexcept;
    [[nodiscard]] const editor_session::EditorWorldSession&
    World() const noexcept;

    [[nodiscard]] editor_session::ActiveBodyModel&
    ActiveBody() noexcept;
    [[nodiscard]] const editor_session::ActiveBodyModel&
    ActiveBody() const noexcept;

    [[nodiscard]] ViewportTargetRegistry&
    Viewports() noexcept;
    [[nodiscard]] const ViewportTargetRegistry&
    Viewports() const noexcept;

    [[nodiscard]] WorldBoundPathNetwork&
    PathNetwork() noexcept;
    [[nodiscard]] const WorldBoundPathNetwork&
    PathNetwork() const noexcept;

    [[nodiscard]] UniverseBoundRoutePlanner&
    PathRouting() noexcept;
    [[nodiscard]] const UniverseBoundRoutePlanner&
    PathRouting() const noexcept;

    [[nodiscard]] UniverseBoundPathCache&
    PathProducts() noexcept;
    [[nodiscard]] const UniverseBoundPathCache&
    PathProducts() const noexcept;

    [[nodiscard]] terrain_debug::TerrainDebugLivePages&
    TerrainDebugPages() noexcept;
    [[nodiscard]] const terrain_debug::TerrainDebugLivePages&
    TerrainDebugPages() const noexcept;

    // Select a bounded sparse set of authored construction pages for the
    // viewport GPU compositor and asynchronous physical residency.
    [[nodiscard]] std::vector<terrain::PhysicalTerrainPageAddress>
    ConstructionPhysicalPages(universe::BodyId body,
        std::size_t maximumPages = 8U,
        const math::Double3* observerDirection = nullptr,
        std::string_view viewportId = {}) const;

    [[nodiscard]] StudioTerrainPhysicalPageService&
    TerrainPhysicalPages() noexcept;
    [[nodiscard]] const StudioTerrainPhysicalPageService&
    TerrainPhysicalPages() const noexcept;

    [[nodiscard]] StudioTerrainRuntimeBridge&
    TerrainRuntime() noexcept;
    [[nodiscard]] const StudioTerrainRuntimeBridge&
    TerrainRuntime() const noexcept;

    [[nodiscard]] StudioTerrainPerformanceDiagnostics&
    TerrainPerformance() noexcept;
    [[nodiscard]] const StudioTerrainPerformanceDiagnostics&
    TerrainPerformance() const noexcept;

    [[nodiscard]] SimulationClock& Clock() noexcept;
    [[nodiscard]] const SimulationClock& Clock() const noexcept;

    // Semantic terrain tools enqueue only bounded M27 scopes here. M12
    // drains them into the production rebuild scheduler.
    void QueueTerrainInvalidation(
        const terrain_dependency::TerrainInvalidationRequest& request);

    // Explicit physical-page bridge for infrastructure construction. The caller
    // must project from the owning planetary body before calling this method.
    // One radius-zero authoring request per unique page; never global.
    // Returns the number of newly queued requests.
    // Resolve a derived corridor from its authored frame into an explicitly
    // selected body's rotating frame. Rejects missing/ambiguous body bindings,
    // excessive spans, and unavailable terrain runtime pages.
    [[nodiscard]] std::size_t QueueInfrastructureCorridorTerrain(
        const path_geometry::PathDerivedProduct& product,
        universe::BodyId body,
        f64 influenceRadiusMeters,
        std::size_t maximumPages = 4096);

    [[nodiscard]] std::size_t QueueInfrastructureTerrainPages(
        std::span<const terrain::PhysicalTerrainPageAddress> pages,
        std::size_t maximumPages = 4096);

    void QueueTerrainInvalidations(
        std::span<const terrain_dependency::TerrainInvalidationRequest> requests);

    [[nodiscard]] std::span<
        const terrain_dependency::TerrainInvalidationRequest>
    PendingTerrainInvalidations() const noexcept;

    [[nodiscard]] std::vector<
        terrain_dependency::TerrainInvalidationRequest>
    TakeTerrainInvalidations();

    [[nodiscard]] std::vector<editor_session::WorldDocumentItem>
    Worlds() const;
    [[nodiscard]] std::optional<editor_session::WorldDocumentItem>
    ActiveWorld() const;

    [[nodiscard]] documents::WorldDescriptor CreateWorld(
        const std::filesystem::path& relativePath,
        std::string_view displayName);
    [[nodiscard]] documents::WorldDescriptor RenameWorld(
        const std::filesystem::path& relativePath,
        std::string_view displayName);
    [[nodiscard]] documents::WorldDescriptor SetStartupWorld(
        const std::filesystem::path& relativePath);

    void OpenWorld(
        const std::filesystem::path& relativePath);
    void CloseWorld();

    [[nodiscard]] std::optional<std::string>
    DispatchRpc(std::string_view payload);

    [[nodiscard]] editor_rpc::EditorSessionRpcHost&
    Rpc() noexcept;
    [[nodiscard]] const editor_rpc::EditorSessionRpcHost&
    Rpc() const noexcept;

    [[nodiscard]] StudioTickResult Tick(bool pollPlugins = true);

    // Planet bake (tectonics, rivers next): status, explicit bake, cancel.
    [[nodiscard]] StudioTerrainBakeController& TerrainBake() noexcept;
    [[nodiscard]] const StudioTerrainBakeController& TerrainBake() const noexcept;

private:
    void RefreshTerrainDebugGeneration();

    void DispatchWorldLifecycle(
        std::string_view method,
        std::optional<std::filesystem::path> path);

    editor_session::EditorWorldSession world_;
    editor_session::WorldDocumentsModel documents_;
    editor_session::ActiveBodyModel activeBody_;
    ViewportTargetRegistry viewports_;
    WorldBoundPathNetwork pathNetwork_;
    UniverseBoundRoutePlanner pathRouting_;
    UniverseBoundPathCache pathProducts_;
    CorridorConstructionRegistry corridorConstruction_;
    struct ConstructionSelection
    {
        universe::BodyId body{};
        u64 worldGeneration{0U};
        std::vector<terrain::PhysicalTerrainPageAddress> pages;
    };
    mutable std::map<std::string, ConstructionSelection>
        constructionSelections_;
    terrain_debug::TerrainDebugLivePages terrainDebugPages_;
    StudioTerrainBakeController terrainBake_;
    StudioTerrainPhysicalPageService terrainPhysicalPages_;
    StudioTerrainRuntimeBridge terrainRuntime_;
    StudioTerrainPerformanceDiagnostics terrainPerformance_;
    SimulationClock clock_;
    std::vector<
        terrain_dependency::TerrainInvalidationRequest>
        pendingTerrainInvalidations_;
    u64 terrainDebugUniverseGeneration_{~u64{0}};
    // World generation the physical page service was last reconciled with.
    u64 terrainPagesWorldGeneration_{~u64{0}};
    u64 infrastructureDiscoveryRevision_{~u64{0}};
    u64 infrastructureDiscoveryWorldGeneration_{~u64{0}};
    editor_rpc::EditorSessionRpcHost rpc_;
};
} // namespace orbit::studio_session
