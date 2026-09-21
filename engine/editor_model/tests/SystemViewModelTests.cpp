#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/SystemViewModel.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/UniverseComposition.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <string>

int main()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-system-view-model-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "System View Model Test");
        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());
        orbit::schema::SchemaRegistry schemas;
        orbit::world_model::RegisterSchemas(schemas);
        orbit::scene::ObjectStore objects(world);
        orbit::commands::CommandService commands(
            objects,
            schemas);
        orbit::selection::SelectionService selection;

        const auto worldObject =
            commands.CreateObject(
                orbit::world_model::kWorldType,
                "World");
        const auto system =
            commands.CreateObject(
                orbit::world_model::kCelestialSystemType,
                "Helion",
                worldObject);
        const auto body =
            commands.CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Asterra",
                system);
        const auto orbitCapability =
            commands.CreateObject(
                orbit::world_model::kOrbitCapabilityType,
                "Orbit",
                body);

        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kCapabilityModel,
            std::string{"Analytic Conic"});
        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kOrbitSemiMajorAxisMeters,
            10'000.0);
        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kOrbitEccentricity,
            0.0);
        commands.SetProperty(
            orbitCapability,
            orbit::world_model::kOrbitGravitationalParameter,
            1.0e8);

        orbit::world_model::UniverseComposition universe;
        universe.Rebuild(objects);

        orbit::editor_model::SystemViewModel model(
            objects,
            universe,
            selection);

        const auto systems =
            model.Systems();

        if (systems.size() != 1U ||
            systems.front().id != system)
        {
            return 1;
        }

        const auto items =
            model.Items(
                system,
                orbit::time::SimulationTime{});

        if (items.size() != 1U ||
            items.front().object != body ||
            items.front().
                distanceFromSystemOriginMeters !=
                10'000.0)
        {
            return 2;
        }

        const auto samples =
            model.SampleTrajectory(
                body,
                system,
                orbit::time::SimulationTime{},
                60.0,
                8U);

        if (samples.size() != 9U ||
            samples.front().
                positionMeters.x !=
                10'000.0)
        {
            return 3;
        }

        model.Select(body);

        const auto selectedSystem =
            model.SystemForSelection();

        if (!selectedSystem.has_value() ||
            selectedSystem->id != system)
        {
            return 4;
        }

        world.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
