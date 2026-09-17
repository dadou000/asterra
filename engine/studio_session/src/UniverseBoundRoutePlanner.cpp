#include <orbit/studio_session/UniverseBoundRoutePlanner.hpp>

#include <stdexcept>

namespace orbit::studio_session
{
UniverseBoundRoutePlanner::UniverseBoundRoutePlanner(
    editor_session::EditorWorldSession& world)
    : world_(&world)
{
    static_cast<void>(RefreshBinding());
}

bool UniverseBoundRoutePlanner::RefreshBinding()
{
    if (world_ == nullptr)
    {
        return false;
    }

    const u64 currentGeneration =
        world_->UniverseGeneration();

    if (currentGeneration ==
        observedUniverseGeneration_)
    {
        return false;
    }

    // Drop every route entry before binding to the new composition. Route
    // requests resolve FrameGraph/BodyRegistry data synchronously before jobs
    // are scheduled, so in-flight workers own immutable resolved requests and
    // do not retain either registry reference.
    planner_.reset();
    observedUniverseGeneration_ =
        currentGeneration;

    if (world_->HasWorld())
    {
        planner_ =
            std::make_unique<path_routing::RoutePlanner>(
                jobs_,
                world_->Universe().Frames(),
                world_->Universe().Bodies());
    }

    ++bindingGeneration_;
    return true;
}

bool UniverseBoundRoutePlanner::HasPlanner() const noexcept
{
    return
        world_ != nullptr &&
        world_->HasWorld() &&
        planner_ != nullptr &&
        observedUniverseGeneration_ ==
            world_->UniverseGeneration();
}

path_routing::RoutePlanner&
UniverseBoundRoutePlanner::Planner()
{
    static_cast<void>(RefreshBinding());

    if (planner_ == nullptr)
    {
        throw std::logic_error(
            "No active world is available for Studio path routing.");
    }

    return *planner_;
}

const path_routing::RoutePlanner&
UniverseBoundRoutePlanner::Planner() const
{
    if (!HasPlanner())
    {
        throw std::logic_error(
            "Studio route planner binding is stale or no world is open.");
    }

    return *planner_;
}

u64 UniverseBoundRoutePlanner::ObservedUniverseGeneration() const noexcept
{
    return observedUniverseGeneration_;
}

u64 UniverseBoundRoutePlanner::BindingGeneration() const noexcept
{
    return bindingGeneration_;
}
} // namespace orbit::studio_session
