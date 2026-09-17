#pragma once

#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/editor_model/ExplorerModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/plugins/PluginManager.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/surface_model/SurfaceComposition.hpp>
#include <orbit/world_model/UniverseComposition.hpp>

#include <filesystem>
#include <memory>

namespace orbit::editor_session
{
// Owns every service whose lifetime is scoped to one authoritative
// .orbitworld document. Switching worlds constructs a complete candidate
// session first, then atomically replaces the old service graph. Any facade
// that retains references into world authority (Explorer, Inspector, plugins,
// command surfaces) lives inside the same state and is destroyed before the
// referenced services.
class EditorWorldSession
{
public:
    explicit EditorWorldSession(
        documents::ProjectDocument& project);
    ~EditorWorldSession();

    EditorWorldSession(
        const EditorWorldSession&) = delete;
    EditorWorldSession& operator=(
        const EditorWorldSession&) = delete;

    EditorWorldSession(
        EditorWorldSession&&) noexcept;
    EditorWorldSession& operator=(
        EditorWorldSession&&) noexcept;

    void OpenStartupWorld();
    void OpenWorld(
        const std::filesystem::path& relativePath);
    void CloseWorld();

    [[nodiscard]] bool HasWorld() const noexcept;
    [[nodiscard]] u64 Generation() const noexcept;

    [[nodiscard]] documents::ProjectDocument&
    Project() noexcept;
    [[nodiscard]] const documents::ProjectDocument&
    Project() const noexcept;

    [[nodiscard]] const documents::WorldDescriptor&
    ActiveWorld() const;

    [[nodiscard]] documents::WorldDatabase& World();
    [[nodiscard]] const documents::WorldDatabase& World() const;

    [[nodiscard]] schema::SchemaRegistry& Schemas();
    [[nodiscard]] const schema::SchemaRegistry& Schemas() const;

    [[nodiscard]] scene::ObjectStore& Objects();
    [[nodiscard]] const scene::ObjectStore& Objects() const;

    [[nodiscard]] selection::SelectionService& Selection();
    [[nodiscard]] const selection::SelectionService& Selection() const;

    [[nodiscard]] commands::CommandService& Commands();
    [[nodiscard]] const commands::CommandService& Commands() const;

    [[nodiscard]] commands::CommandRegistry& CommandRegistry();
    [[nodiscard]] const commands::CommandRegistry& CommandRegistry() const;

    [[nodiscard]] editor_model::CommandSurfaceRegistry&
    CommandSurfaces();
    [[nodiscard]] const editor_model::CommandSurfaceRegistry&
    CommandSurfaces() const;

    [[nodiscard]] editor_model::ExplorerModel& Explorer();
    [[nodiscard]] const editor_model::ExplorerModel& Explorer() const;

    [[nodiscard]] editor_model::InspectorModel& Inspector();
    [[nodiscard]] const editor_model::InspectorModel& Inspector() const;

    [[nodiscard]] plugins::PluginManager& Plugins();
    [[nodiscard]] const plugins::PluginManager& Plugins() const;

    [[nodiscard]] world_model::UniverseComposition& Universe();
    [[nodiscard]] const world_model::UniverseComposition& Universe() const;

    [[nodiscard]] surface_model::SurfaceComposition& Surfaces();
    [[nodiscard]] const surface_model::SurfaceComposition& Surfaces() const;

    // Rebuilds the universe and every runtime capability whose lifetime is
    // bound to its BodyRegistry. Terrain surfaces therefore never retain a
    // registry from a previous universe generation.
    [[nodiscard]] world_model::UniverseCompositionStats
    RebuildUniverse();
    [[nodiscard]] bool RefreshUniverseIfChanged();
    [[nodiscard]] const world_model::UniverseCompositionStats&
    UniverseStats() const;
    [[nodiscard]] const surface_model::SurfaceCompositionStats&
    SurfaceStats() const;

    void Checkpoint();

private:
    struct State;

    [[nodiscard]] State& RequireState();
    [[nodiscard]] const State& RequireState() const;

    documents::ProjectDocument* project_{nullptr};
    std::unique_ptr<State> state_;
    u64 generation_{0};
};
} // namespace orbit::editor_session
