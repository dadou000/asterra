#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/studio_session/StudioAuthoringBinding.hpp>
#include <orbit/studio_session/StudioRuntimeBinding.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Studio authoring binding test failed.\n";
        std::exit(1);
    }
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-authoring-binding-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Studio Authoring Binding Test");
        orbit::studio_session::StudioSession studio(project);
        orbit::studio_session::StudioRuntimeBinding runtime(studio);
        orbit::studio_session::StudioAuthoringBinding authoring(studio);

        auto snapshot = runtime.Refresh();
        Check(snapshot.hasWorld);
        Check(authoring.IsWorldCurrent(snapshot));
        Check(authoring.Explorer(snapshot).Roots().empty());
        Check(authoring.Inspector(snapshot).SelectedObjects().empty());

        const auto worldObject =
            authoring.Commands(snapshot).CreateObject(
                orbit::world_model::kWorldType,
                "World");
        Check(worldObject.IsValid());
        Check(authoring.Explorer(snapshot).Roots().size() == 1U);

        const auto system =
            authoring.Commands(snapshot).CreateObject(
                orbit::world_model::kCelestialSystemType,
                "Helion",
                worldObject);
        const auto body =
            authoring.Commands(snapshot).CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Asterra",
                system);
        authoring.Commands(snapshot).SetProperty(
            body,
            orbit::world_model::kBodyRadius,
            6'000'000.0);

        // UniverseComposition is stale, but the world-scoped authoring graph is
        // intentionally still the same graph and remains safely accessible.
        Check(authoring.IsWorldCurrent(snapshot));
        Check(authoring.Objects(snapshot).Find(body).has_value());
        static_cast<void>(authoring.CommandRegistry(snapshot));
        static_cast<void>(authoring.CommandSurfaces(snapshot));
        static_cast<void>(authoring.Plugins(snapshot));

        const auto composed = runtime.Refresh();
        Check(
            composed.universeGeneration >
            snapshot.universeGeneration);
        Check(authoring.IsWorldCurrent(snapshot));
        Check(authoring.IsWorldCurrent(composed));

        const auto secondary =
            studio.CreateWorld(
                "Secondary",
                "Secondary");
        studio.OpenWorld(secondary.relativePath);

        Check(!authoring.IsWorldCurrent(composed));

        bool staleExplorerRejected = false;
        try
        {
            static_cast<void>(authoring.Explorer(composed));
        }
        catch (const std::logic_error&)
        {
            staleExplorerRejected = true;
        }
        Check(staleExplorerRejected);

        const auto switched = runtime.Refresh();
        Check(authoring.IsWorldCurrent(switched));
        Check(authoring.Explorer(switched).Roots().empty());

        studio.CloseWorld();
        const auto closed = runtime.Refresh();
        Check(!closed.hasWorld);
        Check(!authoring.IsWorldCurrent(closed));

        bool closedCommandsRejected = false;
        try
        {
            static_cast<void>(authoring.Commands(closed));
        }
        catch (const std::logic_error&)
        {
            closedCommandsRejected = true;
        }
        Check(closedCommandsRejected);
    }

    std::filesystem::remove_all(root);
    return 0;
}
