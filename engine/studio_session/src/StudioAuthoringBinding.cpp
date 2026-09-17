#include <orbit/studio_session/StudioAuthoringBinding.hpp>

#include <orbit/studio_session/StudioSession.hpp>

#include <stdexcept>

namespace orbit::studio_session
{
StudioAuthoringBinding::StudioAuthoringBinding(
    StudioSession& session) noexcept
    : session_(&session)
{
}

bool StudioAuthoringBinding::IsWorldCurrent(
    const StudioRuntimeSnapshot& snapshot) const noexcept
{
    if (session_ == nullptr)
    {
        return false;
    }

    const auto& world = session_->World();
    return
        snapshot.hasWorld &&
        world.HasWorld() &&
        snapshot.worldGeneration ==
            world.Generation();
}

scene::ObjectStore& StudioAuthoringBinding::Objects(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrentWorld(snapshot);
    return session_->World().Objects();
}

selection::SelectionService&
StudioAuthoringBinding::Selection(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrentWorld(snapshot);
    return session_->World().Selection();
}

commands::CommandService&
StudioAuthoringBinding::Commands(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrentWorld(snapshot);
    return session_->World().Commands();
}

commands::CommandRegistry&
StudioAuthoringBinding::CommandRegistry(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrentWorld(snapshot);
    return session_->World().CommandRegistry();
}

editor_model::CommandSurfaceRegistry&
StudioAuthoringBinding::CommandSurfaces(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrentWorld(snapshot);
    return session_->World().CommandSurfaces();
}

editor_model::ExplorerModel&
StudioAuthoringBinding::Explorer(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrentWorld(snapshot);
    return session_->World().Explorer();
}

editor_model::InspectorModel&
StudioAuthoringBinding::Inspector(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrentWorld(snapshot);
    return session_->World().Inspector();
}

plugins::PluginManager& StudioAuthoringBinding::Plugins(
    const StudioRuntimeSnapshot& snapshot) const
{
    RequireCurrentWorld(snapshot);
    return session_->World().Plugins();
}

void StudioAuthoringBinding::RequireCurrentWorld(
    const StudioRuntimeSnapshot& snapshot) const
{
    if (!IsWorldCurrent(snapshot))
    {
        throw std::logic_error(
            "Studio authoring snapshot is stale or no world is open; refresh before accessing world-scoped services.");
    }
}
} // namespace orbit::studio_session
