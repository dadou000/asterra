#pragma once

#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>

#include <memory>

namespace orbit::studio_session
{
// Owns route-planner state whose lifetime is tied to the current composed
// FrameGraph + BodyRegistry. UniverseComposition replaces both objects on a
// rebuild, so retaining a RoutePlanner across EditorWorldSession universe
// generations would leave dangling references. This binding makes that
// invalidation explicit and deterministic for Studio/runtime consumers.
class UniverseBoundRoutePlanner
{
public:
    explicit UniverseBoundRoutePlanner(
        editor_session::EditorWorldSession& world);

    UniverseBoundRoutePlanner(
        const UniverseBoundRoutePlanner&) = delete;
    UniverseBoundRoutePlanner& operator=(
        const UniverseBoundRoutePlanner&) = delete;

    // Rebinds to the active universe when its monotonic generation changed.
    // Returns true when planner state was destroyed/recreated or invalidated
    // by closing the active world.
    [[nodiscard]] bool RefreshBinding();

    [[nodiscard]] bool HasPlanner() const noexcept;

    // Refreshes the binding before returning the planner. Throws while no
    // authoring world is open.
    [[nodiscard]] path_routing::RoutePlanner& Planner();
    [[nodiscard]] const path_routing::RoutePlanner& Planner() const;

    [[nodiscard]] u64 ObservedUniverseGeneration() const noexcept;
    [[nodiscard]] u64 BindingGeneration() const noexcept;

private:
    editor_session::EditorWorldSession* world_{nullptr};
    jobs::JobSystem jobs_;
    std::unique_ptr<path_routing::RoutePlanner> planner_;
    u64 observedUniverseGeneration_{~u64{0}};
    u64 bindingGeneration_{0};
};
} // namespace orbit::studio_session
