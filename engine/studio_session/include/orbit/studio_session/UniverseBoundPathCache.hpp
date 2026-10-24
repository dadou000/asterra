#pragma once

#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/path_geometry/CorridorSweep.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace orbit::studio_session
{
// Derived path geometry is cache-only data expressed in a composed runtime
// frame. UniverseComposition may replace the complete FrameGraph while the
// semantic PathEdge remains unchanged, so all frame-bound products must be
// discarded when EditorWorldSession::UniverseGeneration changes.
class UniverseBoundPathCache
{
public:
    explicit UniverseBoundPathCache(
        editor_session::EditorWorldSession& world) noexcept;

    [[nodiscard]] bool RefreshBinding();

    // Build a corridor using the shared path derivation pipeline and publish
    // it to the same cache consumed by the existing Studio viewport renderer.
    // Not a per-frame operation: callers must track authored revisions.
    // Caller supplies a revision that changes whenever centerline, profile,
    // section, banking or generation settings change. Skip unchanged builds.
    // Do not use a simulation step counter as this revision.
    [[nodiscard]] bool StoreCorridorIfChanged(
        u64 authoredSignature,
        const path_geometry::PathCenterline& centerline,
        const paths::PathProfile& profile,
        const path_geometry::CorridorSweep& corridor,
        path_geometry::PathBuildOptions options = {},
        path_geometry::CorridorRefinementOptions refinement = {},
        std::optional<u64> routeGeneration = std::nullopt);

    void StoreCorridor(
        const path_geometry::PathCenterline& centerline,
        const paths::PathProfile& profile,
        const path_geometry::CorridorSweep& corridor,
        path_geometry::PathBuildOptions options = {},
        path_geometry::CorridorRefinementOptions refinement = {},
        std::optional<u64> routeGeneration = std::nullopt);

    void Store(
        path_geometry::PathDerivedProduct product,
        std::optional<u64> routeGeneration = std::nullopt);

    [[nodiscard]] const path_geometry::PathDerivedProduct*
    Find(scene::ObjectId edge) const noexcept;

    // Worker-safe immutable ownership of a published corridor. Retiring an
    // old universe cannot free a mesh still being read by junction workers.
    [[nodiscard]] std::shared_ptr<const path_geometry::PathDerivedProduct>
    Snapshot(scene::ObjectId edge) const noexcept;

    [[nodiscard]] std::optional<u64>
    RouteGeneration(scene::ObjectId edge) const noexcept;

    // Targeted semantic change notification: retains last-valid visual product
    // while marking it ineligible for a revision-cache hit.
    // Called after committed scene changes; compare only dependencies of cached edges.
    // Returns number of newly dirty/removed corridor products.
    [[nodiscard]] std::size_t RefreshSemanticInvalidation();
    void MarkDirty(scene::ObjectId edge) noexcept;
    [[nodiscard]] bool IsDirty(scene::ObjectId edge) const noexcept;
    struct RebuildRequest
    {
        u64 authoredSignature{0};
        path_geometry::PathCenterline centerline;
        paths::PathProfile profile;
        path_geometry::CorridorSweep corridor;
        path_geometry::PathBuildOptions options{};
        path_geometry::CorridorRefinementOptions refinement{};
        std::optional<u64> routeGeneration;
    };
    using RebuildResolver =
        std::function<std::optional<RebuildRequest>(scene::ObjectId)>;
    // Resolver snapshots only authoritative inputs on the caller thread.
    // CPU-heavy BuildCorridorDerived runs on the shared background pool;
    // completed results publish here only if the binding, invalidation token
    // and captured dependencies still match. The previous geometry remains
    // visible while an edit is rebuilding. Budget limits launches per tick.
    [[nodiscard]] std::size_t RebuildDirty(
        const RebuildResolver& resolver, std::size_t budget = 2);
    [[nodiscard]] std::string RebuildError(scene::ObjectId edge) const;
    [[nodiscard]] std::size_t DirtyCount() const noexcept;
    [[nodiscard]] bool Erase(scene::ObjectId edge) noexcept;
    void Clear() noexcept;

    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;

    [[nodiscard]] std::vector<const path_geometry::PathDerivedProduct*>
    Products() const;

    [[nodiscard]] u64 ObservedUniverseGeneration() const noexcept;
    [[nodiscard]] u64 BindingGeneration() const noexcept;

private:
    struct DependencySnapshot
    {
        std::vector<std::optional<schema::PropertyValue>> properties;
        [[nodiscard]] bool operator==(const DependencySnapshot&) const = default;
    };
    [[nodiscard]] DependencySnapshot CaptureDependencies(scene::ObjectId edge) const;
    struct PendingCorridor
    {
        std::future<path_geometry::PathDerivedProduct> result;
        DependencySnapshot sourceDependencies;
        u64 authoredSignature{0};
        u64 bindingGeneration{0};
        u64 dirtyToken{0};
        std::optional<u64> routeGeneration;
    };
    std::unordered_map<scene::ObjectId, PendingCorridor> pendingCorridors_;
    std::unordered_map<scene::ObjectId, u64> dirtyTokens_;
    u64 nextDirtyToken_{0};
    // Account also for discarded jobs that still occupy background workers.
    // Shared ownership keeps admission bookkeeping alive across cache resets.
    std::shared_ptr<std::atomic<u32>> activeCorridorJobs_{
        std::make_shared<std::atomic<u32>>(0U)};
    std::unordered_map<scene::ObjectId, DependencySnapshot> dependencySnapshots_;
    u64 observedSceneRevision_{~u64{0}};
    editor_session::EditorWorldSession* world_{nullptr};
    std::unordered_map<
        scene::ObjectId,
        std::shared_ptr<const path_geometry::PathDerivedProduct>>
        products_;
    std::unordered_map<scene::ObjectId, u64>
        routeGenerations_;
    std::unordered_map<scene::ObjectId, u64>
        corridorAuthoredSignatures_;
    std::unordered_set<scene::ObjectId> dirtyCorridors_;
    std::unordered_map<scene::ObjectId, std::string> rebuildErrors_;
    u64 observedUniverseGeneration_{~u64{0}};
    u64 bindingGeneration_{0};
};
} // namespace orbit::studio_session
