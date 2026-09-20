#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <variant>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr
            << "Rocky planet authoring test failed.\n";
        std::exit(1);
    }
}

template <typename T>
T Property(
    const orbit::scene::ObjectStore& objects,
    const orbit::scene::ObjectId object,
    const orbit::schema::PropertyId property)
{
    const auto value =
        objects.GetProperty(
            object,
            property);

    Check(value.has_value());

    const auto* typed =
        std::get_if<T>(
            &*value);

    Check(typed != nullptr);
    return *typed;
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-rocky-planet-authoring-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(
        root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Rocky Planet Authoring Test");

        orbit::documents::WorldDatabase world(
            project.StartupWorldPath());

        orbit::schema::SchemaRegistry schemas;
        orbit::editor_model::builtin::
            RegisterSchemas(
                schemas);

        orbit::scene::ObjectStore objects(
            world);

        orbit::commands::CommandService
            commands(
                objects,
                schemas);

        orbit::selection::SelectionService
            selection;

        orbit::commands::CommandRegistry
            registry;

        orbit::editor_model::
            authoring_commands::Register(
                registry,
                commands,
                objects,
                selection);

        const auto worldObject =
            commands.CreateObject(
                orbit::world_model::kWorldType,
                "World");

        const orbit::scene::ObjectId
            selected[] = {
                worldObject
            };

        selection.Set(
            selected);

        const auto enabled =
            registry.Enablement(
                orbit::editor_model::
                    authoring_commands::
                        kCreateRockyPlanet);

        Check(enabled.enabled);

        registry.Invoke(
            orbit::editor_model::
                authoring_commands::
                    kCreateRockyPlanet,
            {
                {
                    "name",
                    std::string("Asterra")
                },
                {
                    "radiusMeters",
                    6'000'000.0
                },
                {
                    "massKg",
                    5.7e24
                }
            });

        Check(
            selection.Ordered().size() ==
                1U);

        const auto body =
            selection.Ordered().front();

        const auto bodyRecord =
            objects.Find(
                body);

        Check(bodyRecord.has_value());
        Check(
            bodyRecord->type ==
                orbit::world_model::
                    kCelestialBodyType);
        Check(
            bodyRecord->name ==
                "Asterra");
        Check(
            bodyRecord->parent.
                has_value());

        const auto system =
            *bodyRecord->parent;

        const auto systemRecord =
            objects.Find(
                system);

        Check(systemRecord.has_value());
        Check(
            systemRecord->type ==
                orbit::world_model::
                    kCelestialSystemType);
        Check(
            systemRecord->parent ==
                worldObject);

        Check(
            Property<bool>(
                objects,
                body,
                orbit::world_model::
                    kBodyEllipsoidEnabled) ==
            false);
        Check(
            Property<orbit::f64>(
                objects,
                body,
                orbit::world_model::
                    kBodyRadius) ==
            6'000'000.0);
        Check(
            Property<orbit::f64>(
                objects,
                body,
                orbit::world_model::
                    kBodyPolarRadius) ==
            6'000'000.0);
        Check(
            Property<orbit::f64>(
                objects,
                body,
                orbit::world_model::
                    kBodyMass) ==
            5.7e24);
        Check(
            Property<orbit::f64>(
                objects,
                body,
                orbit::world_model::
                    kBodyRotationPeriodSeconds) ==
            86'400.0);

        const auto bodyChildren =
            objects.Children(
                body);

        Check(
            bodyChildren.size() ==
                1U);
        Check(
            bodyChildren.front().type ==
                orbit::world_model::
                    kTerrainSurfaceType);

        const auto terrain =
            bodyChildren.front().id;

        Check(
            Property<orbit::i64>(
                objects,
                terrain,
                orbit::world_model::
                    kTerrainSeed) ==
            orbit::i64{
                0x41535445525241LL});
        Check(
            Property<orbit::f64>(
                objects,
                terrain,
                orbit::world_model::
                    kTerrainMacroAmplitudeMeters) ==
            1'200.0);
        Check(
            Property<orbit::f64>(
                objects,
                terrain,
                orbit::world_model::
                    kTerrainMacroWavelengthMeters) ==
            800'000.0);
        Check(
            Property<orbit::f64>(
                objects,
                terrain,
                orbit::world_model::
                    kTerrainDetailAmplitudeMeters) ==
            320.0);
        Check(
            Property<orbit::i64>(
                objects,
                terrain,
                orbit::world_model::
                    kTerrainDetailOctaves) ==
            orbit::i64{10});

        // The complete hierarchy was one transaction. One undo removes the
        // terrain, body and the system created for the previously empty World.
        commands.Undo();

        Check(
            !objects.Find(
                terrain).
                has_value());
        Check(
            !objects.Find(
                body).
                has_value());
        Check(
            !objects.Find(
                system).
                has_value());
        Check(
            objects.Find(
                worldObject).
                has_value());
        Check(
            objects.Children(
                worldObject).
                empty());

        commands.Redo();

        Check(
            objects.Find(
                system).
                has_value());
        Check(
            objects.Find(
                body).
                has_value());
        Check(
            objects.Find(
                terrain).
                has_value());

        // Existing systems are reused and therefore survive undo.
        const orbit::scene::ObjectId
            selectedSystem[] = {
                system
            };

        selection.Set(
            selectedSystem);

        registry.Invoke(
            orbit::editor_model::
                authoring_commands::
                    kCreateRockyPlanet);

        const auto secondBody =
            selection.Ordered().
                front();

        Check(
            secondBody !=
                body);

        commands.Undo();

        Check(
            !objects.Find(
                secondBody).
                has_value());
        Check(
            objects.Find(
                system).
                has_value());
    }

    std::filesystem::remove_all(
        root);

    return 0;
}
