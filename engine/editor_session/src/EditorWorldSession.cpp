#include <orbit/editor_session/EditorWorldSession.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>

#include <stdexcept>
#include <utility>

namespace orbit::editor_session
{
struct EditorWorldSession::State
{
    State(
        documents::ProjectDocument& project,
        const std::filesystem::path& relativePath)
        : descriptor(
              project.DescribeWorld(relativePath)),
          world(
              project.RootDirectory() /
              descriptor.relativePath),
          objects(world),
          commands(objects, schemas)
    {
        editor_model::builtin::RegisterSchemas(
            schemas);
        editor_model::authoring_commands::Register(
            commandRegistry,
            commands,
            objects,
            selection);
        editor_model::authoring_commands::
            RegisterMaterialCommands(
                commandRegistry,
                commands,
                objects,
                selection);

        universeStats =
            universe.Rebuild(objects);
    }

    documents::WorldDescriptor descriptor;
    documents::WorldDatabase world;
    schema::SchemaRegistry schemas;
    scene::ObjectStore objects;
    selection::SelectionService selection;
    commands::CommandService commands;
    commands::CommandRegistry commandRegistry;
    world_model::UniverseComposition universe;
    world_model::UniverseCompositionStats
        universeStats{};
};

EditorWorldSession::EditorWorldSession(
    documents::ProjectDocument& project)
    : project_(&project)
{
    OpenStartupWorld();
}

EditorWorldSession::~EditorWorldSession() = default;
EditorWorldSession::EditorWorldSession(
    EditorWorldSession&&) noexcept = default;
EditorWorldSession& EditorWorldSession::operator=(
    EditorWorldSession&&) noexcept = default;

void EditorWorldSession::OpenStartupWorld()
{
    OpenWorld(
        project_->Manifest().startupWorld);
}

void EditorWorldSession::OpenWorld(
    const std::filesystem::path& relativePath)
{
    if (state_ != nullptr &&
        state_->commands.HasActiveTransaction())
    {
        throw std::logic_error(
            "Cannot switch worlds while an authoring transaction is active.");
    }

    // Build every world-scoped service first. A validation, schema or
    // composition failure leaves the currently active session untouched.
    auto candidate =
        std::make_unique<State>(
            *project_,
            relativePath);

    if (state_ != nullptr)
    {
        state_->world.Checkpoint();
    }

    state_ = std::move(candidate);
    ++generation_;
}

void EditorWorldSession::CloseWorld()
{
    if (state_ == nullptr)
    {
        return;
    }

    if (state_->commands.HasActiveTransaction())
    {
        throw std::logic_error(
            "Cannot close a world while an authoring transaction is active.");
    }

    state_->world.Checkpoint();
    state_.reset();
    ++generation_;
}

bool EditorWorldSession::HasWorld() const noexcept
{
    return state_ != nullptr;
}

u64 EditorWorldSession::Generation() const noexcept
{
    return generation_;
}

documents::ProjectDocument&
EditorWorldSession::Project() noexcept
{
    return *project_;
}

const documents::ProjectDocument&
EditorWorldSession::Project() const noexcept
{
    return *project_;
}

const documents::WorldDescriptor&
EditorWorldSession::ActiveWorld() const
{
    return RequireState().descriptor;
}

documents::WorldDatabase& EditorWorldSession::World()
{
    return RequireState().world;
}

const documents::WorldDatabase&
EditorWorldSession::World() const
{
    return RequireState().world;
}

schema::SchemaRegistry& EditorWorldSession::Schemas()
{
    return RequireState().schemas;
}

const schema::SchemaRegistry&
EditorWorldSession::Schemas() const
{
    return RequireState().schemas;
}

scene::ObjectStore& EditorWorldSession::Objects()
{
    return RequireState().objects;
}

const scene::ObjectStore& EditorWorldSession::Objects() const
{
    return RequireState().objects;
}

selection::SelectionService&
EditorWorldSession::Selection()
{
    return RequireState().selection;
}

const selection::SelectionService&
EditorWorldSession::Selection() const
{
    return RequireState().selection;
}

commands::CommandService& EditorWorldSession::Commands()
{
    return RequireState().commands;
}

const commands::CommandService&
EditorWorldSession::Commands() const
{
    return RequireState().commands;
}

commands::CommandRegistry&
EditorWorldSession::CommandRegistry()
{
    return RequireState().commandRegistry;
}

const commands::CommandRegistry&
EditorWorldSession::CommandRegistry() const
{
    return RequireState().commandRegistry;
}

world_model::UniverseComposition&
EditorWorldSession::Universe()
{
    return RequireState().universe;
}

const world_model::UniverseComposition&
EditorWorldSession::Universe() const
{
    return RequireState().universe;
}

world_model::UniverseCompositionStats
EditorWorldSession::RebuildUniverse()
{
    auto& state = RequireState();
    state.universeStats =
        state.universe.Rebuild(
            state.objects);
    return state.universeStats;
}

bool EditorWorldSession::RefreshUniverseIfChanged()
{
    auto& state = RequireState();

    if (state.universe.SourceRevision() ==
        state.objects.Revision())
    {
        return false;
    }

    state.universeStats =
        state.universe.Rebuild(
            state.objects);
    return true;
}

const world_model::UniverseCompositionStats&
EditorWorldSession::UniverseStats() const
{
    return RequireState().universeStats;
}

void EditorWorldSession::Checkpoint()
{
    RequireState().world.Checkpoint();
}

EditorWorldSession::State&
EditorWorldSession::RequireState()
{
    if (state_ == nullptr)
    {
        throw std::logic_error(
            "No world is open in the editor session.");
    }

    return *state_;
}

const EditorWorldSession::State&
EditorWorldSession::RequireState() const
{
    if (state_ == nullptr)
    {
        throw std::logic_error(
            "No world is open in the editor session.");
    }

    return *state_;
}
} // namespace orbit::editor_session
