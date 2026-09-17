#pragma once

#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/editor_model/ExplorerModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/plugins/PluginManager.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>

namespace orbit::studio_session
{
class StudioSession;

// Generation-checked access to services whose lifetime is scoped to an open
// .orbitworld but not to a particular UniverseComposition instance. A body
// property edit may replace FrameGraph/BodyRegistry while these facades remain
// valid; a world switch or close invalidates them immediately.
class StudioAuthoringBinding
{
public:
    explicit StudioAuthoringBinding(
        StudioSession& session) noexcept;

    [[nodiscard]] bool IsWorldCurrent(
        const StudioRuntimeSnapshot& snapshot) const noexcept;

    [[nodiscard]] scene::ObjectStore& Objects(
        const StudioRuntimeSnapshot& snapshot) const;
    [[nodiscard]] selection::SelectionService& Selection(
        const StudioRuntimeSnapshot& snapshot) const;
    [[nodiscard]] commands::CommandService& Commands(
        const StudioRuntimeSnapshot& snapshot) const;
    [[nodiscard]] commands::CommandRegistry& CommandRegistry(
        const StudioRuntimeSnapshot& snapshot) const;
    [[nodiscard]] editor_model::CommandSurfaceRegistry& CommandSurfaces(
        const StudioRuntimeSnapshot& snapshot) const;
    [[nodiscard]] editor_model::ExplorerModel& Explorer(
        const StudioRuntimeSnapshot& snapshot) const;
    [[nodiscard]] editor_model::InspectorModel& Inspector(
        const StudioRuntimeSnapshot& snapshot) const;
    [[nodiscard]] plugins::PluginManager& Plugins(
        const StudioRuntimeSnapshot& snapshot) const;

private:
    void RequireCurrentWorld(
        const StudioRuntimeSnapshot& snapshot) const;

    StudioSession* session_{nullptr};
};
} // namespace orbit::studio_session
