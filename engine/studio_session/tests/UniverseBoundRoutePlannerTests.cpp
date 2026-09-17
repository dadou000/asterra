#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/studio_session/UniverseBoundRoutePlanner.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Universe-bound route planner test failed.\n";
        std::exit(1);
    }
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-universe-route-binding-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Universe Route Binding Test");
        orbit::editor_session::EditorWorldSession world(project);
        orbit::studio_session::UniverseBoundRoutePlanner binding(world);

        Check(binding.HasPlanner());
        Check(binding.BindingGeneration() == 1U);
        Check(
            binding.ObservedUniverseGeneration() ==
            world.UniverseGeneration());
        Check(!binding.RefreshBinding());

        const auto initialUniverseGeneration =
            world.UniverseGeneration();
        static_cast<void>(
            world.Commands().CreateObject(
                orbit::world_model::kWorldType,
                "World"));

        // Semantic edits do not invalidate the binding until composition is
        // actually rebuilt. This keeps the token tied to runtime lifetime.
        Check(!binding.RefreshBinding());
        Check(
            world.UniverseGeneration() ==
            initialUniverseGeneration);

        Check(world.RefreshUniverseIfChanged());
        Check(
            world.UniverseGeneration() >
            initialUniverseGeneration);
        Check(!binding.HasPlanner());

        const auto beforeSemanticRebind =
            binding.BindingGeneration();
        Check(binding.RefreshBinding());
        Check(binding.HasPlanner());
        Check(
            binding.BindingGeneration() >
            beforeSemanticRebind);
        static_cast<void>(binding.Planner());

        const auto beforeExplicitRebuild =
            binding.BindingGeneration();
        static_cast<void>(world.RebuildUniverse());
        Check(!binding.HasPlanner());

        // Non-const Planner() is the safe convenience path: it observes and
        // repairs a stale binding before exposing RoutePlanner.
        static_cast<void>(binding.Planner());
        Check(binding.HasPlanner());
        Check(
            binding.BindingGeneration() >
            beforeExplicitRebuild);

        const auto beforeClose =
            binding.BindingGeneration();
        world.CloseWorld();
        Check(!binding.HasPlanner());
        Check(binding.RefreshBinding());
        Check(!binding.HasPlanner());
        Check(
            binding.BindingGeneration() >
            beforeClose);

        const auto beforeReopen =
            binding.BindingGeneration();
        world.OpenWorld("Main");
        Check(binding.RefreshBinding());
        Check(binding.HasPlanner());
        Check(
            binding.BindingGeneration() >
            beforeReopen);
        Check(
            binding.ObservedUniverseGeneration() ==
            world.UniverseGeneration());
    }

    std::filesystem::remove_all(root);
    return 0;
}
