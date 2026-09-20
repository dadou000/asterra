#pragma once

#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_dependency/TerrainDependencyGraph.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_session
{
enum class TerrainRebuildState : u8
{
    Clean,
    Dirty,
    Queued,
    BuildingCpu,
    BuildingGpu,
    Uploading,
    Ready,
    Failed,
    StaleReplaced
};

[[nodiscard]] const char* TerrainRebuildStateName(
    TerrainRebuildState state) noexcept;

struct StudioTerrainRebuildConfig
{
    f64 editDebounceSeconds{0.15};
    u32 maxBuildRequestsPerTick{4U};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct StudioTerrainPageRebuildStatus
{
    terrain::PhysicalTerrainPageAddress address{};
    TerrainRebuildState state{TerrainRebuildState::Clean};

    terrain_dependency::TerrainDependencyProductMask dirtyProducts{0U};
    u32 completedProducts{0U};
    u32 totalProducts{0U};

    u64 revisionFingerprint{0U};
    u64 staleRejected{0U};
    std::string error;
};

struct StudioTerrainBodyRebuildStatus
{
    world::PlanetId planet{};
    TerrainRebuildState state{TerrainRebuildState::Clean};

    u32 pages{0U};
    u32 dirtyPages{0U};
    u32 queuedPages{0U};
    u32 buildingPages{0U};
    u32 uploadingPages{0U};
    u32 readyPages{0U};
    u32 failedPages{0U};
    u32 stalePages{0U};

    u32 completedProducts{0U};
    u32 totalProducts{0U};
    f64 progress{1.0};

    u64 staleRejected{0U};
    bool paused{false};
};

// Editor-facing M06 controller around the frozen M27 dependency graph.
// It owns no terrain products: it only coalesces authority changes, bounds
// build requests, exposes progress, and rejects obsolete upload completion.
// M27/ProceduralGraph remain the build/dependency/revision authority.
class StudioTerrainRebuildScheduler
{
public:
    explicit StudioTerrainRebuildScheduler(
        terrain_dependency::TerrainDependencyGraph& graph,
        StudioTerrainRebuildConfig config = {});

    void RegisterPage(
        const terrain::PhysicalTerrainPageAddress& address,
        terrain::TerrainGenerationRevisions revisions = {});

    [[nodiscard]] bool ContainsPage(
        const terrain::PhysicalTerrainPageAddress& address) const noexcept;

    // Debounced/coalesced authority change. No build is submitted here.
    void QueueChange(
        const terrain_dependency::TerrainInvalidationRequest& request);

    // Finalizes completed M27 jobs, flushes expired edits, and submits at most
    // maxBuildRequestsPerTick targets. This call never waits for terrain jobs.
    void Tick(f64 deltaSeconds);

    // Flushes pending edit debounce immediately and submits one bounded frame
    // of work. Pause still prevents regeneration.
    void RebuildDirty();

    void SetPaused(bool paused) noexcept;
    [[nodiscard]] bool Paused() const noexcept;

    // Explicit render/RHI upload seam. BeginUpload captures the current
    // physical revision. CompleteUpload accepts only that same revision;
    // completion from an older authority revision is rejected as stale.
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

    [[nodiscard]] std::vector<StudioTerrainPageRebuildStatus>
    Catalog() const;

    [[nodiscard]] StudioTerrainBodyRebuildStatus BodyStatus(
        world::PlanetId planet) const;

private:
    struct PendingChange
    {
        terrain_dependency::TerrainInvalidationRequest request{};
        f64 remainingSeconds{0.0};
    };

    struct PageEntry
    {
        terrain::PhysicalTerrainPageAddress address{};
        terrain_dependency::TerrainDependencyProductMask dirtyProducts{0U};
        terrain_dependency::TerrainDependencyProductMask pendingProducts{0U};
        terrain_dependency::TerrainDependencyProductMask requestedProducts{0U};
        terrain_dependency::TerrainDependencyProductMask cycleProducts{0U};

        std::array<
            u64,
            terrain_dependency::kTerrainDependencyProductCount>
            observedStaleCompletions{};

        u64 staleRejected{0U};
        u32 stalePulseTicks{0U};

        bool uploading{false};
        u64 uploadRevisionFingerprint{0U};
        bool uploadFailed{false};
        std::string uploadError;
    };

    [[nodiscard]] PageEntry* FindPage(
        const terrain::PhysicalTerrainPageAddress& address) noexcept;
    [[nodiscard]] const PageEntry* FindPage(
        const terrain::PhysicalTerrainPageAddress& address) const noexcept;

    [[nodiscard]] static bool AddressMatches(
        const terrain::PhysicalTerrainPageAddress& address,
        const terrain_dependency::TerrainSpatialInvalidationScope& scope);

    void FlushChanges(bool all);
    void RecomputePendingProducts();
    void RefreshPage(PageEntry& page);
    void RefreshAll();
    void ScheduleBudget();

    [[nodiscard]] std::optional<
        terrain_dependency::TerrainDependencyProduct>
    NextTarget(const PageEntry& page) const noexcept;

    [[nodiscard]] StudioTerrainPageRebuildStatus
    MakeStatus(const PageEntry& page) const;

    terrain_dependency::TerrainDependencyGraph* graph_{nullptr};
    StudioTerrainRebuildConfig config_{};
    bool paused_{false};

    std::vector<PageEntry> pages_;
    std::vector<PendingChange> pendingChanges_;
};
} // namespace orbit::studio_session
